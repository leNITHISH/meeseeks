/*
 * libFuzzer target: arbitrary request lines into the protocol handler.
 * Every line must get exactly one well-formed response line, whatever bytes
 * it contains, and a GET must return what the last SET stored.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fuzz_util.h"
#include "server.h"

static kv_t *db;
static char dir[256];
static unsigned runs;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (db && ++runs % 20000 == 0) { /* keep the store from growing forever */
        kv_close(db);
        fuzz_rmrf(dir);
        db = NULL;
    }
    if (!db) {
        fuzz_tmpdir(dir);
        kv_options o;
        kv_default_options(&o);
        o.sync_policy = KV_SYNC_NONE;
        o.segment_size = 1 << 16;
        if (kv_open(dir, &o, &db) != KV_OK)
            abort();
    }
    /* split the input into lines, like the connection loop does */
    size_t start = 0;
    while (start <= size) {
        const uint8_t *nl = memchr(data + start, '\n', size - start);
        size_t len = nl ? (size_t)(nl - (data + start)) : size - start;
        sbuf out = {0};
        if (server_handle_line(db, (const char *)data + start, len, &out) != 0)
            abort();
        /* exactly one line, with a known prefix */
        if (out.len == 0 || out.data[out.len - 1] != '\n' ||
            memchr(out.data, '\n', out.len - 1) != NULL)
            abort();
        if (!(out.len >= 3 && memcmp(out.data, "OK\n", 3) == 0) &&
            !(out.len >= 6 && memcmp(out.data, "VALUE ", 6) == 0) &&
            !(out.len >= 10 && memcmp(out.data, "NOT_FOUND\n", 10) == 0) &&
            !(out.len >= 4 && memcmp(out.data, "ERR ", 4) == 0))
            abort();

        /* a successful SET must be readable right back */
        const char *line = (const char *)data + start;
        if (len > 4 && out.len == 3 && (line[0] | 0x20) == 's') {
            sbuf again = {0};
            char req[600];
            /* re-issue the key as a GET; skip keys too long for the buffer */
            size_t i = 0;
            while (i < len && (line[i] == ' ' || line[i] == '\t'))
                i++;
            while (i < len && line[i] != ' ' && line[i] != '\t')
                i++;
            while (i < len && (line[i] == ' ' || line[i] == '\t'))
                i++;
            size_t ks = i;
            while (i < len && line[i] != ' ' && line[i] != '\t')
                i++;
            size_t klen = i - ks;
            if (klen < 500) {
                /* copy bytes, not a string: keys may contain NULs */
                memcpy(req, "GET ", 4);
                memcpy(req + 4, line + ks, klen);
                size_t n = 4 + klen;
                const char *val = line + i + 1;
                size_t vlen = len - i - 1;
                if (vlen && val[vlen - 1] == '\r')
                    vlen--;
                if (server_handle_line(db, req, n, &again) != 0)
                    abort();
                int has_break = memchr(val, '\n', vlen) || memchr(val, '\r', vlen);
                if (!has_break &&
                    (again.len != 6 + vlen + 1 ||
                     memcmp(again.data, "VALUE ", 6) != 0 ||
                     memcmp(again.data + 6, val, vlen) != 0))
                    abort();
                sbuf_free(&again);
            }
        }
        sbuf_free(&out);
        if (!nl)
            break;
        start += len + 1;
    }
    return 0;
}
