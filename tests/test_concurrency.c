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

int main(void)
{
    static const test_case tests[] = {
        {"readers + writer + compactor", test_readers_writer_compactor},
    };
    return tu_run("test_concurrency", tests, sizeof tests / sizeof *tests);
}
