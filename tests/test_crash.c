/*
 * Crash-injection test.
 *
 * Build a store, then for hundreds of random byte positions damage a copy of
 * it (truncate there, flip a byte there, or append a torn tail) and reopen.
 * The expected contents are computed independently by parsing the segment
 * files with the record format documented in the README: every record before
 * the damaged one in that segment survives, the damaged record and everything
 * after it in the same segment are gone, other segments are untouched. Every
 * key must then read back as exactly that value or as absent. A corrupt or
 * unexpected value fails the test.
 *
 *   CRASH_SEED=<n>    RNG seed (default fixed so CI is deterministic)
 *   CRASH_TRIALS=<n>  trials per phase (default 500)
 */
#include <dirent.h>

#include "crc32.h"
#include "test_util.h"

/* ---------- deterministic RNG ---------- */

static uint64_t rng_state;

static uint64_t rng(void)
{
    /* xorshift64* */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

static uint64_t rng_below(uint64_t n)
{
    return rng() % n;
}

/* ---------- independent model of the on-disk log ---------- */

typedef struct {
    int seg;          /* index into the segment list (replay order) */
    uint64_t start, end;
    uint8_t *key;
    uint32_t klen;
    uint8_t *val;     /* NULL for a tombstone */
    uint32_t vlen;
} model_rec;

typedef struct {
    char name[32];
    uint64_t size;
} model_seg;

typedef struct {
    model_seg segs[256];
    int nsegs;
    model_rec *recs;
    size_t nrecs, cap;
} model;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(((const model_seg *)a)->name, ((const model_seg *)b)->name);
}

/* Parse every *.seg file of a pristine store, strictly per the README. */
static void model_load(model *m, const char *dir)
{
    memset(m, 0, sizeof *m);
    DIR *d = opendir(dir);
    CHECK(d);
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (tu_has_suffix(e->d_name, ".seg")) {
            CHECK(m->nsegs < 256);
            snprintf(m->segs[m->nsegs++].name, 32, "%.31s", e->d_name);
        }
    closedir(d);
    /* zero-padded ids: lexical order is id order, which is replay order */
    qsort(m->segs, (size_t)m->nsegs, sizeof *m->segs, name_cmp);

    for (int s = 0; s < m->nsegs; s++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, m->segs[s].name);
        uint64_t size = (uint64_t)tu_file_size(p);
        m->segs[s].size = size;
        uint8_t *buf = malloc(size + 1);
        int fd = open(p, O_RDONLY);
        CHECK(fd >= 0 && buf);
        CHECK(read(fd, buf, size) == (ssize_t)size);
        close(fd);
        uint64_t off = 0;
        while (off < size) {
            CHECK(off + 20 <= size);
            uint32_t crc = rd32(buf + off);
            uint32_t klen = rd32(buf + off + 12), vlen = rd32(buf + off + 16);
            int tomb = vlen == 0xFFFFFFFFu;
            uint64_t len = 20ull + klen + (tomb ? 0 : vlen);
            CHECK(off + len <= size);
            CHECK(crc == crc32_update(0, buf + off + 4, (size_t)len - 4));
            if (m->nrecs == m->cap) {
                m->cap = m->cap ? m->cap * 2 : 1024;
                m->recs = realloc(m->recs, m->cap * sizeof *m->recs);
                CHECK(m->recs);
            }
            model_rec *r = &m->recs[m->nrecs++];
            r->seg = s;
            r->start = off;
            r->end = off + len;
            r->klen = klen;
            r->key = malloc(klen + 1);
            memcpy(r->key, buf + off + 20, klen);
            r->vlen = tomb ? 0 : vlen;
            r->val = NULL;
            if (!tomb) {
                r->val = malloc(vlen + 1);
                memcpy(r->val, buf + off + 20 + klen, vlen);
            }
            off += len;
        }
        CHECK(off == size);
        free(buf);
    }
}

static void model_free(model *m)
{
    for (size_t i = 0; i < m->nrecs; i++) {
        free(m->recs[i].key);
        free(m->recs[i].val);
    }
    free(m->recs);
}

/* The set of distinct keys, each pointing at its first record. */
typedef struct {
    size_t *first; /* record index of first occurrence */
    size_t n;
} key_list;

