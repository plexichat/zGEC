#include "zgec_bitstream.h"

/*
 * Backward bitstream per RFC 8878 section 4.1 and zGEC
 * section 8.4.
 *
 * Reader model: a 64-bit accumulator holds the next bits
 * to consume, with the next-to-consume bit at position 63
 * (MSB). The stream is bytes B[0..m-1]; the last byte's
 * highest set bit is the sentinel. The consumable bits, in
 * consumption order, are the last byte's bits below the
 * sentinel (from bit s-1 down to bit 0), then the earlier
 * bytes' bits from bit 7 down to bit 0.
 *
 * Writer model: the mirror. Bits accumulate LSB-first into
 * a 64-bit accumulator (the first-written bit of a value
 * lands at the accumulator's low end, so the reader -- which
 * consumes a byte's high bits first -- sees the value's most
 * significant bit first). Full bytes flush forward (low to
 * high address). The sentinel is appended as the final byte.
 */

/* The backward reader (zgec_br_init / _refill / _read / _done) is defined
 * `static inline` in zgec_bitstream.h: it runs twice per decoded symbol, so
 * it must inline into fse.c/seq.c/decode.c rather than be called. */

void zgec_bw_init(zgec_bw *b, void *buf, size_t capacity)
{
    b->ptr = (uint8_t *)buf;
    b->start = (uint8_t *)buf;
    b->end = (uint8_t *)buf + capacity;
    b->acc = 0;
    b->nacc = 0;
    b->overflow = 0;
}

/* Store the low `bytes` bytes (0..8) of `acc` at `dst`, in stream order:
 * dst[0] is acc's least significant byte. This is the defined
 * little-endian store the writer needs -- `*(uint64_t *)dst = acc` would
 * reproduce this ordering only on a little-endian target, and would break
 * the alignment and strict-aliasing rules besides. Each byte comes from
 * the unmodified accumulator, so the extractions are independent and the
 * routine carries no dependency chain through acc: the caller shifts acc
 * once, afterwards. It writes exactly `bytes` bytes and never one more, so
 * it needs no writable padding beyond what the caller emitted. */
static void zgec_bw_flush(uint8_t *dst, uint64_t acc, unsigned bytes)
{
    unsigned i;
    for (i = 0; i < bytes; i++)
        dst[i] = (uint8_t)(acc >> (8u * i));
}

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

size_t zgec_bw_finish(zgec_bw *b)
{
    if (b->overflow) return 0;
    if (b->ptr >= b->end) { b->overflow = 1; return 0; }
    *b->ptr++ = (uint8_t)(b->acc | (1u << b->nacc));
    return (size_t)(b->ptr - b->start);
}
