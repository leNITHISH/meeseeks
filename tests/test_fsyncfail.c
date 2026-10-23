/*
 * fsync failure test (Linux).
 *
 * A child runs the store under fault_shim.so (LD_PRELOAD), which makes every
 * fsync fail with EIO after the first N. Whether the failure hits while
 * opening, on a group commit, or when a full segment is sealed, the store
 * must report KV_ERR_FAILED for that write and refuse every later write,
 * sync and compaction (a retried fsync could falsely succeed after the
 * kernel dropped the dirty pages), keep serving reads, and release every
 * writer waiting on a group commit instead of hanging. Reopened without the
 * fault, the directory must hold every write acknowledged before it.
 *
 * Exercises N from 0 up, with fsync on every write and with no fsync, and
 * with four writers sharing group commits; one writer also compacts every
 * 64 writes, so the failure lands inside compactions as well.
 */
#include <pthread.h>
#include <sys/wait.h>

#include "test_util.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

enum { NKEYS = 32, NWRITERS = 4, MAX_PUTS = 20000 };

static void key_of(int t, int i, char *k)
{
    snprintf(k, 32, "t%d-key-%02d", t, i % NKEYS);
}

static void value_of(int t, int i, char *v)
{
    snprintf(v, 48, "value %d from writer %d", i, t);
}

static kv_options opts(int always)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = always ? KV_SYNC_ALWAYS : KV_SYNC_NONE;
    o.segment_size = 512; /* seal a segment every few writes */
    return o;
}

/* Everything the store must refuse once it has failed. */
static int check_failed_state(kv_t *db, int t, int last_acked)
{
    char k[32], v[48];
    key_of(t, 0, k);
    if (kv_put(db, k, strlen(k), "x", 1) != KV_ERR_FAILED ||
        kv_delete(db, k, strlen(k)) != KV_ERR_FAILED ||
        kv_sync(db) != KV_ERR_FAILED || kv_compact(db) != KV_ERR_FAILED)
        return 0;
    if (last_acked >= 0) { /* reads keep working */
        key_of(t, last_acked, k);
        value_of(t, last_acked, v);
        void *got;
        size_t n;
        if (kv_get(db, k, strlen(k), &got, &n) != KV_OK || n != strlen(v) ||
            memcmp(got, v, n) != 0)
            return 0;
        free(got);
    }
    return 1;
}

/* ---------- child side ---------- */

typedef struct {
    kv_t *db;
    int t, last_acked, rc;
} writer;

static void *write_until_failure(void *arg)
{
    writer *w = arg;
    char k[32], v[48];
    w->last_acked = -1;
    for (int i = 0; i < MAX_PUTS; i++) {
        key_of(w->t, i, k);
        value_of(w->t, i, v);
        w->rc = kv_put(w->db, k, strlen(k), v, strlen(v));
        if (w->rc != KV_OK)
            return NULL;
        w->last_acked = i;
        /* the first writer also compacts, so failures land in there too */
        if (w->t == 0 && i % 64 == 63 &&
            (w->rc = kv_compact(w->db)) != KV_OK)
            return NULL;
    }
    return NULL;
}

/* Exit codes: 0 fine, 10 open failed (fine, nothing to check), else a bug.
 * Prints "<writer> <last acknowledged put>" lines for the parent. */
static int child(const char *dir, int always, int nwriters)
{
    alarm(30); /* a writer stuck in a group commit fails the test */
    kv_options o = opts(always);
    kv_t *db;
    int rc = kv_open(dir, &o, &db);
    if (rc != KV_OK)
        return rc == KV_ERR_IO ? 10 : 2;
    writer w[NWRITERS];
    pthread_t th[NWRITERS];
    for (int t = 0; t < nwriters; t++) {
        w[t] = (writer){db, t, -1, KV_OK};
        if (pthread_create(&th[t], NULL, write_until_failure, &w[t]) != 0)
            return 3;
    }
    for (int t = 0; t < nwriters; t++)
        pthread_join(th[t], NULL);
    for (int t = 0; t < nwriters; t++) {
        if (w[t].rc != KV_ERR_FAILED) /* MAX_PUTS without a failure, or an */
            return 4;                 /* error other than the fsync one */
        if (!check_failed_state(db, t, w[t].last_acked))
            return 5;
        printf("%d %d\n", t, w[t].last_acked);
    }
    fflush(stdout);
    return kv_close(db) == KV_ERR_FAILED ? 0 : 6;
}

