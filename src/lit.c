#include "zgec_lit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---- Annex C classification ---- */

static const uint8_t text_table[256] = {
    [0x00] = 2,
    [0x09] = 1, [0x0A] = 0, [0x0D] = 1,
    /* Annex C: other 0x01-0x1F => 3 (remaining control characters). */
    [0x01] = 3, [0x02] = 3, [0x03] = 3, [0x04] = 3,
    [0x05] = 3, [0x06] = 3, [0x07] = 3, [0x08] = 3,
    [0x0B] = 3, [0x0C] = 3,
    [0x0E] = 3, [0x0F] = 3, [0x10] = 3, [0x11] = 3,
    [0x12] = 3, [0x13] = 3, [0x14] = 3, [0x15] = 3,
    [0x16] = 3, [0x17] = 3, [0x18] = 3, [0x19] = 3,
    [0x1A] = 3, [0x1B] = 3, [0x1C] = 3, [0x1D] = 3,
    [0x1E] = 3, [0x1F] = 3,
    [0x20] = 4, [0x21] = 5, [0x22] = 6, [0x23] = 7,
    [0x24] = 8, [0x25] = 9, [0x26] = 10, [0x27] = 11,
    [0x28] = 12, [0x29] = 13, [0x2A] = 14, [0x2B] = 15,
    [0x2C] = 16, [0x2D] = 17, [0x2E] = 18, [0x2F] = 19,
    [0x30] = 20, [0x31] = 20, [0x32] = 20, [0x33] = 20,
    [0x34] = 20, [0x35] = 20, [0x36] = 20, [0x37] = 20,
    [0x38] = 20, [0x39] = 20,
    [0x3A] = 21, [0x3B] = 22, [0x3C] = 23, [0x3D] = 24,
    [0x3E] = 25, [0x3F] = 26, [0x40] = 27,
    [0x41] = 28, [0x42] = 28, [0x43] = 28, [0x44] = 28,
    [0x45] = 28, [0x46] = 28, [0x47] = 28, [0x48] = 28,
    [0x49] = 28, [0x4A] = 28, [0x4B] = 28, [0x4C] = 28,
    [0x4D] = 28, [0x4E] = 28, [0x4F] = 28, [0x50] = 28,
    [0x51] = 28, [0x52] = 28, [0x53] = 28, [0x54] = 28,
    [0x55] = 28, [0x56] = 28, [0x57] = 28, [0x58] = 28,
    [0x59] = 28, [0x5A] = 28,
    [0x5B] = 29, [0x5C] = 30, [0x5D] = 31, [0x5E] = 32,
    [0x5F] = 33, [0x60] = 34,
    [0x61] = 35, [0x65] = 36, [0x69] = 37, [0x6F] = 38,
    [0x75] = 39,
    [0x62] = 40, [0x63] = 40, [0x64] = 40, [0x66] = 40,
    [0x67] = 40, [0x68] = 40, [0x6A] = 40, [0x6B] = 40,
    [0x6C] = 40, [0x6D] = 40, [0x6E] = 40, [0x70] = 40,
    [0x71] = 40, [0x72] = 40, [0x73] = 40, [0x74] = 40,
    [0x76] = 40, [0x77] = 40, [0x78] = 40, [0x79] = 40,
    [0x7A] = 40,
    [0x7B] = 41, [0x7C] = 42, [0x7D] = 43, [0x7E] = 44,
    [0x7F] = 45,
    [0x80] = 46, [0x81] = 46, [0x82] = 46, [0x83] = 46,
    [0x84] = 46, [0x85] = 46, [0x86] = 46, [0x87] = 46,
    [0x88] = 46, [0x89] = 46, [0x8A] = 46, [0x8B] = 46,
    [0x8C] = 46, [0x8D] = 46, [0x8E] = 46, [0x8F] = 46,
    [0x90] = 46, [0x91] = 46, [0x92] = 46, [0x93] = 46,
    [0x94] = 46, [0x95] = 46, [0x96] = 46, [0x97] = 46,
    [0x98] = 46, [0x99] = 46, [0x9A] = 46, [0x9B] = 46,
    [0x9C] = 46, [0x9D] = 46, [0x9E] = 46, [0x9F] = 46,
    [0xA0] = 46, [0xA1] = 46, [0xA2] = 46, [0xA3] = 46,
    [0xA4] = 46, [0xA5] = 46, [0xA6] = 46, [0xA7] = 46,
    [0xA8] = 46, [0xA9] = 46, [0xAA] = 46, [0xAB] = 46,
    [0xAC] = 46, [0xAD] = 46, [0xAE] = 46, [0xAF] = 46,
    [0xB0] = 46, [0xB1] = 46, [0xB2] = 46, [0xB3] = 46,
    [0xB4] = 46, [0xB5] = 46, [0xB6] = 46, [0xB7] = 46,
    [0xB8] = 46, [0xB9] = 46, [0xBA] = 46, [0xBB] = 46,
    [0xBC] = 46, [0xBD] = 46, [0xBE] = 46, [0xBF] = 46,
    [0xC0] = 47, [0xC1] = 47, [0xC2] = 47, [0xC3] = 47,
    [0xC4] = 47, [0xC5] = 47, [0xC6] = 47, [0xC7] = 47,
    [0xC8] = 47, [0xC9] = 47, [0xCA] = 47, [0xCB] = 47,
    [0xCC] = 47, [0xCD] = 47, [0xCE] = 47, [0xCF] = 47,
    [0xD0] = 47, [0xD1] = 47, [0xD2] = 47, [0xD3] = 47,
    [0xD4] = 47, [0xD5] = 47, [0xD6] = 47, [0xD7] = 47,
    [0xD8] = 47, [0xD9] = 47, [0xDA] = 47, [0xDB] = 47,
    [0xDC] = 47, [0xDD] = 47, [0xDE] = 47, [0xDF] = 47,
    [0xE0] = 48, [0xE1] = 48, [0xE2] = 48, [0xE3] = 48,
    [0xE4] = 48, [0xE5] = 48, [0xE6] = 48, [0xE7] = 48,
    [0xE8] = 48, [0xE9] = 48, [0xEA] = 48, [0xEB] = 48,
    [0xEC] = 48, [0xED] = 48, [0xEE] = 48, [0xEF] = 48,
    [0xF0] = 49, [0xF1] = 49, [0xF2] = 49, [0xF3] = 49,
    [0xF4] = 49, [0xF5] = 49, [0xF6] = 49, [0xF7] = 49,
    [0xF8] = 49, [0xF9] = 49, [0xFA] = 49, [0xFB] = 49,
    [0xFC] = 49, [0xFD] = 49, [0xFE] = 49, [0xFF] = 49,
};

