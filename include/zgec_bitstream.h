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
    const uint8_t *ptr;    /* next byte to load (moving backwards) */
    const uint8_t *start;  /* stream start */
    const uint8_t *end;    /* one past stream end */
    uint64_t acc;          /* accumulator; next bit to consume is bit 63 */
    unsigned nacc;         /* number of valid bits in acc (top nacc bits) */
    size_t left;           /* bytes still loadable at or below ptr */
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

/* Reader.
 *
 * These are the innermost loops of a tANS/rANS decode: the sequence
 * streams call zgec_br_read twice per symbol, so the reader lives here as
 * `static inline` rather than in bitstream.c. An out-of-line call cost more
 * than the shift it performs, and without LTO the compiler could not inline
 * it from another translation unit. */
static inline void zgec_br_init(zgec_br *b, const void *buf, size_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    b->start = p;
    b->end = p + size;
    b->acc = 0;
    b->nacc = 0;
    b->ptr = p;
    b->left = 0;          /* no byte is loadable unless the last one is usable */
    b->overflow = 0;
    if (size == 0) {
        b->overflow = 1;   /* a stream must have a sentinel byte */
        return;
    }
    {
        uint8_t last = p[size - 1];
        if (last == 0) {
            b->overflow = 1;   /* sentinel missing */
            return;
        }
        {
            unsigned s = zgec_highbit32(last);   /* sentinel position 0..7 */
            /* the consumable bits of the last byte are bits [s-1 .. 0];
               place them at the top of the accumulator, bit s-1 at 63 */
            b->acc = (s == 0) ? 0ull
                     : (uint64_t)(last & (uint8_t)((1u << s) - 1u)) << (64 - s);
            b->nacc = s;
            /* The last byte has been consumed into acc, so the loadable
             * bytes are the ones before it. `left == 0` (a one-byte stream)
             * is the exhausted state: refill then never touches ptr, which
             * keeps it a valid pointer for the whole lifetime of the
             * reader instead of one before the buffer. */
            if (size >= 2) {
                b->ptr = p + size - 2;
                b->left = size - 1;
            }
        }
    }
}

static inline void zgec_br_refill(zgec_br *b)
{
    /* Fast path: one 8-byte load covers every byte the loop below would add.
     * The load is little-endian, so b->ptr sits at bit 56 of w and the bytes
     * below it follow downwards; shifting right by nacc drops the byte at
     * b->ptr onto bit (56 - nacc), which is exactly where the loop places it
     * relative to the valid bits. Bytes past the last one counted are ORed
     * in at the positions a later refill writes them at, with the same
     * values, so they are harmless: OR-ing a byte twice at the same offset is
     * idempotent and no OR ever lands on a valid bit (the top of w>>nacc is
     * bit 63-nacc, one below the last valid bit). The pointer, the count and
     * `left` advance exactly as the loop's do -- including the loop's
     * convention that b->ptr is not stepped down on the load that exhausts
     * the stream. */
    if (b->left >= 8 && b->nacc <= 56) {
        unsigned nb8 = (56u - b->nacc) / 8u + 1u;
        uint64_t w = zgec_rd64(b->ptr - 7);
        b->acc |= w >> b->nacc;
        b->nacc += 8u * nb8;
        b->left -= nb8;
        b->ptr -= nb8;
        if (b->left == 0) b->ptr++;
        return;
    }
    while (b->nacc <= 56 && b->left > 0) {
        b->acc |= (uint64_t)(b->ptr[0]) << (56 - b->nacc);
        b->nacc += 8;
        b->left--;
        if (b->left > 0) b->ptr--;   /* never step below the buffer start */
    }
}

static inline uint32_t zgec_br_read(zgec_br *b, unsigned n)
{
    if (n == 0) return 0;
    if (b->nacc < n) {
        zgec_br_refill(b);
        if (b->nacc < n) {
            b->overflow = 1;
            return 0;
        }
    }
    {
        uint32_t v = (uint32_t)(b->acc >> (64 - n));
        b->acc <<= n;
        b->nacc -= n;
        return v;
    }
}

/* Returns 1 if the stream is exactly consumed (all bits read, no overflow). */
static inline int zgec_br_done(const zgec_br *b)
{
    return b->nacc == 0 && b->left == 0 && !b->overflow;
}

/* Writer.
 *
 * zgec_bw_write is the mirror of the reader above and runs once per emitted
 * bit field -- about two calls per sequence, which is several million calls
 * on a single frame. It lives here as `always_inline` for exactly the reason
 * the reader does: an out-of-line call cost more than the few shifts it
 * performs, and without LTO it could not be inlined from bitstream.c. Only
 * the per-stream init and finish stay out of line. */

/* Store the low `bytes` bytes (0..8) of `acc` at `dst`, in stream order:
 * dst[0] is acc's least significant byte. This is the defined little-endian
 * store the writer needs -- `*(uint64_t *)dst = acc` would reproduce this
 * ordering only on a little-endian target, and would break the alignment and
 * strict-aliasing rules besides. Each byte comes from the unmodified
 * accumulator, so the extractions are independent and the routine carries no
 * dependency chain through acc: the caller shifts acc once, afterwards. It
 * writes exactly `bytes` bytes and never one more, so it needs no writable
 * padding beyond what the caller emitted. */
static inline __attribute__((always_inline))
void zgec_bw_flush(uint8_t *dst, uint64_t acc, unsigned bytes)
{
    unsigned i;
    for (i = 0; i < bytes; i++)
        dst[i] = (uint8_t)(acc >> (8u * i));
}

static inline __attribute__((always_inline))
void zgec_bw_write(zgec_bw *b, uint32_t value, unsigned n)
{
    uint64_t mask;
    unsigned bytes;

    if (n == 0) return;
    /* Overflow is sticky. Once the buffer is full the stream is already
     * unusable, and returning here stops acc/nacc from growing on every
     * later call: without it a caller that keeps writing after overflow
     * pushes nacc past 63, and the shift below is then undefined (a shift
     * count >= the 64-bit operand width). One predictable branch on state
     * the function loads anyway. */
    if (b->overflow) return;
    /* `value` is a uint32_t, so n > 32 cannot be honoured: the mask would
     * drop value's high bits and the flush below would then emit
     * fabricated zero bits. Reject the write rather than corrupt the
     * stream. The writer's contract is n <= 32. */
    if (n > 32) { b->overflow = 1; return; }

    mask = (1ull << n) - 1ull;   /* exact for n == 32; n >= 1 here */
    b->acc |= (uint64_t)(value & mask) << b->nacc;
    b->nacc += n;

    /* Flush the completed bytes for the whole call at once: one capacity
     * check, one bounded store and one accumulator shift, instead of a
     * bounds check, branch, store, shift and subtraction per byte. The
     * subtraction form of the capacity test cannot form a pointer past
     * the end of the buffer, which `ptr + bytes > end` could. */
    bytes = b->nacc >> 3;
    b->nacc &= 7u;
    if (bytes > 0) {
        if ((size_t)(b->end - b->ptr) < (size_t)bytes) {
            b->overflow = 1;
            return;
        }
        zgec_bw_flush(b->ptr, b->acc, bytes);
        b->ptr += bytes;
        b->acc >>= bytes * 8u;
    }
}

void zgec_bw_init(zgec_bw *b, void *buf, size_t capacity);
/* Flushes remaining bits and appends the sentinel byte.
   Returns the total stream size in bytes, or 0 on overflow. */
size_t zgec_bw_finish(zgec_bw *b);

#endif /* ZGEC_BITSTREAM_H */
