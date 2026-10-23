/*
 * Power-failure recorder, loaded with LD_PRELOAD (Linux).
 *
 * Logs every operation that changes the contents of directory $PF_DIR (file
 * creation, writes, truncation, fsync, rename, unlink) to $PF_LOG, along
 * with markers the workload inserts through pf_mark(). test_powerfail then
 * replays the log to build the disk images a power cut could have left
 * behind at every point. The store under test is the unmodified library.
 *
 * Log records: u8 type, then type-specific fields (all little-endian u64
 * except names, which are a u64 length plus bytes).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

enum {
    PF_CREATE = 1, /* name, ino */
    PF_WRITE,      /* ino, off, len, bytes */
    PF_TRUNC,      /* ino, size */
    PF_FSYNC,      /* ino (the directory's own ino for a directory fsync) */
    PF_RENAME,     /* from, to */
    PF_UNLINK,     /* name */
    PF_MARK        /* value */
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int logfd = -1;
static dev_t wdev;
static ino_t wino;
static int ready;

static ssize_t (*real_pwrite)(int, const void *, size_t, off_t);
static int (*real_ftruncate)(int, off_t);
static int (*real_fsync)(int);
static int (*real_fdatasync)(int);
static int (*real_renameat)(int, const char *, int, const char *);
static int (*real_unlinkat)(int, const char *, int);
static int (*real_openat)(int, const char *, int, ...);

static void init(void)
{
    if (ready)
        return;
    real_pwrite = dlsym(RTLD_NEXT, "pwrite");
    real_ftruncate = dlsym(RTLD_NEXT, "ftruncate");
    real_fsync = dlsym(RTLD_NEXT, "fsync");
    real_fdatasync = dlsym(RTLD_NEXT, "fdatasync");
    real_renameat = dlsym(RTLD_NEXT, "renameat");
    real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
    real_openat = dlsym(RTLD_NEXT, "openat");
    const char *dir = getenv("PF_DIR"), *log = getenv("PF_LOG");
    struct stat st;
    if (dir && log && stat(dir, &st) == 0) {
        wdev = st.st_dev;
        wino = st.st_ino;
        /* raw syscall: our own log writes must not be intercepted */
        logfd = (int)syscall(SYS_openat, AT_FDCWD, log,
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    ready = 1;
}

static void emit(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n > 0) {
        ssize_t w = syscall(SYS_write, logfd, b, n);
        if (w <= 0)
            abort();
        b += w;
        n -= (size_t)w;
    }
}

static void emit_u64(uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (uint8_t)(v >> (8 * i));
    emit(b, 8);
}

static void emit_type(uint8_t t)
{
    emit(&t, 1);
}

static void emit_name(const char *s)
{
    emit_u64(strlen(s));
    emit(s, strlen(s));
}

static int fd_ino(int fd, uint64_t *ino)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_dev != wdev)
        return 0;
    *ino = (uint64_t)st.st_ino;
    return 1;
}

/* Is dirfd our watched directory? */
static int watched_dir(int dirfd)
{
    struct stat st;
    return logfd >= 0 && fstat(dirfd, &st) == 0 && st.st_dev == wdev &&
           st.st_ino == wino;
}

void pf_mark(uint64_t v);
void pf_mark(uint64_t v)
{
    pthread_mutex_lock(&mu);
    init();
    if (logfd >= 0) {
        emit_type(PF_MARK);
        emit_u64(v);
    }
    pthread_mutex_unlock(&mu);
}

int openat(int dirfd, const char *name, int flags, ...);
int openat(int dirfd, const char *name, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    pthread_mutex_lock(&mu);
    init();
    int watch = watched_dir(dirfd) && !strchr(name, '/');
    struct stat before;
    int existed = watch && fstatat(dirfd, name, &before, AT_SYMLINK_NOFOLLOW) == 0;
    int fd = real_openat(dirfd, name, flags, mode);
    uint64_t ino;
    if (fd >= 0 && watch && fd_ino(fd, &ino)) {
        if (!existed) {
            emit_type(PF_CREATE);
            emit_name(name);
            emit_u64(ino);
        } else if (flags & O_TRUNC) {
            emit_type(PF_TRUNC);
            emit_u64(ino);
            emit_u64(0);
        }
    }
    pthread_mutex_unlock(&mu);
    return fd;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    pthread_mutex_lock(&mu);
    init();
    ssize_t w = real_pwrite(fd, buf, n, off);
    uint64_t ino;
    if (w > 0 && logfd >= 0 && fd_ino(fd, &ino)) {
        emit_type(PF_WRITE);
        emit_u64(ino);
        emit_u64((uint64_t)off);
        emit_u64((uint64_t)w);
        emit(buf, (size_t)w);
    }
    pthread_mutex_unlock(&mu);
    return w;
}

ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off);
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off)
{
    return pwrite(fd, buf, n, off);
}

int ftruncate(int fd, off_t size);
int ftruncate(int fd, off_t size)
{
    pthread_mutex_lock(&mu);
    init();
    int rc = real_ftruncate(fd, size);
    uint64_t ino;
    if (rc == 0 && logfd >= 0 && fd_ino(fd, &ino)) {
        emit_type(PF_TRUNC);
        emit_u64(ino);
        emit_u64((uint64_t)size);
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

int ftruncate64(int fd, off_t size);
int ftruncate64(int fd, off_t size)
{
    return ftruncate(fd, size);
}

/* The sync is logged at the moment it is requested; holding the lock
 * across the real call keeps "everything logged before this is durable"
 * true for the replay. */
static int logged_sync(int fd, int datasync)
{
    pthread_mutex_lock(&mu);
    init();
    int rc = datasync ? real_fdatasync(fd) : real_fsync(fd);
    uint64_t ino;
    if (rc == 0 && logfd >= 0 && fd_ino(fd, &ino)) {
        emit_type(PF_FSYNC);
        emit_u64(ino);
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

int fsync(int fd);
int fsync(int fd)
{
    return logged_sync(fd, 0);
}

int fdatasync(int fd);
int fdatasync(int fd)
{
    return logged_sync(fd, 1);
}

int renameat(int a, const char *from, int b, const char *to);
int renameat(int a, const char *from, int b, const char *to)
{
    pthread_mutex_lock(&mu);
    init();
    int rc = real_renameat(a, from, b, to);
    if (rc == 0 && watched_dir(a) && watched_dir(b)) {
        emit_type(PF_RENAME);
        emit_name(from);
        emit_name(to);
    }
    pthread_mutex_unlock(&mu);
    return rc;
}

int unlinkat(int dirfd, const char *name, int flags);
int unlinkat(int dirfd, const char *name, int flags)
{
    pthread_mutex_lock(&mu);
    init();
    int rc = real_unlinkat(dirfd, name, flags);
    if (rc == 0 && watched_dir(dirfd)) {
        emit_type(PF_UNLINK);
        emit_name(name);
    }
    pthread_mutex_unlock(&mu);
    return rc;
}