static int same_key(const model_rec *a, const model_rec *b)
{
    return a->klen == b->klen && memcmp(a->key, b->key, a->klen) == 0;
}

static void keys_build(key_list *kl, const model *m)
{
    kl->first = malloc((m->nrecs + 1) * sizeof *kl->first);
    kl->n = 0;
    for (size_t i = 0; i < m->nrecs; i++) {
        int seen = 0;
        for (size_t j = 0; j < kl->n && !seen; j++)
            seen = same_key(&m->recs[kl->first[j]], &m->recs[i]);
        if (!seen)
            kl->first[kl->n++] = i;
    }
}

/*
 * Expected value of a key after damaging segment `seg` at byte `cut`:
 * records of that segment ending after `cut` are lost. Returns the record
 * holding the surviving value, or NULL if the key should be absent.
 */
static const model_rec *expected(const model *m, const model_rec *key, int seg,
                                 uint64_t cut)
{
    const model_rec *cur = NULL;
    for (size_t i = 0; i < m->nrecs; i++) {
        const model_rec *r = &m->recs[i];
        if (r->seg == seg && r->end > cut)
            continue;
        if (same_key(r, key))
            cur = r->val ? r : NULL;
    }
    return cur;
}

/* ---------- damage + verify ---------- */

enum { DMG_TRUNCATE, DMG_FLIP, DMG_TORN_TAIL, DMG_NONE };

#define ALL_DAMAGE (1u << DMG_TRUNCATE | 1u << DMG_FLIP | 1u << DMG_TORN_TAIL)

static const char *dmg_name[] = {"truncate", "flip", "torn-tail", "none"};

static void damage(const char *dir, const model_seg *s, int kind, uint64_t pos)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, s->name);
    if (kind == DMG_TRUNCATE) {
        CHECK(truncate(p, (off_t)pos) == 0);
    } else if (kind == DMG_FLIP) {
        int fd = open(p, O_RDWR);
        uint8_t b;
        CHECK(fd >= 0 && pread(fd, &b, 1, (off_t)pos) == 1);
        b ^= (uint8_t)(1 + rng_below(255));
        CHECK(pwrite(fd, &b, 1, (off_t)pos) == 1);
        close(fd);
    } else if (kind == DMG_TORN_TAIL) {
        /* a write that died partway: some header bytes, maybe zeros */
        int fd = open(p, O_WRONLY | O_APPEND);
        uint8_t junk[64];
        size_t n = 1 + rng_below(sizeof junk);
        for (size_t i = 0; i < n; i++)
            junk[i] = rng_below(2) ? 0 : (uint8_t)rng();
        CHECK(fd >= 0 && write(fd, junk, n) == (ssize_t)n);
        close(fd);
    }
}

static int verify(const char *dir, const model *m, const key_list *kl, int seg,
                  uint64_t cut, const kv_options *o, const char *what)
{
    kv_t *db;
    int rc = kv_open(dir, o, &db);
    if (rc != KV_OK) {
        fprintf(stderr, "\n%s: open failed: %s\n", what, kv_strerror(rc));
        return 0;
    }
    if (seg >= 0) {
        /* Recovery must cut the damaged segment exactly at the last good
         * record. Leaving bytes behind is not harmless: a later write of the
         * same length could line up with stale records and revive them. */
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, m->segs[seg].name);
        uint64_t size = (uint64_t)tu_file_size(p);
        if (size != cut) {
            fprintf(stderr, "\n%s: segment is %llu bytes, want %llu\n", what,
                    (unsigned long long)size, (unsigned long long)cut);
            kv_close(db);
            return 0;
        }
    }
    size_t live = 0;
    for (size_t i = 0; i < kl->n; i++) {
        const model_rec *k = &m->recs[kl->first[i]];
        const model_rec *want = expected(m, k, seg, cut);
        void *got;
        size_t n;
        rc = kv_get(db, k->key, k->klen, &got, &n);
        int good = want ? rc == KV_OK && n == want->vlen &&
                              memcmp(got, want->val, n) == 0
                        : rc == KV_NOT_FOUND;
        if (rc == KV_OK)
            free(got);
        if (!good) {
            fprintf(stderr,
                    "\n%s: key '%.*s' rc=%d (%s), want %s\n", what,
                    (int)k->klen, (const char *)k->key, rc, kv_strerror(rc),
                    want ? "a specific value" : "absent");
            kv_close(db);
            return 0;
        }
        live += want != NULL;
    }
    if (kv_count(db) != live) {
        fprintf(stderr, "\n%s: count %zu, want %zu\n", what, kv_count(db), live);
        kv_close(db);
        return 0;
    }
    /* the recovered store must accept writes and keep them */
    CHECK_RC(kv_put(db, "__after_crash__", 15, "ok", 2), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_open(dir, o, &db), KV_OK);
    void *v;
    size_t n;
    CHECK_RC(kv_get(db, "__after_crash__", 15, &v, &n), KV_OK);
    CHECK(n == 2 && memcmp(v, "ok", 2) == 0);
    free(v);
    CHECK_RC(kv_delete(db, "__after_crash__", 15), KV_OK);
    CHECK(kv_count(db) == live); /* recovery was persisted, not redone */
    CHECK_RC(kv_close(db), KV_OK);
    return 1;
}

