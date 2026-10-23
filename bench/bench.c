/*
 * Benchmarks. Prints markdown tables, so results can go straight into the
 * README.
 *
 *   bench [--dir DIR] [--quick] [section...]
 *
 * Sections: write, group, get, cold, compact, startup, readers, during,
 * mixed, memory
 * (default: all). DIR defaults to ./bench-data and should be on the disk you
 * want to measure (not tmpfs). --quick shrinks every workload for a smoke run.
 *
 * Throughput and startup numbers are the best of TRIALS runs, which filters
 * out interference from whatever else the machine is doing; latency
 * percentiles come from a single run of many samples.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#include "kv.h"

#define KEY_LEN 16
#define VAL_LEN 100

static const char *g_dir = "./bench-data";
static int g_quick;

#define TRIALS 3

/* ---------- helpers ---------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t rng_next(uint64_t *s)
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 0x2545F4914F6CDD1Dull;
}

static void make_key(char *k, uint64_t i)
{
    snprintf(k, KEY_LEN + 1, "key-%012llu", (unsigned long long)(i % 1000000000000ull));
}

static void make_val(uint8_t *v, size_t n, uint64_t seed)
{
    for (size_t i = 0; i < n; i++)
        v[i] = (uint8_t)('a' + (seed + i * 7) % 26);
}

static void die(const char *what, int rc)
{
    fprintf(stderr, "bench: %s: %s\n", what, kv_strerror(rc));
    exit(1);
}

static void rmrf(const char *path)
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

static kv_t *fresh(kv_sync_policy pol, uint32_t every_n)
{
    rmrf(g_dir);
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = pol;
    o.sync_every_n = every_n;
    kv_t *db;
    int rc = kv_open(g_dir, &o, &db);
    if (rc != KV_OK)
        die("open", rc);
    return db;
}

static void load(kv_t *db, uint64_t n, size_t vlen)
{
    char k[KEY_LEN + 1];
    uint8_t *v = malloc(vlen);
    for (uint64_t i = 0; i < n; i++) {
        make_key(k, i);
        make_val(v, vlen, i);
        int rc = kv_put(db, k, KEY_LEN, v, vlen);
        if (rc != KV_OK)
            die("put", rc);
    }
    free(v);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double pct(uint64_t *sorted, size_t n, double p)
{
    size_t i = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    return (double)sorted[i] / 1000.0; /* us */
}

/* Drop the store's files from the page cache, so the next reads go to disk.
 * posix_fadvise is advisory; it works on clean pages, hence the fsync. */
static int evict_cache(void)
{
#ifdef POSIX_FADV_DONTNEED
    DIR *d = opendir(g_dir);
    if (!d)
        return 0;
    struct dirent *e;
    char p[1024];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0)
            continue;
        fsync(fd);
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
    }
    closedir(d);
    return 1;
#else
    return 0;
#endif
}

static double mb(uint64_t bytes)
{
    return (double)bytes / (1024.0 * 1024.0);
}

/* ---------- 1. write throughput by fsync policy ---------- */

static void bench_write(void)
{
    printf("\n### Write throughput (1 thread, %d B keys, %d B values)\n\n",
           KEY_LEN, VAL_LEN);
    printf("| fsync policy | writes | ops/sec | avg latency |\n");
    printf("|---|---:|---:|---:|\n");
    struct {
        const char *name;
        kv_sync_policy pol;
        uint32_t n;
        uint64_t ops;
    } cases[] = {
        {"every write (`KV_SYNC_ALWAYS`)", KV_SYNC_ALWAYS, 1, 3000},
        {"every 100 writes (`KV_SYNC_EVERY_N`)", KV_SYNC_EVERY_N, 100, 300000},
        {"never (`KV_SYNC_NONE`)", KV_SYNC_NONE, 1, 1000000},
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        uint64_t ops = g_quick ? cases[c].ops / 50 : cases[c].ops;
        uint64_t dt = UINT64_MAX;
        int trials = cases[c].pol == KV_SYNC_ALWAYS ? 1 : TRIALS;
        for (int t = 0; t < trials; t++) {
            kv_t *db = fresh(cases[c].pol, cases[c].n);
            uint64_t t0 = now_ns();
            load(db, ops, VAL_LEN);
            uint64_t d = now_ns() - t0;
            kv_close(db);
            if (d < dt)
                dt = d;
        }
        printf("| %s | %llu | %.0f | %.1f us |\n", cases[c].name,
               (unsigned long long)ops, (double)ops * 1e9 / (double)dt,
               (double)dt / 1000.0 / (double)ops);
        fflush(stdout);
    }
    rmrf(g_dir);
}

