#ifndef ZGEC_LIT_H
#define ZGEC_LIT_H

#include "zgec_common.h"
#include "zgec_rans.h"

#include <string.h>

/*
 * Literal coding per zGEC section 9.
 *
 * The literals of a segment are coded as a sequence Z of
 * n_lit bytes, in output order. For plain segments Z[j] is
 * the literal byte; for sub-literal segments it is a
 * residual (section 9.6).
 *
 * Literals are coded raw (lit_coder 0) or by rANS
 * (lit_coder 1). For rANS, Z is cut into 8 contiguous
 * lanes (section 9.2).
 *
 * Contexts (section 9.3): for k > 1, a literal at index j
 * uses the run-start table if j is a lane start or a run
 * start, otherwise the table indexed by
 * class_map[classify(Z[j-1])].
 *
 * Sub-literals (section 9.6): the coded value is a residual
 * against the byte at the current repeat offset.
 */

/* Classification functions (Annex C). Maps a byte to one
 * of 64 classes. */
unsigned zgec_classify(int ctx_mode, uint8_t b);

/* Build the class map (64 entries, 3 bits each) from the
 * block parameters. The map is packed least significant
 * bit first into 24 bytes. Returns ZGEC_OK or an error
 * (an entry >= k is invalid). */
zgec_err zgec_class_map_decode(uint8_t *map /* 64 entries */,
                                         const uint8_t *packed /* 24 bytes */,
                                         int k);
void zgec_class_map_encode(uint8_t *packed /* 24 bytes */,
                                 const uint8_t *map /* 64 entries */);

/* Run-start bitmap (9.3), packed one bit per literal, least significant
 * bit first: literal j is a run start exactly when bit (j & 7) of byte
 * (j >> 3) is set. The map is derived from the LL values, never stored in
 * a frame, so packing changes no emitted byte -- it only cuts the
 * per-segment fill and what the rANS context loop reads from n_lit bytes
 * to n_lit/8. Allocate zgec_rs_bytes(n_lit) bytes and use only these
 * accessors. */
static inline size_t zgec_rs_bytes(size_t n_lit)
{
    return (n_lit + 7u) / 8u;
}

static inline void zgec_rs_clear(uint8_t *map, size_t n_lit)
{
    if (map != NULL && n_lit != 0) memset(map, 0, zgec_rs_bytes(n_lit));
}

static inline void zgec_rs_set(uint8_t *map, size_t j)
{
    uint8_t bit = (uint8_t)(1u << (j & 7u));
    map[j >> 3] = (uint8_t)(map[j >> 3] | bit);
}

static inline int zgec_rs_get(const uint8_t *map, size_t j)
{
    return (int)(((unsigned)map[j >> 3] >> (j & 7u)) & 1u);
}

/* Compute the run-start bitmap from the LL values: bit j set if literal j
 * is the first literal of a literal run (section 9.3). n_lit is the total
 * literal count; ll[0..n_seq-1] are the literal lengths; the tail literal
 * count is n_lit - sum(ll). The output must be zgec_rs_bytes(n_lit) bytes.
 * Returns ZGEC_OK or an error (V1: sum of LL exceeds
 * n_lit). */
zgec_err zgec_lit_runstart(uint8_t *runstart, size_t n_lit,
                                     const uint32_t *ll, size_t n_seq);

/* Compute the lane start positions (section 9.2).
 * start[0..7] receives the start index of each lane. */
void zgec_lit_lane_starts(size_t start[ZGEC_NLANES], size_t n_lit);

/* Compute lane starts and lengths (section 9.2). Shared by the rANS
 * decode/encode/histogram paths so the q/r/start/len math lives once. */
void zgec_lit_lane_geom(size_t start[ZGEC_NLANES], size_t len[ZGEC_NLANES],
                        size_t n_lit);

/* Apply sub-literal reconstruction (section 9.6): turn the
 * residual buffer Z into the actual literal bytes, using
 * the repeat offset in effect before each sequence.
 *
 * Z:        residual buffer (n_lit bytes), replaced by the
 *           reconstructed literals.
 * rep0_before[0..n_seq-1]: the rep0 value in effect before
 *           each sequence (for tail literals, the rep0 after
 *           the last sequence is used for the tail).
 * ll[0..n_seq-1]: literal lengths.
 * tail:     tail literal count.
 * vbpos:    the virtual-buffer write position of the first
 *           literal of the segment (Ld + Ll + segment start).
 *
 * The reconstruction is sequential: when rep0 is smaller
 * than the run length, later bytes use earlier bytes of the
 * same run as predictors.
 */
void zgec_lit_sub_reconstruct(uint8_t *Z, size_t n_lit,
                                      const uint32_t *rep0_before,
                                      const uint32_t *ll, size_t n_seq,
                                      size_t tail, size_t vbpos);

/* Compute the predictor for a sub-literal byte (section 9.6).
 * Used by the encoder to produce the residuals. */
static inline uint8_t zgec_lit_sub_predict(const uint8_t *vb,
                                                         size_t vbpos,
                                                         uint32_t rep0) {
    if (vb == NULL || rep0 == 0u || (size_t)rep0 > vbpos) return 0;
    return vb[vbpos - (size_t)rep0];
}

#endif /* ZGEC_LIT_H */