/* Random workload: overwrites, deletes, binary values of varied length. */
static void workload(kv_t *db, int nops, int nkeys)
{
    uint8_t val[200];
    char key[32];
    for (int i = 0; i < nops; i++) {
        snprintf(key, sizeof key, "key-%03d", (int)rng_below((uint64_t)nkeys));
        if (rng_below(10) == 0) {
            int rc = kv_delete(db, key, strlen(key));
            CHECK(rc == KV_OK || rc == KV_NOT_FOUND);
            continue;
        }
        size_t vlen = rng_below(sizeof val);
        for (size_t j = 0; j < vlen; j++)
            val[j] = (uint8_t)rng();
        CHECK_RC(kv_put(db, key, strlen(key), val, vlen), KV_OK);
    }
}

/* Damage random positions across all segments of `pristine`. */
static int run_trials(const char *phase, const char *pristine, int trials,
                      const kv_options *o, unsigned kinds)
{
    model m;
    key_list kl;
    model_load(&m, pristine);
    keys_build(&kl, &m);
    uint64_t total = 0;
    for (int s = 0; s < m.nsegs; s++)
        total += m.segs[s].size;
    printf("  %-34s %d segments, %zu records, %zu keys\n", phase, m.nsegs,
           m.nrecs, kl.n);

    int counts[4] = {0};
    for (int t = 0; t < trials; t++) {
        /* pick a byte uniformly over the whole log */
        uint64_t g = rng_below(total), pos = g;
        int seg = 0;
        while (pos >= m.segs[seg].size)
            pos -= m.segs[seg++].size;
        int kind;
        do
            kind = (int)rng_below(3);
        while (!(kinds & (1u << kind)));

        /* where the store should cut this segment */
        uint64_t cut = m.segs[seg].size;
        if (kind != DMG_TORN_TAIL) {
            for (size_t i = 0; i < m.nrecs; i++)
                if (m.recs[i].seg == seg && pos >= m.recs[i].start &&
                    pos < m.recs[i].end)
                    cut = m.recs[i].start;
        }

        char work[256], what[160];
        tu_tmpdir(work);
        tu_copy_dir(pristine, work);
        damage(work, &m.segs[seg], kind, pos);
        snprintf(what, sizeof what, "%s trial %d: %s %s at byte %llu", phase,
                 t, dmg_name[kind], m.segs[seg].name, (unsigned long long)pos);
        if (!verify(work, &m, &kl, seg, cut, o, what))
            return 0;
        counts[kind]++;
        tu_rmrf(work);
    }
    printf("  %-34s %d trials ok (truncate %d, flip %d, torn tail %d)\n", "",
           trials, counts[DMG_TRUNCATE], counts[DMG_FLIP],
           counts[DMG_TORN_TAIL]);
    free(kl.first);
    model_free(&m);
    return 1;
}

