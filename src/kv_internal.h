/* Internals shared by kv.c, recovery.c and compact.c. */
#ifndef KV_INTERNAL_H
#define KV_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "index.h"
#include "kv.h"

/*
 * Segment ids are (log, sub) pairs packed as log * SEG_SUB_SPAN + sub, and
 * files are named "<log:10>-<sub:6>.seg". Records are replayed in id order.
 *
 *   sub == 0   a log segment: the active one, or a sealed one that was active
 *   sub >= 1   compaction output. Compacting "everything up to log L" writes
 *              L-1, L-2, ..., which sort after all of their inputs and before
 *              log segment L+1, where writes continue during the compaction.
 */
#define SEG_SUB_SPAN 1000000ull
#define SEG_MAX_LOG 9999999999ull
#define SEG_ID(log, sub) ((uint64_t)(log) * SEG_SUB_SPAN + (uint64_t)(sub))
#define SEG_LOG(id) ((id) / SEG_SUB_SPAN)
#define SEG_SUB(id) ((id) % SEG_SUB_SPAN)

#define SEG_NAME_MAX 40

enum {
    SEG_TMP = 1 /* compaction output still under its .tmp name */
};

typedef struct {
    uint64_t id;
    int fd;
    unsigned flags;
    uint64_t size;      /* bytes of valid records */
    uint64_t hint_size; /* size of its .hint file, 0 if none */
} segment;

struct kv {
    int dirfd;
    int lockfd;
    dev_t dev; /* identity of the data directory, for the open-store registry */
    ino_t ino;
    kv_options opts;

    /*
     * Lock order: compact_mu -> write_mu -> lock -> sync_mu, bg_mu.
     *
     * write_mu serializes writers (put, delete, compaction's seal) and is
     * held across the append syscall. `lock` is taken exclusively only to
     * publish a finished append, so readers never wait behind a write: on
     * btrfs a pwrite blocks for the whole of a concurrent fsync of the same
     * file, and on any Linux filesystem dirty-page throttling can stall it
     * for many milliseconds.
     */
    pthread_mutex_t write_mu;
    int act_fd;          /* the active segment, as seen by writers */
    uint64_t act_id;
    uint64_t act_size;   /* bytes appended and published */
    uint64_t next_log;   /* number of the next log segment to create */
    uint32_t unsynced;   /* writes since the last sync (KV_SYNC_EVERY_N) */
    uint64_t last_ts;
    uint8_t *wbuf;       /* scratch space for encoding a record */
    size_t wbuf_cap;

    /*
     * `lock` guards the index, the segment table and the fields below it.
     * Readers share it; publishing writers and compaction's short critical
     * sections take it exclusively. glibc's rwlock prefers readers, so a
     * steady stream of gets could starve writers forever: a would-be writer
     * holds `turnstile` while it waits, and readers pass through it first.
     * Readers only bother when `writers_waiting` says someone is queued, so
     * the common read path touches no extra shared cache line.
     */
    pthread_rwlock_t lock;
    pthread_mutex_t turnstile;
    atomic_int writers_waiting;
    kv_index idx;
    segment *segs; /* sorted by id; the last one is the active log segment */
    size_t nsegs, segs_cap;
    uint64_t written_lsn; /* bytes published since open: the log position */
    uint64_t dir_gen;     /* bumped when a log segment file is created */
    uint64_t live_bytes;
    uint64_t nputs, ndeletes, ncompactions;

    /* Group commit, guarded by sync_mu. Lock order: lock -> sync_mu. */
    pthread_mutex_t sync_mu;
    pthread_cond_t sync_cv;
    uint64_t durable_lsn;  /* every byte below this is on stable storage */
    uint64_t durable_dir;  /* dir_gen as of the last directory fsync */
    int syncing;           /* a leader is inside fsync right now */
    uint64_t nsyncs;

    /* Set after an fsync failure. The kernel may already have dropped the
     * dirty pages it failed to write (see PostgreSQL's "fsyncgate"), so a
     * retry could falsely succeed: refuse all further writes instead. */
    atomic_int failed;
    atomic_int closing; /* kv_close() has started: background work stops */

    pthread_mutex_t compact_mu; /* one compaction at a time */

    /* Background compaction thread (opts.auto_compact_ratio > 0). */
    pthread_t bg_thread;
    int bg_running;
    pthread_mutex_t bg_mu;
    pthread_cond_t bg_cv;
    int bg_stop, bg_kick;
};

/* ---------- locking ---------- */

static inline void kv_read_lock(kv_t *db)
{
    /* A reader that misses a just-arrived writer gets in once more, at most;
     * every later read sees the flag and queues behind the writer. */
    if (atomic_load_explicit(&db->writers_waiting, memory_order_relaxed)) {
        pthread_mutex_lock(&db->turnstile);
        pthread_mutex_unlock(&db->turnstile);
    }
    pthread_rwlock_rdlock(&db->lock);
}

static inline void kv_write_lock(kv_t *db)
{
    atomic_fetch_add_explicit(&db->writers_waiting, 1, memory_order_relaxed);
    pthread_mutex_lock(&db->turnstile);
    pthread_rwlock_wrlock(&db->lock);
    pthread_mutex_unlock(&db->turnstile);
    atomic_fetch_sub_explicit(&db->writers_waiting, 1, memory_order_relaxed);
}

static inline void kv_unlock(kv_t *db)
{
    pthread_rwlock_unlock(&db->lock);
}

/* ---------- segments (kv.c) ---------- */

void seg_name(char *buf, size_t n, uint64_t id, const char *ext);
segment *seg_find(kv_t *db, uint64_t id);
/* Insert keeping the table sorted. 0, or -1 if out of memory. */
int seg_insert(kv_t *db, const segment *s);
/* Create an empty log segment file. With sync_dir, also make its directory
 * entry durable right away; otherwise the next group commit does. */
int seg_create(kv_t *db, uint64_t id, int *fd_out, int sync_dir);
/* Seal the active segment (fsync it) and start log segment next_log.
 * Requires write_mu; takes `lock` only briefly, never across the fsync. */
int seg_rollover(kv_t *db);
/* Wait until every byte written so far is durable (group commit). */
int kv_sync_all(kv_t *db);

static inline segment *seg_active(kv_t *db)
{
    return &db->segs[db->nsegs - 1];
}

static inline uint64_t kv_disk_bytes_locked(kv_t *db)
{
    uint64_t n = 0;
    for (size_t i = 0; i < db->nsegs; i++)
        n += db->segs[i].size + db->segs[i].hint_size;
    return n;
}

/* Mark the store failed after an fsync error (see `failed`). */
void kv_set_failed(kv_t *db);

/* Wake the background compactor if garbage passed the threshold. */
void kv_maybe_kick_compactor(kv_t *db);

/* ---------- recovery (recovery.c) ---------- */

/* Read the MANIFEST: segments with id < *min_live_id are obsolete. */
void manifest_read(kv_t *db, uint64_t *min_live_id);
int manifest_write(kv_t *db, uint64_t min_live_id);

/* Rebuild the index and segment table from the directory. */
int kv_recover(kv_t *db);

#endif
