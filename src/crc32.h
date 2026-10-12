#ifndef KV_CRC32_H
#define KV_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32/IEEE (the zlib/PNG polynomial, reflected 0xEDB88320).
 * Chain calls by passing the previous result as `crc`; start with 0. */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif
