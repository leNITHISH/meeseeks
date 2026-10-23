/*
 * Core API: open/close, writes with group commit, reads, stats.
 *
 * On-disk layout of a data directory:
 *
 *   0000000007-000000.seg   log segment 7 (append-only records)
 *   0000000007-000001.seg   compaction output covering everything up to log 7
 *   0000000007-000001.hint  its hint file
 *   MANIFEST                "segments with id < min_live_id are obsolete"
 *   LOCK                    fcntl lock: one process per directory
 *   *.tmp                   unfinished compaction output, deleted on open
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "io.h"
#include "kv_internal.h"
#include "record.h"

/* ---------- segment table ---------- */

void seg_name(char *buf, size_t n, uint64_t id, const char *ext)
{
    snprintf(buf, n, "%010llu-%06llu.%s", (unsigned long long)SEG_LOG(id),
             (unsigned long long)SEG_SUB(id), ext);
}

static size_t seg_lower_bound(kv_t *db, uint64_t id)
{
    size_t lo = 0, hi = db->nsegs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (db->segs[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

segment *seg_find(kv_t *db, uint64_t id)
{
    size_t i = seg_lower_bound(db, id);
    return i < db->nsegs && db->segs[i].id == id ? &db->segs[i] : NULL;
}

int seg_insert(kv_t *db, const segment *s)
{
    if (db->nsegs == db->segs_cap) {
        size_t cap = db->segs_cap ? db->segs_cap * 2 : 16;
        segment *t = realloc(db->segs, cap * sizeof *t);
        if (!t)
            return -1;
        db->segs = t;
        db->segs_cap = cap;
    }
    size_t i = seg_lower_bound(db, s->id);
    memmove(&db->segs[i + 1], &db->segs[i], (db->nsegs - i) * sizeof *s);
    db->segs[i] = *s;
    db->nsegs++;
    return 0;
}

int seg_create(kv_t *db, uint64_t id, int *fd_out)
{
    char name[SEG_NAME_MAX];
    /* an appendable segment must never be paired with a stale hint */
    seg_name(name, sizeof name, id, "hint");
    unlinkat(db->dirfd, name, 0);
    seg_name(name, sizeof name, id, "seg");
    int fd = openat(db->dirfd, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
                    0644);
    if (fd < 0)
        return -1;
    if (io_fsync_dir(db->dirfd) != 0) {
        close(fd);
        unlinkat(db->dirfd, name, 0);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

void kv_set_failed(kv_t *db)
{
    atomic_store(&db->failed, 1);
    pthread_mutex_lock(&db->sync_mu);
    pthread_cond_broadcast(&db->sync_cv); /* release group-commit waiters */
    pthread_mutex_unlock(&db->sync_mu);
}

int seg_rollover(kv_t *db)
{
    /* Sealed segments are always durable. Group commit relies on this: a
     * leader only has to fsync the active segment. */
    if (io_fsync(seg_active(db)->fd) != 0) {
        kv_set_failed(db);
        return KV_ERR_FAILED;
    }
    pthread_mutex_lock(&db->sync_mu);
    db->nsyncs++;
    if (db->written_lsn > db->durable_lsn) {
        db->durable_lsn = db->written_lsn;
        pthread_cond_broadcast(&db->sync_cv);
    }
    pthread_mutex_unlock(&db->sync_mu);
    db->unsynced = 0;

    if (db->next_log > SEG_MAX_LOG)
        return KV_ERR_IO;
    segment s = {.id = SEG_ID(db->next_log, 0)};
    if (seg_create(db, s.id, &s.fd) != 0)
        return KV_ERR_IO;
    if (seg_insert(db, &s) != 0) {
        char name[SEG_NAME_MAX];
        seg_name(name, sizeof name, s.id, "seg");
        close(s.fd);
        unlinkat(db->dirfd, name, 0);
        return KV_ERR_NOMEM;
    }
    db->next_log++;
    kv_maybe_kick_compactor(db);
    return KV_OK;
}

/* ---------- one open handle per directory ---------- */

/*
 * fcntl() locks keep other processes out, but they are per-process: a second
 * kv_open() of the same directory in this process would get the lock too, and
 * closing it would silently release the first handle's lock. So we also keep
 * a registry of the directories this process has open.
 */
typedef struct {
    dev_t dev;
    ino_t ino;
} dir_key;

static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;
static dir_key *g_open;
static size_t g_nopen, g_open_cap;

static int registry_add(dev_t dev, ino_t ino)
{
    int rc = KV_OK;
    pthread_mutex_lock(&g_open_mu);
    for (size_t i = 0; i < g_nopen; i++)
        if (g_open[i].dev == dev && g_open[i].ino == ino)
            rc = KV_ERR_LOCKED;
    if (rc == KV_OK && g_nopen == g_open_cap) {
        size_t cap = g_open_cap ? g_open_cap * 2 : 8;
        dir_key *t = realloc(g_open, cap * sizeof *t);
        if (t) {
            g_open = t;
            g_open_cap = cap;
        } else {
            rc = KV_ERR_NOMEM;
        }
    }
    if (rc == KV_OK)
        g_open[g_nopen++] = (dir_key){dev, ino};
    pthread_mutex_unlock(&g_open_mu);
    return rc;
}

static void registry_remove(dev_t dev, ino_t ino)
{
    pthread_mutex_lock(&g_open_mu);
    for (size_t i = 0; i < g_nopen; i++)
        if (g_open[i].dev == dev && g_open[i].ino == ino) {
            g_open[i] = g_open[--g_nopen];
            break;
        }
    if (g_nopen == 0) {
        free(g_open);
        g_open = NULL;
        g_open_cap = 0;
    }
    pthread_mutex_unlock(&g_open_mu);
}

static int lock_dir(kv_t *db)
{
    struct stat st;
    if (fstat(db->dirfd, &st) != 0)
        return KV_ERR_IO;
    int rc = registry_add(st.st_dev, st.st_ino);
    if (rc != KV_OK)
        return rc;
    db->dev = st.st_dev;
    db->ino = st.st_ino;
    db->lockfd = openat(db->dirfd, "LOCK", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (db->lockfd < 0) {
        registry_remove(db->dev, db->ino);
        return KV_ERR_IO;
    }
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET; /* l_start = l_len = 0: the whole file */
    if (fcntl(db->lockfd, F_SETLK, &fl) != 0) {
        rc = errno == EACCES || errno == EAGAIN ? KV_ERR_LOCKED : KV_ERR_IO;
        close(db->lockfd);
        db->lockfd = -1;
        registry_remove(db->dev, db->ino);
        return rc;
    }
    return KV_OK;
}

static void unlock_dir(kv_t *db)
{
    if (db->lockfd >= 0) {
        close(db->lockfd); /* releases the fcntl lock */
        db->lockfd = -1;
        registry_remove(db->dev, db->ino);
    }
}

/* ---------- background compaction ---------- */

static int garbage_over_threshold(kv_t *db)
{
    double ratio = db->opts.auto_compact_ratio;
    if (ratio <= 0)
        return 0;
    uint64_t seg_bytes = 0;
    for (size_t i = 0; i < db->nsegs; i++)
        seg_bytes += db->segs[i].size;
    uint64_t dead = seg_bytes - db->live_bytes;
    return dead >= db->opts.segment_size && (double)dead >= ratio * (double)seg_bytes;
}

/* Called with the write lock held, after a rollover. */
void kv_maybe_kick_compactor(kv_t *db)
{
    if (!db->bg_running || !garbage_over_threshold(db))
        return;
    pthread_mutex_lock(&db->bg_mu);
    db->bg_kick = 1;
    pthread_cond_signal(&db->bg_cv);
    pthread_mutex_unlock(&db->bg_mu);
}

static void *bg_main(void *arg)
{
    kv_t *db = arg;
    pthread_mutex_lock(&db->bg_mu);
    while (!db->bg_stop) {
        if (!db->bg_kick) {
            pthread_cond_wait(&db->bg_cv, &db->bg_mu);
            continue;
        }
        db->bg_kick = 0;
        pthread_mutex_unlock(&db->bg_mu);
        kv_read_lock(db);
        int go = garbage_over_threshold(db);
        kv_unlock(db);
        if (go)
            kv_compact(db); /* failures leave the store consistent */
        pthread_mutex_lock(&db->bg_mu);
    }
    pthread_mutex_unlock(&db->bg_mu);
    return NULL;
}

/* ---------- open / close ---------- */

void kv_default_options(kv_options *o)
{
    o->sync_policy = KV_SYNC_EVERY_N;
    o->sync_every_n = 100;
    o->segment_size = KV_DEFAULT_SEGMENT_SIZE;
    o->use_hints = 1;
    o->auto_compact_ratio = 0;
}

static void free_db(kv_t *db)
{
    for (size_t i = 0; i < db->nsegs; i++)
        close(db->segs[i].fd);
    free(db->segs);
    index_free(&db->idx);
    free(db->wbuf);
    unlock_dir(db);
    if (db->dirfd >= 0)
        close(db->dirfd);
    pthread_cond_destroy(&db->bg_cv);
    pthread_mutex_destroy(&db->bg_mu);
    pthread_mutex_destroy(&db->compact_mu);
    pthread_cond_destroy(&db->sync_cv);
    pthread_mutex_destroy(&db->sync_mu);
    pthread_mutex_destroy(&db->turnstile);
    pthread_rwlock_destroy(&db->lock);
    free(db);
}

static int init_sync(kv_t *db)
{
    /* Initialize every primitive up front so free_db can destroy them all. */
    int bad = 0;
    bad |= pthread_rwlock_init(&db->lock, NULL);
    bad |= pthread_mutex_init(&db->turnstile, NULL);
    bad |= pthread_mutex_init(&db->sync_mu, NULL);
    bad |= pthread_cond_init(&db->sync_cv, NULL);
    bad |= pthread_mutex_init(&db->compact_mu, NULL);
    bad |= pthread_mutex_init(&db->bg_mu, NULL);
    bad |= pthread_cond_init(&db->bg_cv, NULL);
    return bad ? -1 : 0;
}

int kv_open(const char *path, const kv_options *opts, kv_t **out)
{
    if (!path || !out)
        return KV_ERR_INVALID;
    *out = NULL;
    kv_options o;
    if (opts)
        o = *opts;
    else
        kv_default_options(&o);
    if (o.segment_size == 0)
        o.segment_size = KV_DEFAULT_SEGMENT_SIZE;
    if (o.sync_every_n == 0)
        o.sync_every_n = 1;
    if ((unsigned)o.sync_policy > KV_SYNC_NONE ||
        !(o.auto_compact_ratio >= 0 && o.auto_compact_ratio < 1))
        return KV_ERR_INVALID;

    kv_t *db = calloc(1, sizeof *db);
    if (!db)
        return KV_ERR_NOMEM;
    db->dirfd = db->lockfd = -1;
    db->opts = o;
    if (init_sync(db) != 0 || index_init(&db->idx, 1024) != 0) {
        free_db(db);
        return KV_ERR_NOMEM;
    }

    int created = mkdir(path, 0755) == 0;
    if (!created && errno != EEXIST) {
        free_db(db);
        return KV_ERR_IO;
    }
    db->dirfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (db->dirfd < 0) {
        free_db(db);
        return KV_ERR_IO;
    }
    if (created) {
        /* make the new directory's own entry durable */
        int parent = openat(db->dirfd, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent >= 0) {
            io_fsync_dir(parent);
            close(parent);
        }
    }
    int rc = lock_dir(db);
    if (rc == KV_OK)
        rc = kv_recover(db);
    if (rc == KV_OK && db->opts.auto_compact_ratio > 0) {
        if (pthread_create(&db->bg_thread, NULL, bg_main, db) == 0)
            db->bg_running = 1;
        else
            rc = KV_ERR_NOMEM;
    }
    if (rc != KV_OK) {
        free_db(db);
        return rc;
    }
    *out = db;
    return KV_OK;
}

int kv_close(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    atomic_store(&db->closing, 1);
    if (db->bg_running) {
        pthread_mutex_lock(&db->bg_mu);
        db->bg_stop = 1;
        pthread_cond_signal(&db->bg_cv);
        pthread_mutex_unlock(&db->bg_mu);
        pthread_join(db->bg_thread, NULL);
        db->bg_running = 0;
    }
    int rc = KV_OK;
    segment *a = seg_active(db);
    if (atomic_load(&db->failed))
        rc = KV_ERR_FAILED;
    else if (io_fsync(a->fd) != 0)
        rc = KV_ERR_IO;
    if (a->size == 0) {
        /* don't leave an empty segment behind */
        char name[SEG_NAME_MAX];
        seg_name(name, sizeof name, a->id, "seg");
        unlinkat(db->dirfd, name, 0);
        io_fsync_dir(db->dirfd);
    }
    free_db(db);
    return rc;
}

/* ---------- writes ---------- */

static uint64_t now_ts(kv_t *db)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (t <= db->last_ts)
        t = db->last_ts + 1; /* strictly increasing, even if the clock isn't */
    db->last_ts = t;
    return t;
}

static int ensure_wbuf(kv_t *db, uint64_t n)
{
    if (n <= db->wbuf_cap)
        return 0;
    size_t cap = db->wbuf_cap ? db->wbuf_cap : 4096;
    while (cap < n)
        cap *= 2;
    uint8_t *b = realloc(db->wbuf, cap);
    if (!b)
        return -1;
    db->wbuf = b;
    db->wbuf_cap = cap;
    return 0;
}

/* Append one record to the active segment. Write lock held. On success *loc
 * is where it landed and *lsn the log position just past it. */
static int append_locked(kv_t *db, const void *key, uint32_t ksz,
                         const void *val, uint32_t vsz, kv_loc *loc,
                         uint64_t *lsn)
{
    uint64_t rsz = record_size(ksz, vsz);
    if (seg_active(db)->size > 0 &&
        seg_active(db)->size + rsz > db->opts.segment_size) {
        int rc = seg_rollover(db);
        if (rc != KV_OK)
            return rc;
    }
    if (ensure_wbuf(db, rsz) != 0)
        return KV_ERR_NOMEM;
    record_encode(db->wbuf, now_ts(db), key, ksz, val, vsz);

    segment *a = seg_active(db);
    if (io_write_all(a->fd, db->wbuf, (size_t)rsz, (off_t)a->size) != 0) {
        /* Cut off any partial tail. If even that fails, the next append
         * overwrites it in place (we always write at a->size), and replay
         * truncates whatever is left past the last good record. */
        int unused = ftruncate(a->fd, (off_t)a->size);
        (void)unused;
        return KV_ERR_IO;
    }
    loc->seg_id = a->id;
    loc->offset = a->size;
    loc->rec_size = (uint32_t)rsz;
    a->size += rsz;
    db->written_lsn += rsz;
    *lsn = db->written_lsn;
    return KV_OK;
}

/* Must this write wait for an fsync? Write lock held. */
static int needs_sync(kv_t *db)
{
    switch (db->opts.sync_policy) {
    case KV_SYNC_ALWAYS:
        return 1;
    case KV_SYNC_EVERY_N:
        if (++db->unsynced < db->opts.sync_every_n)
            return 0;
        db->unsynced = 0;
        return 1;
    case KV_SYNC_NONE:
    default:
        return 0;
    }
}

/*
 * Group commit: block until every byte below `lsn` is durable. The first
 * waiter becomes the leader and fsyncs the active segment on behalf of
 * everyone queued behind it; writers that arrive during that fsync are
 * covered by the next one. No rwlock is held while waiting, so readers and
 * other writers keep going.
 */
static int wait_durable(kv_t *db, uint64_t lsn)
{
    int rc = KV_OK;
    pthread_mutex_lock(&db->sync_mu);
    for (;;) {
        if (db->durable_lsn >= lsn)
            break;
        if (atomic_load(&db->failed)) {
            rc = KV_ERR_FAILED;
            break;
        }
        if (db->syncing) {
            pthread_cond_wait(&db->sync_cv, &db->sync_mu);
            continue;
        }
        db->syncing = 1;
        pthread_mutex_unlock(&db->sync_mu);

        /* Snapshot the log position and the active segment. Bytes below
         * target are either in that segment or in sealed segments, which
         * rollover already made durable. The dup keeps the descriptor valid
         * even if the segment is sealed and compacted away meanwhile. */
        kv_read_lock(db);
        uint64_t target = db->written_lsn;
        int fd = dup(seg_active(db)->fd);
        kv_unlock(db);
        int ok = fd >= 0 && io_fsync(fd) == 0;
        if (fd >= 0)
            close(fd);

        pthread_mutex_lock(&db->sync_mu);
        db->syncing = 0;
        db->nsyncs++;
        if (ok && target > db->durable_lsn)
            db->durable_lsn = target;
        pthread_cond_broadcast(&db->sync_cv);
        if (!ok) {
            pthread_mutex_unlock(&db->sync_mu);
            kv_set_failed(db);
            return KV_ERR_FAILED;
        }
    }
    pthread_mutex_unlock(&db->sync_mu);
    return rc;
}

int kv_put(kv_t *db, const void *key, size_t klen, const void *val, size_t vlen)
{
    if (!db || (!key && klen) || (!val && vlen) || klen > KV_MAX_KEY_SIZE ||
        vlen > KV_MAX_VALUE_SIZE)
        return KV_ERR_INVALID;
    if (atomic_load(&db->failed))
        return KV_ERR_FAILED;
    kv_write_lock(db);
    /* Reserve the index slot first so that a failed append can be undone
     * and a failed allocation leaves nothing on disk. */
    int existed;
    kv_loc *slot = index_upsert(&db->idx, key, klen, &existed);
    if (!slot) {
        kv_unlock(db);
        return KV_ERR_NOMEM;
    }
    kv_loc old = *slot, loc;
    uint64_t lsn = 0;
    int sync = 0;
    int rc = append_locked(db, key, (uint32_t)klen, val, (uint32_t)vlen, &loc,
                           &lsn);
    if (rc != KV_OK) {
        if (existed)
            *slot = old;
        else
            index_remove(&db->idx, key, klen, NULL);
    } else {
        *slot = loc;
        if (existed)
            db->live_bytes -= old.rec_size;
        db->live_bytes += loc.rec_size;
        db->nputs++;
        sync = needs_sync(db);
    }
    kv_unlock(db);
    return rc == KV_OK && sync ? wait_durable(db, lsn) : rc;
}

int kv_delete(kv_t *db, const void *key, size_t klen)
{
    if (!db || (!key && klen) || klen > KV_MAX_KEY_SIZE)
        return KV_ERR_INVALID;
    if (atomic_load(&db->failed))
        return KV_ERR_FAILED;
    kv_write_lock(db);
    kv_loc *slot = index_get(&db->idx, key, klen);
    if (!slot) {
        kv_unlock(db);
        return KV_NOT_FOUND;
    }
    uint32_t old_size = slot->rec_size;
    kv_loc loc;
    uint64_t lsn = 0;
    int sync = 0;
    int rc = append_locked(db, key, (uint32_t)klen, NULL, REC_TOMBSTONE, &loc,
                           &lsn);
    if (rc == KV_OK) {
        index_remove(&db->idx, key, klen, NULL);
        db->live_bytes -= old_size;
        db->ndeletes++;
        sync = needs_sync(db);
    }
    kv_unlock(db);
    return rc == KV_OK && sync ? wait_durable(db, lsn) : rc;
}

int kv_sync(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    if (atomic_load(&db->failed))
        return KV_ERR_FAILED;
    kv_read_lock(db);
    uint64_t target = db->written_lsn;
    kv_unlock(db);
    return wait_durable(db, target);
}

/* ---------- reads ---------- */

int kv_get(kv_t *db, const void *key, size_t klen, void **val, size_t *vlen)
{
    if (!db || (!key && klen) || !val || !vlen)
        return KV_ERR_INVALID;
    *val = NULL;
    *vlen = 0;
    kv_read_lock(db);
    kv_loc *lp = index_get(&db->idx, key, klen);
    if (!lp) {
        kv_unlock(db);
        return KV_NOT_FOUND;
    }
    kv_loc loc = *lp;
    segment *s = seg_find(db, loc.seg_id);
    uint8_t *buf = s ? malloc((size_t)loc.rec_size + 1) : NULL;
    if (!buf) {
        kv_unlock(db);
        return s ? KV_ERR_NOMEM : KV_ERR_CORRUPT;
    }
    /* The one disk read: the whole record in a single pread. The read lock
     * keeps compaction from closing this segment underneath us. */
    ssize_t got = io_read_full(s->fd, buf, loc.rec_size, (off_t)loc.offset);
    kv_unlock(db);

    if (got != (ssize_t)loc.rec_size) {
        free(buf);
        return got < 0 ? KV_ERR_IO : KV_ERR_CORRUPT;
    }
    uint32_t ksz = get_u32(buf + 12), vsz = get_u32(buf + 16);
    if (!record_crc_ok(buf, loc.rec_size) || ksz != klen ||
        vsz == REC_TOMBSTONE || record_size(ksz, vsz) != loc.rec_size ||
        memcmp(buf + REC_HDR_SIZE, key, klen) != 0) {
        free(buf);
        return KV_ERR_CORRUPT;
    }
    memmove(buf, buf + REC_HDR_SIZE + ksz, vsz);
    buf[vsz] = '\0';
    *val = buf;
    *vlen = vsz;
    return KV_OK;
}

/* ---------- introspection ---------- */

void kv_get_stats(kv_t *db, kv_stats *st)
{
    memset(st, 0, sizeof *st);
    kv_read_lock(db);
    uint64_t seg_bytes = 0;
    for (size_t i = 0; i < db->nsegs; i++)
        seg_bytes += db->segs[i].size;
    st->keys = db->idx.count;
    st->segments = db->nsegs;
    st->disk_bytes = kv_disk_bytes_locked(db);
    st->live_bytes = db->live_bytes;
    st->dead_bytes = seg_bytes - db->live_bytes;
    st->puts = db->nputs;
    st->deletes = db->ndeletes;
    st->compactions = db->ncompactions;
    kv_unlock(db);
    pthread_mutex_lock(&db->sync_mu);
    st->syncs = db->nsyncs;
    pthread_mutex_unlock(&db->sync_mu);
}

size_t kv_count(kv_t *db)
{
    kv_read_lock(db);
    size_t n = db->idx.count;
    kv_unlock(db);
    return n;
}

uint64_t kv_disk_usage(kv_t *db)
{
    kv_read_lock(db);
    uint64_t n = kv_disk_bytes_locked(db);
    kv_unlock(db);
    return n;
}

const char *kv_strerror(int rc)
{
    switch (rc) {
    case KV_OK:
        return "ok";
    case KV_NOT_FOUND:
        return "not found";
    case KV_ERR_IO:
        return "I/O error";
    case KV_ERR_NOMEM:
        return "out of memory";
    case KV_ERR_INVALID:
        return "invalid argument";
    case KV_ERR_CORRUPT:
        return "corrupt record";
    case KV_ERR_LOCKED:
        return "store is already open";
    case KV_ERR_FAILED:
        return "store failed after an fsync error; reopen it";
    default:
        return "unknown error";
    }
}
