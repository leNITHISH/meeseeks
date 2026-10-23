/*
 * SIGKILL test: a child process writes as fast as it can (with background and
 * explicit compactions going on) and reports each acknowledged put through a
 * pipe. The parent kills it with SIGKILL at a random moment, reopens the
 * store and checks that every key holds exactly its last acknowledged value,
 * or the single write that was in flight when the child died. Then it starts
 * a new child on the same directory and does it again, so recovery also has
 * to cope with the leftovers of earlier crashes.
 *
 * A process crash keeps the page cache, so this checks atomicity and recovery
 * logic (torn compactions, half-renamed files, stale temp files), not fsync.
 *
 *   KILL_SEED=<n>   RNG seed (default fixed)
 *   KILL_ROUNDS=<n> number of kills (default 100)
 */
#include <signal.h>
#include <sys/wait.h>

#include "test_util.h"

enum { NKEYS = 256 };

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

/* Every value is derived from its sequence number, so it can be checked. */
static size_t make_value(uint64_t seq, uint8_t *buf)
{
    size_t len = 1 + (size_t)((seq * 7919u) % 1500u);
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seq * 31u + i * 17u);
    return len;
}

static void key_of(uint64_t seq, char *k, size_t n)
{
    snprintf(k, n, "key-%03u", (unsigned)(seq % NKEYS));
}

static void child(const char *dir, int ackfd, uint64_t seq)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 64 * 1024;
    o.auto_compact_ratio = 0.4;
    kv_t *db;
    if (kv_open(dir, &o, &db) != KV_OK)
        _exit(3);
    uint8_t val[2048];
    char k[32];
    for (;; seq++) {
        key_of(seq, k, sizeof k);
        size_t n = make_value(seq, val);
        if (kv_put(db, k, strlen(k), val, n) != KV_OK)
            _exit(4);
        if (write(ackfd, &seq, sizeof seq) != (ssize_t)sizeof seq)
            _exit(5);
        if (seq % 997 == 0 && kv_compact(db) != KV_OK)
            _exit(6);
    }
}

int main(void)
{
    const char *s = getenv("KILL_SEED"), *r = getenv("KILL_ROUNDS");
    uint64_t seed = s ? strtoull(s, NULL, 0) : 0xDEAD;
    int rounds = r ? atoi(r) : 100;
    rng_state = seed ? seed : 1;
    printf("test_kill: seed=%llu rounds=%d\n", (unsigned long long)seed,
           rounds);

    char dir[256];
    tu_tmpdir(dir);
    uint64_t last[NKEYS]; /* last acknowledged seq per key, 0 = none */
    memset(last, 0, sizeof last);
    uint64_t next_seq = 1, total_acks = 0;
    int mid_compaction = 0;

    for (int round = 0; round < rounds; round++) {
        int fds[2];
        CHECK(pipe(fds) == 0);
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            close(fds[0]);
            child(dir, fds[1], next_seq);
        }
        close(fds[1]);
        tu_sleep_us(2000 + (long)(rng() % 40000));
        CHECK(kill(pid, SIGKILL) == 0);
        int status;
        CHECK(waitpid(pid, &status, 0) == pid);
        if (!WIFSIGNALED(status)) {
            fprintf(stderr, "child exited on its own with status %d\n",
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return 1;
        }

        uint64_t seq, max_acked = next_seq - 1;
        while (read(fds[0], &seq, sizeof seq) == (ssize_t)sizeof seq) {
            CHECK(seq == max_acked + 1); /* acks arrive in order */
            last[seq % NKEYS] = seq;
            max_acked = seq;
            total_acks++;
        }
        close(fds[0]);
        /* The put after the last ack may or may not have happened. */
        uint64_t inflight = max_acked + 1;

        /* A crash in the middle of a compaction leaves .tmp files. */
        mid_compaction += tu_count_files(dir, ".tmp") > 0;

        kv_t *db;
        kv_options o;
        kv_default_options(&o);
        o.segment_size = 64 * 1024;
        CHECK_RC(kv_open(dir, &o, &db), KV_OK);
        uint8_t want[2048];
        char k[32];
        for (unsigned i = 0; i < NKEYS; i++) {
            snprintf(k, sizeof k, "key-%03u", i);
            void *v;
            size_t n;
            int rc = kv_get(db, k, strlen(k), &v, &n);
            int ok;
            if (rc == KV_NOT_FOUND) {
                ok = last[i] == 0;
            } else {
                CHECK_RC(rc, KV_OK);
                size_t wn = last[i] ? make_value(last[i], want) : 0;
                ok = last[i] && n == wn && memcmp(v, want, n) == 0;
                if (!ok && inflight % NKEYS == i) {
                    wn = make_value(inflight, want);
                    ok = n == wn && memcmp(v, want, n) == 0;
                    if (ok)
                        last[i] = inflight; /* it made it */
                }
                free(v);
            }
            if (!ok) {
                fprintf(stderr,
                        "round %d: key %u: rc=%d, last acked seq %llu, "
                        "in flight %llu\n",
                        round, i, rc, (unsigned long long)last[i],
                        (unsigned long long)inflight);
                return 1;
            }
        }
        CHECK_RC(kv_close(db), KV_OK);
        next_seq = inflight + 1;
    }
    printf("  %d kills ok: %llu acknowledged puts, %d kills left an "
           "unfinished compaction\n",
           rounds, (unsigned long long)total_acks, mid_compaction);
    tu_rmrf(dir);
    printf("test_kill: passed\n");
    return 0;
}
