/*
 * Power-failure test (Linux).
 *
 * A child runs a workload with KV_SYNC_ALWAYS under the pf_shim.so recorder
 * (LD_PRELOAD), which logs every create, write, truncate, fsync, rename and
 * unlink in the store directory plus "op i started" / "op i acknowledged"
 * markers. The parent replays that log through a model of a POSIX file
 * system and, at every point in it, builds the disk images a power cut could
 * have left:
 *
 *   durable   only what was fsynced: file data as of each file's last
 *             fsync, directory entries as of the directory's last fsync
 *   torn      durable, plus a random subset of the unsynced 512-byte
 *             sectors and a random prefix of the unsynced directory changes
 *             (journaling file systems commit those in order)
 *   reorder   like torn, but any subset of the unsynced directory changes,
 *             in any combination: all that POSIX itself promises
 *   cached    everything (what a process crash leaves)
 *
 * Each image is recovered with the real store, and the result must be the
 * state after some prefix of the operations: at least up to the last one
 * known to be durable, at most up to the one in flight at the cut. The test
 * runs twice: with KV_SYNC_ALWAYS every acknowledged operation is durable;
 * with KV_SYNC_NONE only those before a completed kv_sync() or reopen are,
 * but the store must still never show a state that did not exist (newer
 * records surviving while older ones were lost). Together these check that
 * every fsync and directory fsync the store relies on is issued, in order.
 *
 *   PF_SEED=<n>  workload and image seed      PF_OPS=<n>  ops (default 300)
 */
#include <dlfcn.h>
#include <sys/wait.h>

#include "test_util.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SANITIZED 1
#endif
#endif

enum { NKEYS = 12, PUT = 0, DEL, COMPACT, REOPEN, SYNC };
enum { PF_CREATE = 1, PF_WRITE, PF_TRUNC, PF_FSYNC, PF_RENAME, PF_UNLINK,
       PF_MARK };
#define MARK_BEGIN (1ull << 56)
#define MARK_ACK (2ull << 56)

static uint64_t rng_state;

static uint64_t rng(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

/* ---------- the workload (same sequence in child and parent) ---------- */

typedef struct {
    int kind, key;
    size_t len;
} op;

static op *ops;
static int nops;

static void gen_ops(uint64_t seed, int n)
{
    rng_state = seed;
    free(ops);
    ops = calloc((size_t)n, sizeof *ops);
    nops = n;
    for (int i = 0; i < n; i++) {
        uint64_t r = rng() % 100;
        ops[i].key = (int)(rng() % NKEYS);
        ops[i].len = (size_t)(rng() % 90);
        ops[i].kind = r < 66 ? PUT : r < 80 ? DEL : r < 88 ? COMPACT
                    : r < 93 ? REOPEN : SYNC;
    }
}

static void value_of(int i, uint8_t *v, size_t len)
{
    for (size_t j = 0; j < len; j++)
        v[j] = (uint8_t)(i * 31 + (int)j * 7 + 1);
}

static void key_name(int k, char *buf)
{
    snprintf(buf, 16, "key-%02d", k);
}

static kv_options workload_opts(kv_sync_policy pol)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = pol;
    o.segment_size = 400; /* a rollover every few writes */
    return o;
}

/* Does completing op i make everything up to it durable? */
static int durable_after(kv_sync_policy pol, int i)
{
    return pol == KV_SYNC_ALWAYS || ops[i].kind == SYNC ||
           ops[i].kind == REOPEN;
}

static int run_workload(const char *dir, kv_sync_policy pol)
{
    void *self = dlopen(NULL, RTLD_NOW);
    void (*mark)(uint64_t) = NULL;
    if (self) /* the POSIX-sanctioned way to get a function from dlsym */
        *(void **)&mark = dlsym(self, "pf_mark");
    if (!mark)
        return 3; /* not running under the recorder */
    kv_options o = workload_opts(pol);
    kv_t *db;
    if (kv_open(dir, &o, &db) != KV_OK)
        return 4;
    uint8_t v[128];
    char k[16];
    for (int i = 0; i < nops; i++) {
        mark(MARK_BEGIN | (uint64_t)i);
        key_name(ops[i].key, k);
        int rc = KV_OK;
        switch (ops[i].kind) {
        case PUT:
            value_of(i, v, ops[i].len);
            rc = kv_put(db, k, strlen(k), v, ops[i].len);
            break;
        case DEL:
            rc = kv_delete(db, k, strlen(k));
            if (rc == KV_NOT_FOUND)
                rc = KV_OK;
            break;
        case COMPACT:
            rc = kv_compact(db);
            break;
        case REOPEN:
            rc = kv_close(db);
            if (rc == KV_OK)
                rc = kv_open(dir, &o, &db);
            break;
        case SYNC:
            rc = kv_sync(db);
            break;
        }
        if (rc != KV_OK)
            return 5;
        mark(MARK_ACK | (uint64_t)i);
    }
    return kv_close(db) == KV_OK ? 0 : 6;
}

