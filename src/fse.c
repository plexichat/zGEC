#include "zgec_fse.h"

#include <stdlib.h>
#include <string.h>

/* ---- forward bit reader (LSB-first, little-endian words) ---- */

void zgec_fb_init(zgec_fwd_bits *f, const uint8_t *p, size_t size)
{
    f->p = p;
    f->end = p + size;
    f->acc = 0;
    f->nacc = 0;
}

static int zgec_fb_ensure(zgec_fwd_bits *f, unsigned n)
{
    while ((unsigned)f->nacc < n && f->p < f->end) {
        f->acc |= (uint64_t)(*f->p++) << (unsigned)f->nacc;
        f->nacc += 8;
    }
    return (unsigned)f->nacc >= n;
}

uint32_t zgec_fb_read(zgec_fwd_bits *f, unsigned n, zgec_err *err)
{
    if (n > 32) { if (err) *err = ZGEC_ERR_BITSTREAM; return 0; }
    if (n == 0) { if (err) *err = ZGEC_OK; return 0; }
    if (!f || !zgec_fb_ensure(f, n)) { if (err) *err = ZGEC_ERR_BITSTREAM; return 0; }
    uint64_t mask = (n >= 64) ? (uint64_t)~0ull : (((uint64_t)1 << n) - 1u);
    uint32_t v = (uint32_t)(f->acc & mask);
    f->acc >>= n;
    f->nacc -= (int)n;
    if (err) *err = ZGEC_OK;
    return v;
}

/* ---- FSE table construction (Annex A, exact) ---- */

zgec_err zgec_fse_build_dec(zgec_fse_dec_table **out,
                             const int16_t *counts, int nsym, int al)
{
    if (!out || !counts) return ZGEC_ERR_INVAL;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    if (nsym <= 0 || nsym > 256) return ZGEC_ERR_INVAL;
    int S = 1 << al;
    int effective_sum = 0;
    for (int s = 0; s < nsym; s++) {
        if (counts[s] < -1) return ZGEC_ERR_FSE_COUNTS;
        int eff = counts[s] < 0 ? 1 : counts[s];
        effective_sum += eff;
    }
    if (effective_sum != S) return ZGEC_ERR_FSE_SUM;

    size_t dec_size = sizeof(zgec_fse_dec_table) + (size_t)nsym * sizeof(int);
    size_t T_size = (size_t)S * sizeof(zgec_fse_dec_entry);
    zgec_fse_dec_table *t = (zgec_fse_dec_table *)zgec_alloc(dec_size + T_size, _Alignof(zgec_fse_dec_table));
    if (!t) return ZGEC_ERR_NOMEM;
    memset(t, 0, dec_size + T_size);
    t->e = (zgec_fse_dec_entry *)((uint8_t *)t + dec_size);
    t->al = al;
    t->nsym = nsym;

    /* Symbol placement buffer MUST be separate from t->e[] (8-byte entries):
       the second pass reads table[i] while writing t->e[i], which would
       otherwise alias. */
    unsigned char table[1u << ZGEC_MAX_AL];   /* S <= 2048 */
    memset(table, 0, (size_t)S);
    int high = S - 1;
    for (int s = 0; s < nsym; s++) {
        if (counts[s] < 0) table[high--] = (unsigned char)s;
    }
    unsigned step = (unsigned)((S >> 1) + (S >> 3) + 3);
    unsigned mask = (unsigned)(S - 1);
    unsigned pos = 0;
    for (int s = 0; s < nsym; s++) {
        int n = counts[s];
        if (n <= 0) continue;
        for (int i = 0; i < n; i++) {
            table[pos] = (unsigned char)s;
            do { pos = (pos + step) & mask; } while ((int)pos > high);
        }
    }
    if (pos != 0) { zgec_free(t); return ZGEC_ERR_FSE_COUNTS; }

    int next[256];
    memset(next, 0, sizeof(next));
    for (int s = 0; s < nsym; s++) next[s] = counts[s] < 0 ? 1 : counts[s];
    for (int i = 0; i < S; i++) {
        int s = table[i];
        if (s < 0 || s >= nsym) { zgec_free(t); return ZGEC_ERR_FSE_COUNTS; }
        int ns = next[s]++;
        unsigned nbBits = (unsigned)al - (unsigned)zgec_highbit32((uint32_t)ns);
        if (nbBits > (unsigned)al) nbBits = (unsigned)al;
        t->e[i].symbol = (int16_t)s;
        t->e[i].nb_bits = (uint8_t)nbBits;
        t->e[i].baseline = (int32_t)(((unsigned)ns << nbBits) - (unsigned)S);
    }

    memset(t->first_state, 0xFF, (size_t)nsym * sizeof(int));
    for (int i = 0; i < S; i++) {
        int s = t->e[i].symbol;
        if (t->first_state[s] < 0) t->first_state[s] = i;
    }

    *out = t;
    return ZGEC_OK;
}

