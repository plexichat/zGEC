#ifndef ZGEC_MATCH_H
#define ZGEC_MATCH_H

#include "zgec_common.h"

/*
 * Match finder per zGEC section 11.2 (informative).
 *
 * Three tiers: fast, main, high. Entries in packed
 * tables are 32 bits: a 24-bit position in the
 * virtual buffer and an 8-bit tag from spare hash
 * bits.
 *
 * Hashing: the short table hashes 5 bytes (4 for
 * binary data); the long table hashes 8 bytes with
 * one entry per bucket. The minimum non-repeat
 * match is 5, with 4 permitted for repeat offsets.
 */

/* A candidate match. */
typedef struct {
    uint32_t offset;   /* distance back from the current position */
    uint32_t length;   /* match length (>= 3) */
} zgec_match;

/* Match finder state (per thread). */
typedef struct zgec_matcher zgec_matcher;

/* Create a matcher for the given tier and virtual
 * buffer size (the maximum offset the matcher can
 * reference). */
zgec_matcher *zgec_matcher_create(zgec_tier tier,
                                                size_t vb_capacity);
void zgec_matcher_destroy(zgec_matcher *m);

/* Reset the matcher for a new block, optionally
 * pre-loading the dictionary into the hash tables
 * (section 11.8: hash the dictionary once into a
 * table snapshot and copy it into each worker's
 * table at block start). */
void zgec_matcher_reset(zgec_matcher *m,
                                    const uint8_t *vb, size_t vb_size,
                                    const uint8_t *dict, size_t dict_size);

/* Find the best match at position ip in the virtual
 * buffer vb. rep0/rep1 are the current repeat
 * offsets (checked first). The match must satisfy
 * offset <= ip (the position in the virtual buffer).
 * Returns the best match, or a match with length 0
 * if none found.
 *
 * min_len is the minimum acceptable match length
 * (typically 5, or 4 for repeat offsets).
 * max_len caps the match length.
 */
zgec_match zgec_matcher_find(zgec_matcher *m,
                                         const uint8_t *vb, size_t ip,
                                         uint32_t rep0, uint32_t rep1,
                                         uint32_t min_len, uint32_t max_len);

/* Insert the position ip into the hash tables. */
void zgec_matcher_insert(zgec_matcher *m,
                                     const uint8_t *vb, size_t ip);

/* Insert a sampled set of positions inside a match
 * (fast and main tiers insert 2-3 sampled
 * positions). */
void zgec_matcher_insert_match(zgec_matcher *m,
                                           const uint8_t *vb,
                                           size_t start, size_t len);

#endif /* ZGEC_MATCH_H */
