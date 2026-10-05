#ifndef ZGEC_H
#define ZGEC_H

#include "zgec_common.h"
#include "zgec_bitstream.h"
#include "zgec_crc32c.h"
#include "zgec_xxhash.h"
#include "zgec_fse.h"
#include "zgec_rans.h"
#include "zgec_frame.h"
#include "zgec_dict.h"
#include "zgec_seq.h"
#include "zgec_lit.h"
#include "zgec_block.h"
#include "zgec_decode.h"
#include "zgec_encode.h"
#include "zgec_match.h"
#include "zgec_parse.h"

/*
 * zGEC public API.
 *
 * zGEC is a random-access LZ77 + ANS block
 * compressor. Any block of the compressed file
 * can be decoded without decoding any other
 * block, apart from a bounded, explicitly stored
 * dictionary.
 *
 * Basic usage:
 *
 *   // Compress
 *   zgec_params p; zgec_params_default(&p);
 *   zgec_encoder *e = zgec_encoder_create(&p);
 *   uint8_t *cmp; size_t cmp_size;
 *   zgec_encode_frame(e, src, src_size, &cmp, &cmp_size);
 *   zgec_encoder_destroy(e);
 *
 *   // Decompress (sequential)
 *   zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
 *   uint8_t *out; size_t out_size;
 *   zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
 *   zgec_decoder_destroy(d);
 *
 *   // Random access: decode the block containing
 *   // original offset X
 *   const uint8_t *blk; size_t blk_size;
 *   zgec_decode_block(d, cmp, cmp_size, X, &blk, &blk_size);
 *
 * All buffers returned by the API are newly
 * allocated and owned by the caller (free with
 * zgec_free), except the random-access block
 * pointer which points into an internal buffer
 * valid until the next decoder call.
 */

/* Convenience: compress with default parameters. */
zgec_err zgec_compress(const uint8_t *src, size_t src_size,
                                 uint8_t **dst, size_t *dst_size);

/* Convenience: decompress a whole frame. */
zgec_err zgec_decompress(const uint8_t *src, size_t src_size,
                                   uint8_t **dst, size_t *dst_size);

/* Version string. */
const char *zgec_version(void);

#endif /* ZGEC_H */
