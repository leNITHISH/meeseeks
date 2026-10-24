#include "index.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SHIFT 14 /* 16384 entries per chunk */
#define CHUNK_SIZE (1u << CHUNK_SHIFT)
#define MAX_LOAD_NUM 3 /* grow past 3/4 full */
#define MAX_LOAD_DEN 4
#define ENTRY_FREE UINT32_MAX /* klen of a released entry; keys are shorter */

/* ---------- SipHash ---------- */

static inline uint64_t rotl(uint64_t x, int b)
{
    return (x << b) | (x >> (64 - b));
}

static inline uint64_t load_le64(const uint8_t *p)
{
    /* spelled out so compilers emit one load on little-endian machines */
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = rotl(v1, 13);                                                     \
        v1 ^= v0;                                                              \
        v0 = rotl(v0, 32);                                                     \
        v2 += v3;                                                              \
        v3 = rotl(v3, 16);                                                     \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = rotl(v3, 21);                                                     \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = rotl(v1, 17);                                                     \
        v1 ^= v2;                                                              \
        v2 = rotl(v2, 32);                                                     \
    } while (0)

uint64_t siphash(const void *in, size_t len, uint64_t k0, uint64_t k1, int c,
                 int d)
{
    const uint8_t *p = in;
    uint64_t v0 = 0x736f6d6570736575ull ^ k0;
    uint64_t v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0;
    uint64_t v3 = 0x7465646279746573ull ^ k1;
    size_t whole = len - (len & 7);
    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m = load_le64(p + i);
        v3 ^= m;
        for (int r = 0; r < c; r++)
            SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < (len & 7); i++)
        b |= (uint64_t)p[whole + i] << (8 * i);
    v3 ^= b;
    for (int r = 0; r < c; r++)
        SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    for (int r = 0; r < d; r++)
        SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

/* SipHash-1-3 with the rounds unrolled: the generic version's loops over a
 * runtime round count cost a noticeable share of a lookup. */
static inline uint32_t hash_key(const kv_index *idx, const void *key,
                                size_t len)
{
    const uint8_t *p = key;
    uint64_t v0 = 0x736f6d6570736575ull ^ idx->k0;
    uint64_t v1 = 0x646f72616e646f6dull ^ idx->k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ idx->k0;
    uint64_t v3 = 0x7465646279746573ull ^ idx->k1;
    size_t whole = len - (len & 7);
    for (size_t i = 0; i < whole; i += 8) {
        uint64_t m = load_le64(p + i);
        v3 ^= m;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < (len & 7); i++)
        b |= (uint64_t)p[whole + i] << (8 * i);
    v3 ^= b;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return (uint32_t)(v0 ^ v1 ^ v2 ^ v3);
}

/* A random hash key: /dev/urandom where there is one, else whatever entropy
 * the clock, pid and address-space layout give us. */
static void random_key(uint64_t *k0, uint64_t *k1)
{
    uint64_t k[2] = {0, 0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, k, sizeof k);
        close(fd);
        if (n == (ssize_t)sizeof k) {
            *k0 = k[0];
            *k1 = k[1];
            return;
        }
    }
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed[4] = {(uint64_t)ts.tv_sec, (uint64_t)ts.tv_nsec,
                        (uint64_t)getpid(), (uint64_t)(uintptr_t)&ts};
    *k0 = siphash(seed, sizeof seed, 0x0123456789abcdefull, 1, 2, 4);
    *k1 = siphash(seed, sizeof seed, 0xfedcba9876543210ull, 2, 2, 4);
}

/* ---------- entries ---------- */

static inline index_entry *entry_at(const kv_index *idx, uint32_t n)
{
    return &idx->chunks[n >> CHUNK_SHIFT][n & (CHUNK_SIZE - 1)];
}

static inline const uint8_t *entry_key(const index_entry *e)
{
    return e->klen <= INDEX_INLINE_KEY ? e->key.inl : e->key.ptr;
}

/* Hand out an entry number, or UINT32_MAX if memory ran out. */
static uint32_t entry_alloc(kv_index *idx)
{
    if (idx->free_head) {
        uint32_t n = idx->free_head - 1;
        idx->free_head = (uint32_t)entry_at(idx, n)->loc.offset;
        return n;
    }
    if (idx->used == UINT32_MAX - 1)
        return UINT32_MAX;
    if ((idx->used >> CHUNK_SHIFT) == idx->nchunks) {
        index_entry **t =
            realloc(idx->chunks, (idx->nchunks + 1) * sizeof *idx->chunks);
        if (!t)
            return UINT32_MAX;
        idx->chunks = t;
        if (!(t[idx->nchunks] = malloc(CHUNK_SIZE * sizeof(index_entry))))
            return UINT32_MAX;
        idx->nchunks++;
    }
    return idx->used++;
}

static void entry_release(kv_index *idx, uint32_t n)
{
    index_entry *e = entry_at(idx, n);
    if (e->klen > INDEX_INLINE_KEY) {
        free(e->key.ptr);
        idx->heap_bytes -= e->klen;
    }
    e->klen = ENTRY_FREE; /* scans walk entries and must skip this one */
    e->loc.offset = idx->free_head; /* the free list threads through here */
    idx->free_head = n + 1;
}

/* ---------- probe table ---------- */

static inline uint64_t make_slot(uint32_t n, uint32_t h)
{
    return (uint64_t)h << 32 | ((uint64_t)n + 1);
}

static inline uint32_t slot_entry(uint64_t s)
{
    return (uint32_t)s - 1;
}

static inline uint32_t slot_hash(uint64_t s)
{
    return (uint32_t)(s >> 32);
}

