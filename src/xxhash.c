#include "zgec_xxhash.h"

#include <string.h>

/* XXH64 (seed 0 by default), per zGEC section 4.4.
   Reference implementation of the xxHash64 algorithm
   (Y. Collet, BSD-2-Clause).

   Every loop below bounds itself with a remaining-byte or remaining-stripe
   count rather than by comparing a pointer against `start + length`. Forming
   a pointer past one-past-the-end of the object is undefined behaviour in C
   even when the pointer is only compared, so the length form is the portable
   spelling; it is also what lets each entry point handle a null buffer
   without forming a pointer from it. */

#define XXH_P1 0x9E3779B185EBCA87ULL
#define XXH_P2 0xC2B2AE3D27D4EB4FULL
#define XXH_P3 0x165667B19E3779F9ULL
#define XXH_P4 0x85EBCA77C2B2AE63ULL
#define XXH_P5 0x27D4EB2F165667C5ULL

/* The pending-input window: mem[] is 32 bytes and memused is the number
 * of them in use. Only init/update write it, and both keep it in
 * [0, ZGEC_XXH_MEM_MAX]; anything above that is a corrupt state, not a
 * length.
 *
 * The constant restates a fact about a type this file does not own, so anchor
 * it to that type: if mem[] ever changes size in the header, the guards below
 * would silently start rejecting valid states (update) or hashing a truncated
 * stream (final), with a clean build and a passing suite -- the streaming API
 * has no callers today, so nothing would notice. */
#define ZGEC_XXH_MEM_MAX 31u

_Static_assert(sizeof(((zgec_xxh64_state *)0)->mem) == 32u,
               "zgec_xxh64_state.mem[] size changed; ZGEC_XXH_MEM_MAX must follow");

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

/* Tail: the last rem bytes (< 32) of an input, after h carries the length. */
static inline uint64_t zgec_xxh64_tail(uint64_t h, const uint8_t *p, size_t rem)
{
    while (rem >= 8) {
        uint64_t k1 = zgec_xxh64_round(0, zgec_rd64(p));
        h ^= k1;
        h = zgec_rotl64(h, 27) * XXH_P1 + XXH_P4;
        p += 8;
        rem -= 8;
    }
    if (rem >= 4) {
        h ^= (uint64_t)zgec_rd32(p) * XXH_P1;
        h = zgec_rotl64(h, 23) * XXH_P2 + XXH_P3;
        p += 4;
        rem -= 4;
    }
    while (rem != 0) {
        h ^= (uint64_t)(*p++) * XXH_P5;
        h = zgec_rotl64(h, 11) * XXH_P1;
        --rem;
    }
    return h;
}

uint64_t zgec_xxh64(const void *data, size_t len, uint64_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t rem = len;
    uint64_t h;

    /* Nothing can be read, so form no pointer from data at all: the empty
     * input hashes to finalize(seed + P5) on the general path too, which is
     * exactly what this returns. */
    if (len == 0) return zgec_xxh64_finalize(seed + XXH_P5);

    /* Non-empty input with no buffer is a caller error. This API has no error
     * channel, so the only defined answer is a sentinel -- and note that 0 is
     * a value a CORRECT hash can also produce, so it is not distinguishable
     * from a real digest. No caller in the library can reach this path (every
     * one passes a buffer for a non-zero length). The honest fix is a
     * documented precondition, or a checked wrapper returning zgec_err, on the
     * declarations in include/zgec_xxhash.h; this file cannot add either, so
     * the header work is recorded as deferred rather than papered over. */
    if (!p) return 0;

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
            rem -= 32;
        } while (rem >= 32);
        h = zgec_rotl64(v1, 1) + zgec_rotl64(v2, 7)
          + zgec_rotl64(v3, 12) + zgec_rotl64(v4, 18);
        h = zgec_xxh64_mergeRound(h, v1);
        h = zgec_xxh64_mergeRound(h, v2);
        h = zgec_xxh64_mergeRound(h, v3);
        h = zgec_xxh64_mergeRound(h, v4);
    } else {
        h = seed + XXH_P5;
    }

    /* The total input length, not what is left after the stripes. */
    h += (uint64_t)len;

    return zgec_xxh64_finalize(zgec_xxh64_tail(h, p, rem));
}

void zgec_xxh64_init(zgec_xxh64_state *s, uint64_t seed)
{
    if (!s) return;

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

    if (!s) return;

    /* A state written only by init/update holds at most 31 pending bytes.
     * A larger count means corruption, and continuing would write past
     * mem[32]; leave the state untouched rather than reading or writing out
     * of bounds. */
    if (s->memused > ZGEC_XXH_MEM_MAX) return;

    /* A zero-length update changes nothing (total += 0 is a no-op), and
     * returning here keeps a null buffer out of memcpy(). */
    if (len == 0) return;

    if (!p) return;

    s->total += len;

    /* len < 32 - memused rather than memused + len < 32: memused <= 31 here,
     * so the subtraction cannot wrap and a near-SIZE_MAX len cannot wrap the
     * sum into passing this test and then overflow mem[] in the memcpy. */
    if (len < 32 - s->memused) {
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
        /* Whole stripes only; the accumulators stay in locals for the loop
         * and are written back once (the values are identical either way). */
        size_t stripes = len & ~(size_t)31;
        uint64_t v1 = s->v1;
        uint64_t v2 = s->v2;
        uint64_t v3 = s->v3;
        uint64_t v4 = s->v4;
        while (stripes != 0) {
            v1 = zgec_xxh64_round(v1, zgec_rd64(p)); p += 8;
            v2 = zgec_xxh64_round(v2, zgec_rd64(p)); p += 8;
            v3 = zgec_xxh64_round(v3, zgec_rd64(p)); p += 8;
            v4 = zgec_xxh64_round(v4, zgec_rd64(p)); p += 8;
            stripes -= 32;
        }
        s->v1 = v1;
        s->v2 = v2;
        s->v3 = v3;
        s->v4 = v4;
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
    const uint8_t *p;
    size_t rem;

    if (!s) return 0;

    /* A state written only by init/update holds memused <= ZGEC_XXH_MEM_MAX.
     * A larger count means corruption, and update() refuses to touch such a
     * state -- so final() must refuse it too. Returning a digest built from a
     * stream whose pending bytes were dropped is a plausible wrong answer with
     * no signal, which is worse than no answer; 0 is the same sentinel this
     * function already returns for a null state. It also keeps the tail loop
     * from reading past mem[32]. */
    if (s->memused > ZGEC_XXH_MEM_MAX) return 0;

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

    /* memused is in [0, ZGEC_XXH_MEM_MAX] here, proved by the guard above and
     * by init/update, so mem[] is always read inside its 32 bytes. */
    p = s->mem;
    rem = s->memused;

    return zgec_xxh64_finalize(zgec_xxh64_tail(h, p, rem));
}
