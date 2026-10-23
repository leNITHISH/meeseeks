/*
 * kvtool: inspect a meeseeks data directory without modifying it.
 *
 *   kvtool check DIR       verify every record and hint file, summarize
 *   kvtool dump DIR [-v]   print every record (-v: with value previews)
 *
 * Exit status: 0 if everything checks out, 1 if damage was found, 2 on
 * usage or I/O errors. Unlike kv_open(), nothing is truncated or deleted:
 * damage is reported along with what recovery would do about it.
 */
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crc32.h"
#include "index.h"
#include "kv.h"
#include "record.h"

#define SUB_SPAN 1000000ull

typedef struct {
    uint64_t id;
    char name[64];
} segfile;

static int problems;

static void problem(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ! ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    problems++;
}

static int cmp_seg(const void *a, const void *b)
{
    uint64_t x = ((const segfile *)a)->id, y = ((const segfile *)b)->id;
    return (x > y) - (x < y);
}

static int parse_name(const char *n, const char *ext, uint64_t *id)
{
    if (strlen(n) != 18 + strlen(ext) || n[10] != '-' || n[17] != '.' ||
        strcmp(n + 18, ext) != 0)
        return 0;
    uint64_t log = 0, sub = 0;
    for (int i = 0; i < 10; i++) {
        if (n[i] < '0' || n[i] > '9')
            return 0;
        log = log * 10 + (uint64_t)(n[i] - '0');
    }
    for (int i = 11; i < 17; i++) {
        if (n[i] < '0' || n[i] > '9')
            return 0;
        sub = sub * 10 + (uint64_t)(n[i] - '0');
    }
    *id = log * SUB_SPAN + sub;
    return log > 0;
}

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return NULL;
    }
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t got = 0;
    while (b && got < (size_t)st.st_size) {
        ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    *len = got;
    return b;
}

static void print_bytes(const uint8_t *p, size_t n, size_t max)
{
    putchar('"');
    for (size_t i = 0; i < n && i < max; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '"' && p[i] != '\\')
            putchar(p[i]);
        else
            printf("\\x%02x", p[i]);
    }
    printf(n > max ? "\"..." : "\"");
}

