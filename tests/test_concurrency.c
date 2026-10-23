/*
 * Readers, a writer and a compactor hammering one store at the same time.
 *
 * Values are "<key>:<version>" and the writer only ever raises a key's
 * version, so a reader can check two things on every get: the value belongs to
 * the key it asked for (no torn or misdirected reads, even while compaction
 * moves records between files), and versions never go backwards (no stale
 * index entries after a compaction swap).
 */
#include <pthread.h>
#include <stdatomic.h>

#include "test_util.h"

enum { NKEYS = 512, NREADERS = 6, WRITES = 30000 };

typedef struct {
    kv_t *db;
    atomic_int done;
    long reads;
    pthread_mutex_t mu;
} shared;

static void key_of(int i, char *buf, size_t n)
{
    snprintf(buf, n, "key-%04d", i);
}

static void *writer(void *arg)
{
    shared *s = arg;
    char k[32], v[64];
    unsigned seed = 1;
    for (int i = 0; i < WRITES; i++) {
        seed = seed * 1103515245u + 12345u;
        int ki = (int)((seed >> 8) % NKEYS);
        key_of(ki, k, sizeof k);
        snprintf(v, sizeof v, "%s:%d", k, i + 1); /* i grows: versions rise */
        if (ki % 7 == 0 && i % 3 == 0) {
            int rc = kv_delete(s->db, k, strlen(k));
            CHECK(rc == KV_OK || rc == KV_NOT_FOUND);
        } else {
            CHECK_RC(kv_put(s->db, k, strlen(k), v, strlen(v)), KV_OK);
        }
    }
    s->done = 1;
    return NULL;
}

static void *compactor(void *arg)
{
    shared *s = arg;
    while (!s->done) {
        CHECK_RC(kv_compact(s->db), KV_OK);
        tu_sleep_us(2000);
    }
    return NULL;
}

static void *reader(void *arg)
{
    shared *s = arg;
    int last[NKEYS] = {0};
    char k[32];
    long n = 0;
    unsigned seed = (unsigned)(uintptr_t)&last;
    while (!s->done) {
        seed = seed * 1103515245u + 12345u;
        int ki = (int)((seed >> 8) % NKEYS);
        key_of(ki, k, sizeof k);
        void *v;
        size_t vlen;
        int rc = kv_get(s->db, k, strlen(k), &v, &vlen);
        n++;
        if (rc == KV_NOT_FOUND)
            continue; /* never written yet, or deleted */
        CHECK_RC(rc, KV_OK);
        size_t klen = strlen(k);
        CHECK(vlen > klen + 1 && memcmp(v, k, klen) == 0 &&
              ((char *)v)[klen] == ':');
        int ver = atoi((char *)v + klen + 1);
        CHECK(ver > 0);
        /* deletes allow a key to vanish, but a value can't go back in time */
        CHECK(ver >= last[ki]);
        last[ki] = ver;
        free(v);
    }
    pthread_mutex_lock(&s->mu);
    s->reads += n;
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static void test_readers_writer_compactor(const char *dir)
{
    shared s = {0};
    pthread_mutex_init(&s.mu, NULL);
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 32 * 1024; /* many segments, frequent rollover */
    CHECK_RC(kv_open(dir, &o, &s.db), KV_OK);

    pthread_t w, c, r[NREADERS];
    for (int i = 0; i < NREADERS; i++)
        CHECK(pthread_create(&r[i], NULL, reader, &s) == 0);
    CHECK(pthread_create(&c, NULL, compactor, &s) == 0);
    CHECK(pthread_create(&w, NULL, writer, &s) == 0);
    pthread_join(w, NULL);
    pthread_join(c, NULL);
    for (int i = 0; i < NREADERS; i++)
        pthread_join(r[i], NULL);
    CHECK(s.reads > 0);

    /* final state survives a restart, both with and without hints */
    size_t count = kv_count(s.db);
    CHECK_RC(kv_close(s.db), KV_OK);
    for (int hints = 0; hints < 2; hints++) {
        o.use_hints = hints;
        CHECK_RC(kv_open(dir, &o, &s.db), KV_OK);
        CHECK(kv_count(s.db) == count);
        CHECK_RC(kv_close(s.db), KV_OK);
    }
    pthread_mutex_destroy(&s.mu);
}


/*
 * Compaction must not stop the world: with a big store being compacted, puts
 * and gets issued meanwhile have to complete before compaction does.
 */
typedef struct {
    kv_t *db;
    atomic_int started, done;
    long puts_during, gets_during;
} bg_compact;

static void *compact_thread(void *arg)
{
    bg_compact *b = arg;
    atomic_store(&b->started, 1);
    CHECK_RC(kv_compact(b->db), KV_OK);
    atomic_store(&b->done, 1);
    return NULL;
}

static void test_ops_during_compaction(const char *dir)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 4u << 20;
    bg_compact b = {0};
    CHECK_RC(kv_open(dir, &o, &b.db), KV_OK);
    char k[32];
    static char val[2048];
    memset(val, 'v', sizeof val);
    for (int round = 0; round < 3; round++) /* ~90 MB, 2/3 of it garbage */
        for (int i = 0; i < 15000; i++) {
            snprintf(k, sizeof k, "key-%05d", i);
            CHECK_RC(kv_put(b.db, k, strlen(k), val, sizeof val), KV_OK);
        }

    pthread_t th;
    CHECK(pthread_create(&th, NULL, compact_thread, &b) == 0);
    while (!atomic_load(&b.started))
        ;
    int i = 0;
    while (!atomic_load(&b.done)) {
        snprintf(k, sizeof k, "new-%d", i);
        CHECK_RC(kv_put(b.db, k, strlen(k), "fresh", 5), KV_OK);
        if (!atomic_load(&b.done))
            b.puts_during++;
        void *v;
        size_t n;
        snprintf(k, sizeof k, "key-%05d", i % 15000);
        CHECK_RC(kv_get(b.db, k, strlen(k), &v, &n), KV_OK);
        CHECK(n == sizeof val && memcmp(v, val, n) == 0);
        free(v);
        if (!atomic_load(&b.done))
            b.gets_during++;
        i++;
    }
    pthread_join(th, NULL);
    printf("[%ld puts, %ld gets during compaction] ", b.puts_during,
           b.gets_during);
    CHECK(b.puts_during > 100 && b.gets_during > 100);

    for (int j = 0; j < i; j++) {
        snprintf(k, sizeof k, "new-%d", j);
        void *v;
        size_t n;
        CHECK_RC(kv_get(b.db, k, strlen(k), &v, &n), KV_OK);
        CHECK(n == 5 && memcmp(v, "fresh", 5) == 0);
        free(v);
    }
    CHECK(kv_count(b.db) == 15000u + (size_t)i);
    CHECK_RC(kv_close(b.db), KV_OK);
    CHECK_RC(kv_open(dir, &o, &b.db), KV_OK);
    CHECK(kv_count(b.db) == 15000u + (size_t)i);
    CHECK_RC(kv_close(b.db), KV_OK);
}

