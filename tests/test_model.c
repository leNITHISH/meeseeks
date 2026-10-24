/*
 * Model-based randomized test.
 *
 * Drives the store with a long random sequence of puts, deletes, gets,
 * compactions, clean reopens and simulated crashes, and checks it against a
 * trivial in-memory model after every step. Whole-store checks also require
 * a scan to list exactly the live keys and a scrub to find nothing. A "crash" copies the directory
 * while the store is still open (so nothing from kv_close() runs), opens the
 * copy and requires it to match the model exactly: at every operation
 * boundary the files on disk must already describe a consistent store.
 *
 *   MODEL_SEED=<n>   base seed (default fixed)
 *   MODEL_RUNS=<n>   independent runs (default 24)
 *   MODEL_STEPS=<n>  operations per run (default 4000)
 */
#include "test_util.h"

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

static uint64_t below(uint64_t n)
{
    return rng() % n;
}

enum { MAX_KEYS = 300 };

typedef struct {
    int present;
    uint8_t *val;
    size_t len;
} slot;

typedef struct {
    slot s[MAX_KEYS];
    int nkeys;
    size_t live;
} model;

static void key_name(int i, char *buf, size_t n)
{
    /* vary key lengths, including a zero-length key */
    if (i == 0)
        buf[0] = '\0';
    else
        snprintf(buf, n, "k%d%.*s", i, i % 23, "-----------------------");
}

static void check_key(kv_t *db, const model *m, int i, const char *ctx)
{
    char k[64];
    key_name(i, k, sizeof k);
    void *v;
    size_t n;
    int rc = kv_get(db, k, strlen(k), &v, &n);
    const slot *s = &m->s[i];
    int ok = s->present ? rc == KV_OK && n == s->len && memcmp(v, s->val, n) == 0
                        : rc == KV_NOT_FOUND;
    if (rc == KV_OK)
        free(v);
    if (!ok) {
        fprintf(stderr, "\n%s: key %d ('%s'): rc=%d (%s), want %s (len %zu)\n",
                ctx, i, k, rc, kv_strerror(rc),
                s->present ? "value" : "absent", s->len);
        exit(1);
    }
}

typedef struct {
    const model *m;
    int seen[MAX_KEYS];
    int others;
} scan_check;

/* Which key_name() is this, or -1 for none. */
static int key_number(const model *m, const void *key, size_t klen)
{
    char buf[64], want[64];
    if (klen >= sizeof buf)
        return -1;
    memcpy(buf, key, klen);
    buf[klen] = '\0';
    int i = klen == 0 ? 0 : buf[0] == 'k' ? atoi(buf + 1) : -1;
    if (i < 0 || i >= m->nkeys)
        return -1;
    key_name(i, want, sizeof want);
    return strlen(want) == klen && memcmp(want, key, klen) == 0 ? i : -1;
}

static int scan_mark(void *ctx, const void *key, size_t klen)
{
    scan_check *c = ctx;
    int i = key_number(c->m, key, klen);
    if (i < 0)
        c->others++;
    else
        c->seen[i]++;
    return 0;
}

/* Every key, the count, a scan that lists exactly the live keys, and a
 * scrub that finds nothing wrong. */
static void check_all(kv_t *db, const model *m, const char *ctx)
{
    for (int i = 0; i < m->nkeys; i++)
        check_key(db, m, i, ctx);
    if (kv_count(db) != m->live) {
        fprintf(stderr, "\n%s: count %zu, want %zu\n", ctx, kv_count(db),
                m->live);
        exit(1);
    }
    static scan_check c;
    memset(&c, 0, sizeof c);
    c.m = m;
    CHECK_RC(kv_scan(db, scan_mark, &c), KV_OK);
    for (int i = 0; i < m->nkeys; i++)
        if (c.seen[i] != m->s[i].present) {
            fprintf(stderr, "\n%s: scan saw key %d %d times, want %d\n", ctx,
                    i, c.seen[i], m->s[i].present);
            exit(1);
        }
    CHECK(c.others == 0);
    CHECK_RC(kv_scrub(db), KV_OK);
}

