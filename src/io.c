#include "io.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int io_write_all(int fd, const void *buf, size_t len, off_t off)
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

ssize_t io_read_full(int fd, void *buf, size_t len, off_t off)
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

int io_fsync(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
    /* not supported by this filesystem: fall back to a plain fsync */
#endif
    int rc;
    do
        rc = fsync(fd);
    while (rc != 0 && errno == EINTR);
    return rc;
}

int io_fsync_dir(int dirfd)
{
    /* Plain fsync: F_FULLFSYNC is for file data, and the next full sync of
     * any file flushes the drive cache this directory change sits in. */
    int rc;
    do
        rc = fsync(dirfd);
    while (rc != 0 && errno == EINTR);
    return rc == 0 || errno == EINVAL || errno == ENOTSUP ? 0 : -1;
}

int io_reader_init(io_reader *r, int fd, uint64_t file_size)
{
    r->fd = fd;
    r->file_size = file_size;
    r->cap = 1u << 20;
    r->buf = malloc(r->cap);
    r->buf_off = 0;
    r->buf_len = 0;
    return r->buf ? 0 : -1;
}

void io_reader_free(io_reader *r)
{
    free(r->buf);
    r->buf = NULL;
}

const uint8_t *io_reader_at(io_reader *r, uint64_t off, uint64_t n, int *err)
{
    *err = 0;
    if (off > r->file_size || n > r->file_size - off)
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
    if (want > r->file_size - off)
        want = (size_t)(r->file_size - off);
    ssize_t got = io_read_full(r->fd, r->buf, want, (off_t)off);
    if (got < 0) {
        *err = -1;
        r->buf_len = 0;
        return NULL;
    }
    r->buf_off = off;
    r->buf_len = (size_t)got;
    if ((uint64_t)got < n)
        return NULL; /* the file is shorter than its recorded size */
    return r->buf;
}

int io_writer_init(io_writer *w, int fd, size_t cap)
{
    w->fd = fd;
    w->len = 0;
    w->cap = cap;
    w->written = 0;
    w->buf = malloc(cap);
    return w->buf ? 0 : -1;
}

void io_writer_free(io_writer *w)
{
    free(w->buf);
    w->buf = NULL;
}

int io_writer_flush(io_writer *w)
{
    if (w->len && io_write_all(w->fd, w->buf, w->len, (off_t)w->written) != 0)
        return -1;
    w->written += w->len;
    w->len = 0;
    return 0;
}

int io_writer_put(io_writer *w, const void *p, size_t n)
{
    if (w->len + n > w->cap && io_writer_flush(w) != 0)
        return -1;
    if (n > w->cap) {
        if (io_write_all(w->fd, p, n, (off_t)w->written) != 0)
            return -1;
        w->written += n;
        return 0;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
    return 0;
}