/* ---------- the log ---------- */

typedef struct {
    int type;
    uint64_t ino, off, len, val;
    const uint8_t *data;
    char name[64], name2[64];
} rec;

static rec *recs;
static size_t nrecs;

static uint64_t rd64(const uint8_t **p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | (*p)[i];
    *p += 8;
    return v;
}

static void rd_name(const uint8_t **p, char *out)
{
    uint64_t n = rd64(p);
    CHECK(n < 64);
    memcpy(out, *p, n);
    out[n] = '\0';
    *p += n;
}

static uint8_t *load_log(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    off_t n = lseek(fd, 0, SEEK_END);
    uint8_t *b = malloc((size_t)n + 1);
    CHECK(pread(fd, b, (size_t)n, 0) == n);
    close(fd);
    *size = (size_t)n;
    const uint8_t *p = b, *end = b + n;
    size_t cap = 1024;
    recs = malloc(cap * sizeof *recs);
    nrecs = 0;
    while (p < end) {
        if (nrecs == cap)
            recs = realloc(recs, (cap *= 2) * sizeof *recs);
        rec *r = &recs[nrecs++];
        memset(r, 0, sizeof *r);
        r->type = *p++;
        switch (r->type) {
        case PF_CREATE:
            rd_name(&p, r->name);
            r->ino = rd64(&p);
            break;
        case PF_WRITE:
            r->ino = rd64(&p);
            r->off = rd64(&p);
            r->len = rd64(&p);
            r->data = p;
            p += r->len;
            break;
        case PF_TRUNC:
            r->ino = rd64(&p);
            r->len = rd64(&p);
            break;
        case PF_FSYNC:
            r->ino = rd64(&p);
            break;
        case PF_RENAME:
            rd_name(&p, r->name);
            rd_name(&p, r->name2);
            break;
        case PF_UNLINK:
            rd_name(&p, r->name);
            break;
        case PF_MARK:
            r->val = rd64(&p);
            break;
        default:
            CHECK(!"corrupt recorder log");
        }
    }
    return b;
}

/* ---------- a model of the file system ---------- */

enum { MAXFILES = 4096, MAXNAMES = 256 };

typedef struct {
    uint8_t *d;
    size_t len, cap;
} bytes;

static void b_write(bytes *b, uint64_t off, const uint8_t *p, size_t n)
{
    size_t end = (size_t)off + n;
    if (end > b->cap) {
        b->cap = end * 2 + 256;
        b->d = realloc(b->d, b->cap);
    }
    if (off > b->len)
        memset(b->d + b->len, 0, (size_t)off - b->len);
    memcpy(b->d + off, p, n);
    if (end > b->len)
        b->len = end;
}

static void b_trunc(bytes *b, uint64_t size)
{
    if (size > b->len) {
        if (size > b->cap) {
            b->cap = (size_t)size * 2 + 256;
            b->d = realloc(b->d, b->cap);
        }
        memset(b->d + b->len, 0, (size_t)size - b->len);
    }
    b->len = (size_t)size;
}

static void b_copy(bytes *dst, const bytes *src)
{
    b_trunc(dst, 0);
    if (src->len)
        b_write(dst, 0, src->d, src->len);
}

typedef struct {
    uint64_t ino;    /* real inode number while it is current */
    bytes vol, dur;  /* contents now, and as of the last fsync */
    size_t *pending; /* records since the last fsync */
    size_t npend, pend_cap;
} mfile;

typedef struct {
    char name[64];
    int file;
} dentry;

typedef struct {
    dentry e[MAXNAMES];
    int n;
} dirmap;