/* ---------- 2. group commit: fsync-always writers in parallel ---------- */

typedef struct {
    kv_t *db;
    uint64_t base, n;
} wr_arg;

static void *writer_main(void *p)
{
    wr_arg *a = p;
    char k[KEY_LEN + 1];
    uint8_t v[VAL_LEN];
    for (uint64_t i = 0; i < a->n; i++) {
        make_key(k, a->base + i);
        make_val(v, VAL_LEN, i);
        int rc = kv_put(a->db, k, KEY_LEN, v, VAL_LEN);
        if (rc != KV_OK)
            die("put", rc);
    }
    return NULL;
}

static void bench_group_commit(void)
{
    printf("\n### Group commit: `KV_SYNC_ALWAYS` with concurrent writers\n\n");
    printf("Every put returns only after its data is fsynced. Writers waiting "
           "at the same time share one fsync.\n\n");
    printf("| writer threads | ops/sec | writes per fsync |\n");
    printf("|---:|---:|---:|\n");
    uint64_t total = g_quick ? 200 : 2000;
    for (int t = 1; t <= 8; t *= 2) {
        double best = 0, per_sync = 0;
        for (int trial = 0; trial < TRIALS; trial++) {
            kv_t *db = fresh(KV_SYNC_ALWAYS, 1);
            pthread_t th[8];
            wr_arg args[8];
            uint64_t t0 = now_ns();
            for (int i = 0; i < t; i++) {
                args[i] = (wr_arg){db, (uint64_t)i * total, total / (uint64_t)t};
                pthread_create(&th[i], NULL, writer_main, &args[i]);
            }
            for (int i = 0; i < t; i++)
                pthread_join(th[i], NULL);
            uint64_t dt = now_ns() - t0;
            kv_stats st;
            kv_get_stats(db, &st);
            kv_close(db);
            double rate = (double)st.puts * 1e9 / (double)dt;
            if (rate > best) {
                best = rate;
                per_sync = (double)st.puts / (double)(st.syncs ? st.syncs : 1);
            }
        }
        printf("| %d | %.0f | %.2f |\n", t, best, per_sync);
        fflush(stdout);
    }
    rmrf(g_dir);
}

/* ---------- 3. get latency ---------- */

static void measure_gets(kv_t *db, uint64_t nkeys, uint64_t ngets,
                         uint64_t *lat, uint64_t seed)
{
    char k[KEY_LEN + 1];
    for (uint64_t i = 0; i < ngets; i++) {
        make_key(k, rng_next(&seed) % nkeys);
        void *v;
        size_t n;
        uint64_t t0 = now_ns();
        int rc = kv_get(db, k, KEY_LEN, &v, &n);
        lat[i] = now_ns() - t0;
        if (rc != KV_OK)
            die("get", rc);
        free(v);
    }
}

static void print_lat_row(const char *label, uint64_t *lat, size_t n)
{
    qsort(lat, n, sizeof *lat, cmp_u64);
    printf("| %s | %.1f us | %.1f us | %.1f us | %.1f us |\n", label,
           pct(lat, n, 50), pct(lat, n, 99), pct(lat, n, 99.9),
           (double)lat[n - 1] / 1000.0);
    fflush(stdout);
}