static const uint8_t signed_table[256] = {
    [0x00] = 32, [0x01] = 33, [0x02] = 34, [0x03] = 35, [0x04] = 36, [0x05] = 37, [0x06] = 38, [0x07] = 39,
    [0x08] = 40, [0x09] = 41, [0x0A] = 42, [0x0B] = 43, [0x0C] = 44, [0x0D] = 45, [0x0E] = 46, [0x0F] = 47,
    [0x10] = 48, [0x11] = 49, [0x12] = 50, [0x13] = 51, [0x14] = 52, [0x15] = 53, [0x16] = 54, [0x17] = 55,
    [0x18] = 56, [0x19] = 57, [0x1A] = 58, [0x1B] = 59, [0x1C] = 60, [0x1D] = 61, [0x1E] = 62, [0x1F] = 63,
    [0x20] = 63, [0x21] = 63, [0x22] = 63, [0x23] = 63, [0x24] = 63, [0x25] = 63, [0x26] = 63, [0x27] = 63,
    [0x28] = 63, [0x29] = 63, [0x2A] = 63, [0x2B] = 63, [0x2C] = 63, [0x2D] = 63, [0x2E] = 63, [0x2F] = 63,
    [0x30] = 63, [0x31] = 63, [0x32] = 63, [0x33] = 63, [0x34] = 63, [0x35] = 63, [0x36] = 63, [0x37] = 63,
    [0x38] = 63, [0x39] = 63, [0x3A] = 63, [0x3B] = 63, [0x3C] = 63, [0x3D] = 63, [0x3E] = 63, [0x3F] = 63,
    [0x40] = 63, [0x41] = 63, [0x42] = 63, [0x43] = 63, [0x44] = 63, [0x45] = 63, [0x46] = 63, [0x47] = 63,
    [0x48] = 63, [0x49] = 63, [0x4A] = 63, [0x4B] = 63, [0x4C] = 63, [0x4D] = 63, [0x4E] = 63, [0x4F] = 63,
    [0x50] = 63, [0x51] = 63, [0x52] = 63, [0x53] = 63, [0x54] = 63, [0x55] = 63, [0x56] = 63, [0x57] = 63,
    [0x58] = 63, [0x59] = 63, [0x5A] = 63, [0x5B] = 63, [0x5C] = 63, [0x5D] = 63, [0x5E] = 63, [0x5F] = 63,
    [0x60] = 63, [0x61] = 63, [0x62] = 63, [0x63] = 63, [0x64] = 63, [0x65] = 63, [0x66] = 63, [0x67] = 63,
    [0x68] = 63, [0x69] = 63, [0x6A] = 63, [0x6B] = 63, [0x6C] = 63, [0x6D] = 63, [0x6E] = 63, [0x6F] = 63,
    [0x70] = 63, [0x71] = 63, [0x72] = 63, [0x73] = 63, [0x74] = 63, [0x75] = 63, [0x76] = 63, [0x77] = 63,
    [0x78] = 63, [0x79] = 63, [0x7A] = 63, [0x7B] = 63, [0x7C] = 63, [0x7D] = 63, [0x7E] = 63, [0x7F] = 63,
    [0x80] = 0,  [0x81] = 0,  [0x82] = 0,  [0x83] = 0,  [0x84] = 0,  [0x85] = 0,  [0x86] = 0,  [0x87] = 0,
    [0x88] = 0,  [0x89] = 0,  [0x8A] = 0,  [0x8B] = 0,  [0x8C] = 0,  [0x8D] = 0,  [0x8E] = 0,  [0x8F] = 0,
    [0x90] = 0,  [0x91] = 0,  [0x92] = 0,  [0x93] = 0,  [0x94] = 0,  [0x95] = 0,  [0x96] = 0,  [0x97] = 0,
    [0x98] = 0,  [0x99] = 0,  [0x9A] = 0,  [0x9B] = 0,  [0x9C] = 0,  [0x9D] = 0,  [0x9E] = 0,  [0x9F] = 0,
    [0xA0] = 0,  [0xA1] = 0,  [0xA2] = 0,  [0xA3] = 0,  [0xA4] = 0,  [0xA5] = 0,  [0xA6] = 0,  [0xA7] = 0,
    [0xA8] = 0,  [0xA9] = 0,  [0xAA] = 0,  [0xAB] = 0,  [0xAC] = 0,  [0xAD] = 0,  [0xAE] = 0,  [0xAF] = 0,
    [0xB0] = 0,  [0xB1] = 0,  [0xB2] = 0,  [0xB3] = 0,  [0xB4] = 0,  [0xB5] = 0,  [0xB6] = 0,  [0xB7] = 0,
    [0xB8] = 0,  [0xB9] = 0,  [0xBA] = 0,  [0xBB] = 0,  [0xBC] = 0,  [0xBD] = 0,  [0xBE] = 0,  [0xBF] = 0,
    [0xC0] = 0,  [0xC1] = 0,  [0xC2] = 0,  [0xC3] = 0,  [0xC4] = 0,  [0xC5] = 0,  [0xC6] = 0,  [0xC7] = 0,
    [0xC8] = 0,  [0xC9] = 0,  [0xCA] = 0,  [0xCB] = 0,  [0xCC] = 0,  [0xCD] = 0,  [0xCE] = 0,  [0xCF] = 0,
    [0xD0] = 0,  [0xD1] = 0,  [0xD2] = 0,  [0xD3] = 0,  [0xD4] = 0,  [0xD5] = 0,  [0xD6] = 0,  [0xD7] = 0,
    [0xD8] = 0,  [0xD9] = 0,  [0xDA] = 0,  [0xDB] = 0,  [0xDC] = 0,  [0xDD] = 0,  [0xDE] = 0,  [0xDF] = 0,
    [0xE0] = 0,  [0xE1] = 1,  [0xE2] = 2,  [0xE3] = 3,  [0xE4] = 4,  [0xE5] = 5,  [0xE6] = 6,  [0xE7] = 7,
    [0xE8] = 8,  [0xE9] = 9,  [0xEA] = 10, [0xEB] = 11, [0xEC] = 12, [0xED] = 13, [0xEE] = 14, [0xEF] = 15,
    [0xF0] = 16, [0xF1] = 17, [0xF2] = 18, [0xF3] = 19, [0xF4] = 20, [0xF5] = 21, [0xF6] = 22, [0xF7] = 23,
    [0xF8] = 24, [0xF9] = 25, [0xFA] = 26, [0xFB] = 27, [0xFC] = 28, [0xFD] = 29, [0xFE] = 30, [0xFF] = 31,
};