static mfile files[MAXFILES];
static int nfiles;
static dirmap vol_dir, dur_dir;
static size_t *dir_pending; /* directory records since the last dir fsync */
static size_t ndir_pending, dir_pend_cap;
static uint64_t dir_ino;

static int file_of_ino(uint64_t ino)
{
    for (int i = nfiles - 1; i >= 0; i--)
        if (files[i].ino == ino)
            return i;
    return -1;
}

static int dir_find(const dirmap *d, const char *name)
{
    for (int i = 0; i < d->n; i++)
        if (strcmp(d->e[i].name, name) == 0)
            return i;
    return -1;
}

static void dir_set(dirmap *d, const char *name, int file)
{
    int i = dir_find(d, name);
    if (i < 0) {
        CHECK(d->n < MAXNAMES);
        i = d->n++;
        snprintf(d->e[i].name, sizeof d->e[i].name, "%s", name);
    }
    d->e[i].file = file;
}

static void dir_del(dirmap *d, const char *name)
{
    int i = dir_find(d, name);
    if (i >= 0)
        d->e[i] = d->e[--d->n];
}

static void dir_apply(dirmap *d, const rec *r)
{
    if (r->type == PF_CREATE) {
        dir_set(d, r->name, file_of_ino(r->ino));
    } else if (r->type == PF_RENAME) {
        int i = dir_find(d, r->name);
        if (i >= 0) {
            int f = d->e[i].file;
            dir_del(d, r->name);
            dir_set(d, r->name2, f);
        }
    } else if (r->type == PF_UNLINK) {
        dir_del(d, r->name);
    }
}

static void push(size_t **v, size_t *n, size_t *cap, size_t x)
{
    if (*n == *cap)
        *v = realloc(*v, (*cap = *cap ? *cap * 2 : 16) * sizeof **v);
    (*v)[(*n)++] = x;
}

static void model_step(size_t i)
{
    const rec *r = &recs[i];
    int f;
    switch (r->type) {
    case PF_CREATE:
        CHECK(nfiles < MAXFILES);
        f = nfiles++;
        memset(&files[f], 0, sizeof files[f]);
        files[f].ino = r->ino;
        dir_apply(&vol_dir, r);
        push(&dir_pending, &ndir_pending, &dir_pend_cap, i);
        break;
    case PF_WRITE:
    case PF_TRUNC:
        if ((f = file_of_ino(r->ino)) < 0)
            break;
        if (r->type == PF_WRITE)
            b_write(&files[f].vol, r->off, r->data, (size_t)r->len);
        else
            b_trunc(&files[f].vol, r->len);
        push(&files[f].pending, &files[f].npend, &files[f].pend_cap, i);
        break;
    case PF_FSYNC:
        if (r->ino == dir_ino) {
            dur_dir = vol_dir;
            ndir_pending = 0;
        } else if ((f = file_of_ino(r->ino)) >= 0) {
            b_copy(&files[f].dur, &files[f].vol);
            files[f].npend = 0;
        }
        break;
    case PF_RENAME:
    case PF_UNLINK:
        dir_apply(&vol_dir, r);
        push(&dir_pending, &ndir_pending, &dir_pend_cap, i);
        break;
    }
}

enum { IMG_DURABLE, IMG_TORN, IMG_REORDER, IMG_CACHED, NIMG };

/* Materialize one crash image into `out`. */
static void build_image(int kind, const char *out)
{
    dirmap d = kind == IMG_CACHED ? vol_dir : dur_dir;
    if (kind == IMG_TORN) /* a prefix of the unsynced directory changes */
        for (size_t i = 0, keep = (size_t)(rng() % (ndir_pending + 1));
             i < keep; i++)
            dir_apply(&d, &recs[dir_pending[i]]);
    if (kind == IMG_REORDER) /* any subset of them */
        for (size_t i = 0; i < ndir_pending; i++)
            if (rng() & 1)
                dir_apply(&d, &recs[dir_pending[i]]);
    bytes img = {0};
    for (int i = 0; i < d.n; i++) {
        int f = d.e[i].file;
        if (f < 0)
            continue;
        mfile *m = &files[f];
        if (kind == IMG_CACHED) {
            b_copy(&img, &m->vol);
        } else {
            b_copy(&img, &m->dur);
            int torn = kind == IMG_TORN || kind == IMG_REORDER;
            for (size_t p = 0; torn && p < m->npend; p++) {
                const rec *r = &recs[m->pending[p]];
                if (r->type == PF_TRUNC) {
                    if (rng() & 1)
                        b_trunc(&img, r->len);
                    continue;
                }
                /* each 512-byte sector independently reached the disk */
                for (uint64_t s = r->off & ~511ull; s < r->off + r->len;
                     s += 512) {
                    uint64_t a = s < r->off ? r->off : s;
                    uint64_t e = s + 512 < r->off + r->len ? s + 512
                                                           : r->off + r->len;
                    if (rng() & 1)
                        b_write(&img, a, r->data + (a - r->off), (size_t)(e - a));
                }
            }
        }
        char p[512];
        snprintf(p, sizeof p, "%s/%s", out, d.e[i].name);
        int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0);
        CHECK(img.len == 0 || write(fd, img.d, img.len) == (ssize_t)img.len);
        close(fd);
    }
    free(img.d);
}

