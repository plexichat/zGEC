#ifndef ZGEC_ENCODE_H
#define ZGEC_ENCODE_H

#include "zgec_common.h"
#include "zgec_frame.h"

/*
 * Encoder per zGEC section 11 (informative).
 *
 * The encoder pipeline (section 11.1):
 *   1. Dictionary pass: train a candidate dictionary
 *      from the preceding epoch's raw data and decide
 *      by sampling whether to keep it.
 *   2. Block pass, in parallel across blocks: parse
 *      into sequences; segment; build context maps and
 *      tables; entropy code; choose per-segment
 *      options; emit the record.
 *   3. Assemble records in order, write dictionary
 *      records before their first user, write footer
 *      and trailer.
 */


/* Encoder parameters. */
typedef struct {
    int          block_log2;     /* 16..26; 0 = default (21) */
    int          epoch_blocks;   /* 0 = default (10) */
    int          max_dict_log2;  /* 0 = default (20, i.e. 1 MiB) */
    int          seg_hint_log2;  /* 14..22; 0 = default (18) */
    zgec_tier    tier;           /* match finder tier */
    int          n_threads;      /* 0 = one per core (default), 1 = serial */
    int          use_dicts;      /* 0 = never, 1 = epoch dictionaries */
    int          use_litref;     /* 0 = never, 1 = literal references.
                                  * A LITREF predecessor must stay plain and
                                  * unconditioned (6.3), so this also
                                  * suppresses use_sublit and
                                  * use_conditioning in the block pass. */
    int          use_sublit;     /* 0 = never, 1 = sub-literals */
    int          use_contexts;   /* 0 = order-0, 1 = learned contexts */
    int          use_conditioning; /* 0 = off, 1 = sequence conditioning
                                    * (8.6 bit 4); not emitted while
                                    * use_litref is set (6.3) */
    int          use_filter;       /* 0 = never, 1 = sampled block pre-filter */
    int          block_checksums;  /* 0 = off, 1 = on */
    double       lambda;         /* speed/ratio dial (section 11.7) */
} zgec_params;

/* Default parameters. */
void zgec_params_default(zgec_params *p);

typedef struct zgec_encoder zgec_encoder;

/* Create an encoder with the given parameters. */
zgec_encoder *zgec_encoder_create(const zgec_params *p);
void zgec_encoder_destroy(zgec_encoder *e);

/* Register an external dictionary (section 5.8).
 *
 * The bytes are NOT stored in the frame; the frame instead carries a
 * footer dictionary entry of kind 1 with this id, the raw size and the
 * XXH64 content hash, and the frame flag EXTERNAL_DICT is set. A
 * decoder must be given the same bytes through
 * zgec_decoder_add_external_dict(dict_id, data, size); it verifies the
 * size and hash and fails otherwise.
 *
 * Blocks reference the dictionary only when a per-block measurement
 * shows a smaller payload, exactly as for an epoch dictionary (5.5).
 * Up to four may be registered; a repeated id replaces its bytes.
 * dict_id must be non-zero and the size must fit the format limits.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_encoder_set_external_dict(zgec_encoder *e,
                                        uint16_t dict_id,
                                        const uint8_t *data, size_t size);

/* Drop every registered external dictionary, so the next frame carries
 * none. */
void zgec_encoder_clear_external_dicts(zgec_encoder *e);

/* Encode a whole frame.
 * src/src_size: the original data.
 * On success, *dst is a newly allocated buffer of
 * *dst_size bytes (the compressed frame). The caller
 * frees it with zgec_free.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_encode_frame(zgec_encoder *e,
                                     const uint8_t *src, size_t src_size,
                                     uint8_t **dst, size_t *dst_size);

/* Encode a single block (for testing and the block
 * pass). The block is block_index of a frame with the
 * given parameters. dict (may be NULL) is the
 * dictionary for this block.
 * On success, *dst is a newly allocated buffer holding
 * the COMPRESSED record payload (not including the
 * 24-byte record header), and *dst_size is its size.
 * If segment_count is non-NULL it receives the number of
 * segments, which the record header must carry.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_encode_block(zgec_encoder *e,
                                     const uint8_t *src, size_t src_size,
                                     uint32_t block_index,
                                     const uint8_t *dict, size_t dict_size,
                                     uint8_t **dst, size_t *dst_size,
                                     uint32_t *segment_count);

/* Estimate the compressed size of a block with and
 * without a dictionary (for the ratio gate, section
 * 5.5). Returns the estimated payload size, or 0 on
 * error. */
size_t zgec_estimate_block(zgec_encoder *e,
                                       const uint8_t *src, size_t src_size,
                                       uint32_t block_index,
                                       const uint8_t *dict, size_t dict_size);

#endif /* ZGEC_ENCODE_H */