static void bench_get(void)
{
    printf("\n### Get latency (random keys, page cache warm)\n\n");
    printf("| keys | p50 | p99 | p99.9 | max |\n");
    printf("|---:|---:|---:|---:|---:|\n");
    uint64_t sizes[] = {100000, 1000000};
    for (size_t s = 0; s < 2; s++) {
        uint64_t nkeys = g_quick ? sizes[s] / 100 : sizes[s];
        uint64_t ngets = g_quick ? 10000 : 500000;
        kv_t *db = fresh(KV_SYNC_NONE, 1);
        load(db, nkeys, VAL_LEN);
        uint64_t *lat = malloc(ngets * sizeof *lat);
        measure_gets(db, nkeys, ngets / 10, lat, 99); /* warm-up */
        measure_gets(db, nkeys, ngets, lat, 7);
        char label[32];
        snprintf(label, sizeof label, "%lluk",
                 (unsigned long long)(nkeys / 1000));
        print_lat_row(label, lat, ngets);
        free(lat);
        kv_close(db);
    }
    rmrf(g_dir);
}

/* Cold reads: evict the files from the page cache and time gets that have to
 * go to the device. Each one is a single pread. */
static void bench_cold(void)
{
    uint64_t nkeys = g_quick ? 10000 : 1000000;
    uint64_t ngets = g_quick ? 1000 : 20000;
    printf("\n### Get latency, cold (page cache evicted, %lluk keys)\n\n",
           (unsigned long long)(nkeys / 1000));
    kv_t *db = fresh(KV_SYNC_NONE, 1);
    load(db, nkeys, VAL_LEN);
    kv_close(db);
    kv_options o;
    kv_default_options(&o);
    int rc = kv_open(g_dir, &o, &db);
    if (rc != KV_OK)
        die("open", rc);
    if (!evict_cache()) {
        printf("(posix_fadvise unavailable on this platform: skipped)\n");
        kv_close(db);
        rmrf(g_dir);
        return;
    }
    uint64_t *lat = malloc(ngets * sizeof *lat);
    measure_gets(db, nkeys, ngets, lat, 3);
    printf("| reads | p50 | p99 | p99.9 | max |\n");
    printf("|---:|---:|---:|---:|---:|\n");
    char label[32];
    snprintf(label, sizeof label, "%llu", (unsigned long long)ngets);
    print_lat_row(label, lat, ngets);
    free(lat);
    kv_close(db);
    rmrf(g_dir);
}

/* ---------- 4. compaction under heavy overwrite ---------- */

static void bench_compact(void)
{
    uint64_t nkeys = g_quick ? 2000 : 100000;
    int rounds = 10;
    printf("\n### Disk usage before and after compaction\n\n");
    printf("%lluk keys, each overwritten %d times (%d B values), "
           "plus 10%% of keys deleted.\n\n",
           (unsigned long long)(nkeys / 1000), rounds - 1, VAL_LEN);
    kv_t *db = fresh(KV_SYNC_NONE, 1);
    for (int r = 0; r < rounds; r++)
        load(db, nkeys, VAL_LEN);
    char k[KEY_LEN + 1];
    for (uint64_t i = 0; i < nkeys; i += 10) {
        make_key(k, i);
        kv_delete(db, k, KEY_LEN);
    }
    kv_stats before, after;
    kv_get_stats(db, &before);
    kv_sync(db); /* time the compaction, not the writeback of the load */
    uint64_t t0 = now_ns();
    int rc = kv_compact(db);
    uint64_t dt = now_ns() - t0;
    if (rc != KV_OK)
        die("compact", rc);
    kv_get_stats(db, &after);
    kv_close(db);
    printf("| | disk usage | segments | live data |\n");
    printf("|---|---:|---:|---:|\n");
    printf("| before | %.1f MiB | %llu | %.1f MiB |\n", mb(before.disk_bytes),
           (unsigned long long)before.segments, mb(before.live_bytes));
    printf("| after | %.1f MiB | %llu | %.1f MiB |\n", mb(after.disk_bytes),
           (unsigned long long)after.segments, mb(after.live_bytes));
    printf("\nCompaction took %.2f s (%.0f MiB/s of input), leaving %.1f%% of "
           "the original size (the rest is hint files).\n",
           (double)dt / 1e9,
           mb(before.disk_bytes) / ((double)dt / 1e9),
           100.0 * (double)after.disk_bytes / (double)before.disk_bytes);
    fflush(stdout);
    rmrf(g_dir);
}

