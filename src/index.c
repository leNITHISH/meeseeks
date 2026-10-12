#include "index.h"

#include <stdlib.h>
#include <string.h>

uint32_t index_hash(const void *key, size_t klen)
{
    /* FNV-1a */
    const uint8_t *p = key;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < klen; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

int index_init(kv_index *idx, size_t initial_cap)
{
    size_t cap = 16;
    while (cap < initial_cap)
        cap <<= 1;
    idx->slots = calloc(cap, sizeof(index_slot));
    if (!idx->slots)
        return -1;
    idx->cap = cap;
    idx->count = 0;
    return 0;
}

void index_free(kv_index *idx)
{
    if (!idx->slots)
        return;
    for (size_t i = 0; i < idx->cap; i++)
        free(idx->slots[i].key);
    free(idx->slots);
    idx->slots = NULL;
    idx->cap = idx->count = 0;
}

static size_t find_slot(const kv_index *idx, const void *key, size_t klen,
                        uint32_t h)
{
    size_t mask = idx->cap - 1;
    size_t i = h & mask;
    for (;;) {
        index_slot *s = &idx->slots[i];
        if (!s->key)
            return i;
        if (s->hash == h && s->klen == klen && memcmp(s->key, key, klen) == 0)
            return i;
        i = (i + 1) & mask;
    }
}

static int grow(kv_index *idx)
{
    size_t ncap = idx->cap * 2;
    index_slot *ns = calloc(ncap, sizeof(index_slot));
    if (!ns)
        return -1;
    size_t mask = ncap - 1;
    for (size_t i = 0; i < idx->cap; i++) {
        index_slot *s = &idx->slots[i];
        if (!s->key)
            continue;
        size_t j = s->hash & mask;
        while (ns[j].key)
            j = (j + 1) & mask;
        ns[j] = *s;
    }
    free(idx->slots);
    idx->slots = ns;
    idx->cap = ncap;
    return 0;
}

kv_loc *index_get(kv_index *idx, const void *key, size_t klen)
{
    size_t i = find_slot(idx, key, klen, index_hash(key, klen));
    return idx->slots[i].key ? &idx->slots[i].loc : NULL;
}

int index_put(kv_index *idx, const void *key, size_t klen, const kv_loc *loc)
{
    /* keep load factor <= 0.7 */
    if ((idx->count + 1) * 10 > idx->cap * 7 && grow(idx) != 0)
        return -1;
    uint32_t h = index_hash(key, klen);
    size_t i = find_slot(idx, key, klen, h);
    index_slot *s = &idx->slots[i];
    if (!s->key) {
        /* +1 so zero-length keys still get a non-NULL pointer */
        uint8_t *k = malloc(klen + 1);
        if (!k)
            return -1;
        memcpy(k, key, klen);
        s->key = k;
        s->klen = (uint32_t)klen;
        s->hash = h;
        idx->count++;
    }
    s->loc = *loc;
    return 0;
}

int index_remove(kv_index *idx, const void *key, size_t klen)
{
    size_t i = find_slot(idx, key, klen, index_hash(key, klen));
    if (!idx->slots[i].key)
        return 0;
    free(idx->slots[i].key);
    idx->slots[i].key = NULL;
    idx->count--;

    /* Backward-shift: pull later entries in the probe run into the hole. */
    size_t mask = idx->cap - 1;
    size_t hole = i;
    size_t j = (i + 1) & mask;
    while (idx->slots[j].key) {
        size_t home = idx->slots[j].hash & mask;
        /* move j into hole if home is not cyclically in (hole, j] */
        int movable = (hole <= j) ? (home <= hole || home > j)
                                  : (home <= hole && home > j);
        if (movable) {
            idx->slots[hole] = idx->slots[j];
            idx->slots[j].key = NULL;
            hole = j;
        }
        j = (j + 1) & mask;
    }
    return 1;
}