unsigned zgec_classify(int ctx_mode, uint8_t b)
{
    switch (ctx_mode) {
    case 0:
        return 0;
    case 1:
        return (unsigned)(b & 63u);
    case 2:
        return (unsigned)(b >> 2u);
    case 3:
        return (unsigned)text_table[b];
    case 4:
        return (unsigned)signed_table[b];
    default:
        return 0;
    }
}

/* ---- class map ---- */

zgec_err zgec_class_map_decode(uint8_t *map,
                                const uint8_t *packed,
                                int k)
{
    if (map == NULL || packed == NULL) return ZGEC_ERR_INVAL;
    if (k != 1 && k != 2 && k != 4 && k != 8) return ZGEC_ERR_CTX_COUNT;
    for (int g = 0; g < 8; g++) {
        const uint8_t *p = packed + (size_t)g * 3u;
        uint32_t val24 = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        for (int j = 0; j < 8; j++) {
            uint8_t entry = (uint8_t)((val24 >> (unsigned)(j * 3)) & 7u);
            if ((int)entry >= k) return ZGEC_ERR_CLASS_MAP;
            map[(size_t)g * 8u + (size_t)j] = entry;
        }
    }
    return ZGEC_OK;
}

void zgec_class_map_encode(uint8_t *packed,
                           const uint8_t *map)
{
    if (packed == NULL || map == NULL) return;
    /* Every one of the 24 bytes is stored by the loop below (8 groups x
     * 3 bytes), so there is nothing to pre-clear. Entries are masked to
     * their low three bits -- 255 becomes 7 and 8 becomes 0, i.e. they
     * are wrapped modulo 8, not clamped into range. This is the
     * trusted-input fast path, and an entry that is not a legal class
     * for the block's k is not rejected here: zgec_class_map_decode
     * (above) rejects such a stream with ZGEC_ERR_CLASS_MAP, so a
     * wrapped entry can turn an encode into a stream its own decoder
     * refuses. The checked variant that would take k and validate every
     * entry needs a declaration in include/zgec_lit.h. */
    for (int g = 0; g < 8; g++) {
        uint8_t *p = packed + (size_t)g * 3u;
        uint32_t val24 = 0;
        for (int j = 0; j < 8; j++) {
            val24 |= ((uint32_t)(map[(size_t)g * 8u + (size_t)j] & 7u)) << (unsigned)(j * 3);
        }
        p[0] = (uint8_t)(val24 & 0xFFu);
        p[1] = (uint8_t)((val24 >> 8) & 0xFFu);
        p[2] = (uint8_t)((val24 >> 16) & 0xFFu);
    }
}

