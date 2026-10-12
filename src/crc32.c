#include "crc32.h"

#include <pthread.h>

static uint32_t table[256];
static pthread_once_t table_once = PTHREAD_ONCE_INIT;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    pthread_once(&table_once, build_table);
    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
