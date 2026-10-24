# meeseeks

[![CI](https://github.com/leNITHISH/meeseeks/actions/workflows/ci.yml/badge.svg)](https://github.com/leNITHISH/meeseeks/actions/workflows/ci.yml)

A crash-safe, log-structured key-value store in C11 and POSIX, modeled on [Bitcask](https://riak.com/assets/bitcask-intro.pdf). Every write is appended to a log, an in-memory hash index points at the newest record of each key, and every get is exactly one disk read. No dependencies beyond libc and pthreads.

Most of the work went into proving that it survives crashes. The test suite kills it with SIGKILL, corrupts and truncates its files at random byte positions, fills the disk halfway through a compaction, and replays a recorded trace of every write and fsync it made to build the disk image a power cut could leave at each point. All of it recovers to a state that actually existed.

Results on a 2013 dual-core laptop with a budget SATA SSD (details in [Benchmarks](#benchmarks)):

| | |
|---|---|
| writes, fsync on every write | 520/s from one thread, 2,200/s from 8 threads sharing fsyncs |
| writes, fsync every 100 | 39,000/s |
| writes, no fsync | 198,000/s |
| get latency, 1M keys | 1.9 us p50 from the page cache, 122 us p50 from the SSD |
| reads, 1M keys | 474,000/s from one thread, 1.14M/s from four |
| startup, 1M keys | 0.31 s from hint files, 1.4 s replaying 1 GB of segments |
| index memory | 66 bytes per key |
| gets while a compaction runs | p99.9 of 18.5 us, worst 1.5 ms |
| TCP server, 8 pipelining clients | 189,000 requests/s at 90% GET |

## Contents

- [Quick start](#quick-start)
- [How it works](#how-it-works): record format, segments, index, reads and scans, writes and durability, recovery, compaction, backups, scrubbing, concurrency
- [Testing](#testing) and the bugs it found
- [Benchmarks](#benchmarks)
- [TCP server](#tcp-server), [kvcli](#kvcli) and [kvtool](#kvtool)
- [Known limitations](#known-limitations) and the [roadmap](ROADMAP.md)

## Quick start

```sh
make            # library, server, kvcli, kvtool, tests, benchmark (build/release)
make test       # full test suite, about 25 seconds
make test-asan  # the suite under AddressSanitizer + UBSan
make test-tsan  # concurrent tests under ThreadSanitizer
make coverage   # line coverage of src/ by the test suite (gcc + gcov)
make fuzz       # libFuzzer targets, 60 s each (needs clang)
make bench BENCH_DIR=/path/on/real/disk
```

The library API is in [`include/kv.h`](include/kv.h):

```c
#include "kv.h"

kv_options opts;
kv_default_options(&opts);
opts.sync_policy = KV_SYNC_ALWAYS;   /* put() returns once data is on disk */

kv_t *db;
if (kv_open("data", &opts, &db) != KV_OK)
    return 1;

kv_put(db, "user:1", 6, "alice", 5);

void *val;
size_t len;
if (kv_get(db, "user:1", 6, &val, &len) == KV_OK) {
    printf("%.*s\n", (int)len, (char *)val);
    free(val);
}

kv_delete(db, "user:1", 6);
kv_compact(db);    /* reclaims space; reads and writes keep going */
kv_close(db);
```

`kv_sync()` forces everything written so far to disk, `kv_scan()` calls a function for every key, `kv_backup()` writes a consistent copy of the store to another directory while it keeps running, `kv_scrub()` reads the store back to look for damage, and `kv_get_stats()` reports keys, live and dead bytes, fsync count, index memory and what scrubbing found. One `kv_t` can be shared by any number of threads.

To try it by hand, `build/release/kvcli ./data` opens a shell on a store (see [kvcli](#kvcli)).

## How it works

### Record format

Every put and delete appends one record. Integers are little-endian.

| bytes | field | why it is there |
|---|---|---|
| 4 | CRC-32 of everything after it | Detects torn writes and bit rot. It covers the header too, so a damaged length field cannot make replay misread the rest of the file. |
| 8 | timestamp, ns since the epoch | For inspection (`kvtool dump`). Order comes from position in the log, so a clock that jumps backwards changes nothing; the store still keeps timestamps strictly increasing. |
| 4 | key size | Framing: replay can find the next record without an index. |
| 4 | value size | Framing again. `0xFFFFFFFF` marks a tombstone, which has no value bytes. |
| key size | key | Replay rebuilds the index from these, and every get compares the stored key with the requested one. |
| value size | value | |

A delete writes a tombstone instead of touching old data. Without it, a restart would replay the old put and bring the key back.

CRC-32 rather than a cryptographic hash because the threat is hardware and crashes, not an attacker. CRC-32 is cheap and guarantees detection of any error burst up to 32 bits. The implementation uses slice-by-8 tables, which made a full replay 3.7 times faster than the byte-at-a-time version.

### Segments

The log is split into segment files, 64 MiB by default. Writes go to the active segment; when the next record would not fit, the active segment is fsynced, sealed and a new one is started.

Files are named `<log>-<sub>.seg`. Normal log segments have sub 0 (`0000000007-000000.seg`). Compacting everything up to log segment 7 produces `0000000007-000001.seg`, `0000000007-000002.seg` and so on, which sort after all of their inputs and before log segment 8. Recovery replays files in name order, and this naming is what lets new writes continue during compaction: they go to segment 8 and later, so they always win over the compacted copies.

A compacted segment has a hint file next to it with one entry per record: key, offset, record size, timestamp and a CRC of the entry, but no value. For 1M keys with 1 KB values, startup from hints takes 0.31 s instead of 1.4 s with the page cache warm, and 0.40 s instead of 2.1 s with it cold.

### Index

An open-addressing hash table maps each key to (segment id, offset, record size). The probe table holds 8-byte slots, each a 32-bit hash and an entry number. Entries are 48 bytes, kept in 16K-entry chunks that never move, and store keys of up to 16 bytes inline, so most keys need no allocation of their own. Lookups compare hashes inside the slot array and touch an entry only on a match, deletion uses backward shifting so there are no tombstones to slow probes down, and growing rehashes without reading any keys. At 1M keys the index takes 66 bytes per key, by its own accounting and by the growth of the resident set alike. The first version, with 40-byte slots and a separate allocation per key, took about 120.

Keys are hashed with SipHash-1-3 under a random per-process key. A plain hash like FNV would let a client of the TCP server send keys that all collide and turn every lookup into a linear scan.

### Reads

A get takes the read lock, looks up the key, and reads the whole record with a single `pread`. It then checks the CRC, the key and the size before returning the value, so a damaged record is reported as `KV_ERR_CORRUPT` and is never returned as data.

### Scans

`kv_scan()` calls a function for every key, in hash order, while reads and writes go on. It copies keys out of the index under the read lock, at most 512 keys, 1 MiB of keys or 4,096 index entries at a time, and calls the function with no lock held, so the function can use the store itself. `kv_scan_from()` takes a cursor instead, for a caller like the server that answers one batch per request and keeps nothing between them. A full scan of 1M keys takes 17 ms.

The walk goes over the index's entries, not its probe table. Deleting a key pulls later slots of its probe run backwards, and growing the table reshuffles all of them, so a cursor into the table can skip a key that was there all along, or return one twice. Entries never move, and a key keeps its entry for as long as it exists, so every key that exists for the whole scan is visited exactly once. A key added or deleted during the scan may or may not be, and one deleted and added again can be visited twice.

### Writes and durability

Writers are serialized by a mutex and append records with `pwrite`. The store's read-write lock is taken exclusively only for the moment it takes to publish a finished append to the index. Then the write waits for durability, depending on the policy:

| policy | a put returns once... | if the machine loses power |
|---|---|---|
| `KV_SYNC_ALWAYS` | its data has been fsynced | nothing acknowledged is lost |
| `KV_SYNC_EVERY_N` (default, n=100) | written; every nth write waits for an fsync covering all before it | up to n-1 acknowledged writes |
| `KV_SYNC_NONE` | written to the page cache | whatever the OS had not flushed |

Durability uses group commit. A writer that needs an fsync either becomes the leader and fsyncs the active segment, or waits for the leader already inside fsync and is covered by the next round. With 8 writer threads and `KV_SYNC_ALWAYS`, each fsync covers about 4 writes, and throughput goes from 570 to 2,230 writes per second.

Whatever the policy, a crash never leaves a state that did not exist. Recovery always produces the result of some prefix of the operations, with nothing missing from the middle. That takes some care:

- A segment is fsynced when it is sealed, before anything is written to the next one. Otherwise its unsynced tail could be lost while newer records in the next segment survived.
- A new segment file's directory entry is fsynced before any record goes into it. POSIX lets unsynced directory changes persist in any combination, so without this, segment N+2 could survive a power cut while N+1 vanished. The power-failure checker found this one.
- Compaction output is fsynced before it is renamed into place, and the directory is fsynced before the MANIFEST that retires the inputs.

If any fsync fails, of a segment, compaction output or directory, the store refuses all further writes (`KV_ERR_FAILED`) until it is reopened. After a failed fsync, Linux may already have dropped the dirty pages it could not write and will not report the error again, so retrying could falsely succeed. This is what PostgreSQL learned in 2018 ("fsyncgate").

On macOS `fsync` only reaches the drive's cache, so the store uses `fcntl(F_FULLFSYNC)` there.

### Recovery

On open, the store reads the MANIFEST, deletes leftovers (temp files from an interrupted compaction, segments the MANIFEST marks obsolete), then replays the segments in order.

Replay stops at the first record that is cut short, has impossible sizes or fails its CRC, truncates the file there and fsyncs it, and continues with the next segment. Anything after a bad record is dropped, even if some of it looks valid: a crash can lose an earlier sector of the file while a later one made it to disk, and keeping the later records would skip over history. The truncation also has to be durable before new writes go into that file. Otherwise a second crash could bring back the old tail behind them.

A hint file is used only if every entry's CRC checks out and the entries cover the segment exactly, record after record. That also catches a segment truncated after its hint was written. A hint that fails is deleted and the segment is replayed instead.

A data directory can be open in one handle at a time. A `LOCK` file with an `fcntl` lock keeps other processes out. Since `fcntl` locks are per-process (and closing any descriptor of the file drops them), the library also keeps an in-process registry of open directories.

### Compaction

Compaction rewrites every sealed segment, keeping only the newest record of each live key and dropping tombstones. Reads and writes continue while it runs:

1. **Seal** (writer mutex): fsync and close the active log segment L and start L+1. Everything up to L is an input and never changes again.
2. **Copy**, one ~1 MiB chunk of an input at a time. Read the chunk without locks; check which records the index still points at, under the read lock; append those to the current output `L-k` and write its hint entries, without locks; then, under the write lock, point the index at the copies, but only for keys whose entry still names the input record. A key written since keeps its newer location. Lock holds are capped at 512 index operations.
3. **Commit**: fsync the outputs, rename them from `.tmp` to their final names, fsync the directory, then atomically replace the MANIFEST (write a temp file, fsync, rename, fsync the directory) with "segments before L-1 are obsolete".
4. **Retire** (write lock, brief): remove the inputs from the segment table, then delete the files.

A crash at any point leaves a directory that recovers correctly:

| crash during | on restart |
|---|---|
| copying | `.tmp` outputs are deleted; the inputs are still there |
| renaming outputs | inputs replay first, then the outputs, which only repeat what the inputs say, then newer log segments |
| after the MANIFEST commit | inputs are deleted; outputs and newer segments replay |
| deleting inputs | the rest are deleted |

Tombstones can be dropped because every older segment is an input to the same compaction. The MANIFEST is what makes deleting inputs safe: without it, a crash halfway through the deletion could leave an old put whose tombstone was already deleted, and the key would come back.

If compaction fails (out of disk, I/O error), the store keeps working. A partly written output stays in use for the records already redirected to it, but keeps its `.tmp` name, so a restart drops it and replays the untouched inputs.

`kv_options.auto_compact_ratio` starts a background thread that compacts when dead bytes pass that fraction of the log.

### Backups

`kv_backup(db, dir)` makes a consistent copy of a running store. Sealed segments never change, so no data has to be copied: it seals the active segment, which puts everything acknowledged so far into sealed, durable segments, then hard-links every segment, hint file and the MANIFEST into the new directory. That takes milliseconds whatever the size of the store, and later compactions cannot disturb the backup, since deleting a file only removes one of its names. Across filesystems, where links are impossible, it copies.

One file is always copied: the segment it just sealed. Opening a store appends to its newest log segment, and right after a backup that segment is the newest in both directories, so sharing it would let each store write into the other's file. A test catches exactly that when the file is linked instead. Another takes five backups while a writer and a compactor are running, and checks that each holds exactly the state after some prefix of the writes.

### Scrubbing

Replay checks every record it reads, but a compacted segment loaded from its hint file is not read at open, and any file can be damaged later. `kv_scrub()` reads every sealed segment back and checks every record, at about 790 MiB/s from the page cache. It holds no lock while it reads, because sealed segments never change: each segment's descriptor is duplicated under the read lock, so a compaction can retire the segment meanwhile, and only the verdict is published. `kv_get_stats()` reports the bytes checked and the number of damaged segments, and `kvtool check` says where the damage is.

With `kv_options.background_scrub` set, a thread does this once for every sealed segment, first the ones present at open and then each one as it is sealed. It pauses 10 ms after every MiB, so it reads at most about 100 MiB/s and leaves the disk to the store's users. `kvserver` turns it on and prints a warning as soon as it finds damage. Reads go through the page cache, so a segment written a moment ago is checked as cached, not as the drive holds it.

### Concurrency

There are three locks, always taken in the order compaction mutex, writer mutex, store lock:

- The **writer mutex** serializes puts, deletes and compaction's seal step, and is held across the `pwrite`.
- The **store lock**, a read-write lock, protects the index and the segment table. Gets hold it shared. Writers hold it exclusively only to publish a finished append, and compaction only for the steps described above.
- The **compaction mutex** allows one compaction at a time.

Two problems shaped this design, both found by tests:

- glibc's `pthread_rwlock_t` prefers readers, so a steady stream of gets starved writers forever: the concurrency test simply hung. A waiting writer now holds a turnstile mutex that readers must pass through. Readers check an atomic counter first and skip the turnstile when no writer is waiting, so the common read path adds only that one load.
- Originally writers held the store lock across their `pwrite`. On btrfs a `pwrite` can block behind a concurrent fsync of the same file, and on any Linux filesystem dirty-page throttling can stall it for much longer. Every reader waited too. Next to a writer storing 4 KiB values, the worst get took **1.03 seconds** and the p99.9 was 559 us. Now the writer mutex is held across the `pwrite` and the store lock only to publish: in the benchmark below the worst get is 3.6 ms and the p99.9 is 164 us, and the writer itself is twice as fast.

## Testing

`make test` runs ten suites in about 25 seconds. They are also run under AddressSanitizer + UBSan (`make test-asan`) and ThreadSanitizer (`make test-tsan`), and `make coverage` reports how much of `src/` they execute: 94% of `kv.c`, 88% of compaction, 87% of recovery, 93% of the index, 85% of scrubbing, all of the record format and CRC code. Most of what is left is handling of failed memory allocations.

| suite | what it does |
|---|---|
| `test_kv` | 35 functional tests: every operation, tombstones across restarts, segment rollover, compaction (including records larger than its read chunk, and refusing corrupt ones), hint files (and proof that startup really uses them), crash states inside compaction, backups, scans (including one whose callback writes to the store), scrubbing a segment damaged behind a hint file, directory locking across processes, stats, background compaction, CRC-32 and SipHash reference vectors |
| `test_model` | 24 random runs of 4,000 operations each against an in-memory model: puts, deletes, gets, compactions, reopens, and about 1,000 simulated crashes (the directory is copied while the store is open and the copy recovered) |
| `test_crash` | 1,000 random truncations, bit flips and torn tails across log and compacted segments, plus 250 damaged hint files and MANIFESTs. The expected state comes from an independent parser of the record format above, and the damaged segment must end exactly at the last good record. |
| `test_kill` | 100 SIGKILLs at random moments, including mid-compaction, all on one directory. Every acknowledged put (about 160,000 per run) must survive with its exact value. |
| `test_faults` | A full disk (via `RLIMIT_FSIZE`) during writes and halfway through a compaction. The torn record must be rolled back, everything acknowledged must stay readable, and a later compaction must succeed. |
| `test_powerfail` | Power-failure simulation, described below. About 15,600 crash images per run. |
| `test_fsyncfail` | fsync starts failing with EIO after N calls (an `LD_PRELOAD` shim), for every N from 0 to 80: while opening, on group commits, at segment rollover and inside compaction, with one and four writers. The store must refuse further writes, keep serving reads, release every waiting writer, and lose no acknowledged write once reopened. |
| `test_concurrency` | Readers, a writer and a compactor at once, checking values never go backwards; 35,000 puts and gets completing during one compaction; 8 fsync-always writers sharing group commits; background compaction keeping up with 4 writers that never pause; backups taken mid-write and mid-compaction; hundreds of scans that must each see every untouched key exactly once while other keys are deleted and re-added, the index grows past 120,000 keys and compaction runs |
| `test_server` | The protocol, including `SCAN` walking 1,000 keys three at a time, and the TCP server end to end: pipelining, requests split across writes, oversized lines, 24 MB of replies to one pipelined burst, 8 concurrent clients, the connection limit, idle timeouts, shutdown with clients connected |
| `test_cli` | The `kvcli` binary, on a store and against a server, which must answer the same script with the same output; `KEYS` over several `SCAN` batches; a directory another process holds; `EXPORT` and `IMPORT` of keys and values the protocol cannot carry, and of exports cut short or damaged; the `kvserver` binary reporting a damaged segment its scrubber found |

**Power-failure checker.** A SIGKILL test keeps the page cache, so it cannot tell whether the right fsyncs happen. `tests/pf_shim.c` is loaded with `LD_PRELOAD` and records every create, write, truncate, fsync, rename and unlink the unmodified library makes in the store directory. `test_powerfail` replays that log through a model of a POSIX filesystem and, at every point in it, builds four images of what the disk could hold after a power cut:

- **durable:** only fsynced data and directory changes survive.
- **torn:** durable, plus a random subset of the unsynced 512-byte sectors and a random prefix of the unsynced directory changes.
- **reorder:** like torn, but any subset of the unsynced directory changes survives, which is all POSIX promises.
- **cached:** everything survives, as in a process crash.

Each image is recovered and must equal the state after some prefix of the operations, covering at least everything known to be durable. The checker runs three workloads:

- `KV_SYNC_ALWAYS`, where every acknowledged operation must survive.
- `KV_SYNC_NONE` with explicit syncs, which checks that a crash never keeps newer records while losing older ones.
- A store that starts with a hole in a segment. Recovery truncates there, and the records written afterwards are exactly the size of the stale ones beyond the hole, so a truncation that never reached the disk would let those stale records reappear behind the new ones.

To check that this catches anything, I removed each fsync the store depends on, one at a time. The checker fails for all eight: the fsync of the active segment on commit, of a segment when it is sealed, of a new segment's directory entry, of a recovery truncation, of compaction output before its rename, of the directory before the MANIFEST commit, of the MANIFEST temp file, and of the directory after the MANIFEST rename.

**Fuzzing.** Three libFuzzer targets run with ASan and UBSan: arbitrary request lines into the protocol handler; arbitrary directory contents into recovery, where any state recovery decides on must survive compaction and reopening unchanged; and random operation sequences, including crashes, against a model. Five minutes each ran 1.6 million, 105,000 and 26,000 inputs without a failure, and CI fuzzes each target for a minute on every push.

**Testing the tests.** `test_crash` and `test_model` were checked against deliberately broken builds: CRC checks disabled, truncation skipped, tombstones never written, compaction keeping stale versions. The first version of `test_crash` missed the skipped truncation, because new appends happened to overwrite the garbage. It now checks the exact length of every recovered segment. The concurrent scan test was checked the same way: a scan that walks the probe table instead of the entries failed it in all seven runs, between the 13th and the 81st scan, each time by returning a key twice.

### Bugs found along the way

Bugs I wrote, and what found each one:

- **Writer starvation.** glibc's reader-preferring rwlock let readers starve writers; `test_concurrency` hung. Fixed with the turnstile described above.
- **A data race on the server's stop flag**, found by TSan. `volatile sig_atomic_t` is safe against a signal handler but not across threads; it is now a C11 atomic.
- **Oversized request lines accepted** when the newline arrived in the same read as the rest of the line (`test_server`).
- **An 82 ms read stall during compaction** (benchmark). Rollover fsynced the old segment while holding the store lock. Now only the writer mutex is held for that fsync.
- **A 1-second read stall next to a busy writer** (benchmark), described under [Concurrency](#concurrency).
- **Out-of-order durability across rollover.** While fixing the 82 ms stall I first made the sealing fsync lazy, which let a crash keep newer records while losing older ones. I noticed while designing the power-failure checker, and the checker confirms it: removing that fsync fails the test.
- **Segment directory entries made durable out of order**, found by the power-failure checker's reorder model, described under [Writes and durability](#writes-and-durability).
- **A key that could be stored but never read back**, found by `fuzz_protocol` in CI. A key ending in a carriage return was accepted by `SET`, but `GET` puts that carriage return at the end of the line, where it reads as a CRLF line ending, so the lookup used a different key. The protocol now refuses carriage returns in keys and values. The input is kept in `fuzz/regressions/` and replayed on every fuzz run.
- **fsync failures handled inconsistently**, found by `test_fsyncfail` after coverage showed that path had never run. Only a failed fsync of a segment's data made the store refuse writes; a failed directory fsync at rollover returned an ordinary I/O error and writing went on, and a failed fsync of compaction output, whose records may already be serving reads, only aborted the compaction.
- **Unbounded reply buffering in the server**, found while reviewing it for resource limits. Replies to all the requests in one network read were collected before sending, so one 64 KB packet of pipelined `GET`s for a 900 KB value asked the server to hold 7 GB. Replies now go out once 1 MB is pending; in that scenario the server stays at 6 MB.
- **A server that never shut down on macOS**, found by CI. The accept loop was woken by calling `shutdown()` on the listening socket, which works on Linux, but on macOS and the BSDs it fails and `accept()` keeps waiting. The loop now waits in `poll()` on the socket and a self-pipe.
- **A scan that could hold the read lock across a million index entries**, found while reviewing scans for lock hold times. A batch stopped after 512 keys but not after a number of entries, so after mass deletes, with most entries free, one hold could walk all of them while a writer waited to publish. A hold now also stops after 4,096 entries.
- **Options taken at face value**, found while reviewing `kvserver`: `-p abc` listened on a random port and `-t -1` set an idle timeout of about 50 days. Each option is now checked, and anything else is a usage error.

## Benchmarks

Measured with `make bench` (source in [`bench/bench.c`](bench/bench.c)).

- **Machine:** an Intel Core i3-4005U laptop (2 cores, 4 threads, 1.7 GHz, from 2013) with 8 GB of RAM and a Crucial BX500 240 GB SATA SSD (DRAM-less), btrfs, Linux 7.2.
- **Setup:** keys are 16 bytes and values 100 bytes unless noted.
- **Method:** a desktop session with a busy browser shared the four hardware threads. To filter out its interference, the throughput, group-commit, startup, reader-scaling, scan and scrub numbers are the best of three runs; the compaction-latency table is the run with the lowest get p99.9; the table of reads next to a busy writer is the run where the writer got the most done. The rest are single runs of many samples. Between runs, tails and fsync-bound numbers still move by 20 to 50 percent; medians and throughput without fsync move much less.

### Index memory (1000k keys of 16 bytes)

The index holds 62.5 MiB: 66 bytes per key. Opening the store grew the resident set by 63.0 MiB.

### Write throughput (1 thread, 16 B keys, 100 B values)

| fsync policy | writes | ops/sec | avg latency |
|---|---:|---:|---:|
| every write (`KV_SYNC_ALWAYS`) | 3000 | 523 | 1912.3 us |
| every 100 writes (`KV_SYNC_EVERY_N`) | 300000 | 39322 | 25.4 us |
| never (`KV_SYNC_NONE`) | 1000000 | 198289 | 5.0 us |

### Group commit: `KV_SYNC_ALWAYS` with concurrent writers

Every put returns only after its data is fsynced. Writers waiting at the same time share one fsync.

| writer threads | ops/sec | writes per fsync |
|---:|---:|---:|
| 1 | 571 | 1.00 |
| 2 | 654 | 1.24 |
| 4 | 1117 | 2.08 |
| 8 | 2233 | 4.05 |

Two writers gain little: on btrfs a write to a file can wait for an fsync of that file in progress, so two writers mostly take turns instead of sharing. More writers queue up behind each fsync and share it.

### Get latency next to a busy writer (100k keys)

One thread does random gets while another writes as fast as it can. Of 3 runs, the one with the most writes is shown, so the read numbers are under the heaviest load seen.

| writer | writes/sec | get p50 | get p99 | get p99.9 | get max |
|---|---:|---:|---:|---:|---:|
| none | 0 | 1.8 us | 3.1 us | 10.5 us | 56.5 us |
| fsync every write, 100 B | 561 | 1.9 us | 4.5 us | 19.8 us | 1435.8 us |
| no fsync, 4 KiB values | 22687 | 2.7 us | 20.6 us | 164.4 us | 3616.6 us |

### Get latency (random keys, page cache warm)

| keys | p50 | p99 | p99.9 | max |
|---:|---:|---:|---:|---:|
| 100k | 1.9 us | 5.5 us | 18.0 us | 288.1 us |
| 1000k | 1.9 us | 2.8 us | 7.8 us | 34.5 us |

### Get latency, cold (page cache evicted, 1000k keys)

| reads | p50 | p99 | p99.9 | max |
|---:|---:|---:|---:|---:|
| 20000 | 122.0 us | 744.2 us | 3603.0 us | 23174.6 us |

Each get is one `pread`, which here has to go to the SSD. The median is that drive's random-read latency; the tail is the drive too.

### Disk usage before and after compaction

100k keys, each overwritten 9 times (100 B values), plus 10% of keys deleted.

| | disk usage | segments | live data |
|---|---:|---:|---:|
| before | 130.0 MiB | 3 | 11.7 MiB |
| after | 15.4 MiB | 2 | 11.7 MiB |

Compaction took 0.81 s (162 MiB/s of input), leaving 11.9% of the original size (the rest is hint files).

### Scan and scrub (1000k keys, 100 B values, page cache warm)

| operation | covers | time | rate |
|---|---:|---:|---:|
| `kv_scan`, every key | 1000000 keys | 16.7 ms | 59.8M keys/s |
| `kv_scrub`, every sealed segment | 129.7 MiB | 164.3 ms | 790 MiB/s |

The scan's callback only counts keys. Scrubbing is bound by the CRC; the background scrubber is held to about 100 MiB/s.

### Startup time (rebuilding the index of 1000k keys, 1000 B values)

| index source | page cache | open time |
|---|---|---:|
| hint files | cold | 398 ms |
| full replay of segments | cold | 2074 ms |
| hint files | warm | 311 ms |
| full replay of segments | warm | 1414 ms |

Data: 988 MiB of segments; hint files: 42 MiB.

### Read throughput vs reader threads (1000k keys, warm, 4 hardware threads)

| reader threads | gets/sec | speedup |
|---:|---:|---:|
| 1 | 474299 | 1.00x |
| 2 | 868817 | 1.83x |
| 4 | 1139325 | 2.40x |
| 8 | 1143863 | 2.41x |

### Latency while compaction runs (200k keys x 5 versions)

Gets and puts issued from one thread while another thread compacts the store, compared with an idle store. Best of 3 runs by get p99.9 during compaction.

| | op | p50 | p99 | p99.9 | max |
|---|---|---:|---:|---:|---:|
| idle | get | 2.0 us | 3.3 us | 8.4 us | 34.3 us |
| idle | put | 3.4 us | 7.9 us | 14.7 us | 208.9 us |
| during compaction | get | 2.1 us | 5.6 us | 18.5 us | 1461.1 us |
| during compaction | put | 3.5 us | 114.8 us | 213.6 us | 55112.8 us |

The 55 ms put is the fsync that seals the active segment when compaction starts; only writers wait for it. In the first version, compaction held the store lock from start to finish, so every read and write waited for the whole compaction. The next version still did that fsync under the store lock, and the worst get in this test was 82 ms.

## TCP server

`kvserver` serves a store over a line-based text protocol, one thread per connection:

```
$ build/release/kvserver -d ./data -p 7379 &
kvserver: 0 keys in ./data, listening on 127.0.0.1:7379
$ nc 127.0.0.1 7379
SET user:1 alice smith
OK
GET user:1
VALUE alice smith
DEL user:1
OK
GET user:1
NOT_FOUND
FLY me to the moon
ERR unknown command 'FLY'
GET
ERR wrong number of arguments for 'GET'
```

The value is everything after the single space that follows the key, so it can contain spaces. Keys and values cannot contain a carriage return, which would be indistinguishable from a CRLF line ending. Commands are case-insensitive, requests can be pipelined, and lines over 1 MiB get `ERR line too long` without dropping the connection.

The server also answers `SCAN <cursor> [count]`, `STATS`, `COMPACT`, `SYNC`, `SCRUB` and `PING`, and every request still gets exactly one reply line. `SCAN` replies `KEYS <next-cursor> <omitted> <key>...` with up to 100 keys (or `count`), fewer once the reply passes 64 KiB, and the cursor to send next, which is 0 once every key has been listed. The server keeps no state between the calls. `<omitted>` counts keys that cannot be written as a token, empty or holding whitespace or a line break, which only a program using the library can store. `STATS` replies `STATS keys=1 segments=2 ...`, one `name=value` pair per field of `kv_stats`.

Options: `-s always|batch|none` sets the fsync policy, `-n` the batch size, `-H` the listen address, `-c` the fraction of garbage that triggers background compaction (default 0.5, `-c 0` turns it off), `-m` the most clients served at once (default 1024; each gets a thread, and connections beyond that are answered `ERR too many connections` and closed), and `-t` how many seconds a client may sit idle, sending nothing or not reading its replies, before it is disconnected (default 300). Background scrubbing is on, and the server reports any damage it finds on stderr as soon as it is found. SIGINT or SIGTERM closes the store cleanly. In a 6-second soak of 2 KB overwrites, the server took 627,000 puts (about 1.3 GB), compacted 9 times along the way, and ended with 59 MB on disk.

Throughput, measured with [`bench/loadgen.c`](bench/loadgen.c) on the laptop described under [Benchmarks](#benchmarks). Client and server share its four hardware threads, the store uses the default fsync policy (every 100 writes), values are 100 bytes, and each row is the best of three 4-second runs:

| clients | requests in flight per client | mix | requests/s | round trip p50 |
|---:|---:|---|---:|---:|
| 1 | 1 | 90% GET | 28,000 | 29 us |
| 8 | 1 | 90% GET | 58,300 | 112 us |
| 8 | 32 | 90% GET | 188,700 | 1.0 ms |
| 8 | 32 | 50% GET | 83,600 | 2.1 ms |
| 8 | 32 | all SET | 63,400 | 2.3 ms |

Mixed pipelines sometimes stall for around 100 ms. A write can wait for an fsync of the same file in progress, and this SSD's fsyncs occasionally take that long; every client with a SET in its batch then waits too.

## kvcli

`kvcli` is a shell for a store. `kvcli DIR` opens the directory itself, and `kvcli -h HOST -p PORT` talks to a running `kvserver`. Either way each line goes through the server's protocol handler, so the two modes answer alike, and `kvcli` prints the replies in a form that is easier to read:

```
$ build/release/kvcli ./data
./data> SET user:1 alice smith
OK
./data> GET user:1
"alice smith"
./data> DEL user:2
(not found)
./data> KEYS user:*
1) "user:1"
(1 key)
./data> STATS
keys             1
segments         1
disk_bytes       37
live_bytes       37
...
./data> BACKUP ./data-backup
OK
./data> FLY me to the moon
(error) unknown command 'FLY'
```

On top of the protocol it has `KEYS [pattern]`, a loop over `SCAN` with glob matching, and three commands that need the store itself and are refused over the network: `BACKUP DIR`, `EXPORT FILE` and `IMPORT FILE`.

`EXPORT` writes every key and value to a new file in a format of its own, independent of the store's: a header line, then length-prefixed pairs, then an end marker with the number of pairs and a CRC-32 of all of them. It carries what the line protocol cannot, such as line breaks, NUL bytes and the empty key, and since `kvcli` holds the directory lock, nothing else writes while it scans, so the file is one consistent state. `IMPORT` reads the file twice and stores nothing unless the first pass finds it whole. Keys it contains replace the store's, and other keys are kept. A test cuts an export short at seven places and flips one bit in it, and each time the import refuses and the store stays empty. A directory that a server already has open cannot be opened a second time, and `kvcli` says so and suggests connecting to the server instead. Input is read a plain line at a time, without readline, and the prompt appears only on a terminal, so a script can pipe commands in. `kvcli DIR GET user:1` runs a single command. A local store uses the default fsync policy: writes reach the disk in groups of 100, and at `SYNC` or when `kvcli` exits. The exit status is 1 if any command failed, and 2 if the store could not be opened or reached.

## kvtool

`kvtool` inspects a data directory without changing it:

```
$ build/release/kvtool dump ./data -v
== 0000000001-000000.seg (123 bytes)
         0       31 PUT "user:1" (5 bytes) "alice"
        31       29 PUT "user:2" (3 bytes) "bob"
        60       37 PUT "user:1" (11 bytes) "alice smith"
        97       26 DEL "user:2"

$ build/release/kvtool check ./data
./data
  segments: 1 (1 log, 0 compacted), 0 hint files
  records:  4 (1 tombstones)
  live:     1 keys, 37 of 123 segment bytes (69.9% reclaimable by compaction)
  ok
```

`check` verifies every record, every hint file against its segment, and the MANIFEST, and exits with status 1 if anything is damaged. After zeroing one byte at offset 70 of that segment:

```
$ build/release/kvtool check ./data
./data
  ! 0000000001-000000.seg: 63 bytes from offset 60 fail to parse or checksum (recovery truncates there)
  segments: 1 (1 log, 0 compacted), 0 hint files
  records:  2 (0 tombstones)
  live:     2 keys, 60 of 60 segment bytes (0.0% reclaimable by compaction)
  DAMAGED
```

## Known limitations

- **The index must fit in RAM**: 66 bytes per key, plus the key itself for keys over 16 bytes. A billion keys would need about 66 GB.
- **One writer at a time.** Appends are serialized; group commit only shares the fsyncs.
- **No range queries or ordered iteration.** The index is a hash table, and `kv_scan()` visits keys in hash order.
- **Values are read whole.** A get allocates and returns the entire value; values are limited to 256 MiB and keys to 64 KiB.
- **Compaction rewrites everything sealed**, not just the dirtiest segments, and needs free space for a copy of the live data.
- **Durability is only as good as the drive.** A drive that acknowledges flushes it has not done defeats any fsync policy.
- **Hint-loaded segments are not checksummed at startup.** Damage inside them shows up as `KV_ERR_CORRUPT` on the affected gets until a scrub reaches it: `kv_scrub()`, or the background scrubber at about 100 MiB/s. Compaction stops rather than copy it.
- **Locking is local.** The `fcntl` lock is advisory and not meant for network filesystems.
- **The server is minimal**: no authentication or TLS, keys cannot contain whitespace and values cannot contain line breaks, and it runs a thread per connection, so the number of clients is capped (`-m`, 1024 by default).
- **The power-failure checker is Linux-only** and assumes 512-byte sectors are written atomically.

Several of these are on the [roadmap](ROADMAP.md).

## Layout

```
include/kv.h        public API
src/kv.c            open/close, writes and group commit, reads, stats
src/recovery.c      MANIFEST, directory scan, replay, hint loading
src/compact.c       concurrent compaction
src/backup.c        online backups with hard links
src/scrub.c         reading sealed segments back to find damage
src/index.c         hash index (SipHash-1-3, chunked entries)
src/record.h/.c     record and hint encoding
src/crc32.c         slice-by-8 CRC-32
src/io.c            pread/pwrite loops, fsync, buffered reader and writer
src/server.c        TCP server and protocol
tools/kvcli.c       interactive shell, local or over TCP
tools/kvtool.c      offline checker and dump
tests/              the suites above, plus the LD_PRELOAD recorder and fault injector
fuzz/               libFuzzer targets
bench/bench.c       benchmarks
bench/loadgen.c     load generator for kvserver
```

## References

- Justin Sheehy and David Smith, [Bitcask: A Log-Structured Hash Table for Fast Key/Value Data](https://riak.com/assets/bitcask-intro.pdf), Basho, 2010.
- Thanumalayan Sankaranarayana Pillai et al., [All File Systems Are Not Created Equal: On the Complexity of Crafting Crash-Consistent Applications](https://www.usenix.org/conference/osdi14/technical-sessions/presentation/pillai), OSDI 2014. The idea behind the power-failure checker.
- [PostgreSQL's fsync() surprise](https://lwn.net/Articles/752063/), LWN, 2018. Why the store stops accepting writes after a failed fsync.
- Jean-Philippe Aumasson and Daniel J. Bernstein, [SipHash: a fast short-input PRF](https://www.aumasson.jp/siphash/siphash.pdf), 2012.
