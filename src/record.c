#include "record.h"

#include <string.h>

#include "crc32.h"

void record_encode(uint8_t *buf, uint64_t ts, const void *key, uint32_t ksz,
                   const void *val, uint32_t vsz)
{
    put_u64(buf + 4, ts);
    put_u32(buf + 12, ksz);
    put_u32(buf + 16, vsz);
    memcpy(buf + REC_HDR_SIZE, key, ksz);
    if (vsz != REC_TOMBSTONE && vsz > 0)
        memcpy(buf + REC_HDR_SIZE + ksz, val, vsz);
    uint64_t len = record_size(ksz, vsz);
    put_u32(buf, crc32_update(0, buf + 4, (size_t)(len - 4)));
}

int record_crc_ok(const uint8_t *buf, uint64_t len)
{
    return len >= REC_HDR_SIZE &&
           get_u32(buf) == crc32_update(0, buf + 4, (size_t)(len - 4));
}

void hint_encode(uint8_t *buf, uint64_t ts, const void *key, uint32_t ksz,
                 uint64_t offset, uint32_t rec_size)
{
    put_u64(buf + 4, ts);
    put_u32(buf + 12, ksz);
    put_u64(buf + 16, offset);
    put_u32(buf + 24, rec_size);
    memcpy(buf + HINT_HDR_SIZE, key, ksz);
    put_u32(buf, crc32_update(0, buf + 4, HINT_HDR_SIZE - 4 + ksz));
}

int hint_crc_ok(const uint8_t *buf, uint64_t len)
{
    return len >= HINT_HDR_SIZE &&
           get_u32(buf) == crc32_update(0, buf + 4, (size_t)(len - 4));
}
