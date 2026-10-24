/*
 * libFuzzer target: recovery from arbitrary directory contents.
 *
 * The input is decoded into records (valid or with a deliberately wrong
 * checksum, tombstones, raw garbage) spread over a few log segments and a
 * compaction output with a hint file that may itself be mangled, plus an
 * optional MANIFEST. Since the harness fixes up checksums on request, the
 * fuzzer reaches replay and hint logic instead of bouncing off CRC checks.
 *
 * Whatever recovery decides the store contains, it has to stay consistent:
 * every get answers OK, NOT_FOUND or (for hint-loaded data that was never
 * checksummed) CORRUPT, a scrub finds damage exactly when compaction does,
 * and a successful compaction followed by reopening, with and without
 * hints, must give exactly the same answers.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crc32.h"
#include "fuzz_util.h"
#include "record.h"

enum { NSEG = 4, MAXKEYS = 64 };

/* replay order: log 1, its compaction output 1-1, then logs 2 and 3 */
static const char *seg_names[NSEG] = {
    "0000000001-000000", "0000000001-000001", "0000000002-000000",
    "0000000003-000000"};

typedef struct {
    const uint8_t *p;
    size_t n;
} input;

static int next(input *in)
{
    if (!in->n)
        return -1;
    in->n--;
    return *in->p++;
}

typedef struct {
    uint8_t *b;
    size_t len, cap;
} buf;

static void put_bytes(buf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->b = realloc(b->b, b->cap);
        if (!b->b)
            abort();
    }
    memcpy(b->b + b->len, p, n);
    b->len += n;
}

typedef struct {
    uint8_t k[32];
    uint32_t n;
} key;

static key keys[MAXKEYS];
static int nkeys;

static void remember(const uint8_t *k, uint32_t n)
{
    for (int i = 0; i < nkeys; i++)
        if (keys[i].n == n && memcmp(keys[i].k, k, n) == 0)
            return;
    if (nkeys < MAXKEYS) {
        memcpy(keys[nkeys].k, k, n);
        keys[nkeys++].n = n;
    }
}

static void write_file(const char *dir, const char *name, const buf *b)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || (b->len && write(fd, b->b, b->len) != (ssize_t)b->len))
        abort();
    close(fd);
}

typedef struct {
    int rc;
    size_t len;
    uint8_t *val;
} answer;

static void ask_all(kv_t *db, answer *a)
{
    for (int i = 0; i < nkeys; i++) {
        void *v;
        size_t n;
        a[i].rc = kv_get(db, keys[i].k, keys[i].n, &v, &n);
        if (a[i].rc != KV_OK && a[i].rc != KV_NOT_FOUND &&
            a[i].rc != KV_ERR_CORRUPT)
            abort();
        a[i].len = a[i].rc == KV_OK ? n : 0;
        a[i].val = a[i].rc == KV_OK ? v : NULL;
    }
}

static void same_answers(const answer *x, const answer *y)
{
    for (int i = 0; i < nkeys; i++) {
        if (x[i].rc != y[i].rc || x[i].len != y[i].len ||
            (x[i].len && memcmp(x[i].val, y[i].val, x[i].len) != 0))
            abort();
    }
}

