#ifndef ZGEC_PARSE_H
#define ZGEC_PARSE_H

#include "zgec_common.h"
#include "zgec_match.h"

/*
 * Parsing, price gate and segmentation per zGEC
 * sections 11.3, 11.4 and 11.5 (informative).
 *
 * The parse produces, for each block, a sequence
 * array: literal lengths (LL), match lengths (ML)
 * and offset bases (OF), plus the literal bytes.
 *
 * The price gate (section 11.4): a match is
 * accepted only if its estimated saving is
 * positive: len * Lbar > cost(LL, ML, OF), where
 * Lbar is the running average cost in bits of a
 * literal and cost is read from small tables
 * derived from the previous segment's actual code
 * statistics.
 *
 * Segmentation (section 11.5): the parse is split
 * into granules of about 16K sequences. Each
 * granule has literal and sequence histograms,
 * which are additive. Adjacent granules are merged
 * greedily when the entropy-coded size with one
 * set of tables is not larger than with two.
 */

/* A parsed sequence. */
typedef struct {
    uint32_t ll;       /* literal length */
    uint32_t ml;       /* match length (>= 3) */
    uint32_t offbase;  /* offset base (1..3 repeat, >= 4 explicit) */
} zgec_sequence;

/* A parsed block: the sequence array and the
 * literal buffer. */
typedef struct {
    zgec_sequence *seq;   /* n_seq sequences */
    size_t         n_seq;
    uint8_t       *lit;   /* n_lit literals */
    size_t         n_lit;
    uint32_t       raw_size;
} zgec_parse;

void zgec_parse_free(zgec_parse *p);

/* Parse a block into sequences.
 * src: the block's original data (raw_size bytes).
 * dict: the dictionary (may be NULL), dict_size bytes.
 * The virtual buffer is [dict][src].
 * tier: the match finder tier.
 * lambda: the speed/ratio dial (0 = max ratio).
 * On success, *out is a newly allocated parse.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_parse_block(zgec_parse **out,
                                      const uint8_t *src, size_t raw_size,
                                      const uint8_t *dict, size_t dict_size,
                                      zgec_tier tier, double lambda);

/* Extended form (section 6.1): the virtual buffer is
 * [dict][litref][src], where litref/litref_size is the LITREF region
 * of the literal-reference predecessors (may be NULL/0). Both prefixes
 * are seeded into the match-finder tables so a match may reach back
 * into either; the reported sequences, literals and distances are in
 * the same coordinate space as the single-prefix form. Callers MUST
 * keep dict_size + litref_size + raw_size within the P24 position
 * limit (2^24). */
zgec_err zgec_parse_block_ex(zgec_parse **out,
                                      const uint8_t *src, size_t raw_size,
                                      const uint8_t *dict, size_t dict_size,
                                      const uint8_t *litref, size_t litref_size,
                                      zgec_tier tier, double lambda);

/* Segment a parse into segments (section 11.5).
 * Produces segment boundaries (indices into the
 * sequence array) such that each segment owns its
 * literals entirely.
 * On success, *bounds is a newly allocated array of
 * n_segments+1 sequence indices (bounds[0] = 0,
 * bounds[n_segments] = n_seq), and *n_segments is
 * set. The literal boundaries are derived from the
 * LL prefix sums.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_segment(const zgec_parse *p,
                                  size_t **bounds, size_t *n_segments,
                                  size_t target_seg_sequences);

/* Compute the literal boundaries for the given
 * sequence bounds. Fills lit_bounds[0..n_segments]
 * (literal indices). */
void zgec_lit_bounds(const zgec_parse *p,
                             const size_t *seq_bounds,
                             size_t n_segments,
                             size_t *lit_bounds);

/* Estimate the entropy-coded size in bits of a
 * sequence histogram (for the price gate and
 * segmentation). hist[0..65] is the code
 * histogram. Returns the estimated bits. */
double zgec_seq_cost_bits(const uint32_t *hist);

/* Estimate the cost in bits of coding a single
 * (LL, ML, OF) triple, given the code histograms
 * of the current segment. Used by the price gate. */
double zgec_seq_triple_cost(uint32_t ll, uint32_t ml,
                                        uint32_t offbase,
                                        const uint32_t *ll_hist,
                                        const uint32_t *ml_hist,
                                        const uint32_t *of_hist,
                                        double lbar);

#endif /* ZGEC_PARSE_H */
