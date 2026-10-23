# meeseeks

[![CI](https://github.com/leNITHISH/meeseeks/actions/workflows/ci.yml/badge.svg)](https://github.com/leNITHISH/meeseeks/actions/workflows/ci.yml)

A crash-safe, log-structured key-value store in C11 and POSIX, modeled on [Bitcask](https://riak.com/assets/bitcask-intro.pdf). Every write is appended to a log, an in-memory hash index points at the newest record of each key, and every get is exactly one disk read. No dependencies beyond libc and pthreads.

Most of the work went into proving that it survives crashes. The test suite kills it with SIGKILL, corrupts and truncates its files at random byte positions, fills the disk halfway through a compaction, and replays a recorded trace of every write and fsync it made to build the disk image a power cut could leave at each point. All of it recovers to a state that actually existed.

Results on a 2013 dual-core laptop with a budget SATA SSD (details in [Benchmarks](#benchmarks)):

| | |
|---|---|
| writes, fsync on every write | 650/s single thread, 2,300/s with 8 threads sharing fsyncs |
| writes, fsync every 100 | 40,000/s |
| writes, no fsync | 230,000/s |
| get latency, 1M keys | 2.1 us p50 from page cache, 157 us p50 from the SSD |
| startup, 1M keys | 0.32 s from hint files, 1.5 s replaying 1 GB of segments |
| index memory | 69 bytes per key |
| worst get latency while a compaction runs | 0.26 ms |

## Contents

- [Quick start](#quick-start)
- [How it works](#how-it-works): record format, segments, index, writes and durability, recovery, compaction, concurrency
- [Testing](#testing) and the bugs it caught
- [Benchmarks](#benchmarks)
- [TCP server](#tcp-server) and [kvtool](#kvtool)
- [Known limitations](#known-limitations)

## Quick start

```sh
make            # library, server, kvtool, tests, benchmark (build/release)
make test       # full test suite, about 20 seconds
make test-asan  # the suite under AddressSanitizer + UBSan
make test-tsan  # concurrent tests under ThreadSanitizer
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

`kv_sync()` forces everything written so far to disk and `kv_get_stats()` reports keys, live and dead bytes, fsync count and index memory. One `kv_t` can be shared by any number of threads.

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

A compacted segment has a hint file next to it with one entry per record: key, offset, record size and a CRC, but no value. Startup reads hints instead of segments, which is 4.6 times faster for 1M keys with 1 KB values.

### Index

An open-addressing hash table maps each key to (segment id, offset, record size). The probe table holds 8-byte slots, each a 32-bit hash and an entry number. Entries are 48 bytes, kept in 16K-entry chunks that never move, and store keys of up to 16 bytes inline, so most keys need no allocation of their own. Lookups compare hashes inside the slot array and touch an entry only on a match, deletion uses backward shifting so there are no tombstones to slow probes down, and growing rehashes without reading any keys. At 1M keys this costs 69 bytes per key; the first version, with 40-byte slots and a malloc per key, used 126.

Keys are hashed with SipHash-1-3 under a random per-process key. A plain hash like FNV would let a client of the TCP server send keys that all collide and turn every lookup into a linear scan.

### Reads

A get takes the read lock, looks up the key, and reads the whole record with a single `pread`. It then checks the CRC, the key and the size before returning the value, so a damaged record is reported as `KV_ERR_CORRUPT` and is never returned as data.

### Writes and durability

Writers are serialized by a mutex and append records with `pwrite`. The store's read-write lock is taken exclusively only for the moment it takes to publish a finished append to the index. Then the write waits for durability, depending on the policy:

| policy | a put returns once... | if the machine loses power |
|---|---|---|
| `KV_SYNC_ALWAYS` | its data has been fsynced | nothing acknowledged is lost |
| `KV_SYNC_EVERY_N` (default, n=100) | written; every nth write waits for an fsync covering all before it | up to n-1 acknowledged writes |
| `KV_SYNC_NONE` | written to the page cache | whatever the OS had not flushed |

Durability uses group commit. A writer that needs an fsync either becomes the leader and fsyncs the active segment, or waits for the leader already inside fsync and is covered by the next round. With 8 writer threads and `KV_SYNC_ALWAYS`, each fsync covers about 4 writes, and throughput goes from 650 to 2,300 writes per second.

Whatever the policy, a crash never leaves a state that did not exist. Recovery always produces the result of some prefix of the operations, with nothing missing from the middle. That takes some care:

- A segment is fsynced when it is sealed, before anything is written to the next one. Otherwise its unsynced tail could be lost while newer records in the next segment survived.
- A new segment file's directory entry is fsynced before any record goes into it. POSIX lets unsynced directory changes persist in any combination, so without this, segment N+2 could survive a power cut while N+1 vanished. The power-failure checker found this one.
- Compaction output is fsynced before it is renamed into place, and the directory is fsynced before the MANIFEST that retires the inputs.

If an fsync fails, the store refuses all further writes (`KV_ERR_FAILED`) until it is reopened. After a failed fsync, Linux may already have dropped the dirty pages it could not write and will not report the error again, so retrying could falsely succeed. This is what PostgreSQL learned in 2018 ("fsyncgate").

On macOS `fsync` only reaches the drive's cache, so the store uses `fcntl(F_FULLFSYNC)` there.

### Recovery

On open, the store reads the MANIFEST, deletes leftovers (temp files from an interrupted compaction, segments the MANIFEST marks obsolete), then replays the segments in order.

Replay stops at the first record that is cut short, has impossible sizes or fails its CRC, truncates the file there and fsyncs it, and continues with the next segment. Anything after a bad record is dropped, even if some of it looks valid: a crash can lose an earlier sector of the file while a later one made it to disk, and keeping the later records would skip over history. The truncation also has to be durable before new writes go into that file. Otherwise a second crash could bring back the old tail behind them.

A hint file is used only if every entry's CRC checks out and the entries cover the segment exactly, record after record. That also catches a segment truncated after its hint was written. A hint that fails is deleted and the segment is replayed instead.

A data directory can be open in one handle at a time. A `LOCK` file with an `fcntl` lock keeps other processes out. Since `fcntl` locks are per-process (and closing any descriptor of the file drops them), the library also keeps an in-process registry of open directories.

### Compaction

Compaction rewrites every sealed segment, keeping only the newest record of each live key and dropping tombstones. Reads and writes continue while it runs:

1. **Seal** (writer mutex, brief): fsync and close the active log segment L and start L+1. Everything up to L is an input and never changes again.
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

### Concurrency

There are three locks, always taken in the order compaction mutex, writer mutex, store lock:

- The **writer mutex** serializes puts, deletes and compaction's seal step, and is held across the `pwrite`.
- The **store lock**, a read-write lock, protects the index and the segment table. Gets hold it shared. Writers hold it exclusively only to publish a finished append, and compaction only for the steps described above.
- The **compaction mutex** allows one compaction at a time.

Two problems shaped this design, both found by tests:

- glibc's `pthread_rwlock_t` prefers readers, so a steady stream of gets starved writers forever: the concurrency test simply hung. A waiting writer now holds a turnstile mutex that readers must pass through. Readers check an atomic counter first and skip the turnstile when no writer is waiting, so the common read path adds only that one load.
- Originally writers held the store lock across their `pwrite`. On btrfs a `pwrite` blocks for the whole of a concurrent fsync of the same file, and on any Linux filesystem dirty-page throttling can stall it for much longer. Every reader waited too. Next to a writer storing 4 KiB values, the worst get took **1.03 seconds**. With the writer mutex holding the store lock only to publish, it takes 0.3 ms.

## Testing

`make test` runs eight suites in about 20 seconds. They are also run under AddressSanitizer + UBSan (`make test-asan`) and ThreadSanitizer (`make test-tsan`).

| suite | what it does |
|---|---|
| `test_kv` | 25 functional tests: every operation, tombstones across restarts, segment rollover, compaction, hint files (and proof that startup really uses them), crash states inside compaction, directory locking across processes, stats, background compaction, CRC-32 and SipHash reference vectors |
| `test_model` | 24 random runs of 4,000 operations each against an in-memory model: puts, deletes, gets, compactions, reopens, and about 1,000 simulated crashes (the directory is copied while the store is open and the copy recovered) |
| `test_crash` | 1,000 random truncations, bit flips and torn tails across log and compacted segments, plus 250 damaged hint files and MANIFESTs. The expected state comes from an independent parser of the record format above, and the damaged segment must end exactly at the last good record. |
| `test_kill` | 100 SIGKILLs at random moments, including mid-compaction, all on one directory. Every acknowledged put (about 160,000 per run) must survive with its exact value. |
| `test_faults` | A full disk (via `RLIMIT_FSIZE`) during writes and halfway through a compaction. The torn record must be rolled back, everything acknowledged must stay readable, and a later compaction must succeed. |
| `test_powerfail` | Power-failure simulation, described below. About 15,600 crash images per run. |
| `test_concurrency` | Readers, a writer and a compactor at once, checking values never go backwards; 35,000 puts and gets completing during one compaction; 8 fsync-always writers sharing group commits |
| `test_server` | The protocol, and the TCP server end to end: pipelining, requests split across writes, oversized lines, 8 concurrent clients, shutdown with clients connected |

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

**Fuzzing.** Three libFuzzer targets run with ASan and UBSan: arbitrary request lines into the protocol handler; arbitrary directory contents into recovery, where any state recovery decides on must survive compaction and reopening unchanged; and random operation sequences, including crashes, against a model.

**Testing the tests.** `test_crash` and `test_model` were checked against deliberately broken builds: CRC checks disabled, truncation skipped, tombstones never written, compaction keeping stale versions. The first version of `test_crash` missed the skipped truncation, because new appends happened to overwrite the garbage. It now checks the exact length of every recovered segment.

### Bugs the tests caught

Bugs I wrote and the suite found:

- **Writer starvation.** glibc's reader-preferring rwlock let readers starve writers; `test_concurrency` hung. Fixed with the turnstile described above.
- **A data race on the server's stop flag**, found by TSan. `volatile sig_atomic_t` is safe against a signal handler but not across threads; it is now a C11 atomic.
- **Oversized request lines accepted** when the newline arrived in the same read as the rest of the line (`test_server`).
- **An 82 ms read stall during compaction** (benchmark). Rollover fsynced the old segment while holding the store lock. Now only the writer mutex is held for that fsync.
- **A 1-second read stall next to a busy writer** (benchmark), described under [Concurrency](#concurrency).
- **Out-of-order durability across rollover.** While fixing the 82 ms stall I first made the sealing fsync lazy, which let a crash keep newer records while losing older ones. I noticed while designing the power-failure checker, and the checker confirms it: removing that fsync fails the test.
- **Segment directory entries made durable out of order**, found by the power-failure checker's reorder model, described under [Writes and durability](#writes-and-durability).

## Benchmarks

Measured with `make bench` (source in [`bench/bench.c`](bench/bench.c)).

- **Machine:** an Intel Core i3-4005U laptop (2 cores, 4 threads, 1.7 GHz, from 2013) with 8 GB of RAM and a Crucial BX500 240 GB SATA SSD (DRAM-less), btrfs, Linux 7.2.
- **Setup:** keys are 16 bytes and values 100 bytes unless noted.
- **Method:** a desktop session with a browser was running, so throughput and startup numbers are the best of three runs. Latency percentiles come from one run of many samples.

BENCHMARK_RESULTS

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

The value is everything after the single space that follows the key, so it can contain spaces. Commands are case-insensitive, requests can be pipelined, and lines over 1 MiB get `ERR line too long` without dropping the connection. Options: `-s always|batch|none` sets the fsync policy, `-n` the batch size, `-H` the listen address. SIGINT or SIGTERM closes the store cleanly.

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

- **The index must fit in RAM**: 69 bytes per key, plus the key itself for keys over 16 bytes. A billion keys would need about 70 GB.
- **One writer at a time.** Appends are serialized; group commit only shares the fsyncs.
- **No range queries or ordered iteration.** The index is a hash table.
- **Values are read whole.** A get allocates and returns the entire value; values are limited to 256 MiB and keys to 64 KiB.
- **Compaction rewrites everything sealed**, not just the dirtiest segments, and needs free space for a copy of the live data.
- **Durability is only as good as the drive.** A drive that acknowledges flushes it has not done defeats any fsync policy.
- **Hint-loaded segments are checksummed lazily.** Damage there shows up as `KV_ERR_CORRUPT` on the affected gets, and compaction refuses to copy it, rather than at startup.
- **Locking is local.** The `fcntl` lock is advisory and not meant for network filesystems.
- **The server is minimal**: no authentication or TLS, values cannot contain line breaks, and it runs a thread per connection.
- **The power-failure checker is Linux-only** and assumes 512-byte sectors are written atomically.

## Layout

```
include/kv.h        public API
src/kv.c            open/close, writes and group commit, reads, stats
src/recovery.c      MANIFEST, directory scan, replay, hint loading
src/compact.c       concurrent compaction
src/index.c         hash index (SipHash-1-3, chunked entries)
src/record.h/.c     record and hint encoding
src/crc32.c         slice-by-8 CRC-32
src/io.c            pread/pwrite loops, fsync, buffered reader and writer
src/server.c        TCP server and protocol
tools/kvtool.c      offline checker and dump
tests/              the suites above, plus the LD_PRELOAD recorder
fuzz/               libFuzzer targets
bench/bench.c       benchmarks
```

## References

- Justin Sheehy and David Smith, [Bitcask: A Log-Structured Hash Table for Fast Key/Value Data](https://riak.com/assets/bitcask-intro.pdf), Basho, 2010.
- Thanumalayan Sankaranarayana Pillai et al., [All File Systems Are Not Created Equal: On the Complexity of Crafting Crash-Consistent Applications](https://www.usenix.org/conference/osdi14/technical-sessions/presentation/pillai), OSDI 2014. The idea behind the power-failure checker.
- [PostgreSQL's fsync() surprise](https://lwn.net/Articles/752063/), LWN, 2018. Why a failed fsync makes the store read-only.
- Jean-Philippe Aumasson and Daniel J. Bernstein, [SipHash: a fast short-input PRF](https://www.aumasson.jp/siphash/siphash.pdf), 2012.