/* ---- run-start bitmap ---- */

zgec_err zgec_lit_runstart(uint8_t *runstart,
                           size_t n_lit,
                           const uint32_t *ll,
                           size_t n_seq)
{
    /* Caller contract: ll[0..n_seq-1] are the literal lengths and
     * n_lit is the total literal count (tail = n_lit - sum(ll)).
     * The tail start index sum(ll) is itself a run start when a tail
     * exists (sum(ll) < n_lit), per section 9.3.
     *
     * The lengths are validated in full before any byte of runstart is
     * written, so a rejected call leaves the output untouched, and the
     * running position only ever advances by an amount already proved
     * to fit, so it cannot wrap size_t (V1). */
    if (n_lit == 0) {
        /* No literals: every length must be zero. Comparing them one by
         * one avoids accumulating a sum that could itself wrap. */
        if (n_seq > 0 && ll == NULL) return ZGEC_ERR_INVAL;
        for (size_t i = 0; i < n_seq; i++) {
            if (ll[i] != 0) return ZGEC_ERR_LL_SUM;
        }
        return ZGEC_OK;
    }
    if (runstart == NULL) return ZGEC_ERR_INVAL;
    if (n_seq > 0 && ll == NULL) return ZGEC_ERR_INVAL;

    /* Pass 1: prove sum(ll) <= n_lit without touching runstart. */
    {
        size_t vpos = 0;
        for (size_t vi = 0; vi < n_seq; vi++) {
            size_t vadd = (size_t)ll[vi];
            if (vadd > n_lit - vpos) return ZGEC_ERR_LL_SUM;
            vpos += vadd;
        }
    }

    /* Pass 2: fill. Pass 1 proved pos <= n_lit at every step, so both
     * the index below and the advance are in range. */
    zgec_rs_clear(runstart, n_lit);
    size_t pos = 0;
    for (size_t i = 0; i < n_seq; i++) {
        size_t add = (size_t)ll[i];
        if (add != 0) {
            zgec_rs_set(runstart, pos);
        }
        pos += add;
    }
    /* Tail start (j == sum LL) when tail literals exist. */
    if (pos < n_lit) {
        zgec_rs_set(runstart, pos);
    }
    return ZGEC_OK;
}

