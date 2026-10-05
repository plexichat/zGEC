#include "zgec_xxhash.h"

#include <string.h>

/* XXH64 (seed 0 by default), per zGEC section 4.4.
   Reference implementation of the xxHash64 algorithm
   (Y. Collet, BSD-2-Clause). */

#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

static inline uint64_t zgec_rotl64(uint64_t x, unsigned r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t zgec_xxh64_round(uint64_t acc, uint64_t input)
{
    acc += input * XXH_P2;
    acc = zgec_rotl64(acc, 31);
    acc *= XXH_P1;
    return acc;
}

static inline uint64_t zgec_xxh64_mergeRound(uint64_t acc, uint64_t val)
{
    val = zgec_xxh64_round(0, val);
    acc ^= val;
    acc = acc * XXH_P1 + XXH_P4;
    return acc;
}

static inline uint64_t zgec_xxh64_finalize(uint64_t h)
{
    h ^= h >> 33;
    h *= XXH_P2;
    h ^= h >> 29;
    h *= XXH_P3;
    h ^= h >> 32;
    return h;
}

uint64_t zgec_xxh64(const void *data, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + len;
    uint64_t h;

    if (len >= 32) {
        uint64_t v1 = seed + XXH_P1 + XXH_P2;
        uint64_t v2 = seed + XXH_P2;
        uint64_t v3 = seed;
        uint64_t v4 = seed - XXH_P1;
        do {
            v1 = zgec_xxh64_round(v1, zgec_rd64(p)); p += 8;
            v2 = zgec_xxh64_round(v2, zgec_rd64(p)); p += 8;
            v3 = zgec_xxh64_round(v3, zgec_rd64(p)); p += 8;
            v4 = zgec_xxh64_round(v4, zgec_rd64(p)); p += 8;
        } while (p + 32 <= end);
        h = zgec_rotl64(v1, 1) + zgec_rotl64(v2, 7)
          + zgec_rotl64(v3, 12) + zgec_rotl64(v4, 18);
        h = zgec_xxh64_mergeRound(h, v1);
        h = zgec_xxh64_mergeRound(h, v2);
        h = zgec_xxh64_mergeRound(h, v3);
        h = zgec_xxh64_mergeRound(h, v4);
    } else {
        h = seed + XXH_P5;
    }

    h += (uint64_t)len;

    while (p + 8 <= end) {
        uint64_t k1 = zgec_xxh64_round(0, zgec_rd64(p));
        h ^= k1;
        h = zgec_rotl64(h, 27) * XXH_P1 + XXH_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)zgec_rd32(p) * XXH_P1;
        h = zgec_rotl64(h, 23) * XXH_P2 + XXH_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p++) * XXH_P5;
        h = zgec_rotl64(h, 11) * XXH_P1;
    }

    return zgec_xxh64_finalize(h);
}

void zgec_xxh64_init(zgec_xxh64_state *s, uint64_t seed)
{
    s->total = 0;
    s->v1 = seed + XXH_P1 + XXH_P2;
    s->v2 = seed + XXH_P2;
    s->v3 = seed;
    s->v4 = seed - XXH_P1;
    s->memused = 0;
}

void zgec_xxh64_update(zgec_xxh64_state *s, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    s->total += len;

    if (s->memused + len < 32) {
        memcpy(s->mem + s->memused, p, len);
        s->memused += len;
        return;
    }

    if (s->memused) {
        size_t fill = 32 - s->memused;
        memcpy(s->mem + s->memused, p, fill);
        const uint8_t *q = s->mem;
        s->v1 = zgec_xxh64_round(s->v1, zgec_rd64(q)); q += 8;
        s->v2 = zgec_xxh64_round(s->v2, zgec_rd64(q)); q += 8;
        s->v3 = zgec_xxh64_round(s->v3, zgec_rd64(q)); q += 8;
        s->v4 = zgec_xxh64_round(s->v4, zgec_rd64(q));
        p += fill;
        len -= fill;
        s->memused = 0;
    }

    if (len >= 32) {
        const uint8_t *end = p + (len & ~(size_t)31);
        do {
            s->v1 = zgec_xxh64_round(s->v1, zgec_rd64(p)); p += 8;
            s->v2 = zgec_xxh64_round(s->v2, zgec_rd64(p)); p += 8;
            s->v3 = zgec_xxh64_round(s->v3, zgec_rd64(p)); p += 8;
            s->v4 = zgec_xxh64_round(s->v4, zgec_rd64(p)); p += 8;
        } while (p < end);
        len &= 31;
    }

    if (len) {
        memcpy(s->mem, p, len);
        s->memused = len;
    }
}

uint64_t zgec_xxh64_final(zgec_xxh64_state *s)
{
    uint64_t h;
    if (s->total >= 32) {
        h = zgec_rotl64(s->v1, 1) + zgec_rotl64(s->v2, 7)
          + zgec_rotl64(s->v3, 12) + zgec_rotl64(s->v4, 18);
        h = zgec_xxh64_mergeRound(h, s->v1);
        h = zgec_xxh64_mergeRound(h, s->v2);
        h = zgec_xxh64_mergeRound(h, s->v3);
        h = zgec_xxh64_mergeRound(h, s->v4);
    } else {
        h = s->v3 + XXH_P5;   /* v3 == seed when no stripe was processed */
    }
    h += s->total;

    const uint8_t *p = s->mem;
    const uint8_t *end = p + s->memused;
    while (p + 8 <= end) {
        uint64_t k1 = zgec_xxh64_round(0, zgec_rd64(p));
        h ^= k1;
        h = zgec_rotl64(h, 27) * XXH_P1 + XXH_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)zgec_rd32(p) * XXH_P1;
        h = zgec_rotl64(h, 23) * XXH_P2 + XXH_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (uint64_t)(*p++) * XXH_P5;
        h = zgec_rotl64(h, 11) * XXH_P1;
    }

    return zgec_xxh64_finalize(h);
}