int main(int argc, char **argv)
{
    if (argc < 3 || (strcmp(argv[1], "check") != 0 && strcmp(argv[1], "dump") != 0)) {
        fprintf(stderr, "usage: kvtool check DIR\n       kvtool dump DIR [-v]\n");
        return 2;
    }
    int dump = strcmp(argv[1], "dump") == 0;
    int verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
    const char *dir = argv[2];

    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return 2;
    }
    segfile *segs = NULL;
    size_t nsegs = 0, cap = 0;
    int ntmp = 0, nhints = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint64_t id;
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".tmp") == 0)
            ntmp++;
        else if (parse_name(e->d_name, "hint", &id))
            nhints++;
        else if (parse_name(e->d_name, "seg", &id)) {
            if (nsegs == cap)
                segs = realloc(segs, (cap = cap ? cap * 2 : 16) * sizeof *segs);
            segs[nsegs].id = id;
            snprintf(segs[nsegs].name, sizeof segs[nsegs].name, "%.63s",
                     e->d_name); /* a validated 21-character name */
            nsegs++;
        }
    }
    closedir(d);
    if (nsegs)
        qsort(segs, nsegs, sizeof *segs, cmp_seg);

    /* MANIFEST: same format as src/recovery.c */
    uint64_t min_live = 0;
    size_t mlen;
    uint8_t *m = slurp(dir, "MANIFEST", &mlen);
    if (!dump)
        printf("%s\n", dir);
    if (m) {
        if (mlen == 20 && get_u32(m) == 0x464D564Bu && get_u32(m + 4) == 2 &&
            get_u32(m + 16) == crc32_update(0, m, 16)) {
            min_live = get_u64(m + 8);
            if (!dump)
                printf("  MANIFEST: segments before %010llu-%06llu are obsolete\n",
                       (unsigned long long)(min_live / SUB_SPAN),
                       (unsigned long long)(min_live % SUB_SPAN));
        } else {
            problem("MANIFEST is damaged (recovery replays every segment)");
        }
        free(m);
    }

    kv_index idx;
    if (index_init(&idx, 1024) != 0)
        return 2;
    uint64_t records = 0, tombstones = 0, seg_bytes = 0, live_bytes = 0;
    size_t obsolete = 0, compacted = 0;
    for (size_t s = 0; s < nsegs; s++) {
        if (segs[s].id < min_live) {
            obsolete++;
            continue;
        }
        size_t len;
        uint8_t *b = slurp(dir, segs[s].name, &len);
        if (!b) {
            perror(segs[s].name);
            return 2;
        }
        int is_output = segs[s].id % SUB_SPAN != 0;
        compacted += (size_t)is_output;
        if (dump)
            printf("== %s (%zu bytes)\n", segs[s].name, len);
        size_t off = 0;
        while (off < len) {
            uint32_t ksz = len - off >= REC_HDR_SIZE ? get_u32(b + off + 12) : 0;
            uint32_t vsz = len - off >= REC_HDR_SIZE ? get_u32(b + off + 16) : 0;
            uint64_t rsz = record_size(ksz, vsz);
            if (len - off < REC_HDR_SIZE || ksz > KV_MAX_KEY_SIZE ||
                (vsz != REC_TOMBSTONE && vsz > KV_MAX_VALUE_SIZE) ||
                rsz > len - off || !record_crc_ok(b + off, rsz)) {
                problem("%s: %zu bytes from offset %zu fail to parse or "
                        "checksum (recovery truncates there)",
                        segs[s].name, len - off, off);
                break;
            }
            const uint8_t *key = b + off + REC_HDR_SIZE;
            int tomb = vsz == REC_TOMBSTONE;
            records++;
            tombstones += (uint64_t)tomb;
            if (dump) {
                printf("%10zu %8llu %s ", off, (unsigned long long)rsz,
                       tomb ? "DEL" : "PUT");
                print_bytes(key, ksz, 64);
                if (!tomb) {
                    printf(" (%u bytes)", vsz);
                    if (verbose) {
                        putchar(' ');
                        print_bytes(key + ksz, vsz, 48);
                    }
                }
                putchar('\n');
            }
            kv_loc old;
            if (tomb) {
                if (index_remove(&idx, key, ksz, &old))
                    live_bytes -= old.rec_size;
            } else {
                int existed;
                kv_loc *l = index_upsert(&idx, key, ksz, &existed);
                if (!l)
                    return 2;
                if (existed)
                    live_bytes -= l->rec_size;
                *l = (kv_loc){segs[s].id, off, (uint32_t)rsz};
                live_bytes += rsz;
            }
            off += rsz;
        }
        seg_bytes += off;

        if (is_output) { /* its hint must tile exactly what we just read */
            char hname[64];
            snprintf(hname, sizeof hname, "%.17s.hint", segs[s].name);
            size_t hl;
            uint8_t *h = slurp(dir, hname, &hl);
            if (!h) {
                problem("%s: no hint file (startup replays it)", segs[s].name);
            } else {
                size_t ho = 0;
                uint64_t covered = 0;
                int ok = 1;
                while (ok && ho < hl) {
                    if (hl - ho < HINT_HDR_SIZE) {
                        ok = 0;
                        break;
                    }
                    uint32_t ksz = get_u32(h + ho + 12);
                    uint32_t rsz = get_u32(h + ho + 24);
                    /* each entry: intact, next in line, and naming the very
                     * record at that offset in the segment */
                    ok = ksz <= KV_MAX_KEY_SIZE &&
                         HINT_HDR_SIZE + (size_t)ksz <= hl - ho &&
                         hint_crc_ok(h + ho, HINT_HDR_SIZE + ksz) &&
                         get_u64(h + ho + 16) == covered &&
                         rsz >= REC_HDR_SIZE + ksz && covered + rsz <= off &&
                         get_u32(b + covered + 12) == ksz &&
                         memcmp(b + covered + REC_HDR_SIZE,
                                h + ho + HINT_HDR_SIZE, ksz) == 0;
                    if (ok) {
                        covered += rsz;
                        ho += HINT_HDR_SIZE + ksz;
                    }
                }
                if (!ok || covered != off)
                    problem("%s: hint file disagrees with the segment at "
                            "byte %llu (startup replays it)",
                            hname, (unsigned long long)covered);
                free(h);
            }
        }
        free(b);
    }

    if (!dump) {
        printf("  segments: %zu (%zu log, %zu compacted), %d hint files",
               nsegs - obsolete, nsegs - obsolete - compacted, compacted,
               nhints);
        if (obsolete)
            printf(", %zu obsolete (deleted on next open)", obsolete);
        if (ntmp)
            printf(", %d unfinished .tmp files (deleted on next open)", ntmp);
        printf("\n  records:  %llu (%llu tombstones)\n",
               (unsigned long long)records, (unsigned long long)tombstones);
        printf("  live:     %zu keys, %llu of %llu segment bytes (%.1f%% "
               "reclaimable by compaction)\n",
               idx.count, (unsigned long long)live_bytes,
               (unsigned long long)seg_bytes,
               seg_bytes ? 100.0 * (double)(seg_bytes - live_bytes) /
                               (double)seg_bytes
                         : 0.0);
        printf("  %s\n", problems ? "DAMAGED" : "ok");
    }
    index_free(&idx);
    free(segs);
    return problems ? 1 : 0;
}