/* ---------- the oracle ---------- */

typedef struct {
    int present;
    int writer; /* op index whose value it holds */
    size_t len;
} kstate;

/* states[k + 1] = every key's state after ops 0..k (states[0]: empty) */
static kstate (*states)[NKEYS];

static void build_states(void)
{
    free(states);
    states = calloc((size_t)nops + 1, sizeof *states);
    for (int i = 0; i < nops; i++) {
        memcpy(states[i + 1], states[i], sizeof states[i]);
        if (ops[i].kind == PUT)
            states[i + 1][ops[i].key] = (kstate){1, i, ops[i].len};
        else if (ops[i].kind == DEL)
            states[i + 1][ops[i].key].present = 0;
    }
}

typedef struct {
    int rc;
    size_t len;
    uint8_t *val;
} got;

static int matches(const kstate *s, const got *g)
{
    if (!s->present)
        return g->rc == KV_NOT_FOUND;
    if (g->rc != KV_OK || g->len != s->len)
        return 0;
    uint8_t want[128];
    value_of(s->writer, want, s->len);
    return memcmp(g->val, want, g->len) == 0;
}

/* Recover the image; its contents must equal the state after some prefix of
 * the ops: at least ops 0..durable, at most ops 0..latest. */
static int check_image(const char *img, int durable, int latest,
                       const char *what)
{
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    kv_t *db;
    int rc = kv_open(img, &o, &db);
    if (rc != KV_OK) {
        fprintf(stderr, "\n%s: open failed: %s\n", what, kv_strerror(rc));
        return 0;
    }
    got g[NKEYS];
    for (int k = 0; k < NKEYS; k++) {
        char key[16];
        key_name(k, key);
        void *v = NULL;
        g[k].rc = kv_get(db, key, strlen(key), &v, &g[k].len);
        g[k].val = v;
    }
    kv_close(db);
    int ok = 0;
    for (int p = durable; p <= latest && !ok; p++) {
        ok = 1;
        for (int k = 0; k < NKEYS && ok; k++)
            ok = matches(&states[p + 1][k], &g[k]);
    }
    if (!ok) {
        fprintf(stderr,
                "\n%s: recovered state matches no prefix of ops %d..%d:\n",
                what, durable, latest);
        for (int k = 0; k < NKEYS; k++)
            fprintf(stderr, "  key %2d: rc=%d len=%zu; after op %d: %s (op %d)\n",
                    k, g[k].rc, g[k].len, durable,
                    states[durable + 1][k].present ? "present" : "absent",
                    states[durable + 1][k].writer);
    }
    for (int k = 0; k < NKEYS; k++)
        free(g[k].val);
    return ok;
}

static void model_reset(void)
{
    for (int f = 0; f < nfiles; f++) {
        free(files[f].vol.d);
        free(files[f].dur.d);
        free(files[f].pending);
    }
    memset(files, 0, sizeof files);
    nfiles = 0;
    memset(&vol_dir, 0, sizeof vol_dir);
    memset(&dur_dir, 0, sizeof dur_dir);
    ndir_pending = 0;
}

