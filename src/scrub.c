/*
 * Scrubbing: read sealed segments back and check every record, to find
 * damage before a get runs into it.
 *
 * Replay checks every record of the segments it reads at open, but a
 * compacted segment loaded from its hint file is not read then, and any
 * file can rot later. Sealed segments never change, so checking one needs no
 * lock while reading: its descriptor is dup'ed under the read lock, and the
 * duplicate stays readable even if a compaction retires and deletes the
 * segment meanwhile. Only the verdict is published, under the write lock, as
 * a flag on the segment.
 *
 * Compaction output still under its .tmp name is left alone: it is either
 * being written, or the remains of a failed compaction whose inputs are
 * still there and which the next compaction rereads in full anyway.
 */
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "io.h"
#include "kv_internal.h"
#include "record.h"

/* The background scrubber pauses this long after each MiB it checks, so it
 * reads at most about 100 MiB/s and leaves the disk to the store's users. */
#define SCRUB_PAUSE_NS (10 * 1000 * 1000)

/* Check the records in [0, size) of fd. 1 if they are intact, 0 if one is
 * not, -1 on an I/O error or out of memory, -2 if the store is closing. */
static int check_segment(kv_t *db, int fd, uint64_t size, int paced,
                         uint64_t *checked)
{
    io_reader r;
    if (io_reader_init(&r, fd, size) != 0)
        return -1;
    uint64_t off = 0, paused_at = 0;
    int err = 0, rc = 1;
    while (off < size) {
        if (atomic_load(&db->closing)) {
            rc = -2;
            break;
        }
        const uint8_t *h = io_reader_at(&r, off, REC_HDR_SIZE, &err);
        uint32_t ksz = h ? get_u32(h + 12) : 0, vsz = h ? get_u32(h + 16) : 0;
        if (!h || ksz > KV_MAX_KEY_SIZE ||
            (vsz != REC_TOMBSTONE && vsz > KV_MAX_VALUE_SIZE)) {
            rc = err ? -1 : 0;
            break;
        }
        uint64_t rsz = record_size(ksz, vsz);
        const uint8_t *rec = io_reader_at(&r, off, rsz, &err);
        if (!rec || !record_crc_ok(rec, rsz)) {
            rc = err ? -1 : 0; /* cut short (the file shrank) or bad crc */
            break;
        }
        off += rsz;
        if (paced && off - paused_at >= (1u << 20)) {
            struct timespec ts = {0, SCRUB_PAUSE_NS};
            nanosleep(&ts, NULL);
            paused_at = off;
        }
    }
    *checked = off;
    io_reader_free(&r);
    return rc;
}

/*
 * Check the sealed segments: all of them, or with `fresh` only those no
 * scrub has looked at yet. Returns KV_OK, KV_ERR_CORRUPT if any segment is
 * known to be damaged, KV_ERR_IO or KV_ERR_NOMEM.
 */
static int scrub_pass(kv_t *db, int fresh, int paced)
{
    /* Ids only: the table can change while we read. */
    kv_read_lock(db);
    size_t nsealed = db->nsegs - 1; /* all but the active segment */
    uint64_t *ids = malloc((nsealed ? nsealed : 1) * sizeof *ids);
    size_t n = 0;
    for (size_t i = 0; ids && i < nsealed; i++) {
        unsigned f = db->segs[i].flags;
        if (!(f & SEG_TMP) && !(fresh && (f & (SEG_SCRUBBED | SEG_DAMAGED))))
            ids[n++] = db->segs[i].id;
    }
    kv_unlock(db);
    if (!ids)
        return KV_ERR_NOMEM;

    int rc = KV_OK;
    for (size_t i = 0; i < n && rc == KV_OK; i++) {
        kv_read_lock(db);
        segment *s = seg_find(db, ids[i]);
        int fd = s ? dup(s->fd) : -1;
        uint64_t size = s ? s->size : 0;
        kv_unlock(db);
        if (!s)
            continue; /* retired by a compaction in the meantime */
        if (fd < 0) {
            rc = KV_ERR_IO;
            break;
        }
        uint64_t checked = 0;
        int ok = check_segment(db, fd, size, paced, &checked);
        close(fd);
        if (ok < 0) {
            rc = ok == -1 ? KV_ERR_IO : KV_OK;
            break;
        }
        kv_write_lock(db);
        db->scrub_bytes += checked;
        if ((s = seg_find(db, ids[i])) != NULL) {
            s->flags &= ~(unsigned)(SEG_SCRUBBED | SEG_DAMAGED);
            s->flags |= ok ? SEG_SCRUBBED : SEG_DAMAGED;
        }
        kv_unlock(db);
    }
    free(ids);
    if (rc != KV_OK)
        return rc;
    kv_read_lock(db);
    for (size_t i = 0; i < db->nsegs; i++)
        if (db->segs[i].flags & SEG_DAMAGED)
            rc = KV_ERR_CORRUPT;
    kv_unlock(db);
    return rc;
}

int kv_scrub(kv_t *db)
{
    if (!db)
        return KV_ERR_INVALID;
    return scrub_pass(db, 0, 0);
}

void kv_kick_scrubber(kv_t *db)
{
    if (!db->scrub_running)
        return;
    pthread_mutex_lock(&db->bg_mu);
    db->scrub_kick = 1;
    pthread_cond_signal(&db->scrub_cv);
    pthread_mutex_unlock(&db->bg_mu);
}

void *scrub_main(void *arg)
{
    kv_t *db = arg;
    pthread_mutex_lock(&db->bg_mu);
    while (!db->bg_stop) {
        if (!db->scrub_kick) {
            pthread_cond_wait(&db->scrub_cv, &db->bg_mu);
            continue;
        }
        db->scrub_kick = 0;
        pthread_mutex_unlock(&db->bg_mu);
        scrub_pass(db, 1, 1); /* what it finds is reported in the stats */
        pthread_mutex_lock(&db->bg_mu);
    }
    pthread_mutex_unlock(&db->bg_mu);
    return NULL;
}
