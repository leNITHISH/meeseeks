/*
 * Online backup.
 *
 * Sealed segments never change, so a consistent copy of the store needs no
 * copying of data: seal the active segment (everything acknowledged so far is
 * then in sealed, durable segments), and hard-link every segment, hint file
 * and the MANIFEST into the target directory. That takes the same few
 * milliseconds whatever the size of the store, and later compactions cannot
 * disturb the backup: deleting a file only removes one of its names.
 *
 * One file is copied instead of linked: the segment just sealed, which is the
 * newest log segment. Opening a store appends to its newest log segment if it
 * has room, so if that file were shared, opening the backup (or the original,
 * after this session) would write into the other one's file. No store ever
 * writes to an older segment.
 *
 * Across file systems, where links are impossible, everything is copied.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "io.h"
#include "kv_internal.h"

typedef struct {
    char (*names)[SEG_NAME_MAX];
    size_t n;
} created;

static int copy_file(int srcdir, int dstdir, const char *name)
{
    int in = openat(srcdir, name, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = openat(dstdir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                     0644);
    size_t cap = 1u << 20;
    uint8_t *buf = malloc(cap);
    int rc = out >= 0 && buf ? 0 : -1;
    off_t off = 0;
    while (rc == 0) {
        ssize_t n = io_read_full(in, buf, cap, off);
        if (n < 0 || (n > 0 && io_write_all(out, buf, (size_t)n, off) != 0))
            rc = -1;
        if (n <= 0)
            break;
        off += n;
    }
    if (rc == 0 && io_fsync(out) != 0)
        rc = -1;
    free(buf);
    close(in);
    if (out >= 0)
        close(out);
    return rc;
}

/* Put `name` into the backup: a hard link, or a copy when `copy` is set or
 * linking is impossible. Missing optional files are skipped. */
static int add_file(kv_t *db, int dstdir, const char *name, int copy,
                    int optional, created *made)
{
    if (optional && faccessat(db->dirfd, name, F_OK, 0) != 0)
        return 0;
    int rc;
    if (copy) {
        rc = copy_file(db->dirfd, dstdir, name);
    } else {
        rc = linkat(db->dirfd, name, dstdir, name, 0);
        if (rc != 0 && (errno == EXDEV || errno == EPERM || errno == EMLINK ||
                        errno == ENOTSUP))
            rc = copy_file(db->dirfd, dstdir, name);
    }
    if (rc == 0)
        snprintf(made->names[made->n++], SEG_NAME_MAX, "%s", name);
    return rc;
}

int kv_backup(kv_t *db, const char *path)
{
    if (!db || !path)
        return KV_ERR_INVALID;
    if (atomic_load(&db->failed))
        return KV_ERR_FAILED;
    if (mkdir(path, 0755) != 0)
        return errno == EEXIST ? KV_ERR_INVALID : KV_ERR_IO;
    int dst = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dst < 0) {
        rmdir(path);
        return KV_ERR_IO;
    }

    /* No compaction while we link: it would delete files under us. */
    pthread_mutex_lock(&db->compact_mu);
    pthread_mutex_lock(&db->write_mu);
    int rc = seg_rollover(db);
    segment *snap = NULL;
    size_t nsnap = 0;
    if (rc == KV_OK) {
        kv_read_lock(db);
        nsnap = db->nsegs - 1; /* all but the new, empty active segment */
        snap = malloc((nsnap ? nsnap : 1) * sizeof *snap);
        if (snap)
            memcpy(snap, db->segs, nsnap * sizeof *snap);
        else
            rc = KV_ERR_NOMEM;
        kv_unlock(db);
    }
    pthread_mutex_unlock(&db->write_mu);

    created made = {NULL, 0};
    if (rc == KV_OK &&
        !(made.names = malloc((2 * nsnap + 1) * sizeof *made.names)))
        rc = KV_ERR_NOMEM;
    for (size_t i = 0; rc == KV_OK && i < nsnap; i++) {
        if (snap[i].flags & SEG_TMP)
            continue; /* a failed compaction's leftover: the inputs it
                         duplicates are still there */
        char name[SEG_NAME_MAX];
        seg_name(name, sizeof name, snap[i].id, "seg");
        if (add_file(db, dst, name, i + 1 == nsnap, 0, &made) != 0)
            rc = KV_ERR_IO;
        seg_name(name, sizeof name, snap[i].id, "hint");
        if (rc == KV_OK && add_file(db, dst, name, 0, 1, &made) != 0)
            rc = KV_ERR_IO;
    }
    if (rc == KV_OK && add_file(db, dst, "MANIFEST", 0, 1, &made) != 0)
        rc = KV_ERR_IO;
    pthread_mutex_unlock(&db->compact_mu);

    /* Make the backup's own directory entries durable, and its name in the
     * parent directory. Linked data was durable already. */
    if (rc == KV_OK && io_fsync_dir(dst) != 0)
        rc = KV_ERR_IO;
    if (rc == KV_OK) {
        int parent = openat(dst, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0 || io_fsync_dir(parent) != 0)
            rc = KV_ERR_IO;
        if (parent >= 0)
            close(parent);
    }
    if (rc != KV_OK) { /* leave nothing half-made behind */
        for (size_t i = 0; i < made.n; i++)
            unlinkat(dst, made.names[i], 0);
        rmdir(path);
    }
    close(dst);
    free(made.names);
    free(snap);
    return rc;
}
