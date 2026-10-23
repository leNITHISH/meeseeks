#ifndef KV_RECORD_H
#define KV_RECORD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Data record (all integers little-endian):
 *
 *   offset  size  field
 *   0       4     crc32 of bytes [4, end of record)
 *   4       8     timestamp (ns since epoch)
 *   12      4     key size
 *   16      4     value size (0xFFFFFFFF = tombstone, no value bytes follow)
 *   20      ksz   key
 *   20+ksz  vsz   value
 */
#define REC_HDR_SIZE 20u
#define REC_TOMBSTONE 0xFFFFFFFFu

/*
 * Hint entry (one per live key in a compacted segment):
 *
 *   0   4    crc32 of bytes [4, end of entry)
 *   4   8    timestamp
 *   12  4    key size
 *   16  8    offset of the record in the .seg file
 *   24  4    total record size
 *   28  ksz  key
 */
#define HINT_HDR_SIZE 28u

static inline void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static inline uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32;
}

static inline uint64_t record_size(uint32_t ksz, uint32_t vsz)
{
    return (uint64_t)REC_HDR_SIZE + ksz + (vsz == REC_TOMBSTONE ? 0 : vsz);
}

/* Serialize a full record into buf (must hold record_size bytes). */
void record_encode(uint8_t *buf, uint64_t ts, const void *key, uint32_t ksz,
                   const void *val, uint32_t vsz);

/* Verify the crc of a full serialized record of length len. */
int record_crc_ok(const uint8_t *buf, uint64_t len);

/* Fill in the HINT_HDR_SIZE-byte header of a hint entry. The key itself
 * follows the header in the file; it is passed separately because the
 * checksum covers it too, so callers need not copy it next to the header. */
void hint_encode_header(uint8_t *hdr, uint64_t ts, const void *key,
                        uint32_t ksz, uint64_t offset, uint32_t rec_size);

int hint_crc_ok(const uint8_t *buf, uint64_t len);

#endif