void zgec_fse_free_dec(zgec_fse_dec_table *t)
{
    zgec_free(t);
}

zgec_err zgec_fse_build_enc(zgec_fse_enc_table **out, const zgec_fse_dec_table *dec)
{
    if (!dec || !out) return ZGEC_ERR_INVAL;
    int al = dec->al;
    int nsym = dec->nsym;
    int S = 1 << al;

    size_t enc_size = sizeof(zgec_fse_enc_table) + (size_t)nsym * (size_t)S * sizeof(zgec_fse_enc_entry);
    zgec_fse_enc_table *t = (zgec_fse_enc_table *)zgec_alloc(enc_size, _Alignof(zgec_fse_enc_table));
    if (!t) return ZGEC_ERR_NOMEM;
    memset(t, 0, enc_size);
    t->e = (zgec_fse_enc_entry *)((uint8_t *)t + sizeof(zgec_fse_enc_table));
    t->al = al;
    t->nsym = nsym;

    int counts[256];
    memset(counts, 0, sizeof(counts));
    for (int i = 0; i < S; i++) counts[dec->e[i].symbol]++;

    /* Each symbol owns its own 2^al block at (s * S), so the occurrence
     * index j is 0-based within that block. (An earlier version added
     * offset[s] here, which belongs to a flat layout; it shifted every
     * symbol after the first onto uninitialised cells, so the encoder's
     * next-state mapping for those symbols was all zeros.) */
    int used[256];
    memset(used, 0, sizeof(used));
    for (int i = 0; i < S; i++) {
        int s = dec->e[i].symbol;
        int j = used[s]++;
        size_t idx = (size_t)s * (size_t)S + (size_t)j;
        t->e[idx].nb_bits = dec->e[i].nb_bits;
        t->e[idx].new_state = (uint16_t)i;
        t->e[idx].baseline = (uint16_t)dec->e[i].baseline;
    }

    size_t full_size = (size_t)nsym * (size_t)S * sizeof(zgec_fse_enc_entry);
    zgec_fse_enc_table *full = (zgec_fse_enc_table *)zgec_alloc(sizeof(zgec_fse_enc_table) + full_size, _Alignof(zgec_fse_enc_table));
    if (!full) { zgec_fse_free_enc(t); return ZGEC_ERR_NOMEM; }
    memset(full, 0, sizeof(zgec_fse_enc_table) + full_size);
    full->e = (zgec_fse_enc_entry *)((uint8_t *)full + sizeof(zgec_fse_enc_table));
    full->al = al;
    full->nsym = nsym;

    for (int s = 0; s < nsym; s++) {
        for (int j = 0; j < counts[s]; j++) {
            uint8_t nb = t->e[(size_t)s * (size_t)S + (size_t)j].nb_bits;
            uint16_t st = t->e[(size_t)s * (size_t)S + (size_t)j].new_state;
            uint16_t bl = t->e[(size_t)s * (size_t)S + (size_t)j].baseline;
            unsigned range = 1u << nb;
            int lo = (int)bl;                       /* decoder next-state range is
                                                       [baseline, baseline+range-1] */
            int hi = (int)bl + (int)range - 1;
            for (int stprev = lo; stprev <= hi; stprev++) {
                if (stprev >= 0 && stprev < S) {
                    size_t idx = (size_t)s * (size_t)S + (size_t)stprev;
                    full->e[idx].nb_bits = nb;
                    full->e[idx].new_state = st;
                    full->e[idx].baseline = bl;
                }
            }
        }
    }

    zgec_fse_free_enc(t);
    *out = full;
    return ZGEC_OK;
}

