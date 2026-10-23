/* Tiny test harness shared by the test programs. Header-only on purpose. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "kv.h"

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,   \
                    #cond);                                                    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_RC(expr, want)                                                   \
    do {                                                                       \
        int rc_ = (expr);                                                      \
        if (rc_ != (want)) {                                                   \
            fprintf(stderr, "%s:%d: %s returned %d (%s), want %d (%s)\n",      \
                    __FILE__, __LINE__, #expr, rc_, kv_strerror(rc_), (want),  \
                    kv_strerror(want));                                        \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static inline void tu_sleep_us(long us)
{
    struct timespec ts = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&ts, NULL);
}

typedef struct {
    const char *name;
    void (*fn)(const char *dir);
} test_case;

/* Fresh empty directory under $TMPDIR (or /tmp). buf must hold 256 bytes. */
static inline void tu_tmpdir(char *buf)
{
    const char *base = getenv("TMPDIR");
    snprintf(buf, 256, "%s/meeseeks-test-XXXXXX", base && *base ? base : "/tmp");
    CHECK(mkdtemp(buf) != NULL);
}

/* Remove a flat directory (store directories never nest). */
static inline void tu_rmrf(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static inline void tu_copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    CHECK(in >= 0);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(out >= 0);
    char buf[1 << 16];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        CHECK(write(out, buf, (size_t)n) == n);
    CHECK(n == 0);
    close(in);
    close(out);
}

/* Copy every file of src whose name passes keep(name) (NULL = all). */
static inline void tu_copy_dir_filtered(const char *src, const char *dst,
                                        int (*keep)(const char *name))
{
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    CHECK(d != NULL);
    struct dirent *e;
    char a[512], b[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || (keep && !keep(e->d_name)))
            continue;
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        tu_copy_file(a, b);
    }
    closedir(d);
}

static inline void tu_copy_dir(const char *src, const char *dst)
{
    tu_copy_dir_filtered(src, dst, NULL);
}

static inline int tu_has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

static inline int tu_count_files(const char *dir, const char *suffix)
{
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && tu_has_suffix(e->d_name, suffix))
            n++;
    closedir(d);
    return n;
}

static inline int tu_file_exists(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static inline off_t tu_file_size(const char *path)
{
    struct stat st;
    CHECK(stat(path, &st) == 0);
    return st.st_size;
}

static inline int tu_run(const char *prog, const test_case *tests, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char dir[256];
        tu_tmpdir(dir);
        printf("  %-40s", tests[i].name);
        fflush(stdout);
        tests[i].fn(dir);
        tu_rmrf(dir);
        printf("ok\n");
    }
    printf("%s: %zu tests passed\n", prog, n);
    return 0;
}

#endif