static void run(uint64_t seed, int steps, int *stats)
{
    rng_state = seed ? seed : 1;
    char dir[256];
    tu_tmpdir(dir);

    kv_options o;
    kv_default_options(&o);
    o.sync_policy = (kv_sync_policy)below(3);
    if (o.sync_policy == KV_SYNC_ALWAYS)
        steps /= 4; /* an fsync per write is slow and adds no new states */
    o.sync_every_n = 1 + (uint32_t)below(50);
    o.segment_size = 512u << below(8); /* 512 B .. 64 KiB: lots of rollover */

    model m;
    memset(&m, 0, sizeof m);
    m.nkeys = 8 + (int)below(MAX_KEYS - 8);

    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    uint8_t *buf = malloc(200000);
    CHECK(buf);
    char ctx[128], k[64];

    for (int step = 0; step < steps; step++) {
        snprintf(ctx, sizeof ctx, "seed %llu step %d",
                 (unsigned long long)seed, step);
        uint64_t op = below(1000);
        int i = (int)below((uint64_t)m.nkeys);
        key_name(i, k, sizeof k);
        slot *s = &m.s[i];

        if (op < 450) { /* put */
            size_t len = below(100) == 0 ? (size_t)below(200000) /* oversize */
                                         : (size_t)below(300);
            for (size_t j = 0; j < len; j++)
                buf[j] = (uint8_t)rng();
            CHECK_RC(kv_put(db, k, strlen(k), buf, len), KV_OK);
            free(s->val);
            s->val = malloc(len + 1);
            memcpy(s->val, buf, len);
            s->len = len;
            m.live += !s->present;
            s->present = 1;
            stats[0]++;
        } else if (op < 600) { /* delete */
            CHECK_RC(kv_delete(db, k, strlen(k)),
                     s->present ? KV_OK : KV_NOT_FOUND);
            m.live -= s->present;
            s->present = 0;
            stats[1]++;
        } else if (op < 940) { /* get */
            check_key(db, &m, i, ctx);
            stats[2]++;
        } else if (op < 965) { /* compact */
            CHECK_RC(kv_compact(db), KV_OK);
            check_all(db, &m, ctx);
            stats[3]++;
        } else if (op < 985) { /* clean reopen, maybe without hints */
            CHECK_RC(kv_close(db), KV_OK);
            o.use_hints = (int)below(2);
            CHECK_RC(kv_open(dir, &o, &db), KV_OK);
            check_all(db, &m, ctx);
            stats[4]++;
        } else { /* crash: copy the live directory and recover the copy */
            char crash[256];
            tu_tmpdir(crash);
            tu_copy_dir(dir, crash);
            kv_options co = o;
            co.use_hints = (int)below(2);
            kv_t *c;
            CHECK_RC(kv_open(crash, &co, &c), KV_OK);
            check_all(c, &m, ctx);
            CHECK_RC(kv_close(c), KV_OK);
            tu_rmrf(crash);
            stats[5]++;
        }
    }
    check_all(db, &m, "final");
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    check_all(db, &m, "final reopen");
    CHECK_RC(kv_close(db), KV_OK);

    for (int i = 0; i < MAX_KEYS; i++)
        free(m.s[i].val);
    free(buf);
    tu_rmrf(dir);
}

int main(void)
{
    const char *s = getenv("MODEL_SEED"), *r = getenv("MODEL_RUNS"),
               *n = getenv("MODEL_STEPS");
    uint64_t seed = s ? strtoull(s, NULL, 0) : 0x5EED;
    int runs = r ? atoi(r) : 24, steps = n ? atoi(n) : 4000;
    printf("test_model: seed=%llu runs=%d steps=%d\n", (unsigned long long)seed,
           runs, steps);
    int stats[6] = {0};
    for (int i = 0; i < runs; i++) {
        /* each run gets its own seed so a failure names a single run */
        uint64_t rs = seed * 1000003ull + (uint64_t)i;
        run(rs, steps, stats);
    }
    printf("  %d runs ok: %d puts, %d deletes, %d gets, %d compactions, "
           "%d reopens, %d crash copies\n",
           runs, stats[0], stats[1], stats[2], stats[3], stats[4], stats[5]);
    printf("test_model: passed\n");
    return 0;
}
