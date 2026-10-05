#ifndef ZGEC_XXHASH_H
#define ZGEC_XXHASH_H

#include "zgec_common.h"

/* XXH64 (seed 0 by default), per zGEC section 4.4 (dictionary content_hash). */

uint64_t zgec_xxh64(const void *data, size_t len, uint64_t seed);

/* Streaming XXH64. */
typedef struct {
    uint64_t total;   /* total bytes hashed */
    uint64_t v1, v2, v3, v4;
    uint8_t mem[32];    /* up to 32 bytes of pending input */
    size_t   memused; /* bytes in mem (0..31) */
} zgec_xxh64_state;

void zgec_xxh64_init(zgec_xxh64_state *s, uint64_t seed);
void zgec_xxh64_update(zgec_xxh64_state *s, const void *data, size_t len);
uint64_t zgec_xxh64_final(zgec_xxh64_state *s);

#endif /* ZGEC_XXHASH_H */