static void free_answers(answer *a)
{
    for (int i = 0; i < nkeys; i++)
        free(a[i].val);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    input in = {data, size};
    int flags = next(&in);
    if (flags < 0)
        return 0;
    buf seg[NSEG] = {{0}}, hint = {0};
    nkeys = 0;

    /* records */
    int op;
    while ((op = next(&in)) >= 0) {
        buf *s = &seg[op & 3];
        int kind = (op >> 2) & 3; /* 0,1 put, 2 tombstone, 3 garbage */
        if (kind == 2 && (op & 3) == 1)
            kind = 0; /* compaction never writes tombstones into outputs */
        if (kind == 3) {
            int n = next(&in);
            for (int i = 0; i < (n < 0 ? 0 : n % 24); i++) {
                int c = next(&in);
                uint8_t byte = (uint8_t)(c < 0 ? 0 : c);
                put_bytes(s, &byte, 1);
            }
            continue;
        }
        int ksel = next(&in), vlen = next(&in);
        if (ksel < 0 || vlen < 0)
            break;
        uint8_t k[32];
        uint32_t klen = (uint32_t)(ksel & 7) == 7 ? (uint32_t)(ksel >> 3) % 32
                                                   : 2;
        for (uint32_t i = 0; i < klen; i++)
            k[i] = klen == 2 ? (uint8_t)(i ? '0' + (ksel & 7) : 'k')
                             : (uint8_t)next(&in);
        uint32_t vsz = kind == 2 ? REC_TOMBSTONE : (uint32_t)vlen;
        uint64_t rsz = record_size(klen, vsz);
        uint8_t *rec = malloc(rsz);
        uint8_t val[256];
        for (int i = 0; i < (kind == 2 ? 0 : vlen); i++) {
            int c = next(&in);
            val[i] = (uint8_t)(c < 0 ? i : c);
        }
        record_encode(rec, 1000 + seg[0].len, k, klen, val, vsz);
        if (op & 0x80) /* deliberately damage it */
            rec[rsz - 1] ^= 0x5a;
        put_bytes(s, rec, rsz);
        free(rec);
        remember(k, klen);
    }

    /* a hint file for the compaction output, tiling its records */
    for (size_t off = 0; off + REC_HDR_SIZE <= seg[1].len;) {
        uint32_t ksz = get_u32(seg[1].b + off + 12);
        uint32_t vsz = get_u32(seg[1].b + off + 16);
        if (ksz > 32 || (vsz != REC_TOMBSTONE && vsz > 255))
            break;
        uint64_t rsz = record_size(ksz, vsz);
        if (off + rsz > seg[1].len)
            break;
        uint8_t h[HINT_HDR_SIZE + 32];
        hint_encode_header(h, get_u64(seg[1].b + off + 4),
                           seg[1].b + off + REC_HDR_SIZE, ksz, off,
                           (uint32_t)rsz);
        memcpy(h + HINT_HDR_SIZE, seg[1].b + off + REC_HDR_SIZE, ksz);
        put_bytes(&hint, h, HINT_HDR_SIZE + ksz);
        off += rsz;
    }
    if ((flags & 0x10) && hint.len) /* mangle one byte of it */
        hint.b[(size_t)(flags * 7919) % hint.len] ^= (uint8_t)(1 + (flags >> 5));

    char dir[256];
    fuzz_tmpdir(dir);
    for (int i = 0; i < NSEG; i++) {
        char name[64];
        snprintf(name, sizeof name, "%s.seg", seg_names[i]);
        if (seg[i].len || (flags & (1 << i)))
            write_file(dir, name, &seg[i]);
    }
    if (hint.len && !(flags & 0x20)) {
        char name[64];
        snprintf(name, sizeof name, "%s.hint", seg_names[1]);
        write_file(dir, name, &hint);
    }
    if (flags & 0x40) { /* a MANIFEST that retires log 1 (and maybe 1-1) */
        uint8_t m[20];
        put_u32(m, 0x464D564Bu);
        put_u32(m + 4, 2);
        put_u64(m + 8, flags & 0x80 ? 2000000ull : 1000001ull);
        put_u32(m + 16, crc32_update(0, m, 16));
        buf mb = {m, 20, 20};
        write_file(dir, "MANIFEST", &mb);
    }

    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 512;
    kv_t *db;
    if (kv_open(dir, &o, &db) != KV_OK)
        abort(); /* damaged content must never make the store unopenable */
    answer before[MAXKEYS], after[MAXKEYS];
    ask_all(db, before);

    /* A scrub reads what compaction is about to, every record of every
     * sealed segment (replay already cut the active one back to its last
     * good record), so the two must agree about damage. */
    int scrub = kv_scrub(db);
    if (scrub != KV_OK && scrub != KV_ERR_CORRUPT)
        abort();
    int rc = kv_compact(db);
    if (rc != KV_OK && rc != KV_ERR_CORRUPT)
        abort();
    if ((scrub == KV_ERR_CORRUPT) != (rc == KV_ERR_CORRUPT))
        abort();
    if (rc == KV_OK) {
        ask_all(db, after);
        same_answers(before, after);
        free_answers(after);
        if (kv_close(db) != KV_OK)
            abort();
        for (int hints = 1; hints >= 0; hints--) {
            o.use_hints = hints;
            if (kv_open(dir, &o, &db) != KV_OK)
                abort();
            ask_all(db, after);
            same_answers(before, after);
            free_answers(after);
            if (kv_close(db) != KV_OK)
                abort();
        }
    } else {
        kv_close(db);
    }
    free_answers(before);
    fuzz_rmrf(dir);
    for (int i = 0; i < NSEG; i++)
        free(seg[i].b);
    free(hint.b);
    return 0;
}
