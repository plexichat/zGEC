#ifndef ZGEC_DICT_H
#define ZGEC_DICT_H

#include "zgec_common.h"
#include "zgec_frame.h"

/*
 * Dictionaries and epochs per zGEC section 5.
 *
 * A dictionary is a byte string stored in the frame (a DICT record
 * whose payload is an inner record) and referenced by blocks
 * through a 16-bit id. The dictionary bytes form the start of the
 * block's virtual buffer.
 */

/* A decoded dictionary, cached by id. */
typedef struct {
    uint16_t dict_id;
    uint32_t raw_size;
    uint64_t content_hash;
    uint8_t *data;          /* raw_size + ZGEC_OUTPUT_SLACK bytes */
    int      external;      /* 1 if external (not stored in frame) */
} zgec_dict;

/* An LRU cache of decoded dictionaries, keyed by dict_id.
   Shared between worker threads (the caller serialises access
   or uses one cache per worker). */
typedef struct zgec_dict_cache zgec_dict_cache;

/* Create a cache that holds at least `capacity` dictionaries. */
zgec_dict_cache *zgec_dict_cache_create(size_t capacity);
void zgec_dict_cache_destroy(zgec_dict_cache *c);

/* Look up a dictionary by id. Returns the dictionary (owned by
   the cache, valid until the next lookup) or NULL if not cached. */
const zgec_dict *zgec_dict_cache_get(zgec_dict_cache *c, uint16_t dict_id);

/* Insert a dictionary into the cache (takes ownership of data). */
void zgec_dict_cache_put(zgec_dict_cache *c, zgec_dict *d);

/* Decode a DICT record's inner record (RAW, RLE or COMPRESSED)
   into a dictionary. The inner record must have dict_id 0 and
   lit_ref_depth 0. Verifies the content_hash.
   On success, *out is a newly allocated dictionary (caller frees
   with zgec_dict_free). */
zgec_err zgec_dict_decode(zgec_dict **out,
                                  const uint8_t *payload, size_t payload_size,
                                  uint32_t raw_size, uint64_t content_hash,
                                  const zgec_frame_header *fh);

void zgec_dict_free(zgec_dict *d);

/*
 * Dictionary training per zGEC section 5.7 (informative).
 *
 * Sampled greedy selection: content-defined chunking with a
 * gear-style rolling hash (average chunk ~512 bytes, min 64,
 * max 4 KiB), 64-bit fingerprinting, scoring by (occurrences-1)
 * x length, greedy selection by descending score, ordered by
 * ascending score so the best content ends up at the end.
 *
 * Trains a dictionary from `data` (epoch_raw bytes) with the
 * given size budget. Returns a newly allocated buffer of at most
 * `dict_size` bytes, and sets *out_size. Returns ZGEC_OK or an
 * error.
 */
zgec_err zgec_dict_train(uint8_t **out, size_t *out_size,
                                 const uint8_t *data, size_t data_size,
                                 size_t dict_size);

/* Default dictionary size per section 5.5:
   clamp(epoch_raw_bytes / 32, 64 KiB, 1 MiB), capped at
   2^max_dict_log2. */
size_t zgec_dict_default_size(size_t epoch_raw_bytes, int max_dict_log2);

#endif /* ZGEC_DICT_H */
