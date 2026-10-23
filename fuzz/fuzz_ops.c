/*
 * libFuzzer target: operation sequences checked against a model.
 *
 * Each input byte picks an operation (put, delete, get, compact, reopen with
 * or without hints, simulated crash) on a store with tiny segments, and every
 * answer is compared with a plain in-memory model. A simulated crash copies
 * the directory while the store is open and requires the copy to recover to
 * exactly the model's state. Coverage guidance steers toward rare orderings
 * (compactions right after rollovers, reopen right after compaction, ...).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fuzz_util.h"

enum { NKEYS = 24 };

typedef struct {
    int present;
    size_t len;
    uint8_t val[300];
} slot;

static slot model[NKEYS];

static void key_of(int i, char *k)
{
    snprintf(k, 16, "key%d", i);
}

static void check_key(kv_t *db, int i)
{
    char k[16];
    key_of(i, k);
    void *v;
    size_t n;
    int rc = kv_get(db, k, strlen(k), &v, &n);
    if (model[i].present) {
        if (rc != KV_OK || n != model[i].len || memcmp(v, model[i].val, n) != 0)
            abort();
        free(v);
    } else if (rc != KV_NOT_FOUND) {
        abort();
    }
}

static void check_all(kv_t *db)
{
    size_t live = 0;
    for (int i = 0; i < NKEYS; i++) {
        check_key(db, i);
        live += (size_t)model[i].present;
    }
    if (kv_count(db) != live)
        abort();
}

static void copy_dir(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    struct dirent *e;
    char a[512], b[512], buf[8192];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(a, sizeof a, "%s/%s", src, e->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, e->d_name);
        int in = open(a, O_RDONLY), out = open(b, O_WRONLY | O_CREAT, 0644);
        ssize_t r;
        while (in >= 0 && out >= 0 && (r = read(in, buf, sizeof buf)) > 0)
            if (write(out, buf, (size_t)r) != r)
                abort();
        close(in);
        close(out);
    }
    closedir(d);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    memset(model, 0, sizeof model);
    char dir[256];
    fuzz_tmpdir(dir);
    kv_options o;
    kv_default_options(&o);
    o.sync_policy = KV_SYNC_NONE;
    o.segment_size = 256; /* a rollover every few writes */
    kv_t *db;
    if (kv_open(dir, &o, &db) != KV_OK)
        abort();

    for (size_t i = 0; i < size; i++) {
        uint8_t op = data[i];
        int k = op % NKEYS;
        char key[16];
        key_of(k, key);
        switch (op >> 5) {
        case 0:
        case 1:
        case 2: { /* put: length and fill from the next byte */
            uint8_t b = i + 1 < size ? data[++i] : 0;
            size_t len = (size_t)b + (op & 0x20 ? 40 : 0);
            for (size_t j = 0; j < len; j++)
                model[k].val[j] = (uint8_t)(b + j * 13);
            model[k].len = len;
            model[k].present = 1;
            if (kv_put(db, key, strlen(key), model[k].val, len) != KV_OK)
                abort();
            break;
        }
        case 3: /* delete */
            if (kv_delete(db, key, strlen(key)) !=
                (model[k].present ? KV_OK : KV_NOT_FOUND))
                abort();
            model[k].present = 0;
            break;
        case 4: /* get */
            check_key(db, k);
            break;
        case 5: /* compact */
            if (kv_compact(db) != KV_OK)
                abort();
            check_all(db);
            break;
        case 6: /* clean reopen */
            if (kv_close(db) != KV_OK)
                abort();
            o.use_hints = op & 1;
            if (kv_open(dir, &o, &db) != KV_OK)
                abort();
            check_all(db);
            break;
        case 7: { /* crash: recover a copy taken while the store is open */
            char crash[256];
            fuzz_tmpdir(crash);
            copy_dir(dir, crash);
            kv_options co = o;
            co.use_hints = op & 1;
            kv_t *c;
            if (kv_open(crash, &co, &c) != KV_OK)
                abort();
            check_all(c);
            kv_close(c);
            fuzz_rmrf(crash);
            break;
        }
        }
    }
    check_all(db);
    kv_close(db);
    fuzz_rmrf(dir);
    return 0;
}