/* Many writers with an fsync per write, plus explicit syncs: every write
 * must land exactly once, and none may return before its group commit. */
typedef struct {
    kv_t *db;
    int id;
} gc_arg;

static void *gc_writer(void *arg)
{
    gc_arg *g = arg;
    char k[32], v[32];
    for (int i = 0; i < 300; i++) {
        snprintf(k, sizeof k, "w%d-%d", g->id, i);
        snprintf(v, sizeof v, "value %d", i);
        CHECK_RC(kv_put(g->db, k, strlen(k), v, strlen(v)), KV_OK);
        if (i % 50 == 0)
            CHECK_RC(kv_sync(g->db), KV_OK);
        if (i % 3 == 0)
            CHECK_RC(kv_delete(g->db, k, strlen(k)), KV_OK);
    }
    return NULL;
}

static void test_group_commit(const char *dir)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_ALWAYS;
    o.segment_size = 16 * 1024; /* rollovers race with group commits */
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    enum { NW = 8 };
    pthread_t th[NW];
    gc_arg args[NW];
    for (int i = 0; i < NW; i++) {
        args[i] = (gc_arg){db, i};
        CHECK(pthread_create(&th[i], NULL, gc_writer, &args[i]) == 0);
    }
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);
    kv_stats st;
    kv_get_stats(db, &st);
    CHECK(st.puts == NW * 300 && st.deletes == NW * 100);
    CHECK(st.keys == NW * 200);
    CHECK(st.syncs > 0 && st.syncs <= st.puts + st.deletes + 64);
    printf("[%llu writes, %llu fsyncs] ",
           (unsigned long long)(st.puts + st.deletes),
           (unsigned long long)st.syncs);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    CHECK(kv_count(db) == NW * 200);
    CHECK_RC(kv_close(db), KV_OK);
}

/*
 * Background compaction must keep up even when writers never pause; it has
 * to win the writer mutex to seal the active segment each time.
 */
typedef struct {
    kv_t *db;
    int id;
    atomic_int *stop;
} hw_arg;

static void *hammer_writer(void *arg)
{
    hw_arg *a = arg;
    char k[32];
    static const char v[512] = {0};
    for (unsigned i = 0; !atomic_load(a->stop); i++) {
        snprintf(k, sizeof k, "w%d-%u", a->id, i % 200);
        CHECK_RC(kv_put(a->db, k, strlen(k), v, sizeof v), KV_OK);
    }
    return NULL;
}

static void test_compactor_not_starved(const char *dir)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 256 * 1024;
    o.auto_compact_ratio = 0.5;
    kv_t *db;
    CHECK_RC(kv_open(dir, &o, &db), KV_OK);
    enum { NW = 4 };
    atomic_int stop = 0;
    pthread_t th[NW];
    hw_arg args[NW];
    for (int i = 0; i < NW; i++) {
        args[i] = (hw_arg){db, i, &stop};
        CHECK(pthread_create(&th[i], NULL, hammer_writer, &args[i]) == 0);
    }
    kv_stats st;
    for (int ms = 0; ms < 5000; ms += 10) {
        tu_sleep_us(10000);
        kv_get_stats(db, &st);
        if (st.compactions >= 3)
            break;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < NW; i++)
        pthread_join(th[i], NULL);
    kv_get_stats(db, &st);
    printf("[%llu compactions, %.1f MiB on disk] ",
           (unsigned long long)st.compactions,
           (double)st.disk_bytes / (1024.0 * 1024.0));
    CHECK(st.compactions >= 3);
    CHECK_RC(kv_close(db), KV_OK);
}

int main(void)
{
    static const test_case tests[] = {
        {"readers + writer + compactor", test_readers_writer_compactor},
        {"ops complete during compaction", test_ops_during_compaction},
        {"group commit, 8 fsync-always writers", test_group_commit},
        {"compactor not starved by writers", test_compactor_not_starved},
    };
    return tu_run("test_concurrency", tests, sizeof tests / sizeof *tests);
}
