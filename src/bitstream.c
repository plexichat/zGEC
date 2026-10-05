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

void zgec_br_init(zgec_br *b, const void *buf, size_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    b->start = p;
    b->end = p + size;
    b->overflow = 0;
    if (size == 0) {
        b->ptr = p - 1;
        b->acc = 0;
        b->nacc = 0;
        b->overflow = 1;   /* a stream must have a sentinel byte */
        return;
    }
    uint8_t last = p[size - 1];
    if (last == 0) {
        b->ptr = p - 1;
        b->acc = 0;
        b->nacc = 0;
        b->overflow = 1;   /* sentinel missing */
        return;
    }
    unsigned s = zgec_highbit32(last);   /* sentinel position 0..7 */
    /* the consumable bits of the last byte are bits [s-1 .. 0];
       place them at the top of the accumulator, bit s-1 at 63 */
    b->acc = (s == 0) ? 0ull
                      : (uint64_t)(last & (uint8_t)((1u << s) - 1u)) << (64 - s);
    b->nacc = s;
    b->ptr = p + size - 2;   /* next byte to load, moving backwards */
}

void zgec_br_refill(zgec_br *b)
{
    while (b->nacc <= 56 && b->ptr >= b->start) {
        b->acc |= (uint64_t)(b->ptr[0]) << (56 - b->nacc);
        b->nacc += 8;
        b->ptr--;
    }
}

uint32_t zgec_br_read(zgec_br *b, unsigned n)
{
    if (n == 0) return 0;
    if (b->nacc < n) {
        zgec_br_refill(b);
        if (b->nacc < n) {
            b->overflow = 1;
            return 0;
        }
    }
    uint32_t v = (uint32_t)(b->acc >> (64 - n));
    b->acc <<= n;
    b->nacc -= n;
    return v;
}

int zgec_br_done(const zgec_br *b)
{
    return b->nacc == 0 && b->ptr < b->start && !b->overflow;
}

void zgec_bw_init(zgec_bw *b, void *buf, size_t capacity)
{
    b->ptr = (uint8_t *)buf;
    b->start = (uint8_t *)buf;
    b->end = (uint8_t *)buf + capacity;
    b->acc = 0;
    b->nacc = 0;
    b->overflow = 0;
}

void zgec_bw_write(zgec_bw *b, uint32_t value, unsigned n)
{
    if (n == 0) return;
    uint64_t mask = (n >= 32) ? 0xFFFFFFFFull : ((1ull << n) - 1ull);
    b->acc |= (uint64_t)(value & mask) << b->nacc;
    b->nacc += n;
    while (b->nacc >= 8) {
        if (b->ptr >= b->end) { b->overflow = 1; return; }
        *b->ptr++ = (uint8_t)(b->acc & 0xFFu);
        b->acc >>= 8;
        b->nacc -= 8;
    }
}

size_t zgec_bw_finish(zgec_bw *b)
{
    if (b->overflow) return 0;
    if (b->ptr >= b->end) { b->overflow = 1; return 0; }
    *b->ptr++ = (uint8_t)(b->acc | (1u << b->nacc));
    return (size_t)(b->ptr - b->start);
}