void zgec_fse_free_enc(zgec_fse_enc_table *t)
{
    zgec_free(t);
}

/* ---- normalised-count serialisation (RFC 8878 section 4.1.1) ---- */

/* LSB-first bit reader over buf (bits consumed low bit first). */
typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t bitpos;
    int ok;
} zgec_lsbr;

static uint32_t zgec_lsbr_read(zgec_lsbr *r, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (r->bitpos >= r->size * 8u) { r->ok = 0; return 0; }
        unsigned b = (unsigned)((r->buf[r->bitpos >> 3] >> (r->bitpos & 7)) & 1u);
        v |= (uint32_t)(b << i);
        r->bitpos++;
    }
    return v;
}

size_t zgec_fse_read_counts(int16_t *counts, int *nsym, int *al,
                            int max_nsym, const uint8_t *buf, size_t size)
{
    if (!nsym || !al || !buf || size == 0) return 0;
    int probe = (counts == NULL);
    if (!probe && (max_nsym <= 0 || max_nsym > 256)) return 0;

    zgec_lsbr r;
    r.buf = buf;
    r.size = size;
    r.bitpos = 0;
    r.ok = 1;

    uint32_t low4 = zgec_lsbr_read(&r, 4);
    if (!r.ok) return 0;
    int a = (int)low4 + 5;
    if (a < ZGEC_MIN_AL || a > ZGEC_MAX_AL) return 0;
    if (!probe) {
        if (max_nsym == ZGEC_NSYM_LIT) {
            if (a != ZGEC_LIT_AL) return 0;
        } else {
            if (a < ZGEC_MIN_AL || a > ZGEC_MAX_AL) return 0;
        }
    }

    int tableSize = 1 << a;
    int remaining = tableSize + 1;
    int threshold = tableSize;
    unsigned nbBits = (unsigned)a + 1;
    unsigned cap = probe ? 256u : (unsigned)max_nsym;
    int16_t tmp[256];
    memset(tmp, 0, sizeof(tmp));
    if (!probe) memset(counts, 0, (size_t)max_nsym * sizeof(int16_t));

    unsigned charnum = 0;
    int previous0 = 0;
    for (;;) {
        if (previous0) {
            /* Zero-run: 2-bit flags; 3 means continue with +3. */
            for (;;) {
                uint32_t v = zgec_lsbr_read(&r, 2);
                if (!r.ok) return 0;
                if (v == 3) {
                    charnum += 3;
                    if (charnum > cap) return 0;
                } else {
                    charnum += v;
                    break;
                }
            }
            if (charnum > cap) return 0;
        }
        int max = (2 * threshold - 1) - remaining;
        uint32_t low = 0;
        if (nbBits > 1) {
            low = zgec_lsbr_read(&r, nbBits - 1);
            if (!r.ok) return 0;
        } else if (nbBits == 0) {
            return 0;
        }
        int count;
        if ((int)low < max) {
            count = (int)low;
        } else {
            uint32_t high = zgec_lsbr_read(&r, 1);
            if (!r.ok) return 0;
            unsigned v = low + (high << (nbBits - 1));
            count = (int)v;
            if (count >= threshold) count -= max;
        }
        count--;   /* 0 on the wire means -1 ("less than one") */
        if (charnum >= cap) return 0;
        if (!probe) counts[charnum] = (int16_t)count;
        else tmp[charnum] = (int16_t)count;
        (void)tmp;
        charnum++;
        previous0 = (count == 0);
        if (count >= 0) remaining -= count;
        else remaining += count;   /* count == -1: counts as 1 */
        if (remaining < threshold) {
            if (remaining <= 1) break;
            nbBits = (unsigned)zgec_highbit32((uint32_t)remaining) + 1u;
            threshold = 1 << (nbBits - 1);
        }
        if (charnum >= cap) break;
    }
    if (!r.ok) return 0;
    if (remaining != 1) return 0;
    size_t nbytes = (r.bitpos + 7) >> 3;
    if (nbytes == 0 || nbytes > size) return 0;
    *nsym = (int)charnum;
    *al = a;
    return nbytes;
}