/* ---------- 5. startup with and without hint files ---------- */

static double time_open(int use_hints, int cold, uint64_t *keys)
{
    kv_options o;
    kv_default_options(&o);
    o.use_hints = use_hints;
    if (cold)
        evict_cache();
    kv_t *db;
    uint64_t t0 = now_ns();
    int rc = kv_open(g_dir, &o, &db);
    uint64_t dt = now_ns() - t0;
    if (rc != KV_OK)
        die("open", rc);
    *keys = kv_count(db);
    kv_close(db);
    return (double)dt / 1e6;
}

static void bench_startup(void)
{
    uint64_t nkeys = g_quick ? 10000 : 1000000;
    size_t vlen = 1000;
    printf("\n### Startup time (rebuilding the index of %lluk keys, %zu B "
           "values)\n\n",
           (unsigned long long)(nkeys / 1000), vlen);
    kv_t *db = fresh(KV_SYNC_NONE, 1);
    load(db, nkeys, vlen);
    int rc = kv_compact(db); /* produces hint files */
    if (rc != KV_OK)
        die("compact", rc);
    kv_stats st;
    kv_get_stats(db, &st);
    kv_close(db);

    printf("| index source | page cache | open time |\n");
    printf("|---|---|---:|\n");
    uint64_t keys;
    for (int cold = 1; cold >= 0; cold--)
        for (int hints = 1; hints >= 0; hints--) {
            if (!cold)
                time_open(hints, 0, &keys); /* warm the cache first */
            double ms = 1e30;
            for (int t = 0; t < TRIALS; t++) {
                double m = time_open(hints, cold, &keys);
                if (m < ms)
                    ms = m;
                if (keys != nkeys)
                    die("startup key count", KV_ERR_CORRUPT);
            }
            printf("| %s | %s | %.0f ms |\n",
                   hints ? "hint files" : "full replay of segments",
                   cold ? "cold" : "warm", ms);
            fflush(stdout);
        }
    printf("\nData: %.0f MiB of segments; hint files: %.0f MiB.\n",
           mb(st.live_bytes), mb(st.disk_bytes - st.live_bytes));
    rmrf(g_dir);
}

/* ---------- 6. read throughput vs reader threads ---------- */

typedef struct {
    kv_t *db;
    uint64_t nkeys, seed, ops;
    atomic_int *stop;
} rd_arg;

static void *reader_main(void *p)
{
    rd_arg *a = p;
    char k[KEY_LEN + 1];
    uint64_t ops = 0;
    while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
        make_key(k, rng_next(&a->seed) % a->nkeys);
        void *v;
        size_t n;
        if (kv_get(a->db, k, KEY_LEN, &v, &n) != KV_OK)
            die("get", KV_ERR_CORRUPT);
        free(v);
        ops++;
    }
    a->ops = ops;
    return NULL;
}

static void bench_readers(void)
{
    uint64_t nkeys = g_quick ? 10000 : 1000000;
    double secs = g_quick ? 0.2 : 1.5;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    printf("\n### Read throughput vs reader threads (%lluk keys, warm, "
           "%ld hardware threads)\n\n",
           (unsigned long long)(nkeys / 1000), ncpu);
    printf("| reader threads | gets/sec | speedup |\n");
    printf("|---:|---:|---:|\n");
    kv_t *db = fresh(KV_SYNC_NONE, 1);
    load(db, nkeys, VAL_LEN);
    double base = 0;
    for (int t = 1; t <= 8; t *= 2) {
        double rate = 0;
        for (int trial = 0; trial < TRIALS; trial++) {
            atomic_int stop = 0;
            pthread_t th[8];
            rd_arg args[8];
            for (int i = 0; i < t; i++) {
                args[i] = (rd_arg){db, nkeys,
                                   0x9E3779B97F4A7C15ull * (uint64_t)(i + 1), 0,
                                   &stop};
                pthread_create(&th[i], NULL, reader_main, &args[i]);
            }
            struct timespec ts = {(time_t)secs,
                                  (long)((secs - (double)(time_t)secs) * 1e9)};
            uint64_t t0 = now_ns();
            nanosleep(&ts, NULL);
            atomic_store(&stop, 1);
            uint64_t total = 0;
            for (int i = 0; i < t; i++) {
                pthread_join(th[i], NULL);
                total += args[i].ops;
            }
            double r = (double)total * 1e9 / (double)(now_ns() - t0);
            if (r > rate)
                rate = r;
        }
        if (t == 1)
            base = rate;
        printf("| %d | %.0f | %.2fx |\n", t, rate, rate / base);
        fflush(stdout);
    }
    kv_close(db);
    rmrf(g_dir);
}

