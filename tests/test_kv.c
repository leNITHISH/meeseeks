/* Functional tests for the public API. */
#include <sys/wait.h>

#include "crc32.h"
#include "index.h"
#include "test_util.h"

static kv_options small_opts(uint64_t segment_size)
{
    kv_options o;
    kv_default_options(&o);
    o.segment_size = segment_size;
    o.sync_policy = KV_SYNC_NONE;
    return o;
}

static void put_s(kv_t *db, const char *k, const char *v)
{
    CHECK_RC(kv_put(db, k, strlen(k), v, strlen(v)), KV_OK);
}

/* Assert that key k maps to v (v == NULL: key must be absent). */
static void expect(kv_t *db, const char *k, const char *v)
{
    void *got;
    size_t n;
    int rc = kv_get(db, k, strlen(k), &got, &n);
    if (!v) {
        if (rc != KV_NOT_FOUND) {
            fprintf(stderr, "key '%s': want absent, got rc=%d\n", k, rc);
            exit(1);
        }
        return;
    }
    if (rc != KV_OK || n != strlen(v) || memcmp(got, v, n) != 0) {
        fprintf(stderr, "key '%s': want '%s', got rc=%d '%.*s'\n", k, v, rc,
                rc == KV_OK ? (int)n : 0, rc == KV_OK ? (char *)got : "");
        exit(1);
    }
    free(got);
}

static kv_t *open_db(const char *dir, const kv_options *o)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, o, &db), KV_OK);
    return db;
}

/* ---------- checksum ---------- */

static uint32_t crc_reference(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return ~c;
}

static void test_crc32(const char *dir)
{
    (void)dir;
    /* the standard check value of CRC-32/ISO-HDLC (zlib, PNG, Ethernet) */
    CHECK(crc32_update(0, "123456789", 9) == 0xCBF43926u);
    CHECK(crc32_update(0, "", 0) == 0);
    uint8_t buf[1000];
    uint64_t x = 88172645463325252ull;
    for (size_t i = 0; i < sizeof buf; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        buf[i] = (uint8_t)x;
    }
    /* every length and alignment against the bit-at-a-time definition */
    for (size_t off = 0; off < 8; off++)
        for (size_t n = 0; n + off <= 300; n++)
            CHECK(crc32_update(0, buf + off, n) == crc_reference(buf + off, n));
    /* chaining two calls equals one call over the concatenation */
    for (size_t cut = 0; cut <= sizeof buf; cut += 37)
        CHECK(crc32_update(crc32_update(0, buf, cut), buf + cut,
                           sizeof buf - cut) == crc_reference(buf, sizeof buf));
}

static void test_siphash(const char *dir)
{
    (void)dir;
    /* reference vectors for SipHash-2-4, key 00 01 .. 0f, message 00 01 .. */
    uint8_t msg[64];
    for (int i = 0; i < 64; i++)
        msg[i] = (uint8_t)i;
    uint64_t k0 = 0x0706050403020100ull, k1 = 0x0f0e0d0c0b0a0908ull;
    CHECK(siphash(msg, 0, k0, k1, 2, 4) == 0x726fdb47dd0e0e31ull);
    CHECK(siphash(msg, 1, k0, k1, 2, 4) == 0x74f839c593dc67fdull);
    CHECK(siphash(msg, 15, k0, k1, 2, 4) == 0xa129ca6149be45e5ull);

    /* the index's unrolled SipHash-1-3 agrees with the generic one: a
     * lookup with the hash it would compute must land on the right key */
    kv_index idx;
    CHECK(index_init(&idx, 16) == 0);
    for (size_t len = 0; len <= 40; len++) {
        int existed;
        kv_loc *l = index_upsert(&idx, msg, len, &existed);
        CHECK(l && !existed);
        l->offset = len;
        CHECK(idx.slots != NULL);
        uint32_t h = (uint32_t)siphash(msg, len, idx.k0, idx.k1, 1, 3);
        int found = 0;
        for (size_t i = 0; i < idx.cap; i++)
            if (idx.slots[i] && (uint32_t)(idx.slots[i] >> 32) == h)
                found = 1;
        CHECK(found);
    }
    for (size_t len = 0; len <= 40; len++) {
        kv_loc *l = index_get(&idx, msg, len);
        CHECK(l && l->offset == len);
    }
    index_free(&idx);
}

/* ---------- basic operations ---------- */

