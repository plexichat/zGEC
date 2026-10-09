#ifndef ZGEC_SEQ_H
#define ZGEC_SEQ_H

#include "zgec_common.h"
#include "zgec_fse.h"

/*
 * Sequence coding per zGEC section 8.
 *
 * Each of LL, ML, OF is mapped to a value v and coded as a
 * symbol (code) from a 66-symbol alphabet plus raw extra bits
 * (section 8.1). The three fields share one value-code mapping.
 *
 * Offsets use repeat offsets (rep0, rep1, rep2), reset to
 * (1, 4, 8) at the start of every segment (section 8.2).
 */

/* ---- value codes (section 8.1) ---- */

/* Map a value v to its code and extra-bit count.
   v < 8: code = v, nbits = 0.
   v >= 8: e = floor(log2(v)), m = (v >> (e-1)) & 1,
           code = 8 + 2*(e-3) + m, nbits = e - 1. */
static inline uint8_t zgec_seq_code_of(uint32_t v, uint8_t *nbits) {
    if (v < 8) { *nbits = 0; return (uint8_t)v; }
    unsigned e = zgec_highbit32(v);            /* e >= 3 */
    unsigned m = (v >> (e - 1)) & 1u;
    *nbits = (uint8_t)(e - 1);
    return (uint8_t)(8 + 2 * (e - 3) + m);
}

/* Map a code to its base value (the minimum value with that code).
   code < 8: base = code.
   code >= 8: e = 3 + ((code-8) >> 1), m = (code-8) & 1,
              base = (1 << e) + (m << (e-1)). */
static inline uint32_t zgec_seq_base_of(uint8_t code) {
    if (code < 8) return code;
    unsigned e = 3 + ((unsigned)(code - 8) >> 1);
    unsigned m = (unsigned)(code - 8) & 1u;
    return (1u << e) + (m << (e - 1));
}

/* Precomputed base and nbits tables for the 66-symbol alphabet. */
extern const uint32_t zgec_seq_base[ZGEC_NSYM_SEQ];
extern const uint8_t  zgec_seq_nbits[ZGEC_NSYM_SEQ];

/* ---- repeat offsets (section 8.2) ---- */

typedef struct {
    uint32_t rep[3];   /* rep0, rep1, rep2 */
} zgec_reps;

/* Initialise the repeat offsets to (1, 4, 8). */
static inline void zgec_reps_init(zgec_reps *r) {
    r->rep[0] = 1; r->rep[1] = 4; r->rep[2] = 8;
}

/* Resolve an offbase to an explicit offset and update the
   repeat offsets (move-to-front). offbase >= 1.
   Returns the explicit offset d. */
static inline uint32_t zgec_reps_resolve(zgec_reps *r, uint32_t offbase) {
    uint32_t d;
    switch (offbase) {
    case 1: d = r->rep[0]; break;
    case 2: d = r->rep[1]; r->rep[1] = r->rep[0]; r->rep[0] = d; break;
    case 3: d = r->rep[2]; r->rep[2] = r->rep[1]; r->rep[1] = r->rep[0]; r->rep[0] = d; break;
    default:
        d = offbase - 3;
        r->rep[2] = r->rep[1]; r->rep[1] = r->rep[0]; r->rep[0] = d;
        break;
    }
    return d;
}

/* The inverse of zgec_reps_resolve: choose the offbase that selects
   the explicit offset d under (or against) the current repeat
   offsets. Follow it with zgec_reps_resolve(r, offbase) to apply
   the same move-to-front update the decoder performs. */
static inline uint32_t zgec_reps_encode(const zgec_reps *r, uint32_t d) {
    if (d == r->rep[0]) return 1u;
    if (d == r->rep[1]) return 2u;
    if (d == r->rep[2]) return 3u;
    return d + 3u;
}

/* ---- context conditioning (section 8.6) ---- */

/* Match-length class: 0 if ML-3 < 4 (ML 3..6), 1 if ML-3 < 16
   (ML 7..18), 2 otherwise. */
static inline unsigned zgec_mlclass(uint32_t ml) {
    uint32_t m = ml - 3;
    /* Branchless: 0 if m < 4, 1 if m < 16, 2 otherwise. */
    return (unsigned)(m >= 4u) + (unsigned)(m >= 16u);
}

/* ---- stream decode / encode ---- */

/* Decode one sequence stream (LL, ML or OF) of n symbols.
   The stream is RLE if rle_symbol >= 0 (the symbol code, and
   the bitstream contains only extra bits); otherwise it uses
   the FSE decode table t.
   out[0..n-1] receives the decoded values.
   base/nbits are the section 8.1 tables (zgec_seq_base/nbits). */
zgec_err zgec_seq_stream_decode(uint32_t *out, size_t n,
                                          const zgec_fse_dec_table *t,
                                          int rle_symbol,
                                          zgec_br *br,
                                          const uint32_t *base,
                                          const uint8_t *nbits);

/* Encode one sequence stream of n symbols.
   values[0..n-1] are the values; the symbols are derived via
   zgec_seq_code_of. The stream is RLE if all symbols equal
   rle_symbol (the caller decides); otherwise FSE.
   Returns the number of bytes written to the bitstream buffer,
   or 0 on error. */
size_t zgec_seq_stream_encode(const uint32_t *values, size_t n,
                                        const zgec_fse_enc_table *t,
                                        int rle_symbol,
                                        zgec_bw *bw,
                                        const uint32_t *base,
                                        const uint8_t *nbits);

/* Encode one conditioned sequence stream (section 8.6); the exact
   inverse of the decoder's three-table path. enc holds the three
   class tables, which MUST share one accuracy log. The class for
   symbol i is mlclass(ml[i]), or mlclass(ml[i-1]) (0 for i == 0)
   when use_prev is set (the LL stream is conditioned on the previous
   match length); ml holds the real match lengths (>= 3).
   Returns the number of bytes written, or 0 on error. */
size_t zgec_seq_stream_encode_cond(const uint32_t *values, size_t n,
                                   const uint32_t *ml, int use_prev,
                                   const zgec_fse_enc_table *const enc[3],
                                   zgec_bw *bw,
                                   const uint32_t *base,
                                   const uint8_t *nbits);

/* Choose the RLE symbol for a stream if all values map to the
   same code; returns that code, or -1 if not RLE-able. */
int zgec_seq_rle_symbol(const uint32_t *values, size_t n);

/* Build FSE decode/encode tables from a value histogram.
   hist[0..65] is the histogram of codes. The counts are
   normalised to 2^al. Returns ZGEC_OK or an error. */
zgec_err zgec_seq_build_tables(zgec_fse_dec_table **dec,
                                         zgec_fse_enc_table **enc,
                                         const uint32_t *hist, int al);

/* Same as above, but writes the normalised counts to counts_out[0..65]
   (may be NULL) so the caller can reuse them for the table descriptor
   instead of normalising a second time. */
zgec_err zgec_seq_build_tables_counts(zgec_fse_dec_table **dec,
                                                zgec_fse_enc_table **enc,
                                                const uint32_t *hist, int al,
                                                int16_t *counts_out);

#endif /* ZGEC_SEQ_H */