/* ---------- 7. latency while a compaction runs ---------- */

typedef struct {
    kv_t *db;
    atomic_int done;
} cp_arg;

static void *compact_main(void *p)
{
    cp_arg *a = p;
    int rc = kv_compact(a->db);
    if (rc != KV_OK)
        die("compact", rc);
    atomic_store(&a->done, 1);
    return NULL;
}

typedef struct {
    double p50, p99, p999, max;
} lat_summary;

static lat_summary summarize(uint64_t *lat, size_t n)
{
    qsort(lat, n, sizeof *lat, cmp_u64);
    return (lat_summary){pct(lat, n, 50), pct(lat, n, 99), pct(lat, n, 99.9),
                         (double)lat[n - 1] / 1000.0};
}

static void print_summary(const char *label, lat_summary l)
{
    printf("| %s | %.1f us | %.1f us | %.1f us | %.1f us |\n", label, l.p50,
           l.p99, l.p999, l.max);
}

static void bench_during(void)
{
    uint64_t nkeys = g_quick ? 5000 : 200000;
    printf("\n### Latency while compaction runs (%lluk keys x 5 versions)\n\n",
           (unsigned long long)(nkeys / 1000));
    printf("Gets and puts issued from one thread while another thread "
           "compacts the store, compared with an idle store. Best of %d runs "
           "by get p99.9 during compaction.\n\n",
           TRIALS);
    printf("| | op | p50 | p99 | p99.9 | max |\n");
    printf("|---|---|---:|---:|---:|---:|\n");
    size_t cap = 4000000;
    uint64_t *glat = malloc(cap * sizeof *glat), *plat = malloc(cap * sizeof *plat);
    char k[KEY_LEN + 1];
    uint8_t v[VAL_LEN];
    make_val(v, VAL_LEN, 1);
    lat_summary best[4];
    for (int trial = 0; trial < TRIALS; trial++) {
        kv_t *db = fresh(KV_SYNC_NONE, 1);
        for (int r = 0; r < 5; r++)
            load(db, nkeys, VAL_LEN);
        kv_sync(db);
        uint64_t seed = 42;
        lat_summary res[4]; /* idle get, idle put, during get, during put */
        for (int phase = 0; phase < 2; phase++) {
            cp_arg a = {db, 0};
            pthread_t th;
            size_t n = 0;
            if (phase == 1)
                pthread_create(&th, NULL, compact_main, &a);
            uint64_t t_end = now_ns() + 1000000000ull; /* idle phase: 1 s */
            while (n < cap &&
                   (phase == 1 ? !atomic_load(&a.done) : now_ns() < t_end)) {
                make_key(k, rng_next(&seed) % nkeys);
                void *got;
                size_t gl;
                uint64_t t0 = now_ns();
                if (kv_get(db, k, KEY_LEN, &got, &gl) != KV_OK)
                    die("get", KV_ERR_CORRUPT);
                uint64_t t1 = now_ns();
                free(got);
                if (kv_put(db, k, KEY_LEN, v, VAL_LEN) != KV_OK)
                    die("put", KV_ERR_IO);
                uint64_t t2 = now_ns();
                glat[n] = t1 - t0;
                plat[n] = t2 - t1;
                n++;
            }
            if (phase == 1)
                pthread_join(th, NULL);
            res[2 * phase] = summarize(glat, n);
            res[2 * phase + 1] = summarize(plat, n);
        }
        kv_close(db);
        if (trial == 0 || res[2].p999 < best[2].p999)
            memcpy(best, res, sizeof best);
    }
    print_summary("idle | get", best[0]);
    print_summary("idle | put", best[1]);
    print_summary("during compaction | get", best[2]);
    print_summary("during compaction | put", best[3]);
    fflush(stdout);
    free(glat);
    free(plat);
    rmrf(g_dir);
}

