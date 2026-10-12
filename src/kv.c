#define _POSIX_C_SOURCE 200809L

#include "kv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "crc32.h"
#include "index.h"
#include "record.h"

/*
 * On-disk layout of a data directory:
 *
 *   0000000001.seg   append-only log segments, replayed in id order
 *   0000000001.hint  hint file, only for segments produced by compaction
 *   MANIFEST         "segments with id < min_live_id are obsolete"
 *   *.tmp            in-progress compaction output, deleted on open
 *
 * Segment ids only grow. Compaction writes its output under ids larger than
 * every input, so replaying all files in id order always yields the right
 * state; the MANIFEST exists only so the old inputs can be deleted safely.
 */

#define MANIFEST_NAME "MANIFEST"
#define MANIFEST_MAGIC 0x464D564Bu /* "KVMF" */
#define MANIFEST_SIZE 16u
#define READ_BUF_SIZE (1u << 20)

typedef struct {
    uint32_t id;
    int fd;
    uint64_t size;
} segment;

struct kv {
    int dirfd;
    kv_options opts;
    /* Readers share `lock`; writers take it exclusively. glibc's default
     * rwlock prefers readers, so a steady stream of gets can starve put()
     * forever. A waiting writer holds `turnstile`, which new readers must
     * pass through first, so they queue behind it instead. */
    pthread_rwlock_t lock;
    pthread_mutex_t turnstile;
    kv_index idx;

    segment *segs; /* sorted by id; the last one is the active segment */
    size_t nsegs, segs_cap;

    uint32_t next_id;
    uint32_t unsynced;
    uint64_t last_ts;

    uint8_t *wbuf; /* scratch buffer for encoding records */
    size_t wbuf_cap;
};

/* ---------- locking ---------- */

static void read_lock(kv_t *db)
{
    pthread_mutex_lock(&db->turnstile);
    pthread_mutex_unlock(&db->turnstile);
    pthread_rwlock_rdlock(&db->lock);
}

static void write_lock(kv_t *db)
{
    pthread_mutex_lock(&db->turnstile);
    pthread_rwlock_wrlock(&db->lock);
    pthread_mutex_unlock(&db->turnstile);
}

static void unlock(kv_t *db)
{
    pthread_rwlock_unlock(&db->lock);
}

/* ---------- small helpers ---------- */

static void seg_name(char *buf, size_t n, uint32_t id, const char *ext)
{
    snprintf(buf, n, "%010u.%s", id, ext);
}

static int write_all(int fd, const void *buf, size_t len, off_t off)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t w = pwrite(fd, p, len, off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
        off += w;
    }
    return 0;
}

/* Returns bytes read (short only at EOF) or -1. */
static ssize_t read_full(int fd, void *buf, size_t len, off_t off)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, p + got, len - got, off + (off_t)got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static int fsync_retry(int fd)
{
    int rc;
    do
        rc = fsync(fd);
    while (rc != 0 && errno == EINTR);
    return rc;
}

static uint64_t now_ts(kv_t *db)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (t <= db->last_ts)
        t = db->last_ts + 1;
    db->last_ts = t;
    return t;
}

static int ensure_wbuf(kv_t *db, size_t n)
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