/* Damage that must be survived losslessly: hint files and the MANIFEST. */
static int run_metadata_trials(const char *pristine, int trials,
                               const kv_options *o)
{
    model m;
    key_list kl;
    model_load(&m, pristine);
    keys_build(&kl, &m);

    char meta[64][64];
    int nmeta = 0;
    DIR *d = opendir(pristine);
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if ((tu_has_suffix(e->d_name, ".hint") ||
             strcmp(e->d_name, "MANIFEST") == 0) && nmeta < 64)
            snprintf(meta[nmeta++], 64, "%.63s", e->d_name);
    closedir(d);
    CHECK(nmeta >= 2);

    for (int t = 0; t < trials; t++) {
        const char *name = meta[rng_below((uint64_t)nmeta)];
        char work[256], p[512], what[160];
        tu_tmpdir(work);
        tu_copy_dir(pristine, work);
        snprintf(p, sizeof p, "%s/%s", work, name);
        uint64_t size = (uint64_t)tu_file_size(p);
        int kind = (int)rng_below(3);
        uint64_t pos = rng_below(size);
        if (kind == 0) {
            CHECK(truncate(p, (off_t)pos) == 0);
        } else if (kind == 1) {
            int fd = open(p, O_RDWR);
            uint8_t b;
            CHECK(fd >= 0 && pread(fd, &b, 1, (off_t)pos) == 1);
            b ^= (uint8_t)(1 + rng_below(255));
            CHECK(pwrite(fd, &b, 1, (off_t)pos) == 1);
            close(fd);
        } else {
            CHECK(unlink(p) == 0);
        }
        snprintf(what, sizeof what, "metadata trial %d: %s %s at byte %llu", t,
                 kind == 0 ? "truncate" : kind == 1 ? "flip" : "delete", name,
                 (unsigned long long)pos);
        /* seg = -1: no segment damaged, nothing may be lost */
        if (!verify(work, &m, &kl, -1, 0, o, what))
            return 0;
        tu_rmrf(work);
    }
    printf("  %-34s %d trials ok\n", "hint / MANIFEST damage", trials);
    free(kl.first);
    model_free(&m);
    return 1;
}

int main(void)
{
    const char *s = getenv("CRASH_SEED");
    const char *t = getenv("CRASH_TRIALS");
    uint64_t seed = s ? strtoull(s, NULL, 0) : 0xC0FFEEull;
    int trials = t ? atoi(t) : 500;
    rng_state = seed ? seed : 1;
    printf("test_crash: seed=%llu trials=%d (rerun with CRASH_SEED=%llu)\n",
           (unsigned long long)seed, trials, (unsigned long long)seed);

    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 16 * 1024;

    /* Phase 1: plain log, several segments, replayed from scratch. */
    char log_dir[256];
    tu_tmpdir(log_dir);
    kv_t *db;
    CHECK_RC(kv_open(log_dir, &o, &db), KV_OK);
    workload(db, 1500, 150);
    CHECK_RC(kv_close(db), KV_OK);
    if (!run_trials("log segments", log_dir, trials, &o, ALL_DAMAGE))
        return 1;

    /* Phase 2: compacted segments loaded through hint files, plus a live log
     * on top. Truncations and torn tails must be caught (the hint no longer
     * tiles the segment, so it is replayed); bit flips inside compacted
     * values are covered by test_kv (they are detected at read time). */
    char comp_dir[256];
    tu_tmpdir(comp_dir);
    CHECK_RC(kv_open(comp_dir, &o, &db), KV_OK);
    workload(db, 1500, 150);
    CHECK_RC(kv_compact(db), KV_OK);
    workload(db, 300, 150);
    CHECK_RC(kv_close(db), KV_OK);
    if (!run_trials("compacted + hints (truncate/tail)", comp_dir, trials / 2,
                    &o, 1u << DMG_TRUNCATE | 1u << DMG_TORN_TAIL) ||
        !run_metadata_trials(comp_dir, trials / 2, &o))
        return 1;

    /* Phase 3: the same compacted store with hints disabled, full damage. */
    kv_options nohint = o;
    nohint.use_hints = 0;
    if (!run_trials("compacted, hints off", comp_dir, trials / 2, &nohint,
                    ALL_DAMAGE))
        return 1;

    tu_rmrf(log_dir);
    tu_rmrf(comp_dir);
    printf("test_crash: all trials passed\n");
    return 0;
}
