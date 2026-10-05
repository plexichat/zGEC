#ifndef ZGEC_BITSTREAM_H
#define ZGEC_BITSTREAM_H

#include "zgec_common.h"

/*
 * Backward bitstream per RFC 8878 section 4.1 and zGEC section 8.4.
 *
 * Physical model: the stream is bytes B[0..m-1]. The last byte B[m-1] is
 * non-zero; its highest set bit (position s) is the sentinel. The consumable
 * bits, in consumption order, are:
 *   - B[m-1] bits [s-1 .. 0]  (s bits, below the sentinel)
 *   - B[m-2] bits [7 .. 0]
 *   - ... down to B[0] bits [7 .. 0]
 * A read of k bits returns an integer whose most significant bit is the first
 * bit consumed.
 *
 * Reader: a 64-bit accumulator holds the next bits to consume, with the
 * next-to-consume bit at position 63 (MSB). Bytes are refilled from the end
 * of the stream backwards.
 *
 * Writer: the mirror image. Bits accumulate LSB-first into a 64-bit
 * accumulator (the first-written bit of a value is its least significant bit
 * in the accumulator, which the reader consumes last within a byte, so the
 * reader sees the value's most significant bit first). Full bytes are flushed
 * forward (low to high address). The sentinel is appended as the final byte.
 */

typedef struct {
    const uint8_t *ptr;    /* next byte to load (moving backwards); starts at end-2 */
    const uint8_t *start;  /* stream start */
    const uint8_t *end;    /* one past stream end */
    uint64_t acc;          /* accumulator; next bit to consume is bit 63 */
    unsigned nacc;         /* number of valid bits in acc (top nacc bits) */
    int overflow;          /* set if a read exceeded the stream */
} zgec_br;

typedef struct {
    uint8_t *ptr;          /* next byte to write (moving forward); starts at buffer start */
    uint8_t *start;        /* buffer start */
    uint8_t *end;          /* one past buffer end (capacity) */
    uint64_t acc;          /* accumulator; next bit to write is bit (nacc) */
    unsigned nacc;         /* number of bits in acc not yet flushed (0..63) */
    int overflow;          /* set if a write exceeded the buffer */
} zgec_bw;

/* Reader */
void zgec_br_init(zgec_br *b, const void *buf, size_t size);
void zgec_br_refill(zgec_br *b);
uint32_t zgec_br_read(zgec_br *b, unsigned n);
/* Returns 1 if the stream is exactly consumed (all bits read, no overflow). */
int zgec_br_done(const zgec_br *b);

/* Writer */
void zgec_bw_init(zgec_bw *b, void *buf, size_t capacity);
void zgec_bw_write(zgec_bw *b, uint32_t value, unsigned n);
/* Flushes remaining bits and appends the sentinel byte.
   Returns the total stream size in bytes, or 0 on overflow. */
size_t zgec_bw_finish(zgec_bw *b);

#endif /* ZGEC_BITSTREAM_H */
