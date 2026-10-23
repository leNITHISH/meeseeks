#ifndef KV_IO_H
#define KV_IO_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* pwrite all of buf at off, retrying short writes and EINTR. 0 or -1. */
int io_write_all(int fd, const void *buf, size_t len, off_t off);

/* pread up to len bytes at off; short only at EOF. Bytes read, or -1. */
ssize_t io_read_full(int fd, void *buf, size_t len, off_t off);

/* Flush a file's data to stable storage. On macOS plain fsync() only reaches
 * the drive's cache, so this uses F_FULLFSYNC there. 0 or -1. */
int io_fsync(int fd);

/* Make directory entries (creates, renames, unlinks) durable. Filesystems
 * that cannot fsync a directory report EINVAL; that counts as success,
 * since there is nothing more we could do. 0 or -1. */
int io_fsync_dir(int dirfd);

/* Sequential reader with a growable window, for replay and compaction. */
typedef struct {
    int fd;
    uint64_t file_size;
    uint8_t *buf;
    size_t cap;
    uint64_t buf_off; /* file offset of buf[0] */
    size_t buf_len;
} io_reader;

int io_reader_init(io_reader *r, int fd, uint64_t file_size);
void io_reader_free(io_reader *r);

/* Make [off, off+n) available and return a pointer to it. Returns NULL with
 * *err = 0 if the range runs past the end of the file, or *err = -1 on an
 * I/O or allocation failure. Earlier pointers are invalidated. */
const uint8_t *io_reader_at(io_reader *r, uint64_t off, uint64_t n, int *err);

/* Append-only buffered writer. */
typedef struct {
    int fd;
    uint8_t *buf;
    size_t len, cap;
    uint64_t written; /* bytes already handed to pwrite */
} io_writer;

int io_writer_init(io_writer *w, int fd, size_t cap);
void io_writer_free(io_writer *w);
int io_writer_put(io_writer *w, const void *p, size_t n);
int io_writer_flush(io_writer *w);

static inline uint64_t io_writer_size(const io_writer *w)
{
    return w->written + w->len;
}

#endif
