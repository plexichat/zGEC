#ifndef ZGEC_CRC32C_H
#define ZGEC_CRC32C_H

#include "zgec_common.h"

/* Castagnoli CRC-32C (polynomial 0x1EDC6F41, reflected,
   initial value and final XOR 0xFFFFFFFF), per zGEC section 2.1. */

/* One-shot CRC32C. crc is the initial value (use 0 for a fresh checksum;
   the function applies the standard init/final XOR internally). */
uint32_t zgec_crc32c(const void *data, size_t len, uint32_t crc);

/* Streaming CRC32C. crc is the running state (start with 0). */
uint32_t zgec_crc32c_init(void);
uint32_t zgec_crc32c_update(uint32_t crc, const void *data, size_t len);
uint32_t zgec_crc32c_final(uint32_t crc);

#endif /* ZGEC_CRC32C_H */
