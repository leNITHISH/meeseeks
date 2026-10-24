/*
 * meeseeks: a crash-safe, log-structured key-value store (Bitcask design).
 *
 * Thread safety: one kv_t may be shared by any number of threads. Gets run in
 * parallel; puts and deletes are serialized but never wait for each other's
 * fsyncs (group commit); compaction runs alongside both. kv_close() must not
 * race with other calls on the same handle. A data directory can be open in
 * only one kv_t at a time, across threads and processes (kv_open returns
 * KV_ERR_LOCKED otherwise).
 */
#ifndef KV_H
#define KV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kv kv_t;

typedef enum {
    KV_SYNC_ALWAYS = 0, /* a write returns only once it is on stable storage */
    KV_SYNC_EVERY_N,    /* fsync once per sync_every_n writes */
    KV_SYNC_NONE        /* leave flushing to the OS (fsync on rollover/close) */
} kv_sync_policy;

typedef struct {
    kv_sync_policy sync_policy;
    uint32_t sync_every_n; /* for KV_SYNC_EVERY_N; 0 means 1 */
    uint64_t segment_size; /* roll to a new segment past this many bytes */
    int use_hints;         /* load compacted segments from hint files */
    /* Background compaction: when dead bytes reach this fraction of all
     * segment bytes (and at least one segment's worth), compact in a
     * background thread. 0 disables it. */
    double auto_compact_ratio;
} kv_options;

/* Return codes. Negative values are errors. */
enum {
    KV_OK = 0,
    KV_NOT_FOUND = 1,
    KV_ERR_IO = -1,
    KV_ERR_NOMEM = -2,
    KV_ERR_INVALID = -3,
    KV_ERR_CORRUPT = -4,
    KV_ERR_LOCKED = -5, /* the directory is already open elsewhere */
    KV_ERR_FAILED = -6  /* an fsync failed earlier; writes are refused */
};

#define KV_DEFAULT_SEGMENT_SIZE (64u * 1024u * 1024u)
#define KV_MAX_KEY_SIZE (64u * 1024u)
#define KV_MAX_VALUE_SIZE (256u * 1024u * 1024u)

typedef struct {
    uint64_t keys;        /* live keys */
    uint64_t segments;    /* segment files, including the active one */
    uint64_t disk_bytes;  /* segment + hint file bytes */
    uint64_t live_bytes;  /* bytes of the newest record of every live key */
    uint64_t dead_bytes;  /* segment bytes that compaction would reclaim */
    uint64_t puts;        /* since open */
    uint64_t deletes;     /* since open */
    uint64_t syncs;       /* fsyncs issued for writes (one per group commit) */
    uint64_t compactions; /* since open */
    uint64_t index_bytes; /* memory held by the in-memory index */
} kv_stats;

/* Fill opts with defaults: KV_SYNC_EVERY_N (n=100), 64 MiB segments, hints on,
 * no background compaction. */
void kv_default_options(kv_options *opts);

/* Open (creating if needed) the store in directory `path`. opts may be NULL. */
int kv_open(const char *path, const kv_options *opts, kv_t **out);

int kv_put(kv_t *db, const void *key, size_t klen, const void *val, size_t vlen);

/* On KV_OK, *val is a malloc'd buffer (caller frees) of length *vlen, with a
 * NUL byte after the end for convenience. Costs exactly one disk read. */
int kv_get(kv_t *db, const void *key, size_t klen, void **val, size_t *vlen);

/* Returns KV_OK if the key existed, KV_NOT_FOUND otherwise. */
int kv_delete(kv_t *db, const void *key, size_t klen);

/* Called once per key by kv_scan(). Return 0 to continue; any other value
 * stops the scan, and kv_scan() returns it. The key is only valid during the
 * call. No lock is held, so the callback may use the store, kv_get() and
 * kv_put() included. */
typedef int (*kv_scan_fn)(void *ctx, const void *key, size_t klen);

/*
 * Call fn for every key, in no particular order. Reads and writes go on
 * during the scan. Every key that exists for the whole scan is visited
 * exactly once; a key added or deleted while it runs may or may not be, and
 * one deleted and added again may be visited twice. Returns KV_OK, the
 * callback's nonzero value, or KV_ERR_NOMEM.
 */
int kv_scan(kv_t *db, kv_scan_fn fn, void *ctx);

/* Resumable kv_scan(): visit at most max_keys keys (0 = no limit), starting
 * from *cursor, which is 0 for a new scan. On return *cursor says where to
 * resume, and is 0 once every key has been visited. A cursor is only
 * meaningful for the handle that produced it. */
int kv_scan_from(kv_t *db, uint64_t *cursor, size_t max_keys, kv_scan_fn fn,
                 void *ctx);

/* Rewrite all sealed segments, keeping only live records, and write a hint
 * file for each new segment. Reads and writes continue while it runs. */
int kv_compact(kv_t *db);

/* Make every write so far durable. */
int kv_sync(kv_t *db);

/* Write a consistent copy of the store, as of this call, to the directory
 * `path`, which must not exist yet. Files are hard-linked where possible, so
 * it takes milliseconds regardless of the store's size; the result opens like
 * any store. Waits for a running compaction to finish. */
int kv_backup(kv_t *db, const char *path);

/* Flush, fsync and free. Returns the first error seen while closing. */
int kv_close(kv_t *db);

void kv_get_stats(kv_t *db, kv_stats *out);
size_t kv_count(kv_t *db);
uint64_t kv_disk_usage(kv_t *db);

const char *kv_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif
