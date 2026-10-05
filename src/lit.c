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
    case 4: {
        int s = (int8_t)b;
        int cl = s < -32 ? -32 : s > 31 ? 31 : s;
        return (unsigned)(cl + 32);
    }
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
    memset(packed, 0, 24);
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
     * exists (sum(ll) < n_lit), per section 9.3. */
    if (n_lit == 0) {
        size_t sum = 0;
        if (n_seq > 0 && ll == NULL) return ZGEC_ERR_INVAL;
        for (size_t i = 0; i < n_seq; i++) sum += (size_t)ll[i];
        return sum == 0 ? ZGEC_OK : ZGEC_ERR_LL_SUM;
    }
    if (runstart == NULL) return ZGEC_ERR_INVAL;
    if (n_seq > 0 && ll == NULL) return ZGEC_ERR_INVAL;
    memset(runstart, 0, n_lit);
    size_t pos = 0;
    for (size_t i = 0; i < n_seq; i++) {
        if (pos > n_lit) return ZGEC_ERR_LL_SUM;
        if (ll[i] > 0 && pos < n_lit) {
            runstart[pos] = 1;
        }
        pos += (size_t)ll[i];
    }
    if (pos > n_lit) return ZGEC_ERR_LL_SUM;
    /* Tail start (j == sum LL) when tail literals exist. */
    if (pos < n_lit) {
        runstart[pos] = 1;
    }
    return ZGEC_OK;
}

/* ---- lane starts ---- */

void zgec_lit_lane_starts(size_t start[ZGEC_NLANES],
                          size_t n_lit)
{
    size_t q;
    size_t r;
    if (start == NULL) return;
    q = n_lit / 8;
    r = n_lit % 8;
    for (unsigned lane = 0; lane < ZGEC_NLANES; lane++) {
        start[lane] = (size_t)lane * q + ((size_t)lane < r ? (size_t)lane : r);
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
    if (tail > 0 && pos >= n_lit) return;

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
