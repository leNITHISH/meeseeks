/*
 * meeseeks: a crash-safe, log-structured key-value store (Bitcask design).
 *
 * Thread safety: a single kv_t may be shared between threads. Reads run
 * concurrently under a read lock; put/delete/compact serialize on a write lock.
 * A data directory must be opened by at most one process at a time.
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
    KV_SYNC_ALWAYS = 0, /* fsync after every put/delete */
    KV_SYNC_EVERY_N,    /* fsync after every sync_every_n writes */
    KV_SYNC_NONE        /* never fsync on writes (only on rollover/close) */
} kv_sync_policy;

typedef struct {
    kv_sync_policy sync_policy;
    uint32_t sync_every_n;   /* used with KV_SYNC_EVERY_N; 0 means 1 */
    uint64_t segment_size;   /* max bytes per segment file; 0 means default */
    int use_hints;           /* load index from hint files when present (default 1) */
} kv_options;

/* Return codes. Negative values are errors. */
enum {
    KV_OK = 0,
    KV_NOT_FOUND = 1,
    KV_ERR_IO = -1,
    KV_ERR_NOMEM = -2,
    KV_ERR_INVALID = -3,
    KV_ERR_CORRUPT = -4
};

#define KV_DEFAULT_SEGMENT_SIZE (64u * 1024u * 1024u)
#define KV_MAX_KEY_SIZE (64u * 1024u)
#define KV_MAX_VALUE_SIZE (256u * 1024u * 1024u)

/* Fill opts with defaults: KV_SYNC_EVERY_N (n=100), 64 MiB segments, hints on. */
void kv_default_options(kv_options *opts);

/* Open (creating if needed) the store in directory `path`. opts may be NULL. */
int kv_open(const char *path, const kv_options *opts, kv_t **out);

int kv_put(kv_t *db, const void *key, size_t klen, const void *val, size_t vlen);

/* On KV_OK, *val is a malloc'd buffer (caller frees) of length *vlen.
 * A NUL byte is appended past the end for convenience. */
int kv_get(kv_t *db, const void *key, size_t klen, void **val, size_t *vlen);

/* Returns KV_OK if the key existed, KV_NOT_FOUND otherwise. */
int kv_delete(kv_t *db, const void *key, size_t klen);

/* Rewrite all immutable segments, keeping only live keys. */
int kv_compact(kv_t *db);

/* Force an fsync of the active segment. */
int kv_sync(kv_t *db);

/* Flush, fsync and free. Returns the first error seen while closing. */
int kv_close(kv_t *db);

/* Introspection, mostly for tests and benchmarks. */
size_t kv_count(kv_t *db);
uint64_t kv_disk_usage(kv_t *db);

const char *kv_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif
