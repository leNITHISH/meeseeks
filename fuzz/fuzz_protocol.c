/*
 * libFuzzer target: arbitrary request lines into the protocol handler.
 * Every line must get exactly one well-formed response line, whatever bytes
 * it contains, a GET must return what the last SET stored, and a SCAN reply
 * must be a cursor, a count and then keys that are each a single token.
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

/* Does the line's first token spell SET? SYNC and COMPACT answer OK too. */
static int is_set(const char *line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        i++;
    return len - i >= 4 && (line[i] | 0x20) == 's' &&
           (line[i + 1] | 0x20) == 'e' && (line[i + 2] | 0x20) == 't' &&
           (line[i + 3] == ' ' || line[i + 3] == '\t');
}

/* "KEYS <cursor> <omitted>" and then keys, each one space and a token. */
static void check_keys_reply(const sbuf *out)
{
    const char *p = out->data + 5, *end = out->data + out->len - 1;
    for (int field = 0; field < 2; field++) {
        if (p == end || *p < '0' || *p > '9')
            abort();
        while (p < end && *p >= '0' && *p <= '9')
            p++;
        if (field == 0 && (p == end || *p++ != ' '))
            abort();
    }
    while (p < end) {
        if (*p++ != ' ' || p == end || *p == ' ')
            abort();
        while (p < end && *p != ' ') {
            if (*p == '\t' || *p == '\r')
                abort();
            p++;
        }
    }
}

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
            !(out.len >= 4 && memcmp(out.data, "ERR ", 4) == 0) &&
            !(out.len >= 5 && memcmp(out.data, "KEYS ", 5) == 0) &&
            !(out.len >= 6 && memcmp(out.data, "STATS ", 6) == 0) &&
            !(out.len == 5 && memcmp(out.data, "PONG\n", 5) == 0))
            abort();
        if (out.len >= 5 && memcmp(out.data, "KEYS ", 5) == 0)
            check_keys_reply(&out);

        /* a successful SET must be readable right back */
        const char *line = (const char *)data + start;
        if (len > 4 && out.len == 3 && is_set(line, len)) {
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
