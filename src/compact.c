/*
 * Compaction that runs alongside reads and writes.
 *
 * 1. Seal (write lock, brief): roll the active log segment L, so new writes go
 *    to L+1. Every segment up to and including L is an input; inputs never
 *    change again.
 *
 * 2. Copy (mostly unlocked), one ~1 MiB chunk of an input at a time:
 *      a. read the chunk                                         (no lock)
 *      b. find which records are live: the index still points at
 *         exactly this (segment, offset)                         (read lock)
 *      c. append the live records to the current output L-k, pwrite them,
 *         and add their entries to its hint file                 (no lock)
 *      d. point the index at the copies, but only for keys whose entry
 *         still names the input record; a key written since then
 *         keeps its newer location                               (write lock)
 *    Outputs are fsynced, then renamed from .tmp to their final names. They
 *    sort after every input and before L+1, so replaying the directory in id
 *    order gives the right answer even if the process dies right here: the
 *    outputs only repeat what the inputs already say, and anything written
 *    since lives in L+1 or later and wins.
 *
 * 3. Commit: fsync the directory, then atomically replace the MANIFEST with
 *    min_live_id = L-1. From this point the inputs are garbage.
 *
 * 4. Retire (write lock, brief): drop the inputs from the segment table and
 *    close them; no index entry can point at them any more, since every live
 *    one was redirected in 2d. Then unlink the files.
 *
 * Writers wait at most for one chunk's redirect step, readers only for the
 * short write-locked steps. Memory use is bounded by the chunk size.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h> /* renameat */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crc32.h"
#include "io.h"
#include "kv_internal.h"
#include "record.h"

#define CHUNK_BYTES (1u << 20)
/* Index operations per lock hold: bounds how long one compaction step can
 * make a writer (read lock) or a reader (write lock) wait. */
#define LOCK_BATCH 512

typedef struct {
    uint64_t off; /* offset in the input segment */
    uint32_t rsz, ksz;
    int tomb, live;
} chunk_rec;

typedef struct {
    const uint8_t *key; /* points into the chunk; valid for one chunk */
    uint32_t ksz;
    kv_loc from, to;
} redirect;

typedef struct {
    kv_t *db;
    uint64_t log;      /* the sealed log segment: outputs are log-1, log-2... */
    uint64_t next_sub;

    /* the output being written */
    int open;
    uint64_t id;
    int fd, hint_fd;   /* fd belongs to the segment table once registered */
    uint64_t size;     /* bytes pwritten to the .seg so far */
    io_writer hint;
    uint8_t *obuf;     /* records staged for the next pwrite */
    size_t olen, ocap;

    chunk_rec *recs;
    size_t nrecs, recs_cap;
    redirect *redirs;
    size_t nredirs, redirs_cap;
} compactor;

static int grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap : 64;
    while (c < need)
        c *= 2;
    void *t = realloc(*p, c * elem);
    if (!t)
        return -1;
    *p = t;
    *cap = c;
    return 0;
}

static void unlink_named(kv_t *db, uint64_t id, const char *ext)
{
    char name[SEG_NAME_MAX];
    seg_name(name, sizeof name, id, ext);
    unlinkat(db->dirfd, name, 0);
}

/* Remove a segment from the table, closing it. Write lock held. */
static void table_drop(kv_t *db, uint64_t id)
{
    segment *s = seg_find(db, id);
    if (!s)
        return;
    close(s->fd);
    size_t i = (size_t)(s - db->segs);
    memmove(&db->segs[i], &db->segs[i + 1],
            (db->nsegs - i - 1) * sizeof *db->segs);
    db->nsegs--;
}

