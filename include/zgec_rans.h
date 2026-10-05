#ifndef ZGEC_RANS_H
#define ZGEC_RANS_H

#include "zgec_common.h"

/*
 * Interleaved 8-lane rANS literal coder per zGEC section 9 and Annex B.
 *
 * Probability scale M = 2^11 = 2048 (accuracy log 11). State is 32 bits
 * with invariant range [2^16, 2^32). Renormalisation emits 16-bit words
 * (little-endian), b = 2^16.
 *
 * Stream layout (section 9.5):
 *   bytes 0..31   : initial states x0..x7 (u32 each)
 *   bytes 32..end : 16-bit renormalisation words, consumed in decode order
 *
 * Decode proceeds in rounds r = 0, 1, ...; within a round the lanes
 * k = 0..7 decode the symbol at index start(k) + r (if within the
 * lane's length) and renormalise immediately, taking words from a
 * single shared cursor.
 *
 * Contexts (section 9.3): for k > 1 contexts, a literal at index j uses
 * the run-start table (tables[k]) if j is a lane start or a run start,
 * otherwise the table indexed by class_map[classify(Z[j-1])]. For k == 1
 * there is a single table and no context logic.
 */

#define ZGEC_RANS_L 11          /* accuracy log */
#define ZGEC_RANS_M 2048        /* probability scale */
#define ZGEC_RANS_STATE_MIN 65536u

/* Decoder tables (Annex B.1). One per context (plus the run-start table). */
typedef struct {
    uint16_t symbol_of_slot[ZGEC_RANS_M];  /* 2048 entries */
    uint16_t f[ZGEC_NSYM_LIT];             /* frequency of each symbol (0 allowed) */
    uint16_t c[ZGEC_NSYM_LIT];             /* cumulative start */
} zgec_rans_dec_table;

/* Encoder tables (Annex B.2). */
typedef struct {
    uint32_t f_recip[ZGEC_NSYM_LIT];  /* precomputed reciprocal for fast division */
    uint16_t f[ZGEC_NSYM_LIT];
    uint16_t c[ZGEC_NSYM_LIT];
} zgec_rans_enc_table;

/* Build decoder tables from normalised counts (256 entries).
   counts[s] is the normalised count (-1 for "less than one", 0 for
   never, positive otherwise). The sum of effective counts must equal
   2^11 = 2048. */
zgec_err zgec_rans_build_dec(zgec_rans_dec_table *t, const int16_t *counts);

/* Build encoder tables from normalised counts. */
zgec_err zgec_rans_build_enc(zgec_rans_enc_table *t, const int16_t *counts);

/*
 * Decode n_lit literals from the rANS stream.
 *
 * stream: the literal stream bytes (stream_size bytes).
 * Z:      output buffer (n_lit + ZGEC_LIT_SLACK bytes).
 * tables: array of decode tables. For k == 1: 1 table (tables[0]).
 *         For k > 1: k+1 tables (tables[0..k-1] are the context tables,
 *         tables[k] is the run-start table).
 * k:          number of contexts (1, 2, 4 or 8).
 * ctx_mode:   classification function (ZGEC_CTX_*).
 * class_map:  map from class (0..63) to context (0..k-1); NULL if k == 1.
 * runstart:   run-start bitmap, runstart[j] != 0 if literal j is a run
 *             start; NULL if k == 1.
 *
 * On success returns ZGEC_OK and the stream is exactly consumed (V5).
 */
zgec_err zgec_rans_decode(uint8_t *Z, size_t n_lit,
                                const uint8_t *stream, size_t stream_size,
                                const zgec_rans_dec_table *tables, int k,
                                int ctx_mode, const uint8_t *class_map,
                                const uint8_t *runstart);

/*
 * Encode n_lit literals into the rANS stream.
 *
 * Z:      input literals (n_lit bytes).
 * stream: output stream buffer (stream_cap bytes).
 * Returns the stream size in bytes, or 0 on error.
 * The tables/k/ctx_mode/class_map/runstart arguments are as for decode;
 * the encoder selects contexts identically to the decoder.
 */
size_t zgec_rans_encode(const uint8_t *Z, size_t n_lit,
                              uint8_t *stream, size_t stream_cap,
                              const zgec_rans_enc_table *tables, int k,
                              int ctx_mode, const uint8_t *class_map,
                              const uint8_t *runstart);

/* Compute the literal histograms for the k+1 tables from the literal
   sequence Z, the run-start bitmap, the class map and the context mode.
   Fills tables_counts[0..n_tables-1] (each 256 entries) with u32 counts.
   n_tables is 1 (k == 1) or k + 1 (k > 1, last table is run-start).
   Counts are exact (no saturation): each table's entries sum to the
   number of literals assigned to it, so n_lit above 32767 is safe.
   Used by the encoder to build the tables it will encode with. */
void zgec_rans_histograms(uint32_t *tables_counts, int n_tables,
                                const uint8_t *Z, size_t n_lit,
                                int ctx_mode, const uint8_t *class_map,
                                const uint8_t *runstart);

/* Normalise a 256-entry histogram to a sum of 2048, producing counts
   suitable for zgec_rans_build_dec/enc. Returns ZGEC_OK or an error
   (e.g. if a symbol count would be negative). */
zgec_err zgec_rans_normalise(int16_t *counts, const uint32_t *hist);

#endif /* ZGEC_RANS_H */
