/*
 * Out-of-space faults, simulated with RLIMIT_FSIZE: past the limit, writes
 * fail with EFBIG (after a short write that tears the record in half), which
 * is how a full disk looks to the store. Each scenario runs in a child
 * process, since the limit and the SIGXFSZ disposition are process-wide.
 *
 * 1. Plain writes hit the wall: the failing put must report an error, the
 *    torn record must be rolled back, and everything acknowledged before
 *    must still be readable, both in that process and after reopening.
 * 2. Compaction hits the wall halfway through an output segment: it must
 *    fail cleanly, with every key still readable (some already redirected to
 *    the half-written output), and a later compaction must succeed.
 */
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "test_util.h"

enum { NKEYS = 50 };

static void limit_file_size(rlim_t bytes)
{
    struct rlimit rl = {bytes, bytes};
    CHECK(setrlimit(RLIMIT_FSIZE, &rl) == 0);
    signal(SIGXFSZ, SIG_IGN); /* get EFBIG instead of dying */
}

static void value_for(int key, int version, char *buf, size_t n)
{
    snprintf(buf, n, "key %d version %d %0*d", key, version, 80, version);
}

/* last[k] = newest acknowledged version of key k, or -1 */
static void verify(kv_t *db, const int *last, const char *ctx)
{
    char k[16], want[128];
    for (int i = 0; i < NKEYS; i++) {
        snprintf(k, sizeof k, "k%d", i);
        void *v;
        size_t n;
        int rc = kv_get(db, k, strlen(k), &v, &n);
        if (last[i] < 0) {
            if (rc != KV_NOT_FOUND) {
                fprintf(stderr, "%s: key %d should be absent, rc=%d\n", ctx,
                        i, rc);
                exit(1);
            }
            continue;
        }
        value_for(i, last[i], want, sizeof want);
        if (rc != KV_OK || n != strlen(want) || memcmp(v, want, n) != 0) {
            fprintf(stderr, "%s: key %d: rc=%d (%s), want version %d\n", ctx,
                    i, rc, kv_strerror(rc), last[i]);
            exit(1);
        }
        free(v);
    }
}