static segment *find_seg(kv_t *db, uint32_t id)
{
    size_t lo = 0, hi = db->nsegs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (db->segs[mid].id == id)
            return &db->segs[mid];
        if (db->segs[mid].id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static segment *active_seg(kv_t *db)
{
    return &db->segs[db->nsegs - 1];
}

static int push_seg(kv_t *db, uint32_t id, int fd, uint64_t size)
{
    if (db->nsegs == db->segs_cap) {
        size_t cap = db->segs_cap ? db->segs_cap * 2 : 16;
        segment *s = realloc(db->segs, cap * sizeof(*s));
        if (!s)
            return -1;
        db->segs = s;
        db->segs_cap = cap;
    }
    db->segs[db->nsegs++] = (segment){.id = id, .fd = fd, .size = size};
    return 0;
}

/* ---------- buffered sequential reader over a segment or hint file ---------- */

typedef struct {
    int fd;
    uint64_t file_size;
    uint8_t *buf;
    size_t cap;
    uint64_t buf_off; /* file offset of buf[0] */
    size_t buf_len;
} reader;

static int reader_init(reader *r, int fd, uint64_t file_size)
{
    r->fd = fd;
    r->file_size = file_size;
    r->cap = READ_BUF_SIZE;
    r->buf = malloc(r->cap);
    r->buf_off = 0;
    r->buf_len = 0;
    return r->buf ? 0 : -1;
}

static void reader_free(reader *r)
{
    free(r->buf);
}

/* Make [off, off+n) available. Returns a pointer to it, or NULL if the range
 * passes EOF (*err = 0) or on I/O / allocation failure (*err = -1). */
static const uint8_t *reader_at(reader *r, uint64_t off, uint64_t n, int *err)
{
    *err = 0;
    if (off + n > r->file_size)
        return NULL;
    if (off >= r->buf_off && off + n <= r->buf_off + r->buf_len)
        return r->buf + (off - r->buf_off);
    if (n > r->cap) {
        uint8_t *b = realloc(r->buf, (size_t)n);
        if (!b) {
            *err = -1;
            return NULL;
        }
        r->buf = b;
        r->cap = (size_t)n;
    }
    size_t want = r->cap;
    if (off + want > r->file_size)
        want = (size_t)(r->file_size - off);
    ssize_t got = read_full(r->fd, r->buf, want, (off_t)off);
    if (got < 0) {
        *err = -1;
        return NULL;
    }
    r->buf_off = off;
    r->buf_len = (size_t)got;
    if ((uint64_t)got < n)
        return NULL; /* file shrank underneath us */
    return r->buf;
}

/* ---------- MANIFEST ---------- */

static uint32_t read_manifest(int dirfd)
{
    int fd = openat(dirfd, MANIFEST_NAME, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t b[MANIFEST_SIZE];
    ssize_t n = read_full(fd, b, sizeof b, 0);
    close(fd);
    /* A bad manifest only means we cannot garbage-collect old inputs; replaying
     * everything in id order is still correct as long as they all exist. */
    if (n != (ssize_t)sizeof b || get_u32(b) != MANIFEST_MAGIC ||
        get_u32(b + 12) != crc32_update(0, b, 12))
        return 0;
    return get_u32(b + 8);
}

static int fsync_dir(kv_t *db)
{
    return fsync_retry(db->dirfd);
}

static int write_manifest(kv_t *db, uint32_t min_live_id)
{
    uint8_t b[MANIFEST_SIZE];
    put_u32(b, MANIFEST_MAGIC);
    put_u32(b + 4, 1); /* version */
    put_u32(b + 8, min_live_id);
    put_u32(b + 12, crc32_update(0, b, 12));

    int fd = openat(db->dirfd, MANIFEST_NAME ".tmp",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write_all(fd, b, sizeof b, 0) != 0 || fsync_retry(fd) != 0) {
        close(fd);
        unlinkat(db->dirfd, MANIFEST_NAME ".tmp", 0);
        return -1;
    }
    close(fd);
    if (renameat(db->dirfd, MANIFEST_NAME ".tmp", db->dirfd, MANIFEST_NAME) != 0)
        return -1;
    return fsync_dir(db);
}

/* ---------- recovery ---------- */

/* Apply one valid record found at seg_id/off to the index. */
static int apply_record(kv_t *db, const uint8_t *rec, uint32_t seg_id,
                        uint64_t off)
{
    uint64_t ts = get_u64(rec + 4);
    uint32_t ksz = get_u32(rec + 12);
    uint32_t vsz = get_u32(rec + 16);
    if (ts > db->last_ts)
        db->last_ts = ts;
    if (vsz == REC_TOMBSTONE) {
        index_remove(&db->idx, rec + REC_HDR_SIZE, ksz);
        return 0;
    }
    kv_loc loc = {.seg_id = seg_id,
                  .rec_size = (uint32_t)record_size(ksz, vsz),
                  .offset = off,
                  .ts = ts};
    return index_put(&db->idx, rec + REC_HDR_SIZE, ksz, &loc);
}

/*
 * Replay every record of a segment into the index. At the first record that
 * is cut short, has impossible sizes or fails its checksum, the file is
 * truncated there: everything after a torn write is untrustworthy.
 * On return *size is the (possibly truncated) valid length.
 */
static int replay_segment(kv_t *db, uint32_t id, int fd, uint64_t *size,
                          int *truncated)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return KV_ERR_IO;
    reader r;
    if (reader_init(&r, fd, (uint64_t)st.st_size) != 0)
        return KV_ERR_NOMEM;

    uint64_t off = 0;
    int err = 0, rc = KV_OK;
    *truncated = 0;
    while (off < r.file_size) {
        const uint8_t *h = reader_at(&r, off, REC_HDR_SIZE, &err);
        if (!h)
            break;
        uint32_t ksz = get_u32(h + 12), vsz = get_u32(h + 16);
        if (ksz > KV_MAX_KEY_SIZE ||
            (vsz != REC_TOMBSTONE && vsz > KV_MAX_VALUE_SIZE))
            break;
        uint64_t rsz = record_size(ksz, vsz);
        const uint8_t *rec = reader_at(&r, off, rsz, &err);
        if (!rec || !record_crc_ok(rec, rsz))
            break;
        if (apply_record(db, rec, id, off) != 0) {
            err = -1;
            break;
        }
        off += rsz;
    }
    if (err) {
        rc = KV_ERR_IO;
    } else if (off < r.file_size) {
        if (ftruncate(fd, (off_t)off) != 0 || fsync_retry(fd) != 0)
            rc = KV_ERR_IO;
        *truncated = 1;
    }
    *size = off;
    reader_free(&r);
    return rc;
}

/*
 * Load a compacted segment's index entries from its hint file. Returns 1 if the
 * hint was used, 0 if it is missing or damaged (caller replays the segment).
 * The hint is accepted only if every entry checks out and the entries tile the
 * segment exactly, which also catches a truncated .seg file.
 */
static int load_hint(kv_t *db, uint32_t id, uint64_t seg_size)
{
    char name[32];
    seg_name(name, sizeof name, id, "hint");
    int fd = openat(db->dirfd, name, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    reader r;
    if (fstat(fd, &st) != 0 || reader_init(&r, fd, (uint64_t)st.st_size) != 0) {
        close(fd);
        return 0;
    }

    /* Validate the whole file before touching the index. */
    int ok = 1, err = 0;
    uint64_t off = 0, covered = 0;
    while (off < r.file_size) {
        const uint8_t *h = reader_at(&r, off, HINT_HDR_SIZE, &err);
        if (!h) {
            ok = 0;
            break;
        }
        uint32_t ksz = get_u32(h + 12);
        uint64_t rec_off = get_u64(h + 16);
        uint32_t rsz = get_u32(h + 24);
        const uint8_t *e = ksz <= KV_MAX_KEY_SIZE
                               ? reader_at(&r, off, HINT_HDR_SIZE + ksz, &err)
                               : NULL;
        if (!e || !hint_crc_ok(e, HINT_HDR_SIZE + ksz) || rec_off != covered ||
            rsz < REC_HDR_SIZE + ksz) {
            ok = 0;
            break;
        }
        covered += rsz;
        off += HINT_HDR_SIZE + ksz;
    }
    if (ok && covered != seg_size)
        ok = 0;

    if (ok) {
        off = 0;
        while (off < r.file_size) {
            const uint8_t *h = reader_at(&r, off, HINT_HDR_SIZE, &err);
            uint32_t ksz = h ? get_u32(h + 12) : 0;
            if (h)
                h = reader_at(&r, off, HINT_HDR_SIZE + ksz, &err);
            if (!h) {
                ok = -1;
                break;
            }
            kv_loc loc = {.seg_id = id,
                          .rec_size = get_u32(h + 24),
                          .offset = get_u64(h + 16),
                          .ts = get_u64(h + 4)};
            if (loc.ts > db->last_ts)
                db->last_ts = loc.ts;
            if (index_put(&db->idx, h + HINT_HDR_SIZE, ksz, &loc) != 0) {
                ok = -1;
                break;
            }
            off += HINT_HDR_SIZE + ksz;
        }
    }
    reader_free(&r);
    close(fd);
    return ok;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static int has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

/* Parse "0000000042.seg" style names: exactly ten digits, a dot, then ext. */
static int parse_id(const char *name, const char *ext, uint32_t *id)
{
    if (strlen(name) != 11 + strlen(ext) || name[10] != '.' ||
        strcmp(name + 11, ext) != 0)
        return 0;
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
        if (name[i] < '0' || name[i] > '9')
            return 0;
        v = v * 10 + (uint64_t)(name[i] - '0');
    }
    if (v == 0 || v > UINT32_MAX)
        return 0;
    *id = (uint32_t)v;
    return 1;
}

/* Collect segment ids, removing temp files, obsolete segments and orphans. */
static int scan_dir(kv_t *db, uint32_t min_live_id, uint32_t **ids_out,
                    size_t *n_out)
{
    int fd = dup(db->dirfd);
    if (fd < 0)
        return -1;
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return -1;
    }
    uint32_t *ids = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    /* Collect names first; unlinking while iterating is unspecified. */
    char **doomed = NULL;
    size_t ndoomed = 0, doomed_cap = 0;
    int rc = 0;
    while ((e = readdir(d)) != NULL) {
        uint32_t id;
        int kill = 0;
        if (has_suffix(e->d_name, ".tmp")) {
            kill = 1;
        } else if (parse_id(e->d_name, "seg", &id)) {
            if (id < min_live_id) {
                kill = 1;
            } else {
                if (n == cap) {
                    cap = cap ? cap * 2 : 64;
                    uint32_t *t = realloc(ids, cap * sizeof *t);
                    if (!t) {
                        rc = -1;
                        break;
                    }
                    ids = t;
                }
                ids[n++] = id;
            }
        } else if (parse_id(e->d_name, "hint", &id)) {
            /* Hints are only consulted for segments that exist, and
             * create_segment() clears any stale hint for a reused id. */
            if (id < min_live_id)
                kill = 1;
        }
        if (kill) {
            if (ndoomed == doomed_cap) {
                doomed_cap = doomed_cap ? doomed_cap * 2 : 16;
                char **t = realloc(doomed, doomed_cap * sizeof *t);
                if (!t) {
                    rc = -1;
                    break;
                }
                doomed = t;
            }
            doomed[ndoomed] = strdup(e->d_name);
            if (!doomed[ndoomed]) {
                rc = -1;
                break;
            }
            ndoomed++;
        }
    }
    closedir(d);
    for (size_t i = 0; i < ndoomed; i++) {
        unlinkat(db->dirfd, doomed[i], 0);
        free(doomed[i]);
    }
    free(doomed);
    if (rc != 0) {
        free(ids);
        return -1;
    }
    if (n)
        qsort(ids, n, sizeof *ids, cmp_u32);
    *ids_out = ids;
    *n_out = n;
    return 0;
}

static int create_segment(kv_t *db, uint32_t id, int *fd_out)
{
    char name[32];
    /* a fresh, appendable segment must never be paired with a stale hint */
    seg_name(name, sizeof name, id, "hint");
    unlinkat(db->dirfd, name, 0);
    seg_name(name, sizeof name, id, "seg");
    int fd = openat(db->dirfd, name, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return -1;
    if (fsync_dir(db) != 0) {
        close(fd);
        unlinkat(db->dirfd, name, 0);
        return -1;
    }
    *fd_out = fd;
    return 0;
}

static int segment_has_hint(kv_t *db, uint32_t id)
{
    char name[32];
    seg_name(name, sizeof name, id, "hint");
    return faccessat(db->dirfd, name, F_OK, 0) == 0;
}

/* ---------- open / close ---------- */

void kv_default_options(kv_options *o)
{
    o->sync_policy = KV_SYNC_EVERY_N;
    o->sync_every_n = 100;
    o->segment_size = KV_DEFAULT_SEGMENT_SIZE;
    o->use_hints = 1;
}

static void free_db(kv_t *db)
{
    for (size_t i = 0; i < db->nsegs; i++)
        close(db->segs[i].fd);
    free(db->segs);
    index_free(&db->idx);
    free(db->wbuf);
    if (db->dirfd >= 0)
        close(db->dirfd);
    pthread_mutex_destroy(&db->turnstile);
    pthread_rwlock_destroy(&db->lock);
    free(db);
}

int kv_open(const char *path, const kv_options *opts, kv_t **out)
{
    if (!path || !out)
        return KV_ERR_INVALID;
    *out = NULL;
    kv_t *db = calloc(1, sizeof *db);
    if (!db)
        return KV_ERR_NOMEM;
    db->dirfd = -1;
    if (opts)
        db->opts = *opts;
    else
        kv_default_options(&db->opts);
    if (db->opts.segment_size == 0)
        db->opts.segment_size = KV_DEFAULT_SEGMENT_SIZE;
    if (db->opts.sync_every_n == 0)
        db->opts.sync_every_n = 1;
    if (pthread_rwlock_init(&db->lock, NULL) != 0) {
        free(db);
        return KV_ERR_NOMEM;
    }
    if (pthread_mutex_init(&db->turnstile, NULL) != 0) {
        pthread_rwlock_destroy(&db->lock);
        free(db);
        return KV_ERR_NOMEM;
    }
    if (index_init(&db->idx, 1024) != 0) {
        pthread_mutex_destroy(&db->turnstile);
        pthread_rwlock_destroy(&db->lock);
        free(db);
        return KV_ERR_NOMEM;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        goto io_fail;
    db->dirfd = open(path, O_RDONLY | O_DIRECTORY);
    if (db->dirfd < 0)
        goto io_fail;

    uint32_t min_live = read_manifest(db->dirfd);
    uint32_t *ids;
    size_t nids;
    if (scan_dir(db, min_live, &ids, &nids) != 0)
        goto io_fail;

    int rc = KV_OK;
    for (size_t i = 0; i < nids; i++) {
        char name[32];
        seg_name(name, sizeof name, ids[i], "seg");
        int fd = openat(db->dirfd, name, O_RDWR);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) != 0) {
            if (fd >= 0)
                close(fd);
            rc = KV_ERR_IO;
            break;
        }
        uint64_t size = (uint64_t)st.st_size;
        int used_hint = 0;
        if (db->opts.use_hints) {
            used_hint = load_hint(db, ids[i], size);
            if (used_hint < 0) {
                close(fd);
                rc = KV_ERR_NOMEM;
                break;
            }
        }
        if (!used_hint) {
            int truncated;
            rc = replay_segment(db, ids[i], fd, &size, &truncated);
            if (rc == KV_OK && truncated) {
                /* a stale hint would no longer tile the file; drop it */
                seg_name(name, sizeof name, ids[i], "hint");
                unlinkat(db->dirfd, name, 0);
            }
        }
        if (rc != KV_OK || push_seg(db, ids[i], fd, size) != 0) {
            close(fd);
            if (rc == KV_OK)
                rc = KV_ERR_NOMEM;
            break;
        }
    }
    free(ids);
    if (rc != KV_OK) {
        free_db(db);
        return rc;
    }

    db->next_id = db->nsegs ? db->segs[db->nsegs - 1].id + 1 : 1;
    if (db->next_id < min_live)
        db->next_id = min_live;

    /* Keep appending to the newest segment unless it is a compacted (hinted)
     * one or already full; otherwise start a fresh active segment. */
    if (db->nsegs == 0 || segment_has_hint(db, active_seg(db)->id) ||
        active_seg(db)->size >= db->opts.segment_size) {
        int fd;
        uint32_t id = db->next_id++;
        if (create_segment(db, id, &fd) != 0)
            goto io_fail;
        if (push_seg(db, id, fd, 0) != 0) {
            close(fd);
            free_db(db);
            return KV_ERR_NOMEM;
        }
    }
    *out = db;
    return KV_OK;

io_fail:
    free_db(db);
    return KV_ERR_IO;
}

int kv_close(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    int rc = KV_OK;
    segment *a = active_seg(db);
    if (fsync_retry(a->fd) != 0)
        rc = KV_ERR_IO;
    if (a->size == 0 && db->nsegs > 0) {
        /* don't leave empty segments behind */
        char name[32];
        seg_name(name, sizeof name, a->id, "seg");
        unlinkat(db->dirfd, name, 0);
        fsync_dir(db);
    }
    free_db(db);
    return rc;
}

/* ---------- writes ---------- */

static int rollover(kv_t *db)
{
    if (fsync_retry(active_seg(db)->fd) != 0)
        return KV_ERR_IO;
    db->unsynced = 0;
    int fd;
    uint32_t id = db->next_id;
    if (create_segment(db, id, &fd) != 0)
        return KV_ERR_IO;
    if (push_seg(db, id, fd, 0) != 0) {
        char name[32];
        seg_name(name, sizeof name, id, "seg");
        close(fd);
        unlinkat(db->dirfd, name, 0);
        return KV_ERR_NOMEM;
    }
    db->next_id++;
    return KV_OK;
}

static int maybe_sync(kv_t *db)
{
    switch (db->opts.sync_policy) {
    case KV_SYNC_ALWAYS:
        return fsync_retry(active_seg(db)->fd) == 0 ? KV_OK : KV_ERR_IO;
    case KV_SYNC_EVERY_N:
        if (++db->unsynced >= db->opts.sync_every_n) {
            db->unsynced = 0;
            return fsync_retry(active_seg(db)->fd) == 0 ? KV_OK : KV_ERR_IO;
        }
        return KV_OK;
    case KV_SYNC_NONE:
    default:
        return KV_OK;
    }
}

/* Append a record to the active segment. Caller holds the write lock. */
static int append_record(kv_t *db, const void *key, uint32_t ksz,
                         const void *val, uint32_t vsz, kv_loc *loc_out)
{
    uint64_t rsz = record_size(ksz, vsz);
    if (active_seg(db)->size > 0 &&
        active_seg(db)->size + rsz > db->opts.segment_size) {
        int rc = rollover(db);
        if (rc != KV_OK)
            return rc;
    }
    if (ensure_wbuf(db, (size_t)rsz) != 0)
        return KV_ERR_NOMEM;
    uint64_t ts = now_ts(db);
    record_encode(db->wbuf, ts, key, ksz, val, vsz);

    segment *a = active_seg(db);
    if (write_all(a->fd, db->wbuf, (size_t)rsz, (off_t)a->size) != 0) {
        /* drop any partial tail so the next append starts clean */
        /* Cut off any partial tail. If even that fails, the next append
         * overwrites it in place (we always write at a->size), and replay
         * truncates whatever is left past the last good record. */
        int unused = ftruncate(a->fd, (off_t)a->size);
        (void)unused;
        return KV_ERR_IO;
    }
    loc_out->seg_id = a->id;
    loc_out->offset = a->size;
    loc_out->rec_size = (uint32_t)rsz;
    loc_out->ts = ts;
    a->size += rsz;
    return KV_OK;
}

int kv_put(kv_t *db, const void *key, size_t klen, const void *val, size_t vlen)
{
    if (!db || (!key && klen) || (!val && vlen) || klen > KV_MAX_KEY_SIZE ||
        vlen > KV_MAX_VALUE_SIZE)
        return KV_ERR_INVALID;
    write_lock(db);
    kv_loc loc;
    int rc = append_record(db, key, (uint32_t)klen, val, (uint32_t)vlen, &loc);
    if (rc == KV_OK && index_put(&db->idx, key, klen, &loc) != 0)
        rc = KV_ERR_NOMEM;
    if (rc == KV_OK)
        rc = maybe_sync(db);
    unlock(db);
    return rc;
}

int kv_delete(kv_t *db, const void *key, size_t klen)
{
    if (!db || (!key && klen) || klen > KV_MAX_KEY_SIZE)
        return KV_ERR_INVALID;
    write_lock(db);
    int rc;
    if (!index_get(&db->idx, key, klen)) {
        rc = KV_NOT_FOUND;
    } else {
        kv_loc loc;
        rc = append_record(db, key, (uint32_t)klen, NULL, REC_TOMBSTONE, &loc);
        if (rc == KV_OK) {
            index_remove(&db->idx, key, klen);
            rc = maybe_sync(db);
        }
    }
    unlock(db);
    return rc;
}

int kv_sync(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    write_lock(db);
    int rc = fsync_retry(active_seg(db)->fd) == 0 ? KV_OK : KV_ERR_IO;
    db->unsynced = 0;
    unlock(db);
    return rc;
}

/* ---------- reads ---------- */

int kv_get(kv_t *db, const void *key, size_t klen, void **val, size_t *vlen)
{
    if (!db || (!key && klen) || !val || !vlen)
        return KV_ERR_INVALID;
    *val = NULL;
    *vlen = 0;
    read_lock(db);
    kv_loc *lp = index_get(&db->idx, key, klen);
    if (!lp) {
        unlock(db);
        return KV_NOT_FOUND;
    }
    kv_loc loc = *lp;
    segment *s = find_seg(db, loc.seg_id);
    uint8_t *buf = s ? malloc((size_t)loc.rec_size + 1) : NULL;
    if (!buf) {
        unlock(db);
        return s ? KV_ERR_NOMEM : KV_ERR_CORRUPT;
    }
    /* The one disk read: the whole record in a single pread. */
    ssize_t got = read_full(s->fd, buf, loc.rec_size, (off_t)loc.offset);
    unlock(db);

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

size_t kv_count(kv_t *db)
{
    read_lock(db);
    size_t n = db->idx.count;
    unlock(db);
    return n;
}

uint64_t kv_disk_usage(kv_t *db)
{
    read_lock(db);
    uint64_t total = 0;
    for (size_t i = 0; i < db->nsegs; i++) {
        char name[32];
        struct stat st;
        total += db->segs[i].size;
        seg_name(name, sizeof name, db->segs[i].id, "hint");
        if (fstatat(db->dirfd, name, &st, 0) == 0)
            total += (uint64_t)st.st_size;
    }
    unlock(db);
    return total;
}

/* ---------- compaction ---------- */

typedef struct {
    int fd;
    uint8_t *buf;
    size_t len, cap;
    uint64_t written;
} bufwriter;

static int bw_flush(bufwriter *w)
{
    if (w->len && write_all(w->fd, w->buf, w->len, (off_t)w->written) != 0)
        return -1;
    w->written += w->len;
    w->len = 0;
    return 0;
}

static int bw_write(bufwriter *w, const void *p, size_t n)
{
    if (w->len + n > w->cap && bw_flush(w) != 0)
        return -1;
    if (n > w->cap) {
        if (write_all(w->fd, p, n, (off_t)w->written) != 0)
            return -1;
        w->written += n;
        return 0;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 0;
}

typedef struct {
    uint32_t id;
    int seg_fd, hint_fd;
    bufwriter seg, hint;
} output;

typedef struct {
    kv_loc *slot; /* points into the index; stable while we hold the lock */
    kv_loc loc;
} pending_move;

static void out_close(output *o)
{
    if (o->seg_fd >= 0)
        close(o->seg_fd);
    if (o->hint_fd >= 0)
        close(o->hint_fd);
    free(o->seg.buf);
    free(o->hint.buf);
    o->seg_fd = o->hint_fd = -1;
    o->seg.buf = o->hint.buf = NULL;
}

static int out_open(kv_t *db, output *o, uint32_t id)
{
    char name[40];
    memset(o, 0, sizeof *o);
    o->id = id;
    o->seg_fd = o->hint_fd = -1;
    o->seg.cap = o->hint.cap = READ_BUF_SIZE;
    o->seg.buf = malloc(o->seg.cap);
    o->hint.buf = malloc(o->hint.cap);
    seg_name(name, sizeof name, id, "seg.tmp");
    o->seg_fd = openat(db->dirfd, name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    seg_name(name, sizeof name, id, "hint.tmp");
    o->hint_fd = openat(db->dirfd, name, O_RDWR | O_CREAT | O_TRUNC, 0644);
    o->seg.fd = o->seg_fd;
    o->hint.fd = o->hint_fd;
    if (!o->seg.buf || !o->hint.buf || o->seg_fd < 0 || o->hint_fd < 0) {
        out_close(o);
        return -1;
    }
    return 0;
}

/* Flush and fsync both temp files; they are renamed later. */
static int out_finish(output *o)
{
    if (bw_flush(&o->seg) != 0 || bw_flush(&o->hint) != 0 ||
        fsync_retry(o->seg_fd) != 0 || fsync_retry(o->hint_fd) != 0)
        return -1;
    return 0;
}

static void unlink_outputs(kv_t *db, output *outs, size_t n)
{
    char name[40];
    for (size_t i = 0; i < n; i++) {
        out_close(&outs[i]);
        const char *exts[] = {"seg.tmp", "hint.tmp", "seg", "hint"};
        for (int k = 0; k < 4; k++) {
            seg_name(name, sizeof name, outs[i].id, exts[k]);
            unlinkat(db->dirfd, name, 0);
        }
    }
}

/*
 * Copy every live record from one input segment into the outputs. A record is
 * live if the index still points at exactly this (segment, offset).
 */
static int compact_segment(kv_t *db, segment *in, output **outs, size_t *nouts,
                           size_t *outs_cap, pending_move **moves,
                           size_t *nmoves)
{
    reader r;
    if (reader_init(&r, in->fd, in->size) != 0)
        return KV_ERR_NOMEM;
    int rc = KV_OK, err = 0;
    uint64_t off = 0;
    uint8_t hint_buf[HINT_HDR_SIZE];
    while (off < in->size) {
        const uint8_t *h = reader_at(&r, off, REC_HDR_SIZE, &err);
        if (!h) {
            rc = err ? KV_ERR_IO : KV_ERR_CORRUPT;
            break;
        }
        uint32_t ksz = get_u32(h + 12), vsz = get_u32(h + 16);
        uint64_t rsz = record_size(ksz, vsz);
        const uint8_t *rec = reader_at(&r, off, rsz, &err);
        if (!rec) {
            rc = err ? KV_ERR_IO : KV_ERR_CORRUPT;
            break;
        }
        kv_loc *slot = vsz == REC_TOMBSTONE
                           ? NULL
                           : index_get(&db->idx, rec + REC_HDR_SIZE, ksz);
        if (slot && slot->seg_id == in->id && slot->offset == off) {
            output *o = &(*outs)[*nouts - 1];
            if (o->seg.written + o->seg.len > 0 &&
                o->seg.written + o->seg.len + rsz > db->opts.segment_size) {
                if (out_finish(o) != 0) {
                    rc = KV_ERR_IO;
                    break;
                }
                if (*nouts == *outs_cap) {
                    size_t cap = *outs_cap * 2;
                    output *t = realloc(*outs, cap * sizeof *t);
                    if (!t) {
                        rc = KV_ERR_NOMEM;
                        break;
                    }
                    *outs = t;
                    *outs_cap = cap;
                }
                if (out_open(db, &(*outs)[*nouts], db->next_id) != 0) {
                    rc = KV_ERR_IO;
                    break;
                }
                db->next_id++;
                (*nouts)++;
                o = &(*outs)[*nouts - 1];
            }
            uint64_t new_off = o->seg.written + o->seg.len;
            /* The record bytes (and so its checksum) are copied verbatim. */
            if (!record_crc_ok(rec, rsz)) {
                rc = KV_ERR_CORRUPT;
                break;
            }
            if (bw_write(&o->seg, rec, (size_t)rsz) != 0) {
                rc = KV_ERR_IO;
                break;
            }
            /* hint entry: header then key */
            uint8_t *hb = hint_buf;
            put_u64(hb + 4, slot->ts);
            put_u32(hb + 12, ksz);
            put_u64(hb + 16, new_off);
            put_u32(hb + 24, (uint32_t)rsz);
            uint32_t crc = crc32_update(0, hb + 4, HINT_HDR_SIZE - 4);
            crc = crc32_update(crc, rec + REC_HDR_SIZE, ksz);
            put_u32(hb, crc);
            if (bw_write(&o->hint, hb, HINT_HDR_SIZE) != 0 ||
                bw_write(&o->hint, rec + REC_HDR_SIZE, ksz) != 0) {
                rc = KV_ERR_IO;
                break;
            }
            pending_move *m = &(*moves)[(*nmoves)++];
            m->slot = slot;
            m->loc = (kv_loc){.seg_id = o->id,
                              .rec_size = (uint32_t)rsz,
                              .offset = new_off,
                              .ts = slot->ts};
        }
        off += rsz;
    }
    reader_free(&r);
    return rc;
}

int kv_compact(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    write_lock(db);

    /* Every existing segment, including the current active one, is an input.
     * Writers are blocked for the whole compaction, so the set cannot change. */
    if (fsync_retry(active_seg(db)->fd) != 0) {
        unlock(db);
        return KV_ERR_IO;
    }
    db->unsynced = 0;
    size_t ninputs = db->nsegs;

    int rc = KV_OK;
    int new_fd = -1;
    uint32_t new_active = 0;
    size_t outs_cap = 4, nouts = 0, nmoves = 0;
    output *outs = malloc(outs_cap * sizeof *outs);
    pending_move *moves = malloc((db->idx.count + 1) * sizeof *moves);
    if (!outs || !moves) {
        rc = KV_ERR_NOMEM;
        goto fail;
    }
    /* Outputs get ids above every input, so replaying in id order stays
     * correct even if we crash before the MANIFEST commit below. */
    uint32_t first_out = db->next_id;
    if (out_open(db, &outs[0], db->next_id) != 0) {
        rc = KV_ERR_IO;
        goto fail;
    }
    db->next_id++;
    nouts = 1;

    for (size_t i = 0; i < ninputs && rc == KV_OK; i++)
        rc = compact_segment(db, &db->segs[i], &outs, &nouts, &outs_cap,
                             &moves, &nmoves);
    if (rc != KV_OK)
        goto fail;
    if (out_finish(&outs[nouts - 1]) != 0) {
        rc = KV_ERR_IO;
        goto fail;
    }
    if (nouts == 1 && outs[0].seg.written == 0) {
        /* no live keys at all: nothing to publish */
        unlink_outputs(db, outs, 1);
        nouts = 0;
    }

    /* Publish the outputs under their final names. Until the MANIFEST changes
     * they only duplicate data the inputs already hold, so a crash here is
     * harmless. */
    for (size_t i = 0; i < nouts; i++) {
        static const char *exts[][2] = {{"seg.tmp", "seg"}, {"hint.tmp", "hint"}};
        for (int k = 0; k < 2; k++) {
            char from[40], to[40];
            seg_name(from, sizeof from, outs[i].id, exts[k][0]);
            seg_name(to, sizeof to, outs[i].id, exts[k][1]);
            if (renameat(db->dirfd, from, db->dirfd, to) != 0) {
                rc = KV_ERR_IO;
                goto fail;
            }
        }
    }

    /* The new active segment must sort after the outputs, otherwise replay
     * would let the compacted copies shadow newer writes. */
    new_active = db->next_id;
    if (create_segment(db, new_active, &new_fd) != 0) { /* fsyncs the dir */
        rc = KV_ERR_IO;
        goto fail;
    }
    db->next_id++;

    /* Commit point: once MANIFEST says min_live_id = first_out, every input is
     * garbage and may be deleted. */
    if (write_manifest(db, first_out) != 0) {
        rc = KV_ERR_IO;
        goto fail;
    }

    /* Swap in the new segment table: outputs, then the new active segment. */
    segment *old = db->segs;
    size_t nold = db->nsegs;
    db->segs = NULL;
    db->nsegs = db->segs_cap = 0;
    int table_ok = 1;
    for (size_t i = 0; i < nouts && table_ok; i++) {
        /* reuse the already-open temp fd; rename does not affect it */
        if (push_seg(db, outs[i].id, outs[i].seg_fd, outs[i].seg.written) != 0)
            table_ok = 0;
        else
            outs[i].seg_fd = -1;
    }
    if (table_ok && push_seg(db, new_active, new_fd, 0) == 0)
        new_fd = -1;
    else
        table_ok = 0;
    if (!table_ok) {
        /* On-disk state is committed and consistent; only the in-memory table
         * could not be built. Reopening the store recovers fully. */
        rc = KV_ERR_NOMEM;
    }
    for (size_t i = 0; i < nmoves; i++)
        *moves[i].slot = moves[i].loc;

    for (size_t i = 0; i < nold; i++) {
        char name[32];
        close(old[i].fd);
        seg_name(name, sizeof name, old[i].id, "seg");
        unlinkat(db->dirfd, name, 0);
        seg_name(name, sizeof name, old[i].id, "hint");
        unlinkat(db->dirfd, name, 0);
    }
    free(old);
    fsync_dir(db);
    if (new_fd >= 0)
        close(new_fd);

    for (size_t i = 0; i < nouts; i++)
        out_close(&outs[i]);
    free(outs);
    free(moves);
    unlock(db);
    return rc;

fail:
    if (outs)
        unlink_outputs(db, outs, nouts);
    if (new_fd >= 0) {
        char name[32];
        close(new_fd);
        seg_name(name, sizeof name, new_active, "seg");
        unlinkat(db->dirfd, name, 0);
    }
    free(outs);
    free(moves);
    unlock(db);
    return rc;
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
    default:
        return "unknown error";
    }
}
