#ifndef KV_INDEX_H
#define KV_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* Where the newest record for a key lives on disk. */
typedef struct {
    uint64_t seg_id;
    uint64_t offset;
    uint32_t rec_size;
} kv_loc;

#define INDEX_INLINE_KEY 16

/* One key. Keys of up to INDEX_INLINE_KEY bytes live inside the entry; only
 * longer ones get a heap allocation. */
typedef struct {
    kv_loc loc;
    uint32_t klen;
    uint32_t hash;
    union {
        uint8_t inl[INDEX_INLINE_KEY];
        uint8_t *ptr;
    } key;
} index_entry;

/*
 * Hash index. Entries sit in fixed-size chunks, so they never move: a kv_loc
 * pointer stays valid until its key is removed. The probe table is an array
 * of 8-byte slots, (entry number + 1) | hash << 32, with linear probing and
 * backward-shift deletion: lookups compare hashes within a few cache lines
 * and only touch an entry on a match, growth rehashes without reading keys,
 * and deletes leave no tombstones behind.
 *
 * Hashing is keyed SipHash-1-3 with a random key per index, so a client of
 * the TCP server cannot pick keys that all collide (hash flooding).
 */
typedef struct {
    uint64_t *slots;
    size_t cap; /* power of two */
    size_t count;
    index_entry **chunks;
    size_t nchunks;
    uint32_t used;      /* entries handed out so far */
    uint32_t free_head; /* 1 + first free entry, 0 if none */
    size_t heap_bytes;  /* bytes of long keys stored outside entries */
    uint64_t k0, k1;    /* hash key */
} kv_index;

/* SipHash with c compression and d finalization rounds (tests use 2-4 to
 * check against the reference vectors; the index uses 1-3). */
uint64_t siphash(const void *in, size_t len, uint64_t k0, uint64_t k1, int c,
                 int d);

int index_init(kv_index *idx, size_t initial_cap);
void index_free(kv_index *idx);

/* NULL if absent. */
kv_loc *index_get(kv_index *idx, const void *key, size_t klen);

/* Find key, inserting it (with a zeroed location) if absent. Returns its
 * location for the caller to read and overwrite, or NULL if memory ran out.
 * *existed says whether the key was already present. */
kv_loc *index_upsert(kv_index *idx, const void *key, size_t klen, int *existed);

/* Remove key. Returns 1 and its old location in *old (if non-NULL), or 0 if
 * the key was absent. */
int index_remove(kv_index *idx, const void *key, size_t klen, kv_loc *old);

/* Iterate: start with *cursor = 0; returns NULL when done. */
kv_loc *index_next(kv_index *idx, size_t *cursor);

/* Bytes of memory held by the index (tables, entries, long keys). */
size_t index_memory(const kv_index *idx);

#endif