static void test_put_get(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    expect(db, "missing", NULL);
    put_s(db, "a", "1");
    put_s(db, "b", "2");
    expect(db, "a", "1");
    expect(db, "b", "2");
    put_s(db, "a", "one");
    expect(db, "a", "one");
    CHECK(kv_count(db) == 2);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_delete(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    put_s(db, "k", "v");
    CHECK_RC(kv_delete(db, "k", 1), KV_OK);
    expect(db, "k", NULL);
    CHECK_RC(kv_delete(db, "k", 1), KV_NOT_FOUND);
    CHECK_RC(kv_delete(db, "never", 5), KV_NOT_FOUND);
    put_s(db, "k", "again");
    expect(db, "k", "again");
    CHECK(kv_count(db) == 1);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_binary_and_empty(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    const uint8_t key[] = {0, 1, 2, 0, 255};
    const uint8_t val[] = {0, 0, 7, 0, 0, 9};
    CHECK_RC(kv_put(db, key, sizeof key, val, sizeof val), KV_OK);
    CHECK_RC(kv_put(db, NULL, 0, "empty key", 9), KV_OK); /* NULL, 0 = "" */
    CHECK_RC(kv_put(db, "nullval", 7, NULL, 0), KV_OK);
    CHECK_RC(kv_put(db, "empty value", 11, "", 0), KV_OK);

    for (int pass = 0; pass < 2; pass++) {
        void *got;
        size_t n;
        CHECK_RC(kv_get(db, key, sizeof key, &got, &n), KV_OK);
        CHECK(n == sizeof val && memcmp(got, val, n) == 0);
        free(got);
        expect(db, "", "empty key");
        expect(db, "nullval", "");
        CHECK_RC(kv_get(db, NULL, 0, &got, &n), KV_OK);
        CHECK(n == 9 && memcmp(got, "empty key", 9) == 0);
        free(got);
        CHECK_RC(kv_get(db, "empty value", 11, &got, &n), KV_OK);
        CHECK(n == 0 && ((char *)got)[0] == '\0');
        free(got);
        CHECK_RC(kv_close(db), KV_OK);
        db = open_db(dir, NULL); /* everything must survive a reopen */
    }
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_invalid_args(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    char *big = calloc(1, KV_MAX_KEY_SIZE + 1);
    CHECK(big);
    CHECK_RC(kv_put(db, big, KV_MAX_KEY_SIZE + 1, "v", 1), KV_ERR_INVALID);
    CHECK_RC(kv_put(db, NULL, 3, "v", 1), KV_ERR_INVALID);
    CHECK_RC(kv_delete(db, big, KV_MAX_KEY_SIZE + 1), KV_ERR_INVALID);
    free(big);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_open(NULL, NULL, &db), KV_ERR_INVALID);
}

static void test_reopen_persistence(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    char k[32], v[64];
    for (int i = 0; i < 5000; i++) {
        snprintf(k, sizeof k, "key-%d", i);
        snprintf(v, sizeof v, "value-%d", i * 7);
        put_s(db, k, v);
    }
    for (int i = 0; i < 5000; i += 3) {
        snprintf(k, sizeof k, "key-%d", i);
        CHECK_RC(kv_delete(db, k, strlen(k)), KV_OK);
    }
    CHECK_RC(kv_close(db), KV_OK);

    db = open_db(dir, NULL);
    for (int i = 0; i < 5000; i++) {
        snprintf(k, sizeof k, "key-%d", i);
        snprintf(v, sizeof v, "value-%d", i * 7);
        expect(db, k, i % 3 == 0 ? NULL : v);
    }
    CHECK(kv_count(db) == 5000 - 1667);
    CHECK_RC(kv_close(db), KV_OK);
}

/* A delete must outlive the original put across restarts: the tombstone is
 * the only thing standing between the old value and resurrection. */
static void test_tombstone_survives_restart(const char *dir)
{
    kv_options o = small_opts(256);
    kv_t *db = open_db(dir, &o);
    put_s(db, "ghost", "boo");
    for (int i = 0; i < 20; i++) /* push the put into an older segment */
        put_s(db, "filler", "xxxxxxxxxxxxxxxxxxxxxxxx");
    CHECK_RC(kv_delete(db, "ghost", 5), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK(tu_count_files(dir, ".seg") > 1);

    db = open_db(dir, &o);
    expect(db, "ghost", NULL);
    CHECK_RC(kv_close(db), KV_OK);
}

/* ---------- segments ---------- */

static void test_segment_rollover(const char *dir)
{
    kv_options o = small_opts(4096);
    kv_t *db = open_db(dir, &o);
    char k[32], v[128];
    for (int i = 0; i < 2000; i++) {
        snprintf(k, sizeof k, "k%04d", i % 500);
        snprintf(v, sizeof v, "v%d-%0*d", i, 40, i);
        put_s(db, k, v);
    }
    int nseg = tu_count_files(dir, ".seg");
    CHECK(nseg > 20);

    /* no segment may exceed the limit (each record is far smaller) */
    DIR *d = opendir(dir);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!tu_has_suffix(e->d_name, ".seg"))
            continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        CHECK(tu_file_size(p) <= 4096);
    }
    closedir(d);

    /* a record bigger than a whole segment still works */
    char *huge = malloc(10000);
    memset(huge, 'h', 9999);
    huge[9999] = '\0';
    put_s(db, "huge", huge);
    expect(db, "huge", huge);
    CHECK_RC(kv_close(db), KV_OK);

    db = open_db(dir, &o);
    for (int i = 1500; i < 2000; i++) {
        snprintf(k, sizeof k, "k%04d", i % 500);
        snprintf(v, sizeof v, "v%d-%0*d", i, 40, i);
        expect(db, k, v);
    }
    expect(db, "huge", huge);
    free(huge);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_reopen_appends_to_last_segment(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    put_s(db, "a", "1");
    CHECK_RC(kv_close(db), KV_OK);
    for (int i = 0; i < 5; i++) {
        db = open_db(dir, NULL);
        put_s(db, "a", "2");
        CHECK_RC(kv_close(db), KV_OK);
    }
    CHECK(tu_count_files(dir, ".seg") == 1);
    /* opening and closing without writes must not leave empty segments */
    db = open_db(dir, NULL);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
    db = open_db(dir, NULL);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK(tu_count_files(dir, ".seg") == 1);
}

/* ---------- compaction ---------- */

static void fill_overwrites(kv_t *db, int nkeys, int rounds)
{
    char k[32], v[128];
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < nkeys; i++) {
            snprintf(k, sizeof k, "key-%05d", i);
            snprintf(v, sizeof v, "round-%d-value-%d-padding-padding", r, i);
            put_s(db, k, v);
        }
}

static void check_final_round(kv_t *db, int nkeys, int rounds, int deleted_mod)
{
    char k[32], v[128];
    for (int i = 0; i < nkeys; i++) {
        snprintf(k, sizeof k, "key-%05d", i);
        snprintf(v, sizeof v, "round-%d-value-%d-padding-padding", rounds - 1,
                 i);
        expect(db, k, deleted_mod && i % deleted_mod == 0 ? NULL : v);
    }
}

static void test_compaction(const char *dir)
{
    kv_options o = small_opts(16 * 1024);
    kv_t *db = open_db(dir, &o);
    fill_overwrites(db, 1000, 10);
    char k[32];
    for (int i = 0; i < 1000; i += 4) {
        snprintf(k, sizeof k, "key-%05d", i);
        CHECK_RC(kv_delete(db, k, strlen(k)), KV_OK);
    }
    uint64_t before = kv_disk_usage(db);
    CHECK_RC(kv_compact(db), KV_OK);
    uint64_t after = kv_disk_usage(db);
    CHECK(after < before / 5);
    check_final_round(db, 1000, 10, 4);
    CHECK(kv_count(db) == 750);
    CHECK(tu_count_files(dir, ".hint") >= 1);
    CHECK(tu_count_files(dir, ".tmp") == 0);
    CHECK(tu_file_exists(dir, "MANIFEST"));

    /* writes after compaction must win over the compacted copies */
    put_s(db, "key-00001", "fresh");
    CHECK_RC(kv_delete(db, "key-00002", 9), KV_OK);
    expect(db, "key-00001", "fresh");
    CHECK_RC(kv_close(db), KV_OK);

    for (int hints = 0; hints < 2; hints++) {
        o.use_hints = hints;
        db = open_db(dir, &o);
        expect(db, "key-00001", "fresh");
        expect(db, "key-00002", NULL);
        expect(db, "key-00000", NULL); /* deleted before compaction */
        expect(db, "key-00003", "round-9-value-3-padding-padding");
        CHECK(kv_count(db) == 749);
        CHECK_RC(kv_close(db), KV_OK);
    }
}

static void test_compaction_repeated(const char *dir)
{
    kv_options o = small_opts(8 * 1024);
    kv_t *db = open_db(dir, &o);
    for (int round = 0; round < 5; round++) {
        fill_overwrites(db, 300, 3);
        CHECK_RC(kv_compact(db), KV_OK);
        check_final_round(db, 300, 3, 0);
        CHECK_RC(kv_compact(db), KV_OK); /* compacting compacted data */
        check_final_round(db, 300, 3, 0);
    }
    CHECK_RC(kv_close(db), KV_OK);
    db = open_db(dir, &o);
    check_final_round(db, 300, 3, 0);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_compaction_everything_deleted(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    put_s(db, "a", "1");
    put_s(db, "b", "2");
    CHECK_RC(kv_delete(db, "a", 1), KV_OK);
    CHECK_RC(kv_delete(db, "b", 1), KV_OK);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK(kv_count(db) == 0);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK(tu_count_files(dir, ".seg") == 0);
    db = open_db(dir, NULL);
    expect(db, "a", NULL);
    expect(db, "b", NULL);
    put_s(db, "c", "3");
    CHECK_RC(kv_close(db), KV_OK);
    db = open_db(dir, NULL);
    expect(db, "c", "3");
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_compaction_empty_db(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_compact(db), KV_OK);
    put_s(db, "x", "y");
    CHECK_RC(kv_compact(db), KV_OK);
    expect(db, "x", "y");
    CHECK_RC(kv_close(db), KV_OK);
}

/* ---------- hint files ---------- */

static void first_file(const char *dir, const char *suffix, char *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    out[0] = '\0';
    while ((e = readdir(d)) != NULL)
        if (tu_has_suffix(e->d_name, suffix) &&
            (!out[0] || strcmp(e->d_name, out) < 0))
            strcpy(out, e->d_name);
    closedir(d);
    CHECK(out[0]);
}

static void build_compacted(const char *dir, const kv_options *o)
{
    kv_t *db = open_db(dir, o);
    fill_overwrites(db, 500, 4);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
}

/* Prove the hint path is really taken: damage a value inside a compacted
 * segment. Loading via hints never reads values, so the store opens with all
 * keys, and the damaged one is reported as corrupt rather than returned.
 * Without hints, replay notices the bad checksum and truncates instead. */
static void test_hint_path_is_used(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    build_compacted(dir, &o);
    char seg[64], p[512];
    first_file(dir, ".seg", seg);
    snprintf(p, sizeof p, "%s/%s", dir, seg);
    off_t size = tu_file_size(p);
    int fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    uint8_t b;
    off_t at = size - 3; /* inside the value of the last record */
    CHECK(pread(fd, &b, 1, at) == 1);
    b ^= 0x5A;
    CHECK(pwrite(fd, &b, 1, at) == 1);
    close(fd);

    kv_t *db = open_db(dir, &o);
    CHECK(kv_count(db) == 500);
    int corrupt = 0, ok = 0;
    char k[32];
    for (int i = 0; i < 500; i++) {
        void *v;
        size_t n;
        snprintf(k, sizeof k, "key-%05d", i);
        int rc = kv_get(db, k, strlen(k), &v, &n);
        if (rc == KV_OK) {
            ok++;
            free(v);
        } else {
            CHECK_RC(rc, KV_ERR_CORRUPT);
            corrupt++;
        }
    }
    CHECK(corrupt == 1 && ok == 499);
    CHECK_RC(kv_close(db), KV_OK);

    o.use_hints = 0;
    db = open_db(dir, &o);
    CHECK(kv_count(db) == 499); /* replay dropped the bad tail record */
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_hint_damage_falls_back(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    build_compacted(dir, &o);
    char hint[64], p[512];
    first_file(dir, ".hint", hint);
    snprintf(p, sizeof p, "%s/%s", dir, hint);
    off_t size = tu_file_size(p);

    /* bit flip, truncation, deletion: each must fall back to full replay */
    for (int mode = 0; mode < 3; mode++) {
        char work[256], wp[512];
        tu_tmpdir(work);
        tu_copy_dir(dir, work);
        snprintf(wp, sizeof wp, "%s/%s", work, hint);
        if (mode == 0) {
            int fd = open(wp, O_RDWR);
            uint8_t b;
            CHECK(pread(fd, &b, 1, size / 2) == 1);
            b ^= 1;
            CHECK(pwrite(fd, &b, 1, size / 2) == 1);
            close(fd);
        } else if (mode == 1) {
            CHECK(truncate(wp, size - 5) == 0);
        } else {
            CHECK(unlink(wp) == 0);
        }
        kv_t *db = open_db(work, &o);
        check_final_round(db, 500, 4, 0);
        CHECK(kv_count(db) == 500);
        CHECK_RC(kv_close(db), KV_OK);
        tu_rmrf(work);
    }
}

/* A hint that no longer tiles its (truncated) segment must be ignored. */
static void test_truncated_segment_ignores_hint(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    build_compacted(dir, &o);
    char seg[64], p[512];
    first_file(dir, ".seg", seg);
    snprintf(p, sizeof p, "%s/%s", dir, seg);
    CHECK(truncate(p, tu_file_size(p) - 1) == 0);
    kv_t *db = open_db(dir, &o);
    CHECK(kv_count(db) == 499);
    void *v;
    size_t n;
    for (int i = 0; i < 500; i++) {
        char k[32];
        snprintf(k, sizeof k, "key-%05d", i);
        int rc = kv_get(db, k, strlen(k), &v, &n);
        CHECK(rc == KV_OK || rc == KV_NOT_FOUND);
        if (rc == KV_OK)
            free(v);
    }
    CHECK_RC(kv_close(db), KV_OK);
    /* the stale hint was dropped once the segment was truncated */
    CHECK(tu_count_files(dir, ".hint") == 0);
}

/* ---------- crash points inside compaction ---------- */

static int is_seg_or_hint(const char *n)
{
    return tu_has_suffix(n, ".seg") || tu_has_suffix(n, ".hint");
}

/*
 * Rebuild the directory states a crash could leave behind during compaction,
 * from a "before" and "after" snapshot, and check each one recovers to the
 * same logical contents. Deleted keys are the interesting part: compaction
 * drops their tombstones, so any mix that resurrected them would be a bug.
 */
static void test_compaction_crash_states(const char *dir)
{
    kv_options o = small_opts(4096);
    kv_t *db = open_db(dir, &o);
    fill_overwrites(db, 200, 3);
    char k[32];
    for (int i = 0; i < 200; i += 2) {
        snprintf(k, sizeof k, "key-%05d", i);
        CHECK_RC(kv_delete(db, k, strlen(k)), KV_OK);
    }
    CHECK_RC(kv_close(db), KV_OK);

    char before[256], after[256];
    tu_tmpdir(before);
    tu_copy_dir(dir, before);
    db = open_db(dir, &o);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
    tu_tmpdir(after);
    tu_copy_dir(dir, after);

    for (int state = 0; state < 4; state++) {
        char work[256];
        tu_tmpdir(work);
        tu_copy_dir(before, work);
        if (state >= 1) /* outputs renamed into place, MANIFEST not written */
            tu_copy_dir_filtered(after, work, is_seg_or_hint);
        if (state >= 2) { /* MANIFEST committed, inputs not yet deleted */
            char a[512], b[512];
            snprintf(a, sizeof a, "%s/MANIFEST", after);
            snprintf(b, sizeof b, "%s/MANIFEST", work);
            tu_copy_file(a, b);
        }
        if (state == 3) { /* MANIFEST committed, some inputs deleted */
            char seg[64], p[512];
            first_file(work, ".seg", seg);
            snprintf(p, sizeof p, "%s/%s", work, seg);
            unlink(p);
        }
        /* a half-written temp file is always possible too */
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s/0000009999-000001.seg.tmp", work);
        int fd = open(tmp, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0 && write(fd, "junk", 4) == 4);
        close(fd);

        db = open_db(work, &o);
        check_final_round(db, 200, 3, 2);
        CHECK(kv_count(db) == 100);
        CHECK_RC(kv_close(db), KV_OK);
        CHECK(tu_count_files(work, ".tmp") == 0);
        tu_rmrf(work);
    }
    tu_rmrf(before);
    tu_rmrf(after);
}

static void test_garbage_files_ignored(const char *dir)
{
    kv_t *db = open_db(dir, NULL);
    put_s(db, "a", "1");
    CHECK_RC(kv_close(db), KV_OK);
    const char *names[] = {"notes.txt",         "12.seg",
                           "0000000001.seg",    "abcdefghij-000000.seg",
                           "0000000000-000000.seg", "0000000001-00000x.seg",
                           "0000000001-000000.seg.bak"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        int fd = open(p, O_WRONLY | O_CREAT, 0644);
        CHECK(fd >= 0 && write(fd, "garbage!", 8) == 8);
        close(fd);
    }
    db = open_db(dir, NULL);
    expect(db, "a", "1");
    CHECK(kv_count(db) == 1);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_sync_policies(const char *dir)
{
    kv_sync_policy pols[] = {KV_SYNC_ALWAYS, KV_SYNC_EVERY_N, KV_SYNC_NONE};
    for (int i = 0; i < 3; i++) {
        char sub[256];
        tu_tmpdir(sub);
        kv_options o;
        kv_default_options(&o);
        o.sync_policy = pols[i];
        o.sync_every_n = 7;
        kv_t *db = open_db(sub, &o);
        fill_overwrites(db, 20, 2);
        CHECK_RC(kv_sync(db), KV_OK);
        check_final_round(db, 20, 2, 0);
        CHECK_RC(kv_close(db), KV_OK);
        db = open_db(sub, &o);
        check_final_round(db, 20, 2, 0);
        CHECK_RC(kv_close(db), KV_OK);
        tu_rmrf(sub);
    }
    (void)dir;
}


/* ---------- locking, stats, background compaction ---------- */

static void test_lock_same_process(const char *dir)
{
    kv_t *a, *b;
    CHECK_RC(kv_open(dir, NULL, &a), KV_OK);
    CHECK_RC(kv_open(dir, NULL, &b), KV_ERR_LOCKED);
    char alias[512]; /* same directory through another path */
    snprintf(alias, sizeof alias, "%s/.", dir);
    CHECK_RC(kv_open(alias, NULL, &b), KV_ERR_LOCKED);
    put_s(a, "k", "v");
    CHECK_RC(kv_close(a), KV_OK);
    CHECK_RC(kv_open(alias, NULL, &b), KV_OK);
    expect(b, "k", "v");
    CHECK_RC(kv_close(b), KV_OK);
}

static int open_in_child(const char *dir)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        kv_t *db;
        int rc = kv_open(dir, NULL, &db);
        if (rc == KV_OK)
            kv_close(db);
        _exit(rc == KV_OK ? 0 : rc == KV_ERR_LOCKED ? 1 : 2);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

static void test_lock_other_process(const char *dir)
{
    kv_t *db;
    CHECK_RC(kv_open(dir, NULL, &db), KV_OK);
    CHECK(open_in_child(dir) == 1); /* locked while we hold it */
    CHECK_RC(kv_close(db), KV_OK);
    CHECK(open_in_child(dir) == 0); /* free once closed */
}

static void test_stats(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    kv_t *db = open_db(dir, &o);
    put_s(db, "a", "1234");     /* 20 + 1 + 4 = 25 bytes */
    put_s(db, "bb", "123456");  /* 28 */
    put_s(db, "a", "12345678"); /* 29, replaces the 25 */
    put_s(db, "ccc", "x");      /* 24 */
    CHECK_RC(kv_delete(db, "ccc", 3), KV_OK); /* tombstone: 23 */
    kv_stats st;
    kv_get_stats(db, &st);
    CHECK(st.keys == 2);
    CHECK(st.live_bytes == 29 + 28);
    CHECK(st.dead_bytes == 25 + 24 + 23);
    CHECK(st.puts == 4 && st.deletes == 1 && st.compactions == 0);
    CHECK(st.disk_bytes == 29 + 28 + 25 + 24 + 23);

    CHECK_RC(kv_compact(db), KV_OK);
    kv_get_stats(db, &st);
    CHECK(st.keys == 2 && st.live_bytes == 57 && st.dead_bytes == 0);
    CHECK(st.compactions == 1);
    CHECK(st.disk_bytes > 57); /* plus the hint file */
    CHECK_RC(kv_close(db), KV_OK);

    db = open_db(dir, &o); /* the numbers are rebuilt on recovery */
    kv_get_stats(db, &st);
    CHECK(st.keys == 2 && st.live_bytes == 57 && st.dead_bytes == 0);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_auto_compaction(const char *dir)
{
    kv_options o = small_opts(4096);
    o.auto_compact_ratio = 0.5;
    kv_t *db = open_db(dir, &o);
    kv_stats st;
    char k[16], v[64];
    int rounds = 0;
    do {
        for (int i = 0; i < 50; i++) {
            snprintf(k, sizeof k, "k%d", i);
            snprintf(v, sizeof v, "round %d of key %d, padded out a bit", rounds,
                     i);
            put_s(db, k, v);
        }
        rounds++;
        kv_get_stats(db, &st);
        if (st.compactions == 0)
            tu_sleep_us(1000);
    } while (st.compactions < 3 && rounds < 5000);
    CHECK(st.compactions >= 3);
    for (int i = 0; i < 50; i++) {
        snprintf(k, sizeof k, "k%d", i);
        snprintf(v, sizeof v, "round %d of key %d, padded out a bit",
                 rounds - 1, i);
        expect(db, k, v);
    }
    /* garbage stays bounded instead of growing with every round */
    CHECK(st.disk_bytes < 40u * 4096u);
    CHECK_RC(kv_close(db), KV_OK); /* joins the background thread */
    o.auto_compact_ratio = 0;
    db = open_db(dir, &o);
    CHECK(kv_count(db) == 50);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_bad_options(const char *dir)
{
    kv_options o;
    kv_t *db;
    kv_default_options(&o);
    o.auto_compact_ratio = 1.5;
    CHECK_RC(kv_open(dir, &o, &db), KV_ERR_INVALID);
    kv_default_options(&o);
    o.sync_policy = (kv_sync_policy)42;
    CHECK_RC(kv_open(dir, &o, &db), KV_ERR_INVALID);
}

/* ---------- edges found by coverage ---------- */

static void test_open_creates_directory(const char *dir)
{
    char sub[512], nested[600];
    snprintf(sub, sizeof sub, "%s/store", dir);
    snprintf(nested, sizeof nested, "%s/missing/store", dir);
    kv_t *db;
    CHECK_RC(kv_open(nested, NULL, &db), KV_ERR_IO); /* not recursive */
    CHECK_RC(kv_open(sub, NULL, &db), KV_OK);
    put_s(db, "k", "v");
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_open(sub, NULL, &db), KV_OK);
    expect(db, "k", "v");
    CHECK_RC(kv_close(db), KV_OK);
    tu_rmrf(sub);
}

/* A record bigger than compaction's 1 MiB read chunk takes its own path. */
static void test_compact_huge_record(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    kv_t *db = open_db(dir, &o);
    size_t big = 3u << 20;
    char *huge = malloc(big + 1);
    CHECK(huge);
    for (size_t i = 0; i < big; i++)
        huge[i] = (char)('a' + i % 23);
    huge[big] = '\0';
    fill_overwrites(db, 100, 3);
    put_s(db, "huge", huge);
    fill_overwrites(db, 100, 3); /* garbage after it too */
    CHECK_RC(kv_compact(db), KV_OK);
    expect(db, "huge", huge);
    check_final_round(db, 100, 3, 0);
    CHECK_RC(kv_close(db), KV_OK);
    for (int hints = 1; hints >= 0; hints--) {
        o.use_hints = hints;
        db = open_db(dir, &o);
        expect(db, "huge", huge);
        check_final_round(db, 100, 3, 0);
        CHECK_RC(kv_close(db), KV_OK);
    }
    free(huge);
}

/*
 * Segments loaded from hint files are not checksummed at startup, so damage
 * there first shows up later. Gets must report it rather than return it,
 * and compaction must refuse to copy it, leaving the store usable.
 */
static void test_compaction_refuses_corruption(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    build_compacted(dir, &o);
    char seg[64], p[512];
    first_file(dir, ".seg", seg);
    snprintf(p, sizeof p, "%s/%s", dir, seg);
    off_t size = tu_file_size(p);
    int fd = open(p, O_RDWR);
    uint8_t b;
    CHECK(fd >= 0 && pread(fd, &b, 1, size - 3) == 1);
    b ^= 0x40;
    CHECK(pwrite(fd, &b, 1, size - 3) == 1);
    close(fd);

    kv_t *db = open_db(dir, &o); /* via the hint: damage not seen yet */
    CHECK(kv_count(db) == 500);
    CHECK_RC(kv_compact(db), KV_ERR_CORRUPT);
    int corrupt = 0;
    for (int i = 0; i < 500; i++) {
        char k[32];
        void *v;
        size_t n;
        snprintf(k, sizeof k, "key-%05d", i);
        int rc = kv_get(db, k, strlen(k), &v, &n);
        if (rc == KV_OK)
            free(v);
        else
            CHECK_RC(rc, KV_ERR_CORRUPT);
        corrupt += rc != KV_OK;
    }
    CHECK(corrupt == 1);
    put_s(db, "after", "still writable");
    expect(db, "after", "still writable");
    CHECK_RC(kv_close(db), KV_OK);
    o.use_hints = 0; /* a replay truncates the damaged record away */
    db = open_db(dir, &o);
    expect(db, "after", "still writable");
    CHECK(kv_count(db) == 500);
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_strerror(const char *dir)
{
    (void)dir;
    int codes[] = {KV_OK,          KV_NOT_FOUND,   KV_ERR_IO,
                   KV_ERR_NOMEM,   KV_ERR_INVALID, KV_ERR_CORRUPT,
                   KV_ERR_LOCKED,  KV_ERR_FAILED};
    size_t n = sizeof codes / sizeof *codes;
    for (size_t i = 0; i < n; i++) {
        CHECK(strcmp(kv_strerror(codes[i]), "unknown error") != 0);
        for (size_t j = 0; j < i; j++)
            CHECK(strcmp(kv_strerror(codes[i]), kv_strerror(codes[j])) != 0);
    }
    CHECK(strcmp(kv_strerror(-99), "unknown error") == 0);
}

/* ---------- backup ---------- */

static void backup_and_check(const char *dir, const char *target)
{
    kv_options o = small_opts(8 * 1024);
    kv_t *db = open_db(dir, &o);
    fill_overwrites(db, 300, 3);
    char k[32];
    for (int i = 0; i < 300; i += 5) {
        snprintf(k, sizeof k, "key-%05d", i);
        CHECK_RC(kv_delete(db, k, strlen(k)), KV_OK);
    }
    CHECK_RC(kv_compact(db), KV_OK); /* some compacted segments too */
    fill_overwrites(db, 300, 3);     /* rewrite every key... */
    for (int i = 0; i < 300; i += 5) {
        snprintf(k, sizeof k, "key-%05d", i);
        CHECK_RC(kv_delete(db, k, strlen(k)), KV_OK);
    }
    CHECK_RC(kv_backup(db, target), KV_OK);
    CHECK_RC(kv_backup(db, target), KV_ERR_INVALID); /* must not exist */

    /* the live store moves on: new values, compaction, close */
    put_s(db, "key-00000", "after the backup");
    put_s(db, "key-00001", "after the backup");
    CHECK_RC(kv_compact(db), KV_OK);
    CHECK_RC(kv_close(db), KV_OK);

    /* the backup holds exactly the state when kv_backup ran */
    for (int hints = 1; hints >= 0; hints--) {
        o.use_hints = hints;
        kv_t *b = open_db(target, &o);
        check_final_round(b, 300, 3, 5);
        CHECK(kv_count(b) == 240);
        CHECK_RC(kv_close(b), KV_OK);
    }
    /* writing to the backup must not touch the original's files */
    kv_t *b = open_db(target, &o);
    for (int i = 0; i < 200; i++)
        put_s(b, "only-in-backup", "x");
    CHECK_RC(kv_close(b), KV_OK);
    db = open_db(dir, &o);
    expect(db, "only-in-backup", NULL);
    expect(db, "key-00000", "after the backup");
    expect(db, "key-00001", "after the backup");
    expect(db, "key-00002", "round-2-value-2-padding-padding");
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_backup(const char *dir)
{
    char target[512];
    snprintf(target, sizeof target, "%s-backup", dir);
    backup_and_check(dir, target);
    tu_rmrf(target);
}

/* Across file systems nothing can be hard-linked, so everything is copied.
 * The store lives under $TMPDIR; the backup goes to the working directory,
 * which is usually elsewhere (if not, this repeats the linked case). */
static void test_backup_other_filesystem(const char *dir)
{
    char target[64];
    snprintf(target, sizeof target, "meeseeks-backup-%d", (int)getpid());
    backup_and_check(dir, target);
    tu_rmrf(target);
}

/* Right after a backup, the newest segment of both directories is the one
 * just sealed; both sides append to it when reopened. That file must not
 * be shared, or each would write into the other. */
static void test_backup_shares_nothing_writable(const char *dir)
{
    char target[512];
    snprintf(target, sizeof target, "%s-backup", dir);
    kv_options o = small_opts(1 << 20);
    kv_t *db = open_db(dir, &o);
    fill_overwrites(db, 50, 2);
    CHECK_RC(kv_backup(db, target), KV_OK);
    CHECK_RC(kv_close(db), KV_OK); /* its fresh active is empty: deleted */

    kv_t *b = open_db(target, &o);
    put_s(b, "backup-only", "1");
    CHECK_RC(kv_close(b), KV_OK);
    db = open_db(dir, &o);
    put_s(db, "live-only", "2");
    expect(db, "backup-only", NULL);
    check_final_round(db, 50, 2, 0);
    CHECK_RC(kv_close(db), KV_OK);
    b = open_db(target, &o);
    expect(b, "live-only", NULL);
    expect(b, "backup-only", "1");
    check_final_round(b, 50, 2, 0);
    CHECK_RC(kv_close(b), KV_OK);
    tu_rmrf(target);
}

/* ---------- key iteration ---------- */

enum { SCAN_KEYS = 3000 };

/* Even keys fit inside an index entry, odd ones are long enough to be
 * stored on the heap. */
static size_t scan_key(int i, char *buf, size_t n)
{
    return (size_t)snprintf(buf, n, i % 2 ? "a-key-long-enough-for-the-heap:%d"
                                          : "k:%d",
                            i);
}

/* Number of the key made by scan_key, or -1 for any other key. */
static int scan_key_number(const void *key, size_t klen)
{
    char buf[64];
    if (klen == 0 || klen >= sizeof buf)
        return -1;
    memcpy(buf, key, klen);
    buf[klen] = '\0';
    const char *colon = strrchr(buf, ':');
    if (!colon || memchr(buf, '\0', klen))
        return -1;
    int i = atoi(colon + 1);
    char again[64];
    if (i < 0 || i >= SCAN_KEYS || scan_key(i, again, sizeof again) != klen ||
        memcmp(again, key, klen) != 0)
        return -1;
    return i;
}

typedef struct {
    int seen[SCAN_KEYS];
    int others, visited;
    int stop_at;   /* return 42 from this visit on (0: never) */
    kv_t *db;      /* if set, write to the store from inside the callback */
} scan_tally;

static int tally(void *ctx, const void *key, size_t klen)
{
    scan_tally *t = ctx;
    t->visited++;
    int i = scan_key_number(key, klen);
    if (i < 0)
        t->others++;
    else
        t->seen[i]++;
    if (t->db && i >= 0) {
        /* Delete the key just visited and add a new one, which may land in
         * the freed entry (behind the cursor) or a fresh one (ahead). */
        char k[64];
        CHECK_RC(kv_delete(t->db, key, klen), KV_OK);
        size_t n = (size_t)snprintf(k, sizeof k, "added during the scan %d", i);
        CHECK_RC(kv_put(t->db, k, n, "x", 1), KV_OK);
        void *v;
        size_t vlen;
        CHECK_RC(kv_get(t->db, k, n, &v, &vlen), KV_OK);
        free(v);
    }
    return t->stop_at && t->visited >= t->stop_at ? 42 : 0;
}

/* Every key with present(i) seen exactly once, and no other scan_key. */
static void check_tally(const scan_tally *t, int (*present)(int i))
{
    for (int i = 0; i < SCAN_KEYS; i++)
        if (t->seen[i] != (present(i) ? 1 : 0)) {
            fprintf(stderr, "key %d seen %d times\n", i, t->seen[i]);
            exit(1);
        }
}

static int all_keys(int i)
{
    (void)i;
    return 1;
}

static int not_third(int i)
{
    return i % 3 != 0;
}

static void test_scan(const char *dir)
{
    kv_options o = small_opts(64 * 1024);
    kv_t *db = open_db(dir, &o);
    scan_tally *t = calloc(1, sizeof *t);
    CHECK(t);
    CHECK_RC(kv_scan(db, tally, t), KV_OK); /* empty store */
    CHECK(t->visited == 0);

    char k[64];
    for (int i = 0; i < SCAN_KEYS; i++) {
        size_t n = scan_key(i, k, sizeof k);
        CHECK_RC(kv_put(db, k, n, "v", 1), KV_OK);
    }
    CHECK_RC(kv_put(db, "", 0, "empty key", 9), KV_OK);
    CHECK_RC(kv_put(db, "nul\0key", 7, "v", 1), KV_OK);
    CHECK_RC(kv_scan(db, tally, t), KV_OK);
    check_tally(t, all_keys);
    CHECK(t->others == 2 && t->visited == SCAN_KEYS + 2);

    /* Deleted keys vanish, overwritten ones are still seen once, and the
     * same holds after compaction and after the index is rebuilt. */
    for (int i = 0; i < SCAN_KEYS; i += 3) {
        size_t n = scan_key(i, k, sizeof k);
        CHECK_RC(kv_delete(db, k, n), KV_OK);
    }
    for (int i = 1; i < SCAN_KEYS; i += 5) {
        size_t n = scan_key(i, k, sizeof k);
        if (not_third(i))
            CHECK_RC(kv_put(db, k, n, "again", 5), KV_OK);
    }
    for (int round = 0; round < 3; round++) {
        if (round == 1)
            CHECK_RC(kv_compact(db), KV_OK);
        if (round == 2) {
            CHECK_RC(kv_close(db), KV_OK);
            db = open_db(dir, &o);
        }
        memset(t, 0, sizeof *t);
        CHECK_RC(kv_scan(db, tally, t), KV_OK);
        check_tally(t, not_third);
        CHECK(t->others == 2);
    }

    /* A nonzero return stops the scan and becomes its result. */
    memset(t, 0, sizeof *t);
    t->stop_at = 10;
    CHECK(kv_scan(db, tally, t) == 42);
    CHECK(t->visited == 10);

    /* Resumable: a few keys per call, every key exactly once overall. */
    memset(t, 0, sizeof *t);
    uint64_t cursor = 0;
    int calls = 0;
    do {
        int before = t->visited;
        CHECK_RC(kv_scan_from(db, &cursor, 7, tally, t), KV_OK);
        CHECK(t->visited - before <= 7);
        calls++;
    } while (cursor != 0);
    check_tally(t, not_third);
    CHECK(t->others == 2 && calls >= t->visited / 7);

    /* Resuming after the callback stopped picks up at the next key. */
    memset(t, 0, sizeof *t);
    cursor = 0;
    t->stop_at = 1;
    int stops = 0;
    for (;;) {
        int rc = kv_scan_from(db, &cursor, 0, tally, t);
        CHECK(rc == KV_OK || rc == 42);
        stops += rc == 42;
        t->stop_at = t->visited + 1;
        if (cursor == 0)
            break;
    }
    check_tally(t, not_third);
    CHECK(stops == t->visited && t->others == 2);
    CHECK_RC(kv_scan_from(db, NULL, 0, tally, t), KV_ERR_INVALID);
    CHECK_RC(kv_scan_from(db, &cursor, 0, NULL, t), KV_ERR_INVALID);
    cursor = UINT64_MAX; /* a cursor past the end is simply done */
    CHECK_RC(kv_scan_from(db, &cursor, 0, tally, t), KV_OK);
    CHECK(cursor == 0);

    /* Mostly free entries: whole lock holds find no key, and the walk must
     * go on past them, in kv_scan and one key at a time alike. */
    char big[32];
    for (int i = 0; i < 20000; i++) {
        snprintf(big, sizeof big, "doomed-%d", i);
        CHECK_RC(kv_put(db, big, strlen(big), "v", 1), KV_OK);
    }
    for (int i = 0; i < 20000; i++) {
        snprintf(big, sizeof big, "doomed-%d", i);
        if (i != 7 && i != 19999)
            CHECK_RC(kv_delete(db, big, strlen(big)), KV_OK);
    }
    memset(t, 0, sizeof *t);
    CHECK_RC(kv_scan(db, tally, t), KV_OK);
    check_tally(t, not_third);
    CHECK(t->others == 4);
    memset(t, 0, sizeof *t);
    cursor = 0;
    do
        CHECK_RC(kv_scan_from(db, &cursor, 1, tally, t), KV_OK);
    while (cursor != 0);
    check_tally(t, not_third);
    CHECK(t->others == 4);
    CHECK_RC(kv_delete(db, "doomed-7", 8), KV_OK);
    CHECK_RC(kv_delete(db, "doomed-19999", 12), KV_OK);

    /* The callback may write: no lock is held while it runs. Keys that
     * exist for the whole scan are still visited exactly once each. */
    memset(t, 0, sizeof *t);
    t->db = db;
    CHECK_RC(kv_scan(db, tally, t), KV_OK);
    check_tally(t, not_third);
    CHECK(kv_count(db) == 2 + (size_t)(SCAN_KEYS - (SCAN_KEYS + 2) / 3));
    free(t);
    CHECK_RC(kv_close(db), KV_OK);
}

/* ---------- scrubbing ---------- */

/* Flip a byte in the value of the last record of the compacted segment
 * build_compacted() left behind. Returns the segment's size. */
static off_t damage_compacted(const char *dir)
{
    char seg[64], p[512];
    first_file(dir, ".seg", seg);
    snprintf(p, sizeof p, "%s/%s", dir, seg);
    off_t size = tu_file_size(p);
    int fd = open(p, O_RDWR);
    CHECK(fd >= 0);
    uint8_t b;
    CHECK(pread(fd, &b, 1, size - 3) == 1);
    b ^= 0x5A;
    CHECK(pwrite(fd, &b, 1, size - 3) == 1);
    close(fd);
    return size;
}

/* A segment loaded from its hint is not checksummed at open: the damage is
 * invisible until a get reads that record, or a scrub reads them all. */
static void test_scrub(const char *dir)
{
    kv_options o = small_opts(1 << 20);
    build_compacted(dir, &o);
    kv_t *db = open_db(dir, &o);
    kv_stats st;
    CHECK_RC(kv_scrub(db), KV_OK);
    kv_get_stats(db, &st);
    CHECK(st.damaged_segments == 0 && st.scrub_bytes > 0);
    CHECK_RC(kv_close(db), KV_OK);
    CHECK_RC(kv_scrub(NULL), KV_ERR_INVALID);

    off_t size = damage_compacted(dir);
    db = open_db(dir, &o);
    kv_get_stats(db, &st);
    CHECK(st.keys == 500 && st.damaged_segments == 0); /* not noticed */
    CHECK_RC(kv_scrub(db), KV_ERR_CORRUPT);
    kv_get_stats(db, &st);
    CHECK(st.damaged_segments == 1);
    /* it read up to the bad record, the last one, and no further */
    CHECK(st.scrub_bytes < (uint64_t)size && st.scrub_bytes > (uint64_t)size / 2);

    /* The store keeps working, and newly sealed segments are fine. */
    put_s(db, "after", "the scrub");
    CHECK_RC(kv_sync(db), KV_OK);
    expect(db, "after", "the scrub");
    CHECK_RC(kv_scrub(db), KV_ERR_CORRUPT); /* still damaged */
    kv_get_stats(db, &st);
    CHECK(st.damaged_segments == 1);
    CHECK_RC(kv_close(db), KV_OK);
}

static void test_background_scrub(const char *dir)
{
    kv_options o = small_opts(16 * 1024);
    build_compacted(dir, &o);
    damage_compacted(dir);
    o.background_scrub = 1;
    kv_t *db = open_db(dir, &o);
    kv_stats st;
    for (int i = 0; i < 2000; i++) { /* it starts with what is there */
        kv_get_stats(db, &st);
        if (st.damaged_segments)
            break;
        tu_sleep_us(5000);
    }
    CHECK(st.damaged_segments == 1);

    /* Segments sealed after open are read back too: wait for at least
     * 100 KiB more to be checked. */
    uint64_t before = st.scrub_bytes;
    fill_overwrites(db, 500, 4); /* about 300 KiB, many rollovers */
    for (int i = 0; i < 2000; i++) {
        kv_get_stats(db, &st);
        if (st.scrub_bytes >= before + 100 * 1024)
            break;
        tu_sleep_us(5000);
    }
    CHECK(st.scrub_bytes >= before + 100 * 1024);
    CHECK(st.damaged_segments == 1);
    check_final_round(db, 500, 4, 0);
    CHECK_RC(kv_close(db), KV_OK); /* stops the scrubber */
}

int main(void)
{
    static const test_case tests[] = {
        {"crc32 known answers", test_crc32},
        {"siphash reference vectors", test_siphash},
        {"put/get/overwrite", test_put_get},
        {"delete", test_delete},
        {"binary keys, empty key and value", test_binary_and_empty},
        {"invalid arguments", test_invalid_args},
        {"reopen persistence", test_reopen_persistence},
        {"tombstone survives restart", test_tombstone_survives_restart},
        {"segment rollover", test_segment_rollover},
        {"reopen appends to last segment", test_reopen_appends_to_last_segment},
        {"compaction correctness", test_compaction},
        {"repeated compaction", test_compaction_repeated},
        {"compaction, all keys deleted", test_compaction_everything_deleted},
        {"compaction, empty store", test_compaction_empty_db},
        {"hint path is used", test_hint_path_is_used},
        {"damaged hint falls back to replay", test_hint_damage_falls_back},
        {"truncated segment ignores hint", test_truncated_segment_ignores_hint},
        {"crash states during compaction", test_compaction_crash_states},
        {"garbage files ignored", test_garbage_files_ignored},
        {"sync policies", test_sync_policies},
        {"lock: same process", test_lock_same_process},
        {"lock: other process", test_lock_other_process},
        {"stats", test_stats},
        {"background compaction", test_auto_compaction},
        {"bad options", test_bad_options},
        {"open creates its directory", test_open_creates_directory},
        {"compaction of a record over 1 MiB", test_compact_huge_record},
        {"compaction refuses corrupt records", test_compaction_refuses_corruption},
        {"error strings", test_strerror},
        {"backup", test_backup},
        {"backup to another file system", test_backup_other_filesystem},
        {"backup is isolated from the original",
         test_backup_shares_nothing_writable},
        {"scan", test_scan},
        {"scrub finds damage open() missed", test_scrub},
        {"background scrub", test_background_scrub},
    };
    return tu_run("test_kv", tests, sizeof tests / sizeof *tests);
}
