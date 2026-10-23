/*
 * Startup: rebuild the in-memory index from the data directory.
 *
 * Segments are replayed oldest to newest (by id). A record that is cut short,
 * has impossible sizes or fails its checksum marks the end of the valid data
 * in that segment: the file is truncated there and replay moves on to the next
 * segment. Compacted segments come with a hint file listing just (key, offset,
 * size) for each record, which is loaded instead of reading every value.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "io.h"
#include "kv_internal.h"
#include "record.h"

/* ---------- MANIFEST ---------- */

/*
 * MANIFEST (20 bytes, little-endian):
 *   0   4  magic "KVMF"
 *   4   4  format version (2)
 *   8   8  min_live_id: segments with a smaller id are obsolete
 *   16  4  crc32 of bytes [0, 16)
 *
 * It is replaced atomically (write temp file, fsync, rename, fsync dir) and
 * changes only when a compaction commits.
 */
#define MANIFEST_NAME "MANIFEST"
#define MANIFEST_MAGIC 0x464D564Bu
#define MANIFEST_VERSION 2u
#define MANIFEST_SIZE 20u

void manifest_read(kv_t *db, uint64_t *min_live_id)
{
    *min_live_id = 0;
    int fd = openat(db->dirfd, MANIFEST_NAME, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    uint8_t b[MANIFEST_SIZE];
    ssize_t n = io_read_full(fd, b, sizeof b, 0);
    close(fd);
    /* A damaged MANIFEST only costs us garbage collection: replaying every
     * segment in id order is correct as long as the old ones still exist,
     * and they are deleted only after a MANIFEST naming their successors is
     * durable. */
    if (n != (ssize_t)sizeof b || get_u32(b) != MANIFEST_MAGIC ||
        get_u32(b + 4) != MANIFEST_VERSION ||
        get_u32(b + 16) != crc32_update(0, b, 16))
        return;
    *min_live_id = get_u64(b + 8);
}

int manifest_write(kv_t *db, uint64_t min_live_id)
{
    uint8_t b[MANIFEST_SIZE];
    put_u32(b, MANIFEST_MAGIC);
    put_u32(b + 4, MANIFEST_VERSION);
    put_u64(b + 8, min_live_id);
    put_u32(b + 16, crc32_update(0, b, 16));

    int fd = openat(db->dirfd, MANIFEST_NAME ".tmp",
                    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    if (io_write_all(fd, b, sizeof b, 0) != 0 || io_fsync(fd) != 0) {
        close(fd);
        unlinkat(db->dirfd, MANIFEST_NAME ".tmp", 0);
        return -1;
    }
    close(fd);
    if (renameat(db->dirfd, MANIFEST_NAME ".tmp", db->dirfd, MANIFEST_NAME) != 0)
        return -1;
    return io_fsync_dir(db->dirfd);
}

/* ---------- file names ---------- */

static int digits(const char *p, int n, uint64_t *out)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return 0;
        v = v * 10 + (uint64_t)(p[i] - '0');
    }
    *out = v;
    return 1;
}

/* Parse "<log:10>-<sub:6>.<ext>" exactly. */
static int parse_seg_name(const char *name, const char *ext, uint64_t *id)
{
    uint64_t log, sub;
    if (strlen(name) != 18 + strlen(ext) || name[10] != '-' ||
        name[17] != '.' || strcmp(name + 18, ext) != 0 ||
        !digits(name, 10, &log) || !digits(name + 11, 6, &sub) || log == 0)
        return 0;
    *id = SEG_ID(log, sub);
    return 1;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    char **v;
    size_t n, cap;
} strlist;

static int strlist_add(strlist *l, const char *s)
{
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        char **t = realloc(l->v, cap * sizeof *t);
        if (!t)
            return -1;
        l->v = t;
        l->cap = cap;
    }
    if (!(l->v[l->n] = strdup(s)))
        return -1;
    l->n++;
    return 0;
}

/*
 * Collect the live segment ids in ascending order. Leftovers are deleted:
 * temp files from an interrupted compaction, and segments (with their hints)
 * made obsolete by a committed compaction.
 */
static int scan_dir(kv_t *db, uint64_t min_live_id, uint64_t **ids_out,
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
    rewinddir(d); /* the dup shares its position with dirfd */
    uint64_t *ids = NULL;
    size_t n = 0, cap = 0;
    strlist doomed = {0};
    int rc = 0;
    struct dirent *e;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        uint64_t id;
        if (has_suffix(e->d_name, ".tmp")) {
            rc = strlist_add(&doomed, e->d_name);
        } else if (parse_seg_name(e->d_name, "seg", &id)) {
            if (id < min_live_id) {
                rc = strlist_add(&doomed, e->d_name);
            } else {
                if (n == cap) {
                    cap = cap ? cap * 2 : 64;
                    uint64_t *t = realloc(ids, cap * sizeof *t);
                    if (!t) {
                        rc = -1;
                        break;
                    }
                    ids = t;
                }
                ids[n++] = id;
            }
        } else if (parse_seg_name(e->d_name, "hint", &id)) {
            /* Hints are consulted only for segments that exist, and a new log
             * segment clears any stale hint under its name. */
            if (id < min_live_id)
                rc = strlist_add(&doomed, e->d_name);
        }
    }
    closedir(d);
    /* unlink after iterating: removing entries mid-readdir is unspecified */
    for (size_t i = 0; i < doomed.n; i++) {
        unlinkat(db->dirfd, doomed.v[i], 0);
        free(doomed.v[i]);
    }
    free(doomed.v);
    if (rc != 0) {
        free(ids);
        return -1;
    }
    if (n)
        qsort(ids, n, sizeof *ids, cmp_u64);
    *ids_out = ids;
    *n_out = n;
    return 0;
}

