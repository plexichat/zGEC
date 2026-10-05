#ifndef ZGEC_DECODE_H
#define ZGEC_DECODE_H

#include "zgec_common.h"
#include "zgec_frame.h"
#include "zgec_dict.h"

/*
 * Decoding procedure per zGEC section 10.
 *
 * Decoding a COMPRESSED block has two phases:
 *   Phase A: decode each segment's bitstreams into
 *            plain arrays and validate them completely
 *            (rules V1..V10).
 *   Phase B: execute the sequences and write the
 *            output, with no bounds checks.
 *
 * The decoder is stateful: it caches dictionaries and
 * (for Extended level) literal buffers of referenced
 * blocks.
 */

/* Conformance level (section 14). */
typedef enum {
    ZGEC_LEVEL_CORE = 0,     /* everything except literal references */
    ZGEC_LEVEL_EXTENDED = 1  /* adds literal references */
} zgec_level;

/* Memory limit configuration (section 13).
 *
 * A decoder computes a per-worker upper bound before decoding
 * anything:
 *
 *   bound = 2^block_log2 + 2^max_dict_log2 + litref + slack
 *
 * (slack = ZGEC_OUTPUT_SLACK; litref is the block's actual
 * literal-reference bytes, at most 3 * block size). The bound
 * and every actual allocation derived from the stream are
 * refused when they exceed the configured limits:
 * block/dict-cap violations return ZGEC_ERR_INVAL, total-limit
 * violations return ZGEC_ERR_NOMEM.
 *
 * A decoder handle is NOT thread-safe for concurrent API calls:
 * the caller MUST serialise calls on the same handle. Decode
 * work IS parallelised internally (Phase A of a block's
 * segments runs on worker threads, §10.6); the shared
 * dictionary cache is guarded by an internal mutex so that
 * concurrent block decodes that share only the cache stay
 * safe. */
typedef struct {
    size_t max_block_size;   /* 0 = use the frame's block_log2 */
    size_t max_dict_size;    /* 0 = use the frame's max_dict_log2 */
    size_t max_total;        /* 0 = no total limit */
    int    n_threads;        /* worker threads (0 = one per core, 1 = serial) */
} zgec_limits;

typedef struct zgec_decoder zgec_decoder;

/* Set the worker-thread count after creation (0 = one per
 * core, 1 = serial, N > 1 = up to N workers, always capped
 * at the segment/block count and at 64). Calls on the same
 * handle MUST be serialised by the caller. */
zgec_err zgec_decoder_set_threads(zgec_decoder *d, int n_threads);

/* Create a decoder. level selects Core or Extended.
   limits may be NULL for defaults. */
zgec_decoder *zgec_decoder_create(zgec_level level,
                                            const zgec_limits *limits);
void zgec_decoder_destroy(zgec_decoder *d);

/* Decode a whole frame (sequential).
 * src/src_size: the compressed frame.
 * On success, *dst is a newly allocated buffer of
 * *dst_size bytes (the original data). The caller
 * frees it with zgec_free.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_decode_frame(zgec_decoder *d,
                                     const uint8_t *src, size_t src_size,
                                     uint8_t **dst, size_t *dst_size);

/* Decode a single block by original-data offset
 * (random access, section 4.6).
 * src/src_size: the compressed frame (must include
 * the footer and trailer).
 * offset: the original-data byte offset; the block
 * containing it is decoded.
 * On success, *dst points into an internal buffer
 * (valid until the next call) and *dst_size is the
 * block's raw size. To obtain a specific byte range,
 * the caller copies from *dst + (offset & (block_size-1)).
 * Returns ZGEC_OK or an error. */
zgec_err zgec_decode_block(zgec_decoder *d,
                                     const uint8_t *src, size_t src_size,
                                     uint64_t offset,
                                     const uint8_t **dst, size_t *dst_size);

/* Decode a block by block index (random access).
 * block_index is the index of the block in file order
 * (0-based). */
zgec_err zgec_decode_block_index(zgec_decoder *d,
                                             const uint8_t *src,
                                             size_t src_size,
                                             uint32_t block_index,
                                             const uint8_t **dst,
                                             size_t *dst_size);

/* Export the literal buffer of a block (section 6.3),
 * without executing it. Used for literal references.
 * Returns ZGEC_OK and fills *lit_buf (newly allocated,
 * *lit_size bytes) or an error. */
zgec_err zgec_export_literals(zgec_decoder *d,
                                          const uint8_t *src,
                                          size_t src_size,
                                          uint32_t block_index,
                                          uint8_t **lit_buf,
                                          size_t *lit_size);

/* Register an external dictionary (section 5.8).
 * The decoder verifies the length and XXH64 hash
 * against the footer entry. */
zgec_err zgec_decoder_add_external_dict(zgec_decoder *d,
                                                    uint16_t dict_id,
                                                    const uint8_t *data,
                                                    size_t size);

#endif /* ZGEC_DECODE_H */
