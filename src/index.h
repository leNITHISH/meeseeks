#ifndef KV_INDEX_H
#define KV_INDEX_H

#include <stddef.h>
#include <stdint.h>

/* Where the latest record for a key lives on disk. */
typedef struct {
    uint32_t seg_id;
    uint32_t rec_size;
    uint64_t offset;
    uint64_t ts;
} kv_loc;

typedef struct {
    uint8_t *key;  /* NULL = empty slot */
    uint32_t klen;
    uint32_t hash;
    kv_loc loc;
} index_slot;

/* Open-addressing hash table, linear probing, backward-shift deletion. */
typedef struct {
    index_slot *slots;
    size_t cap;  /* power of two */
    size_t count;
} kv_index;

uint32_t index_hash(const void *key, size_t klen);

int index_init(kv_index *idx, size_t initial_cap);
void index_free(kv_index *idx);

/* NULL if absent. The returned pointer is invalidated by put/remove. */
kv_loc *index_get(kv_index *idx, const void *key, size_t klen);

/* Insert or overwrite. Copies the key on insert. 0 on success, -1 on ENOMEM. */
int index_put(kv_index *idx, const void *key, size_t klen, const kv_loc *loc);

/* 1 if removed, 0 if absent. */
int index_remove(kv_index *idx, const void *key, size_t klen);

#endif
