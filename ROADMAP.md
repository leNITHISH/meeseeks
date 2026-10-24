# Roadmap

Version 1.0 does what the project set out to do: a Bitcask-style store that survives crashes, with the tests to show it. This is the work planned after it, roughly in the order I expect to do it. Each item has to clear the same bar as the rest of the store. If it touches what is on disk, the power-failure checker has to cover it before it ships.

## Interactive shell (`kvcli`)

A prompt in the style of `redis-cli` or `sqlite3`: type `GET`, `SET` and `DEL`, plus `COMPACT`, `SYNC`, `STATS` and `BACKUP`, and see the result. Right now the only ways to poke at a store are writing a C program or typing raw protocol into `nc`.

It would have two modes. `kvcli ./data` links the library and opens the directory directly. `kvcli -h host -p port` talks to a running `kvserver`. Local mode cannot open a directory a server already has open, because the directory lock keeps a second handle out, so the shell should say so plainly instead of reporting a bare `KV_ERR_LOCKED`. Input is read a plain line at a time, without readline, to keep the store free of dependencies.

`KEYS` and `SCAN` in the shell depend on the next item.

## Key iteration

There is no way to list the keys in a store. The index knows all of them, but the API only answers questions about one key at a time. A `kv_scan(db, callback, ctx)` or an iterator object would fix that, and it would also give the server and the shell a `KEYS` command and make a logical export possible (dump every key and value, load them into a fresh store).

The open question is what a scan promises while writes continue. Copying the index under the read lock gives a true snapshot but costs memory proportional to the store, and at 1M keys the index is 62 MiB. Walking it in chunks, dropping the lock between them as compaction already does, costs nothing up front, but a key written during the scan may or may not appear. The chunked walk is probably the right default, documented honestly, with an export built on top of a `kv_backup()` taken first.

## Atomic batches

Several puts and deletes that commit together or not at all, for callers that need to update two keys consistently. Today each put stands alone, and a crash between two of them keeps the first.

This needs a change to the record format: a batch is written as a run of records followed by a commit marker, and recovery drops a run that has no marker. The writer mutex already serializes appends, so a batch can go out in one `pwrite` without readers ever seeing half of it, as long as the index is updated for the whole batch in one publish. The power-failure checker needs a workload that tears batches at every sector boundary.

## Selective compaction

Compaction now rewrites every sealed segment, so it needs free space for a full copy of the live data, and it rewrites cold segments that have hardly any garbage. Compacting only the segments with the most dead bytes fixes both.

Two parts of the current design assume a full compaction:

- **Tombstones.** They can be dropped today because every older segment is an input to the same compaction, so no older put survives to come back. With a partial compaction, a tombstone has to be kept as long as any segment outside the compacted set might still hold an older copy of its key.
- **The MANIFEST.** It holds one number, the lowest segment id still live, which only works when everything below it is retired at once. A partial compaction has to name the retired segments explicitly, and recovery has to trust that list.

The store would also need to count live bytes per segment. It counts them only for the whole store now.

## Background scrub

A segment loaded from its hint file is not checksummed at startup, so damage inside it goes unnoticed until a get reads the bad record. `kvtool check` already finds such damage, but only offline. A low-priority background thread that reads each sealed segment once, checks every record and reports the result in `kv_get_stats()` would find damage before a client hits it, while there is still a backup to restore from.

## Server protocol

The text protocol cannot carry line breaks in keys or values, and every client needs its own thread. Two changes would fix that:

- Speak RESP, the Redis protocol. Values become binary-safe, and `redis-cli` and `redis-benchmark` work against `kvserver` out of the box, which would make a direct comparison with Redis possible.
- Replace thread-per-connection with an event loop (epoll on Linux, kqueue on macOS), so the client limit is set by file descriptors rather than threads.

## Smaller ideas

- Streaming or ranged reads, so a large value does not have to be read and allocated whole.
- Optional per-record compression for large values.