/* ---------- replay ---------- */

static void note_ts(kv_t *db, uint64_t ts)
{
    if (ts > db->last_ts)
        db->last_ts = ts;
}

/* Point key at loc, keeping live_bytes in step. 0, or -1 if out of memory. */
static int index_set(kv_t *db, const uint8_t *key, uint32_t ksz,
                     const kv_loc *loc)
{
    int existed;
    kv_loc *slot = index_upsert(&db->idx, key, ksz, &existed);
    if (!slot)
        return -1;
    if (existed)
        db->live_bytes -= slot->rec_size;
    *slot = *loc;
    db->live_bytes += loc->rec_size;
    return 0;
}

static int apply_record(kv_t *db, const uint8_t *rec, uint64_t seg_id,
                        uint64_t off)
{
    uint32_t ksz = get_u32(rec + 12);
    uint32_t vsz = get_u32(rec + 16);
    note_ts(db, get_u64(rec + 4));
    if (vsz == REC_TOMBSTONE) {
        kv_loc old;
        if (index_remove(&db->idx, rec + REC_HDR_SIZE, ksz, &old))
            db->live_bytes -= old.rec_size;
        return 0;
    }
    kv_loc loc = {.seg_id = seg_id,
                  .offset = off,
                  .rec_size = (uint32_t)record_size(ksz, vsz)};
    return index_set(db, rec + REC_HDR_SIZE, ksz, &loc);
}

/*
 * Replay every record of a segment into the index, truncating the file at the
 * first record that does not check out: a torn write leaves garbage only at
 * the tail, and anything after a bad record cannot be trusted to be in order.
 * *size receives the valid length.
 */
static int replay_segment(kv_t *db, uint64_t id, int fd, uint64_t *size,
                          int *truncated)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return KV_ERR_IO;
    io_reader r;
    if (io_reader_init(&r, fd, (uint64_t)st.st_size) != 0)
        return KV_ERR_NOMEM;

    uint64_t off = 0;
    int err = 0, rc = KV_OK;
    *truncated = 0;
    while (off < r.file_size) {
        const uint8_t *h = io_reader_at(&r, off, REC_HDR_SIZE, &err);
        if (!h)
            break;
        uint32_t ksz = get_u32(h + 12), vsz = get_u32(h + 16);
        if (ksz > KV_MAX_KEY_SIZE ||
            (vsz != REC_TOMBSTONE && vsz > KV_MAX_VALUE_SIZE))
            break;
        uint64_t rsz = record_size(ksz, vsz);
        const uint8_t *rec = io_reader_at(&r, off, rsz, &err);
        if (!rec || !record_crc_ok(rec, rsz))
            break;
        if (apply_record(db, rec, id, off) != 0) {
            rc = KV_ERR_NOMEM;
            break;
        }
        off += rsz;
    }
    if (rc == KV_OK && err) {
        rc = KV_ERR_IO;
    } else if (rc == KV_OK && off < r.file_size) {
        if (ftruncate(fd, (off_t)off) != 0 || io_fsync(fd) != 0)
            rc = KV_ERR_IO;
        *truncated = 1;
    }
    *size = off;
    io_reader_free(&r);
    return rc;
}

/*
 * Load a compacted segment's index entries from its hint file. Returns 1 if the
 * hint was used, 0 if it is missing or damaged (the caller replays the segment
 * instead), -1 if memory ran out. The hint is trusted only if every entry's
 * checksum holds and the entries tile the segment exactly, which also catches
 * a segment that was truncated after its hint was written.
 */
