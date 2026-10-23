/*
 * fsync fault injector, loaded with LD_PRELOAD (Linux).
 *
 * The first $FAULT_FSYNC_OK calls to fsync()/fdatasync() behave normally;
 * every call after that fails with EIO without doing anything, the way a
 * failing disk or a lost network block device would look.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_long calls;

static int should_fail(void)
{
    const char *s = getenv("FAULT_FSYNC_OK");
    long ok = s ? atol(s) : -1;
    return ok >= 0 && atomic_fetch_add(&calls, 1) >= ok;
}

int fsync(int fd);
int fsync(int fd)
{
    static int (*real)(int);
    if (should_fail()) {
        errno = EIO;
        return -1;
    }
    if (!real)
        real = (int (*)(int))dlsym(RTLD_NEXT, "fsync");
    return real(fd);
}

int fdatasync(int fd);
int fdatasync(int fd)
{
    static int (*real)(int);
    if (should_fail()) {
        errno = EIO;
        return -1;
    }
    if (!real)
        real = (int (*)(int))dlsym(RTLD_NEXT, "fdatasync");
    return real(fd);
}
