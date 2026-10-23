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

typedef struct {
    uint8_t *key; /* NULL = empty slot */
    uint32_t klen;
    uint32_t hash;
    kv_loc loc;
} index_slot;

/* Open-addressing hash table, linear probing, backward-shift deletion (no
 * tombstones, so lookups never degrade after many deletes). */
typedef struct {
    index_slot *slots;
    size_t cap; /* power of two */
    size_t count;
} kv_index;

uint32_t index_hash(const void *key, size_t klen);

int index_init(kv_index *idx, size_t initial_cap);
void index_free(kv_index *idx);

/* NULL if absent. The pointer stays valid until the next upsert or remove. */
kv_loc *index_get(kv_index *idx, const void *key, size_t klen);

/* Find key, inserting it (with a zeroed location) if absent. Returns the slot's
 * location for the caller to read and overwrite, or NULL if memory ran out.
 * *existed says whether the key was already present. */
kv_loc *index_upsert(kv_index *idx, const void *key, size_t klen, int *existed);

/* Remove key. Returns 1 and its old location in *old (if non-NULL), or 0 if
 * the key was absent. */
int index_remove(kv_index *idx, const void *key, size_t klen, kv_loc *old);

#endif
