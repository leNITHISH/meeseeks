#include "crc32.h"

#include <pthread.h>

/*
 * Slice-by-8: eight lookup tables let the loop fold in 8 bytes per step
 * instead of 1, several times faster than the classic byte-at-a-time table
 * (it matters for replay and compaction, which checksum every byte they
 * read). Loads are assembled byte by byte, so this is endian-independent;
 * compilers turn them into plain loads on little-endian machines.
 */
static uint32_t table[8][256];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++)
            table[t][i] = (table[t - 1][i] >> 8) ^ table[0][table[t - 1][i] & 0xFF];
}

static inline uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    pthread_once(&table_once, build_table);
    const uint8_t *p = data;
    crc = ~crc;
    while (len >= 8) {
        uint32_t a = crc ^ load_le32(p), b = load_le32(p + 4);
        crc = table[7][a & 0xFF] ^ table[6][(a >> 8) & 0xFF] ^
              table[5][(a >> 16) & 0xFF] ^ table[4][a >> 24] ^
              table[3][b & 0xFF] ^ table[2][(b >> 8) & 0xFF] ^
              table[1][(b >> 16) & 0xFF] ^ table[0][b >> 24];
        p += 8;
        len -= 8;
    }
    while (len--)
        crc = table[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