size_t zgec_fse_write_counts(uint8_t *buf, size_t cap,
                             const int16_t *counts, int nsym, int al)
{
    if (!buf || !counts || nsym <= 0 || nsym > 256) return 0;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return 0;
    if (nsym == ZGEC_NSYM_LIT && al != ZGEC_LIT_AL) return 0;
    int S = 1 << al;
    long sum = 0;
    for (int s = 0; s < nsym; s++) {
        if (counts[s] < -1) return 0;
        sum += counts[s] < 0 ? 1 : counts[s];
    }
    if (sum != S) return 0;

    int maxSym = -1;
    for (int s = nsym - 1; s >= 0; s--) {
        if (counts[s] != 0) { maxSym = s; break; }
    }
    if (maxSym < 0) return 0;

    uint8_t * const ostart = buf;
    uint8_t *out = ostart;
    uint8_t * const oend = ostart + cap;
    uint32_t bitStream = 0;
    int bitCount = 0;
    unsigned symbol = 0;
    unsigned alphabetSize = (unsigned)maxSym + 1;
    int remaining = S + 1;
    int threshold = S;
    int nbBits = al + 1;
    int previousIs0 = 0;

    /* Accuracy log in low 4 bits. */
    bitStream += (uint32_t)(al - 5) << bitCount;
    bitCount += 4;

    while (symbol < alphabetSize && remaining > 1) {
        if (previousIs0) {
            unsigned start = symbol;
            while (symbol < alphabetSize && counts[symbol] == 0) symbol++;
            if (symbol == alphabetSize) break;
            while (symbol >= start + 24) {
                start += 24;
                bitStream += (uint32_t)0xFFFFu << bitCount;
                if (out > oend - 2) return 0;
                out[0] = (uint8_t)bitStream;
                out[1] = (uint8_t)(bitStream >> 8);
                out += 2;
                bitStream >>= 16;
            }
            while (symbol >= start + 3) {
                start += 3;
                bitStream += (uint32_t)3u << bitCount;
                bitCount += 2;
            }
            bitStream += (uint32_t)(symbol - start) << bitCount;
            bitCount += 2;
            if (bitCount > 16) {
                if (out > oend - 2) return 0;
                out[0] = (uint8_t)bitStream;
                out[1] = (uint8_t)(bitStream >> 8);
                out += 2;
                bitStream >>= 16;
                bitCount -= 16;
            }
        }
        {
            int count = counts[symbol++];
            int max = (2 * threshold - 1) - remaining;
            remaining -= count < 0 ? -count : count;
            count++;   /* +1 for extra accuracy */
            if (count >= threshold) count += max;
            bitStream += (uint32_t)count << bitCount;
            bitCount += nbBits;
            bitCount -= (count < max);
            previousIs0 = (count == 1);
            if (remaining < 1) return 0;
            while (remaining < threshold) { nbBits--; threshold >>= 1; }
        }
        if (bitCount > 16) {
            if (out > oend - 2) return 0;
            out[0] = (uint8_t)bitStream;
            out[1] = (uint8_t)(bitStream >> 8);
            out += 2;
            bitStream >>= 16;
            bitCount -= 16;
        }
    }

    if (remaining != 1) return 0;
    if (symbol > alphabetSize) return 0;

    size_t tail = (size_t)(bitCount + 7) / 8u;
    if ((size_t)(oend - out) < 2 && tail > (size_t)(oend - out)) return 0;
    if (out > oend - 2 && tail > 0) {
        /* Not enough room for the 2-byte flush window. */
        if ((size_t)(oend - out) < tail) return 0;
    }
    if ((size_t)(oend - out) < tail) return 0;
    out[0] = (uint8_t)bitStream;
    if (tail > 1) out[1] = (uint8_t)(bitStream >> 8);
    out += tail;
    return (size_t)(out - ostart);
}