static int out_open(compactor *c)
{
    kv_t *db = c->db;
    if (c->next_sub >= SEG_SUB_SPAN)
        return KV_ERR_IO;
    c->id = SEG_ID(c->log, c->next_sub++);
    char name[SEG_NAME_MAX];
    seg_name(name, sizeof name, c->id, "seg.tmp");
    c->fd = openat(db->dirfd, name, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC,
                   0644);
    seg_name(name, sizeof name, c->id, "hint.tmp");
    c->hint_fd = openat(db->dirfd, name,
                        O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int rc = KV_OK;
    if (c->fd < 0 || c->hint_fd < 0)
        rc = KV_ERR_IO;
    else if (io_writer_init(&c->hint, c->hint_fd, CHUNK_BYTES) != 0)
        rc = KV_ERR_NOMEM;
    if (rc == KV_OK) {
        /* Register it now so readers can be redirected to it chunk by chunk;
         * its id sorts between the inputs and the new active segment. */
        segment s = {.id = c->id, .fd = c->fd, .flags = SEG_TMP};
        kv_write_lock(db);
        if (seg_insert(db, &s) != 0)
            rc = KV_ERR_NOMEM;
        kv_unlock(db);
    }
    if (rc != KV_OK) {
        if (c->fd >= 0)
            close(c->fd);
        if (c->hint_fd >= 0)
            close(c->hint_fd);
        io_writer_free(&c->hint);
        unlink_named(db, c->id, "seg.tmp");
        unlink_named(db, c->id, "hint.tmp");
        return rc;
    }
    c->open = 1;
    c->size = 0;
    c->olen = 0;
    return KV_OK;
}

/* pwrite the staged records of the current output. */
static int out_flush(compactor *c)
{
    if (c->olen == 0)
        return KV_OK;
    if (io_write_all(c->fd, c->obuf, c->olen, (off_t)c->size) != 0)
        return KV_ERR_IO;
    c->size += c->olen;
    c->olen = 0;
    return KV_OK;
}

/* Make the current output durable and give it its final names. */
static int out_finish(compactor *c)
{
    kv_t *db = c->db;
    int rc = out_flush(c);
    if (rc == KV_OK && (io_writer_flush(&c->hint) != 0 || io_fsync(c->fd) != 0 ||
                        io_fsync(c->hint_fd) != 0))
        rc = KV_ERR_IO;
    uint64_t hint_size = io_writer_size(&c->hint);
    close(c->hint_fd);
    io_writer_free(&c->hint);
    c->open = 0;

    char from[SEG_NAME_MAX], to[SEG_NAME_MAX];
    int seg_renamed = 0;
    if (rc == KV_OK) {
        seg_name(from, sizeof from, c->id, "seg.tmp");
        seg_name(to, sizeof to, c->id, "seg");
        seg_renamed = renameat(db->dirfd, from, db->dirfd, to) == 0;
        if (!seg_renamed)
            rc = KV_ERR_IO;
    }
    if (rc == KV_OK) {
        seg_name(from, sizeof from, c->id, "hint.tmp");
        seg_name(to, sizeof to, c->id, "hint");
        if (renameat(db->dirfd, from, db->dirfd, to) != 0)
            rc = KV_ERR_IO;
    }
    if (rc != KV_OK) {
        unlink_named(db, c->id, "hint.tmp");
        hint_size = 0;
    }

    kv_write_lock(db);
    segment *s = seg_find(db, c->id);
    if (s) {
        s->size = c->size;
        if (seg_renamed)
            s->flags &= ~(unsigned)SEG_TMP;
        if (rc == KV_OK)
            s->hint_size = hint_size;
    }
    kv_unlock(db);
    return rc;
}

/*
 * Give up on the current output. Records already redirected to it are valid
 * (each was fully written before its redirect), so it stays in the table as
 * an ordinary segment; the next compaction will take it as an input. On disk
 * it keeps its .tmp name, so a restart drops it and replays the inputs,
 * which this compaction never deleted.
 */
static void out_abandon(compactor *c)
{
    kv_t *db = c->db;
    if (!c->open)
        return;
    close(c->hint_fd);
    io_writer_free(&c->hint);
    unlink_named(db, c->id, "hint.tmp");
    c->open = 0;
    kv_write_lock(db);
    segment *s = seg_find(db, c->id);
    if (s && c->size == 0) {
        table_drop(db, c->id); /* nothing in it: remove it entirely */
        unlink_named(db, c->id, "seg.tmp");
    } else if (s) {
        s->size = c->size; /* only fully written records count */
    }
    kv_unlock(db);
}

/* Stage one live record into the current output, opening a new output when
 * this one is full. */
static int out_add(compactor *c, const uint8_t *rec, const chunk_rec *r,
                   uint64_t from_seg)
{
    kv_t *db = c->db;
    if (c->open && c->size + c->olen > 0 &&
        c->size + c->olen + r->rsz > db->opts.segment_size) {
        int rc = out_finish(c);
        if (rc != KV_OK)
            return rc;
    }
    if (!c->open) {
        int rc = out_open(c);
        if (rc != KV_OK)
            return rc;
    }
    if (c->olen + r->rsz > c->ocap) {
        /* flush before growing: records are staged per output and chunk */
        int rc = out_flush(c);
        if (rc != KV_OK)
            return rc;
        if (grow((void **)&c->obuf, &c->ocap,
                 r->rsz > CHUNK_BYTES ? r->rsz : CHUNK_BYTES, 1) != 0)
            return KV_ERR_NOMEM;
    }
    uint64_t new_off = c->size + c->olen;
    memcpy(c->obuf + c->olen, rec, r->rsz); /* verbatim: same bytes, same crc */
    c->olen += r->rsz;

    uint8_t h[HINT_HDR_SIZE];
    put_u64(h + 4, get_u64(rec + 4));
    put_u32(h + 12, r->ksz);
    put_u64(h + 16, new_off);
    put_u32(h + 24, r->rsz);
    uint32_t crc = crc32_update(0, h + 4, HINT_HDR_SIZE - 4);
    put_u32(h, crc32_update(crc, rec + REC_HDR_SIZE, r->ksz));
    if (io_writer_put(&c->hint, h, sizeof h) != 0 ||
        io_writer_put(&c->hint, rec + REC_HDR_SIZE, r->ksz) != 0)
        return KV_ERR_IO;

    if (grow((void **)&c->redirs, &c->redirs_cap, c->nredirs + 1,
             sizeof *c->redirs) != 0)
        return KV_ERR_NOMEM;
    c->redirs[c->nredirs++] = (redirect){
        .key = rec + REC_HDR_SIZE,
        .ksz = r->ksz,
        .from = {.seg_id = from_seg, .offset = r->off, .rec_size = r->rsz},
        .to = {.seg_id = c->id, .offset = new_off, .rec_size = r->rsz},
    };
    return KV_OK;
}

#ifndef NDEBUG
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Debug check of the invariant retirement relies on: once the copy phase is
 * done, no index entry names an input segment. */
static int inputs_unreferenced(kv_t *db, const segment *in, size_t nin)
{
    uint64_t *ids = malloc((nin ? nin : 1) * sizeof *ids);
    if (!ids)
        return 1;
    for (size_t i = 0; i < nin; i++)
        ids[i] = in[i].id;
    qsort(ids, nin, sizeof *ids, cmp_u64);
    int ok = 1;
    size_t cursor = 0;
    kv_loc *l;
    kv_read_lock(db);
    while (ok && (l = index_next(&db->idx, &cursor)) != NULL)
        if (bsearch(&l->seg_id, ids, nin, sizeof *ids, cmp_u64))
            ok = 0;
    kv_unlock(db);
    free(ids);
    return ok;
}
#endif

/* Copy the live records of one input segment. */
static int compact_input(compactor *c, const segment *in)
{
    kv_t *db = c->db;
    io_reader r;
    if (io_reader_init(&r, in->fd, in->size) != 0)
        return KV_ERR_NOMEM;
    int rc = KV_OK, err;
    uint64_t off = 0;
    while (rc == KV_OK && off < in->size) {
        if (atomic_load(&db->closing)) {
            rc = KV_ERR_IO; /* kv_close() is waiting for us */
            break;
        }
        /* a. Load a chunk of whole records. Input sizes always end on a
         * record boundary, so anything malformed here is real corruption. */
        uint64_t want = in->size - off < CHUNK_BYTES ? in->size - off
                                                     : CHUNK_BYTES;
        const uint8_t *base = io_reader_at(&r, off, want, &err);
        if (base && want >= REC_HDR_SIZE) {
            uint64_t first = record_size(get_u32(base + 12),
                                         get_u32(base + 16));
            if (first > want) { /* one record bigger than a chunk */
                want = first;
                base = io_reader_at(&r, off, want, &err);
            }
        }
        if (!base || want < REC_HDR_SIZE) {
            rc = base || !err ? KV_ERR_CORRUPT : KV_ERR_IO;
            break;
        }
        c->nrecs = 0;
        uint64_t pos = 0;
        while (pos + REC_HDR_SIZE <= want) {
            const uint8_t *h = base + pos;
            uint32_t ksz = get_u32(h + 12), vsz = get_u32(h + 16);
            uint64_t rsz = record_size(ksz, vsz);
            if (ksz > KV_MAX_KEY_SIZE || pos + rsz > want)
                break;
            /* Check every record, not just live ones: a damaged length would
             * misalign everything after it, and live records hiding there
             * would be lost when the input is deleted. */
            if (!record_crc_ok(h, rsz)) {
                rc = KV_ERR_CORRUPT;
                break;
            }
            if (grow((void **)&c->recs, &c->recs_cap, c->nrecs + 1,
                     sizeof *c->recs) != 0) {
                rc = KV_ERR_NOMEM;
                break;
            }
            c->recs[c->nrecs++] = (chunk_rec){.off = off + pos,
                                              .rsz = (uint32_t)rsz,
                                              .ksz = ksz,
                                              .tomb = vsz == REC_TOMBSTONE};
            pos += rsz;
        }
        if (rc != KV_OK)
            break;
        if (c->nrecs == 0) {
            rc = KV_ERR_CORRUPT;
            break;
        }

        /* b. Which records are live? */
        for (size_t i = 0; i < c->nrecs; i++) {
            if (i % LOCK_BATCH == 0)
                kv_read_lock(db);
            chunk_rec *cr = &c->recs[i];
            kv_loc *l = cr->tomb ? NULL
                                 : index_get(&db->idx,
                                             base + (cr->off - off) + REC_HDR_SIZE,
                                             cr->ksz);
            cr->live = l && l->seg_id == in->id && l->offset == cr->off;
            if (i % LOCK_BATCH == LOCK_BATCH - 1 || i + 1 == c->nrecs)
                kv_unlock(db);
        }

        /* c. Copy them. */
        c->nredirs = 0;
        for (size_t i = 0; i < c->nrecs && rc == KV_OK; i++) {
            const chunk_rec *cr = &c->recs[i];
            if (cr->live)
                rc = out_add(c, base + (cr->off - off), cr, in->id);
        }
        if (rc == KV_OK)
            rc = out_flush(c);

        /* d. Redirect index entries that still point at the inputs. Only
         * copies that reached the file count: the latest output has
         * c->size bytes written (even if finishing it failed), earlier
         * ones were flushed in full before the switch. */
        for (size_t i = 0; i < c->nredirs; i++) {
            if (i % LOCK_BATCH == 0)
                kv_write_lock(db);
            redirect *d = &c->redirs[i];
            if (d->to.seg_id != c->id ||
                d->to.offset + d->to.rec_size <= c->size) {
                kv_loc *l = index_get(&db->idx, d->key, d->ksz);
                if (l && l->seg_id == d->from.seg_id &&
                    l->offset == d->from.offset)
                    *l = d->to;
            }
            if (i % LOCK_BATCH == LOCK_BATCH - 1 || i + 1 == c->nredirs)
                kv_unlock(db);
        }
        kv_write_lock(db);
        if (c->open) {
            segment *s = seg_find(db, c->id);
            if (s)
                s->size = c->size;
        }
        kv_unlock(db);
        off += pos;
    }
    io_reader_free(&r);
    return rc;
}

int kv_compact(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    if (atomic_load(&db->failed))
        return KV_ERR_FAILED;
    pthread_mutex_lock(&db->compact_mu);

    /* 1. Seal. Holding write_mu keeps writers out until the new active
     * segment is in place, so the input set is exactly what precedes it. */
    pthread_mutex_lock(&db->write_mu);
    int rc = seg_rollover(db);
    size_t nin = 0;
    segment *inputs = NULL;
    if (rc == KV_OK) {
        kv_read_lock(db);
        nin = db->nsegs - 1;
        inputs = malloc((nin ? nin : 1) * sizeof *inputs);
        if (inputs)
            memcpy(inputs, db->segs, nin * sizeof *inputs);
        else
            rc = KV_ERR_NOMEM;
        kv_unlock(db);
    }
    pthread_mutex_unlock(&db->write_mu);
    if (rc != KV_OK) {
        pthread_mutex_unlock(&db->compact_mu);
        return rc;
    }

    /* The last input is the log segment we just sealed. */
    compactor c;
    memset(&c, 0, sizeof c);
    c.db = db;
    c.log = SEG_LOG(inputs[nin - 1].id);
    c.next_sub = 1;
    c.fd = c.hint_fd = -1;

    /* 2. Copy. Inputs are immutable and only this thread closes them, so
     * their descriptors can be read without holding any lock. */
    for (size_t i = 0; i < nin && rc == KV_OK; i++)
        rc = compact_input(&c, &inputs[i]);
    if (rc == KV_OK && c.open)
        rc = out_finish(&c);
    if (rc != KV_OK)
        out_abandon(&c);

    /* 3. Commit. */
    if (rc == KV_OK && io_fsync_dir(db->dirfd) != 0)
        rc = KV_ERR_IO;
    if (rc == KV_OK && manifest_write(db, SEG_ID(c.log, 1)) != 0)
        rc = KV_ERR_IO;

    /* 4. Retire the inputs. */
    assert(rc != KV_OK || inputs_unreferenced(db, inputs, nin));
    if (rc == KV_OK) {
        kv_write_lock(db);
        for (size_t i = 0; i < nin; i++)
            table_drop(db, inputs[i].id);
        db->ncompactions++;
        kv_unlock(db);
        for (size_t i = 0; i < nin; i++) {
            int tmp = inputs[i].flags & SEG_TMP;
            unlink_named(db, inputs[i].id, tmp ? "seg.tmp" : "seg");
            unlink_named(db, inputs[i].id, "hint");
        }
        io_fsync_dir(db->dirfd); /* not required: the MANIFEST already
                                    marks them garbage for the next open */
    }

    free(inputs);
    free(c.obuf);
    free(c.recs);
    free(c.redirs);
    pthread_mutex_unlock(&db->compact_mu);
    return rc;
}