#if !defined(SANITIZED) && defined(__linux__)
/* Record one workload run and check every crash image of it. */
static int run_phase(kv_sync_policy pol, uint64_t seed, int nop,
                     const char *self, const char *shim)
{
    gen_ops(seed, nop);
    build_states();
    model_reset();
    char base[256], dir[512], log[512];
    tu_tmpdir(base);
    snprintf(dir, sizeof dir, "%s/store", base);
    snprintf(log, sizeof log, "%s/ops.log", base);
    CHECK(mkdir(dir, 0755) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        char seedarg[32];
        snprintf(seedarg, sizeof seedarg, "%llu", (unsigned long long)seed);
        setenv("LD_PRELOAD", shim, 1);
        setenv("PF_DIR", dir, 1);
        setenv("PF_LOG", log, 1);
        execl(self, self, "--workload", dir,
              pol == KV_SYNC_ALWAYS ? "always" : "none", seedarg, (char *)NULL);
        _exit(127);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "workload failed (status %d)\n", status);
        return 0;
    }
    size_t logsize;
    uint8_t *logbuf = load_log(log, &logsize);
    struct stat st;
    CHECK(stat(dir, &st) == 0);
    dir_ino = (uint64_t)st.st_ino;

    /* last op known durable, last op acknowledged, last op started */
    int durable = -1, acked = -1, latest = -1;
    long images = 0, fsyncs = 0, renames = 0;
    rng_state = seed ^ 0x5bd1e995;
    static const char *kind_name[] = {"durable", "torn", "reorder", "cached"};
    for (size_t i = 0; i <= nrecs; i++) {
        /* a power cut right here, before record i */
        for (int kind = 0; kind < NIMG; kind++) {
            char img[256], what[200];
            tu_tmpdir(img);
            build_image(kind, img);
            snprintf(what, sizeof what,
                     "%s, cut before record %zu/%zu, %s image",
                     pol == KV_SYNC_ALWAYS ? "fsync always" : "no fsync", i,
                     nrecs, kind_name[kind]);
            /* a process crash (cached image) loses nothing that was
             * written, so everything acknowledged must be there */
            int ok = check_image(img, kind == IMG_CACHED ? acked : durable,
                                 latest, what);
            tu_rmrf(img);
            if (!ok)
                return 0;
            images++;
        }
        if (i == nrecs)
            break;
        model_step(i);
        const rec *r = &recs[i];
        fsyncs += r->type == PF_FSYNC;
        renames += r->type == PF_RENAME;
        if (r->type == PF_MARK) {
            int which = (int)(r->val & 0xFFFFFFFFull);
            if ((r->val & ~0xFFFFFFFFull) == MARK_BEGIN) {
                latest = which;
            } else {
                CHECK(which == latest);
                acked = which;
                if (durable_after(pol, which))
                    durable = which;
            }
        }
    }
    printf("  %-13s %ld crash images ok (%zu cut points x %d models; %ld "
           "fsyncs, %ld renames)\n",
           pol == KV_SYNC_ALWAYS ? "fsync always:" : "no fsync:", images,
           nrecs + 1, NIMG, fsyncs, renames);
    free(logbuf);
    free(recs);
    tu_rmrf(dir);
    unlink(log);
    rmdir(base);
    return 1;
}
#endif

int main(int argc, char **argv)
{
    const char *s = getenv("PF_SEED"), *n = getenv("PF_OPS");
    uint64_t seed = s ? strtoull(s, NULL, 0) : 0xF00D;
    int nop = n ? atoi(n) : 300;
    if (argc == 5 && strcmp(argv[1], "--workload") == 0) {
        gen_ops(strtoull(argv[4], NULL, 10), nop);
        return run_workload(argv[2], strcmp(argv[3], "always") == 0
                                         ? KV_SYNC_ALWAYS : KV_SYNC_NONE);
    }
#if defined(SANITIZED) || !defined(__linux__)
    (void)argv;
    printf("test_powerfail: skipped (needs Linux and an uninstrumented build)\n");
    return 0;
#else
    /* the recorder sits next to this binary */
    char self[512], shim[600];
    ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
    CHECK(sl > 0);
    self[sl] = '\0';
    snprintf(shim, sizeof shim, "%.*s/pf_shim.so",
             (int)(strrchr(self, '/') - self), self);
    CHECK(access(shim, R_OK) == 0);
    printf("test_powerfail: seed=%llu, %d ops per phase\n",
           (unsigned long long)seed, nop);
    if (!run_phase(KV_SYNC_ALWAYS, seed, nop, self, shim) ||
        !run_phase(KV_SYNC_NONE, seed + 1, nop, self, shim))
        return 1;
    model_reset();
    free(states);
    free(ops);
    printf("test_powerfail: passed\n");
    return 0;
#endif
}