static int run_child(void (*fn)(const char *, int *), const char *dir,
                     int *last)
{
    /* the child reports its acknowledged versions back through a pipe */
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(fds[0]);
        fn(dir, last);
        ssize_t want = (ssize_t)(NKEYS * sizeof *last);
        _exit(write(fds[1], last, (size_t)want) == want ? 0 : 9);
    }
    close(fds[1]);
    ssize_t got = read(fds[0], last, NKEYS * sizeof *last);
    close(fds[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(got == (ssize_t)(NKEYS * sizeof *last) || WEXITSTATUS(status) != 0);
    return WEXITSTATUS(status);
}

/* ---------- 1: writes fill the disk ---------- */

static void writes_child(const char *dir, int *last)
{
    limit_file_size(48 * 1024);
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 1 << 20; /* never roll: the active segment hits the wall */
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    char k[16], v[128];
    int failed = 0;
    off_t acked_bytes = 0;
    for (int ver = 0; ver < 100000 && !failed; ver++)
        for (int i = 0; i < NKEYS; i++) {
            snprintf(k, sizeof k, "k%d", i);
            value_for(i, ver, v, sizeof v);
            int rc = kv_put(db, k, strlen(k), v, strlen(v));
            if (rc != KV_OK) {
                CHECK_RC(rc, KV_ERR_IO);
                failed = 1;
                break;
            }
            last[i] = ver;
            acked_bytes += (off_t)(20 + strlen(k) + strlen(v));
        }
    CHECK(failed);
    /* the half-written record was cut off again: the file holds exactly
     * the acknowledged records */
    CHECK(tu_count_files(dir, ".seg") == 1);
    char seg[512];
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (tu_has_suffix(e->d_name, ".seg"))
            snprintf(seg, sizeof seg, "%s/%s", dir, e->d_name);
    closedir(d);
    CHECK(tu_file_size(seg) == acked_bytes);
    verify(db, last, "after the failed write");
    /* keep trying: each attempt must fail cleanly or succeed for real */
    for (int t = 0; t < 20; t++) {
        snprintf(k, sizeof k, "k%d", t % NKEYS);
        value_for(t % NKEYS, 900000 + t, v, sizeof v);
        int rc = kv_put(db, k, strlen(k), v, strlen(v));
        CHECK(rc == KV_OK || rc == KV_ERR_IO);
        if (rc == KV_OK)
            last[t % NKEYS] = 900000 + t;
    }
    verify(db, last, "after retries");
    kv_close(db);
}

static void test_disk_full_writes(const char *dir)
{
    int last[NKEYS];
    for (int i = 0; i < NKEYS; i++)
        last[i] = -1;
    CHECK(run_child(writes_child, dir, last) == 0);
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    verify(db, last, "after reopen");
    CHECK_RC(kv_close(db), KV_OK);
}

/* ---------- 2: compaction fills the disk ---------- */

static void compaction_child(const char *dir, int *last)
{
    limit_file_size(64 * 1024);
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 256 * 1024; /* outputs would grow past the limit */
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    CHECK_RC(kv_compact(db), KV_ERR_IO);
    /* the half-written output stays in use, under its .tmp name */
    CHECK(tu_count_files(dir, ".seg.tmp") == 1);
    verify(db, last, "after the failed compaction");
    /* the store keeps working: new writes go to the fresh active segment */
    char k[16], v[128];
    for (int i = 0; i < 5; i++) {
        snprintf(k, sizeof k, "k%d", i);
        value_for(i, 500 + i, v, sizeof v);
        CHECK_RC(kv_put(db, k, strlen(k), v, strlen(v)), KV_OK);
        last[i] = 500 + i;
    }
    verify(db, last, "after writes following the failed compaction");
    /* a second attempt fails the same way and must not make things worse */
    CHECK_RC(kv_compact(db), KV_ERR_IO);
    verify(db, last, "after a second failed compaction");
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_disk_full_compaction(const char *dir)
{
    /* Small input segments: compaction copies (and flushes) one input chunk
     * at a time, so the first outputs flushes succeed and a later one runs
     * into the limit, leaving an output that is partly written and partly
     * in use. */
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 24 * 1024;
    int last[NKEYS];
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    char k[16], v[128];
    for (int ver = 0; ver < 60; ver++) /* ~330 KB, nearly all garbage... */
        for (int i = 0; i < NKEYS; i++) {
            snprintf(k, sizeof k, "k%d", i);
            value_for(i, ver, v, sizeof v);
            CHECK_RC(kv_put(db, k, strlen(k), v, strlen(v)), KV_OK);
            last[i] = ver;
        }
    /* ...so make the live set bigger than the limit with large values */
    static char big[3000];
    memset(big, 'b', sizeof big);
    for (int i = 0; i < 40; i++) {
        snprintf(k, sizeof k, "big%d", i);
        CHECK_RC(kv_put(db, k, strlen(k), big, sizeof big), KV_OK);
    }
    CHECK_RC(kv_close(db), KV_OK);

    CHECK(run_child(compaction_child, dir, last) == 0);

    o.segment_size = 256 * 1024;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    CHECK(tu_count_files(dir, ".tmp") == 0);
    verify(db, last, "after reopen");
    CHECK_RC(kv_compact(db), KV_OK); /* no limit now */
    verify(db, last, "after a successful compaction");
    for (int i = 0; i < 40; i++) {
        snprintf(k, sizeof k, "big%d", i);
        void *got;
        size_t n;
        CHECK_RC(kv_get(db, k, strlen(k), &got, &n), KV_OK);
        CHECK(n == sizeof big && memcmp(got, big, n) == 0);
        free(got);
    }
    CHECK_RC(kv_close(db), KV_OK);
}

int main(void)
{
    static const test_case tests[] = {
        {"disk full during writes", test_disk_full_writes},
        {"disk full during compaction", test_disk_full_compaction},
    };
    return tu_run("test_faults", tests, sizeof tests / sizeof *tests);
}