int index_init(kv_index *idx, size_t initial_cap)
{
    memset(idx, 0, sizeof *idx);
    size_t cap = 16;
    while (cap < initial_cap)
        cap <<= 1;
    idx->slots = calloc(cap, sizeof *idx->slots);
    if (!idx->slots)
        return -1;
    idx->cap = cap;
    random_key(&idx->k0, &idx->k1);
    return 0;
}

void index_free(kv_index *idx)
{
    if (idx->slots)
        for (size_t i = 0; i < idx->cap; i++)
            if (idx->slots[i]) {
                index_entry *e = entry_at(idx, slot_entry(idx->slots[i]));
                if (e->klen > INDEX_INLINE_KEY)
                    free(e->key.ptr);
            }
    for (size_t c = 0; c < idx->nchunks; c++)
        free(idx->chunks[c]);
    free(idx->chunks);
    free(idx->slots);
    memset(idx, 0, sizeof *idx);
}

/* Slot holding key, or the empty slot where it would go. */
static size_t find_slot(const kv_index *idx, const void *key, size_t klen,
                        uint32_t h)
{
    size_t mask = idx->cap - 1;
    for (size_t i = h & mask;; i = (i + 1) & mask) {
        uint64_t s = idx->slots[i];
        if (!s)
            return i;
        if (slot_hash(s) == h) {
            const index_entry *e = entry_at(idx, slot_entry(s));
            if (e->klen == klen && memcmp(entry_key(e), key, klen) == 0)
                return i;
        }
    }
}

static int grow(kv_index *idx)
{
    size_t ncap = idx->cap * 2, mask = ncap - 1;
    uint64_t *ns = calloc(ncap, sizeof *ns);
    if (!ns)
        return -1;
    for (size_t i = 0; i < idx->cap; i++) {
        uint64_t s = idx->slots[i];
        if (!s)
            continue;
        size_t j = slot_hash(s) & mask; /* no key access needed */
        while (ns[j])
            j = (j + 1) & mask;
        ns[j] = s;
    }
    free(idx->slots);
    idx->slots = ns;
    idx->cap = ncap;
    return 0;
}

kv_loc *index_get(kv_index *idx, const void *key, size_t klen)
{
    uint64_t s = idx->slots[find_slot(idx, key, klen, hash_key(idx, key, klen))];
    return s ? &entry_at(idx, slot_entry(s))->loc : NULL;
}

kv_loc *index_upsert(kv_index *idx, const void *key, size_t klen, int *existed)
{
    if ((idx->count + 1) * MAX_LOAD_DEN > idx->cap * MAX_LOAD_NUM &&
        grow(idx) != 0)
        return NULL;
    uint32_t h = hash_key(idx, key, klen);
    size_t i = find_slot(idx, key, klen, h);
    *existed = idx->slots[i] != 0;
    if (*existed)
        return &entry_at(idx, slot_entry(idx->slots[i]))->loc;

    uint8_t *heap = NULL;
    if (klen > INDEX_INLINE_KEY && !(heap = malloc(klen)))
        return NULL;
    uint32_t n = entry_alloc(idx);
    if (n == UINT32_MAX) {
        free(heap);
        return NULL;
    }
    index_entry *e = entry_at(idx, n);
    memset(e, 0, sizeof *e);
    e->klen = (uint32_t)klen;
    e->hash = h;
    if (heap) {
        memcpy(heap, key, klen);
        e->key.ptr = heap;
        idx->heap_bytes += klen;
    } else if (klen) {
        memcpy(e->key.inl, key, klen);
    }
    idx->slots[i] = make_slot(n, h);
    idx->count++;
    return &e->loc;
}

int index_remove(kv_index *idx, const void *key, size_t klen, kv_loc *old)
{
    size_t i = find_slot(idx, key, klen, hash_key(idx, key, klen));
    if (!idx->slots[i])
        return 0;
    uint32_t n = slot_entry(idx->slots[i]);
    if (old)
        *old = entry_at(idx, n)->loc;
    entry_release(idx, n);
    idx->slots[i] = 0;
    idx->count--;

    /* Backward shift: pull later members of the probe run into the hole, so
     * lookups never need tombstones. */
    size_t mask = idx->cap - 1, hole = i;
    for (size_t j = (i + 1) & mask; idx->slots[j]; j = (j + 1) & mask) {
        size_t home = slot_hash(idx->slots[j]) & mask;
        /* movable unless home lies cyclically in (hole, j] */
        int movable = hole <= j ? (home <= hole || home > j)
                                : (home <= hole && home > j);
        if (movable) {
            idx->slots[hole] = idx->slots[j];
            idx->slots[j] = 0;
            hole = j;
        }
    }
    return 1;
}

kv_loc *index_next(kv_index *idx, size_t *cursor)
{
    while (*cursor < idx->cap) {
        uint64_t s = idx->slots[(*cursor)++];
        if (s)
            return &entry_at(idx, slot_entry(s))->loc;
    }
    return NULL;
}

uint32_t index_entry_limit(const kv_index *idx)
{
    return idx->used;
}

const uint8_t *index_entry_key(const kv_index *idx, uint32_t n, uint32_t *klen)
{
    if (n >= idx->used)
        return NULL;
    const index_entry *e = entry_at(idx, n);
    if (e->klen == ENTRY_FREE)
        return NULL;
    *klen = e->klen;
    return entry_key(e);
}

size_t index_memory(const kv_index *idx)
{
    return idx->cap * sizeof *idx->slots +
           idx->nchunks * (CHUNK_SIZE * sizeof(index_entry) +
                           sizeof *idx->chunks) +
           idx->heap_bytes;
}