/* ---- lane starts ---- */

/* Section 9.2 cuts Z into exactly eight contiguous lanes, so the lane
 * divisor below is the lane count and not an arbitrary eight. */
_Static_assert(ZGEC_NLANES == 8, "literal lane layout requires eight lanes");

void zgec_lit_lane_starts(size_t start[ZGEC_NLANES],
                          size_t n_lit)
{
    size_t q;
    size_t r;
    if (start == NULL) return;
    q = n_lit / ZGEC_NLANES;
    r = n_lit % ZGEC_NLANES;
    for (unsigned lane = 0; lane < ZGEC_NLANES; lane++) {
        start[lane] = (size_t)lane * q + ((size_t)lane < r ? (size_t)lane : r);
    }
}

void zgec_lit_lane_geom(size_t start[ZGEC_NLANES], size_t len[ZGEC_NLANES],
                        size_t n_lit)
{
    size_t q = n_lit / ZGEC_NLANES;
    size_t r = n_lit % ZGEC_NLANES;
    for (unsigned lane = 0; lane < ZGEC_NLANES; lane++) {
        if (start != NULL)
            start[lane] = (size_t)lane * q + ((size_t)lane < r ? (size_t)lane : r);
        if (len != NULL)
            len[lane] = q + ((size_t)lane < r ? 1u : 0u);
    }
}

/* ---- sub-literal reconstruction ---- */

