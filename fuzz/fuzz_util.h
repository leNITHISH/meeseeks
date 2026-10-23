/* Helpers shared by the fuzz targets. */
#ifndef FUZZ_UTIL_H
#define FUZZ_UTIL_H

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kv.h"

static inline void fuzz_tmpdir(char *buf)
{
    const char *base = getenv("TMPDIR");
    snprintf(buf, 256, "%s/meeseeks-fuzz-XXXXXX", base && *base ? base : "/tmp");
    if (!mkdtemp(buf))
        abort();
}

static inline void fuzz_rmrf(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    char p[512];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(path);
}

#endif
