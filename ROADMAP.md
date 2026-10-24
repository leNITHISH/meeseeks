# Roadmap

Version 1.0 does what the project set out to do: a Bitcask-style store that survives crashes, with the tests to show it. This is the work planned after it, roughly in the order I expect to do it. Each item has to clear the same bar as the rest of the store. If it touches what is on disk, the power-failure checker has to cover it before it ships.

Done since 1.0: the interactive shell (`kvcli`), key iteration (`kv_scan()` and the server's `SCAN`), scrubbing (`kv_scrub()` and a background scrubber), and a logical export and import in `kvcli`. None of them changed the on-disk format.

## Atomic batches

Several puts and deletes that commit together or not at all, for callers that need to update two keys consistently. Today each put stands alone, and a crash between two of them keeps the first.

This needs a change to the record format: a batch is written as a run of records followed by a commit marker, and recovery drops a run that has no marker. The writer mutex already serializes appends, so a batch can go out in one `pwrite` without readers ever seeing half of it, as long as the index is updated for the whole batch in one publish. The power-failure checker needs a workload that tears batches at every sector boundary.

## Selective compaction

Compaction now rewrites every sealed segment, so it needs free space for a full copy of the live data, and it rewrites cold segments that have hardly any garbage. Compacting only the segments with the most dead bytes fixes both.

Two parts of the current design assume a full compaction:

- **Tombstones.** They can be dropped today because every older segment is an input to the same compaction, so no older put survives to come back. With a partial compaction, a tombstone has to be kept as long as any segment outside the compacted set might still hold an older copy of its key.
- **The MANIFEST.** It holds one number, the lowest segment id still live, which only works when everything below it is retired at once. A partial compaction has to name the retired segments explicitly, and recovery has to trust that list.

The store would also need to count live bytes per segment. It counts them only for the whole store now.

## Server protocol

The text protocol cannot carry line breaks in keys or values, `SCAN` cannot list keys that hold whitespace, and every client needs its own thread. Two changes would fix that:

- Speak RESP, the Redis protocol. Keys and values become binary-safe, and `redis-cli` and `redis-benchmark` work against `kvserver` out of the box, which would make a direct comparison with Redis possible.
- Replace thread-per-connection with an event loop (epoll on Linux, kqueue on macOS), so the client limit is set by file descriptors rather than threads.

## Smaller ideas

- Export from a running server. `kvcli EXPORT` needs the directory itself today; a server could export a `kv_backup()` taken first, so it sees one consistent state while it keeps serving.
- Streaming or ranged reads, so a large value does not have to be read and allocated whole.
- Optional per-record compression for large values.