/* ---------- parent side ---------- */

#if !defined(SANITIZED) && defined(__linux__)
static int run_case(const char *self, const char *shim, int always,
                    int nwriters, int fsyncs_ok)
{
    char dir[256];
    tu_tmpdir(dir);
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        char n[16], w[16];
        snprintf(n, sizeof n, "%d", fsyncs_ok);
        snprintf(w, sizeof w, "%d", nwriters);
        dup2(fds[1], 1);
        setenv("LD_PRELOAD", shim, 1);
        setenv("FAULT_FSYNC_OK", n, 1);
        execl(self, self, "--child", dir, always ? "always" : "none", w,
              (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char out[1024] = {0};
    size_t got = 0;
    ssize_t r;
    while (got + 1 < sizeof out &&
           (r = read(fds[0], out + got, sizeof out - 1 - got)) > 0)
        got += (size_t)r;
    close(fds[0]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    const char *what = always ? "fsync always" : "no fsync";
    if (!WIFEXITED(status) ||
        (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 10)) {
        fprintf(stderr, "\n%s, %d writers, fsync fails after %d: child %s %d\n",
                what, nwriters, fsyncs_ok,
                WIFEXITED(status) ? "exited with" : "killed by signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        return 0;
    }

    /* Reopened without the fault: every acknowledged write is there. */
    kv_options o = opts(always);
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    int acked_total = 0;
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
        int t, last;
        CHECK(sscanf(line, "%d %d", &t, &last) == 2);
        acked_total += last + 1;
        /* newest acknowledged value of each key; the put that failed may
         * or may not have made it, so its key may hold that one instead */
        for (int i = last - NKEYS + 1 < 0 ? 0 : last - NKEYS + 1; i <= last;
             i++) {
            char k[32], want[48], failed[48];
            key_of(t, i, k);
            value_of(t, i, want);
            value_of(t, i + NKEYS, failed);
            void *v;
            size_t n;
            int rc = kv_get(db, k, strlen(k), &v, &n);
            int ok = rc == KV_OK && ((n == strlen(want) && !memcmp(v, want, n)) ||
                                     (i + NKEYS == last + 1 &&
                                      n == strlen(failed) && !memcmp(v, failed, n)));
            if (rc == KV_OK)
                free(v);
            if (!ok) {
                fprintf(stderr,
                        "\n%s, %d writers, fsync fails after %d: writer %d "
                        "put %d lost (rc=%d)\n",
                        what, nwriters, fsyncs_ok, t, i, rc);
                return 0;
            }
        }
    }
    CHECK_RC(kv_put(db, "after", 5, "ok", 2), KV_OK); /* and it works again */
    CHECK_RC(kv_close(db), KV_OK);
    tu_rmrf(dir);
    return acked_total + 1; /* > 0: passed */
}
#endif

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--child") == 0)
        return child(argv[2], strcmp(argv[3], "always") == 0, atoi(argv[4]));
#if defined(SANITIZED) || !defined(__linux__)
    (void)argv;
    printf("test_fsyncfail: skipped (needs Linux and an uninstrumented build)\n");
    return 0;
#else
    char self[512], shim[600];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    CHECK(sl > 0);
    self[sl] = '\0';
    snprintf(shim, sizeof shim, "%.*s/fault_shim.so",
             (int)(strrchr(self, '/') - self), self);
    CHECK(access(shim, R_OK) == 0);
    int cases = 0;
    long acked = 0;
    for (int always = 1; always >= 0; always--)
        for (int nw = 1; nw <= NWRITERS; nw += NWRITERS - 1)
            for (int n = 0; n <= 80; n++) {
                int r = run_case(self, shim, always, nw, n);
                if (!r)
                    return 1;
                acked += r - 1;
                cases++;
            }
    printf("  %d failure points ok, %ld acknowledged writes verified after "
           "reopening\n", cases, acked);
    printf("test_fsyncfail: passed\n");
    return 0;
#endif
}