/* ---------- 8. read latency next to a busy writer ---------- */

typedef struct {
    kv_t *db;
    size_t vlen;
    atomic_int stop;
    uint64_t ops;
} bw_arg;

static void *busy_writer(void *p)
{
    bw_arg *a = p;
    char k[KEY_LEN + 1];
    uint8_t *v = malloc(a->vlen);
    make_val(v, a->vlen, 5);
    uint64_t seed = 77;
    while (!atomic_load(&a->stop)) {
        make_key(k, rng_next(&seed) % 100000);
        if (kv_put(a->db, k, KEY_LEN, v, a->vlen) != KV_OK)
            die("put", KV_ERR_IO);
        a->ops++;
    }
    free(v);
    return NULL;
}

static void bench_mixed(void)
{
    uint64_t nkeys = g_quick ? 5000 : 100000;
    double secs = g_quick ? 0.3 : 3.0;
    printf("\n### Get latency next to a busy writer (%lluk keys)\n\n",
           (unsigned long long)(nkeys / 1000));
    printf("One thread does random gets while another writes as fast as it "
           "can. Of %d runs, the one with the most writes is shown, so the "
           "read numbers are under the heaviest load seen.\n\n",
           TRIALS);
    printf("| writer | writes/sec | get p50 | get p99 | get p99.9 | get max |\n");
    printf("|---|---:|---:|---:|---:|---:|\n");
    struct {
        const char *name;
        kv_sync_policy pol;
        size_t vlen;
    } cases[] = {
        {"none", KV_SYNC_NONE, 0},
        {"fsync every write, 100 B", KV_SYNC_ALWAYS, VAL_LEN},
        {"no fsync, 4 KiB values", KV_SYNC_NONE, 4096},
    };
    size_t cap = 20000000;
    uint64_t *lat = malloc(cap * sizeof *lat);
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        lat_summary best = {0};
        double best_rate = 0;
        for (int trial = 0; trial < TRIALS; trial++) {
            /* preload without fsync, then reopen with the policy under test */
            kv_t *db = fresh(KV_SYNC_NONE, 1);
            load(db, nkeys, VAL_LEN);
            kv_close(db);
            kv_options o;
            kv_default_options(&o);
            o.sync_policy = cases[c].pol;
            int orc = kv_open(g_dir, &o, &db);
            if (orc != KV_OK)
                die("open", orc);
            bw_arg w = {db, cases[c].vlen, 0, 0};
            pthread_t th;
            if (cases[c].vlen)
                pthread_create(&th, NULL, busy_writer, &w);
            uint64_t seed = 11, n = 0, t0 = now_ns();
            uint64_t t_end = t0 + (uint64_t)(secs * 1e9);
            char k[KEY_LEN + 1];
            while (n < cap && now_ns() < t_end) {
                make_key(k, rng_next(&seed) % nkeys);
                void *got;
                size_t gl;
                uint64_t a = now_ns();
                if (kv_get(db, k, KEY_LEN, &got, &gl) != KV_OK)
                    die("get", KV_ERR_CORRUPT);
                lat[n++] = now_ns() - a;
                free(got);
            }
            uint64_t dt = now_ns() - t0;
            if (cases[c].vlen) {
                atomic_store(&w.stop, 1);
                pthread_join(th, NULL);
            }
            kv_close(db);
            /* Choosing by read latency would favour runs where the writer
             * made little progress; choose by the writer instead. */
            lat_summary l = summarize(lat, n);
            double rate = (double)w.ops * 1e9 / (double)dt;
            if (trial == 0 || rate > best_rate) {
                best = l;
                best_rate = rate;
            }
        }
        printf("| %s | %.0f | %.1f us | %.1f us | %.1f us | %.1f us |\n",
               cases[c].name, best_rate, best.p50, best.p99, best.p999,
               best.max);
        fflush(stdout);
    }
    free(lat);
    rmrf(g_dir);
}