/* ---- FSE stream decode / encode (section 8.4) ---- */

zgec_err zgec_fse_decode(const zgec_fse_dec_table *t, zgec_br *br,
                          uint32_t *out, uint8_t *syms, size_t n,
                          const uint32_t *base, const uint8_t *nbits)
{
    if (!t || !br) return ZGEC_ERR_INVAL;
    if (n == 0) {
        if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
        return ZGEC_OK;
    }
    int S = 1 << t->al;
    uint32_t state = zgec_br_read(br, (unsigned)t->al);
    if (br->overflow) return ZGEC_ERR_BITSTREAM;
    if (state >= (unsigned)S) return ZGEC_ERR_BITSTREAM;

    for (size_t i = 0; i < n; i++) {
        const zgec_fse_dec_entry *e = &t->e[state];
        if (e->symbol < 0 || e->symbol >= t->nsym) return ZGEC_ERR_FSE_SYMBOL;
        uint8_t sym = (uint8_t)e->symbol;
        if (syms) syms[i] = sym;

        uint32_t extra = 0;
        if (nbits && nbits[sym] > 0) {
            extra = zgec_br_read(br, (unsigned)nbits[sym]);
            if (br->overflow) return ZGEC_ERR_BITSTREAM;
        }
        if (out) {
            out[i] = (base ? base[sym] : (uint32_t)sym) + extra;
        }

        if (i + 1 < n) {
            uint32_t bits = zgec_br_read(br, e->nb_bits);
            if (br->overflow) return ZGEC_ERR_BITSTREAM;
            int32_t next = (int32_t)(e->baseline + (int32_t)bits);
            if (next < 0 || next >= S) return ZGEC_ERR_BITSTREAM;
            state = (unsigned)next;
        }
    }
    if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
    return ZGEC_OK;
}

zgec_err zgec_fse_encode(const zgec_fse_enc_table *t, zgec_bw *bw,
                          const uint32_t *values, const uint8_t *syms, size_t n,
                          const uint32_t *base, const uint8_t *nbits)
{
    if (!t || !bw) return ZGEC_ERR_INVAL;
    if (n == 0) return ZGEC_OK;
    if (!values || !syms) return ZGEC_ERR_INVAL;
    int S = 1 << t->al;

    uint8_t last_s = syms[n - 1];
    if ((int)last_s >= t->nsym) return ZGEC_ERR_FSE_SYMBOL;
    zgec_fse_enc_entry first_entry = t->e[(size_t)last_s * (size_t)S + 0];
    uint32_t state = first_entry.new_state;

    if (nbits && nbits[last_s] > 0) {
        uint32_t extra = values[n - 1] - (base ? base[last_s] : (uint32_t)last_s);
        zgec_bw_write(bw, extra, (unsigned)nbits[last_s]);
    }

    for (size_t i = n - 1; i > 0; i--) {
        uint8_t s = syms[i - 1];
        if ((int)s >= t->nsym) return ZGEC_ERR_FSE_SYMBOL;
        zgec_fse_enc_entry entry = t->e[(size_t)s * (size_t)S + state];
        uint32_t bits = (state >= (uint32_t)entry.baseline)
                            ? (uint32_t)(state - (uint32_t)entry.baseline)
                            : 0u;
        zgec_bw_write(bw, bits, (unsigned)entry.nb_bits);
        if (nbits && nbits[s] > 0) {
            uint32_t extra = values[i - 1] - (base ? base[s] : (uint32_t)s);
            zgec_bw_write(bw, extra, (unsigned)nbits[s]);
        }
        state = entry.new_state;
    }

    zgec_bw_write(bw, state, (unsigned)t->al);
    return ZGEC_OK;
}