static int load_hint(kv_t *db, uint64_t id, uint64_t seg_size,
                     uint64_t *hint_size)
{
    char name[SEG_NAME_MAX];
    seg_name(name, sizeof name, id, "hint");
    int fd = openat(db->dirfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat st;
    io_reader r;
    if (fstat(fd, &st) != 0 ||
        io_reader_init(&r, fd, (uint64_t)st.st_size) != 0) {
        close(fd);
        return 0;
    }

    /* Pass 1: validate everything before touching the index. */
    int ok = 1, err = 0;
    uint64_t off = 0, covered = 0;
    while (off < r.file_size) {
        const uint8_t *h = io_reader_at(&r, off, HINT_HDR_SIZE, &err);
        if (!h) {
            ok = 0;
            break;
        }
        uint32_t ksz = get_u32(h + 12);
        uint64_t rec_off = get_u64(h + 16);
        uint32_t rsz = get_u32(h + 24);
        const uint8_t *e =
            ksz <= KV_MAX_KEY_SIZE
                ? io_reader_at(&r, off, HINT_HDR_SIZE + ksz, &err)
                : NULL;
        if (!e || !hint_crc_ok(e, HINT_HDR_SIZE + ksz) || rec_off != covered ||
            rsz < REC_HDR_SIZE + ksz ||
            rsz > (uint64_t)REC_HDR_SIZE + ksz + KV_MAX_VALUE_SIZE) {
            ok = 0;
            break;
        }
        covered += rsz;
        off += HINT_HDR_SIZE + ksz;
    }
    if (ok && covered != seg_size)
        ok = 0;

    /* Pass 2: apply. */
    for (off = 0; ok == 1 && off < r.file_size;) {
        const uint8_t *h = io_reader_at(&r, off, HINT_HDR_SIZE, &err);
        uint32_t ksz = h ? get_u32(h + 12) : 0;
        if (h)
            h = io_reader_at(&r, off, HINT_HDR_SIZE + ksz, &err);
        if (!h) {
            /* The file changed under us: replay instead. Entries applied so
             * far are harmless, the replay re-applies the same locations. */
            ok = 0;
            break;
        }
        kv_loc loc = {.seg_id = id,
                      .offset = get_u64(h + 16),
                      .rec_size = get_u32(h + 24)};
        note_ts(db, get_u64(h + 4));
        if (index_set(db, h + HINT_HDR_SIZE, ksz, &loc) != 0)
            ok = -1;
        off += HINT_HDR_SIZE + ksz;
    }
    *hint_size = ok == 1 ? r.file_size : 0;
    io_reader_free(&r);
    close(fd);
    return ok;
}

int kv_recover(kv_t *db)
{
    uint64_t min_live;
    manifest_read(db, &min_live);
    uint64_t *ids;
    size_t nids;
    if (scan_dir(db, min_live, &ids, &nids) != 0)
        return KV_ERR_IO;

    int rc = KV_OK;
    for (size_t i = 0; i < nids && rc == KV_OK; i++) {
        char name[SEG_NAME_MAX];
        seg_name(name, sizeof name, ids[i], "seg");
        segment s = {.id = ids[i], .fd = -1};
        s.fd = openat(db->dirfd, name, O_RDWR | O_CLOEXEC);
        struct stat st;
        if (s.fd < 0 || fstat(s.fd, &st) != 0) {
            rc = KV_ERR_IO;
            break;
        }
        s.size = (uint64_t)st.st_size;
        int used_hint = 0;
        if (db->opts.use_hints && SEG_SUB(s.id) != 0) {
            used_hint = load_hint(db, s.id, s.size, &s.hint_size);
            if (used_hint < 0)
                rc = KV_ERR_NOMEM;
        }
        if (rc == KV_OK && used_hint != 1) {
            int truncated;
            rc = replay_segment(db, s.id, s.fd, &s.size, &truncated);
            seg_name(name, sizeof name, s.id, "hint");
            if (rc == KV_OK && SEG_SUB(s.id) != 0) {
                if (truncated || db->opts.use_hints) {
                    /* the hint was rejected, or no longer tiles the file:
                     * drop it rather than retry it on every startup */
                    unlinkat(db->dirfd, name, 0);
                } else if (fstatat(db->dirfd, name, &st, 0) == 0) {
                    s.hint_size = (uint64_t)st.st_size; /* unused, kept */
                }
            }
        }
        if (rc != KV_OK || seg_insert(db, &s) != 0) {
            close(s.fd);
            if (rc == KV_OK)
                rc = KV_ERR_NOMEM;
        }
    }
    free(ids);
    if (rc != KV_OK)
        return rc;

    /* New log segments must sort after everything that exists or ever
     * existed: after the newest file, and after the last compaction. */
    uint64_t next = 1;
    if (db->nsegs && SEG_LOG(seg_active(db)->id) + 1 > next)
        next = SEG_LOG(seg_active(db)->id) + 1;
    if (SEG_LOG(min_live) + 1 > next)
        next = SEG_LOG(min_live) + 1;
    db->next_log = next;

    /* Keep appending to the newest segment if it is a log segment with room
     * left; compaction outputs are never appended to. */
    if (!db->nsegs || SEG_SUB(seg_active(db)->id) != 0 ||
        seg_active(db)->size >= db->opts.segment_size) {
        if (db->next_log > SEG_MAX_LOG)
            return KV_ERR_IO;
        segment s = {.id = SEG_ID(db->next_log, 0)};
        if (seg_create(db, s.id, &s.fd) != 0)
            return KV_ERR_IO;
        if (seg_insert(db, &s) != 0) {
            close(s.fd);
            return KV_ERR_NOMEM;
        }
        db->next_log++;
    }
    db->act_fd = seg_active(db)->fd;
    db->act_id = seg_active(db)->id;
    db->act_size = seg_active(db)->size;
    return KV_OK;
}