/* ---------- 9. index memory per key ---------- */

/* Resident set size right now, where the OS tells us (Linux), else 0. */
static double rss_mib(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    long pages = 0, resident = 0;
    int n = fscanf(f, "%ld %ld", &pages, &resident);
    fclose(f);
    return n == 2 ? (double)resident * (double)sysconf(_SC_PAGESIZE) /
                        (1024.0 * 1024.0)
                  : 0;
}

static void bench_memory(void)
{
    uint64_t nkeys = g_quick ? 10000 : 1000000;
    printf("\n### Index memory (%lluk keys of %d bytes)\n\n",
           (unsigned long long)(nkeys / 1000), KEY_LEN);
    /* Load in a child, so this process has never held an index: its
     * resident set then grows by what opening the store really costs. */
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        kv_t *c = fresh(KV_SYNC_NONE, 1);
        load(c, nkeys, VAL_LEN);
        _exit(kv_close(c) == KV_OK ? 0 : 1);
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || status != 0)
        die("loading in a child", KV_ERR_IO);
    kv_t *db;
    double before = rss_mib();
    int rc = kv_open(g_dir, NULL, &db);
    if (rc != KV_OK)
        die("open", rc);
    double after = rss_mib();
    kv_stats st;
    kv_get_stats(db, &st);
    printf("The index holds %.1f MiB: %.0f bytes per key.", 
           mb(st.index_bytes), (double)st.index_bytes / (double)st.keys);
    if (before > 0)
        printf(" Opening the store grew the resident set by %.1f MiB.",
               after - before);
    printf("\n");
    fflush(stdout);
    kv_close(db);
    rmrf(g_dir);
}

/* ---------- main ---------- */

static void print_env(void)
{
    struct utsname u;
    uname(&u);
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    char cpu[256] = "unknown CPU";
#ifdef __APPLE__
    size_t cpulen = sizeof cpu;
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &cpulen, NULL, 0) != 0)
        snprintf(cpu, sizeof cpu, "unknown CPU");
#endif
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "model name", 10) == 0) {
                char *c = strchr(line, ':');
                if (c) {
                    snprintf(cpu, sizeof cpu, "%s", c + 2);
                    cpu[strcspn(cpu, "\n")] = '\0';
                }
                break;
            }
        fclose(f);
    }
    printf("## meeseeks benchmarks%s\n\n", g_quick ? " (quick mode)" : "");
    printf("- CPU: %s (%ld hardware threads)\n", cpu, ncpu);
    printf("- OS: %s %s\n", u.sysname, u.release);
    printf("- Data directory: `%s`\n", g_dir);
}

int main(int argc, char **argv)
{
    const char *only[16];
    int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
            g_dir = argv[++i];
        else if (strcmp(argv[i], "--quick") == 0)
            g_quick = 1;
        else if (nonly < 16)
            only[nonly++] = argv[i];
    }
    struct {
        const char *name;
        void (*fn)(void);
    } sections[] = {
        /* memory runs first, in a process that has not yet allocated and
         * freed big indexes (the allocator would reuse that memory); mixed
         * runs before the sections that write a lot, whose aftermath makes
         * the SSD's fsync latency erratic */
        {"memory", bench_memory},   {"write", bench_write},
        {"group", bench_group_commit}, {"mixed", bench_mixed},
        {"get", bench_get},         {"cold", bench_cold},
        {"compact", bench_compact},
        {"startup", bench_startup}, {"readers", bench_readers},
        {"during", bench_during},
    };
    print_env();
    for (size_t s = 0; s < sizeof sections / sizeof *sections; s++) {
        int run = nonly == 0;
        for (int i = 0; i < nonly; i++)
            run |= strcmp(only[i], sections[s].name) == 0;
        if (run)
            sections[s].fn();
    }
    return 0;
}