void zgec_lit_sub_reconstruct(uint8_t *Z,
                              size_t n_lit,
                              const uint32_t *rep0_before,
                              const uint32_t *ll,
                              size_t n_seq,
                              size_t tail,
                              size_t vbpos)
{
    /* Caller contract: Z points into the virtual buffer at VB offset
     * vbpos (Z == VB + vbpos), so the spec predictor VB[vbpos + pos -
     * rep0] (section 9.6, zgec_lit_sub_predict) is available as
     * *(Z + pos - rep0) with a signed offset. Predictors that fall
     * before the segment therefore read the dictionary /
     * literal-reference prefix via negative offsets; pred is 0 when
     * rep0 exceeds the current VB write position. Reconstruction is
     * sequential (scalar path, conforming; the rep0 >= 32 vector add
     * of Annex D is an optimisation only): later bytes of a run reuse
     * earlier bytes of the same run.
     *
     * rep0_before[i] is rep0 in effect before sequence i. The tail run
     * uses rep0 after the last sequence (section 9.6), supplied as
     * rep0_before[n_seq]; callers with tail > 0 and n_seq > 0 must
     * provide an array of n_seq + 1 entries. */
    size_t pos = 0;

    if (n_lit == 0) return;
    if (Z == NULL) return;
    if (n_seq > 0 && (rep0_before == NULL || ll == NULL)) return;
    if (tail > n_lit) return;

    /* Check the metadata the loops below rely on before writing
     * anything: sum(ll[0..n_seq-1]) + tail == n_lit, with the running
     * sum held at or below n_lit so it cannot wrap. This replaces a
     * guard that was dead - "tail > 0 && pos >= n_lit" could never
     * hold, since pos is still 0 there and the n_lit == 0 case already
     * returned above - and it is what stops a malformed segment from
     * being half-reconstructed.
     *
     * Note the consequence of the void return, which include/zgec_lit.h
     * fixes: a mismatch makes this call a no-op, and the caller has no
     * way to tell that from a completed reconstruction. That is still
     * strictly better than the previous behaviour, which wrote a prefix
     * and presented the half-built buffer as success, but it is not a
     * resting place. As of this writing the routine has no callers in
     * the tree, so the decision owed here is whether it survives at all:
     * either delete it together with its declaration, or change the
     * return to zgec_err so the refusal above is observable. Both need
     * include/zgec_lit.h, so neither is done here. */
    {
        size_t vsum = 0;
        for (size_t vi = 0; vi < n_seq; vi++) {
            size_t vadd = (size_t)ll[vi];
            if (vadd > n_lit - vsum) return;
            vsum += vadd;
        }
        if (vsum != n_lit - tail) return;
    }

    for (size_t i = 0; i < n_seq; i++) {
        uint32_t rep0 = rep0_before[i];
        size_t runlen = (size_t)ll[i];

        for (size_t t = 0; t < runlen && pos < n_lit; t++, pos++) {
            size_t cur = vbpos + pos;
            uint8_t pred;
            if (rep0 == 0u || (size_t)rep0 > cur) {
                pred = 0;
            } else {
                pred = (uint8_t)*(Z + (ptrdiff_t)pos - (ptrdiff_t)rep0);
            }
            Z[pos] = (uint8_t)(Z[pos] + pred);
        }
    }

    /* Tail literals: use rep0 after the last sequence. */
    if (tail > 0) {
        uint32_t rep0 = 1u;
        if (n_seq > 0) {
            rep0 = rep0_before[n_seq];
        }

        for (size_t t = 0; t < tail && pos < n_lit; t++, pos++) {
            size_t cur = vbpos + pos;
            uint8_t pred;
            if (rep0 == 0u || (size_t)rep0 > cur) {
                pred = 0;
            } else {
                pred = (uint8_t)*(Z + (ptrdiff_t)pos - (ptrdiff_t)rep0);
            }
            Z[pos] = (uint8_t)(Z[pos] + pred);
        }
    }
}
