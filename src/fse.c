#include "zgec_fse.h"

#include <stdlib.h>
#include <string.h>

/* ---- forward bit reader (LSB-first, little-endian words) ---- */

void zgec_fb_init(zgec_fwd_bits *f, const uint8_t *p, size_t size)
{
    /* A NULL buffer is an empty stream, not "NULL + size": NULL + 0 is
     * already an undefined pointer, and NULL + size lands on a bogus
     * non-NULL end, after which the reader's `f->p < f->end` comparison
     * succeeds and it dereferences NULL. Leaving p and end both NULL makes
     * every subsequent read report a truncated stream, which is what an
     * empty buffer is. The signature is fixed by zgec_fse.h, so this cannot
     * report the invalid case; see fix.md entry 27.
     *
     * A non-NULL p still forms p + size unconditionally, so `size` must be
     * the true length of the object p points at: a larger value forms the
     * same out-of-range pointer, one branch later. The sibling reader
     * zgec_lsbr carries a remaining count instead, which is what this would
     * become if the signature allowed it (review finding 5). */
    if (!f) return;
    f->p = p;
    f->end = (p == NULL) ? NULL : p + size;
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
    /* A failed build must not leave the caller holding whatever pointer it
     * had before (fix.md entry 26), so the out-parameter is cleared before
     * anything below can fail. */
    *out = NULL;
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
    /* One (symbol, occurrence index) record. Held in a flat S-entry
     * scratch instead of a full nsym * S intermediate table: the sum of
     * counts[s] is exactly S, so this is at most 2^ZGEC_MAX_AL = 2048
     * entries (16 KiB) rather than nsym * S entries (~1 MiB for
     * sequences, allocated and zeroed per table build, i.e. per stream
     * per segment). */
    struct zgec_fse_occ {
        uint8_t  nb_bits;
        uint16_t new_state;
        uint16_t baseline;
    };

    if (!dec || !out) return ZGEC_ERR_INVAL;
    *out = NULL;                        /* fix.md entry 26 */
    /* Validate the table object before indexing it (fix.md entry 25): al
     * bounds S below, nsym bounds the rows of the table built here, and e is
     * dereferenced throughout. */
    if (dec->al < ZGEC_MIN_AL || dec->al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    if (dec->nsym <= 0 || dec->nsym > 256) return ZGEC_ERR_FSE_COUNTS;
    if (dec->e == NULL) return ZGEC_ERR_FSE_COUNTS;
    int al = dec->al;
    int nsym = dec->nsym;
    int S = 1 << al;

    struct zgec_fse_occ *occ;
    int counts[256];
    int off[256];
    int run;
    size_t full_size;
    zgec_fse_enc_table *full;

    /* counts/off/used are 256-entry locals, so a symbol outside the alphabet
     * has to be caught before it indexes them (fix.md entry 25). */
    memset(counts, 0, sizeof(counts));
    for (int i = 0; i < S; i++) {
        int sym = dec->e[i].symbol;
        if (sym < 0 || sym >= nsym) return ZGEC_ERR_FSE_SYMBOL;
        counts[sym]++;
    }

    /* Per-symbol base within the flat scratch. */
    run = 0;
    for (int s = 0; s < nsym; s++) {
        off[s] = run;
        run += counts[s];
    }

    occ = (struct zgec_fse_occ *)zgec_alloc(
        (size_t)S * sizeof(*occ), _Alignof(struct zgec_fse_occ));
    if (!occ) return ZGEC_ERR_NOMEM;

    /* Each symbol owns its own 2^al block at (s * S) in the encoder
     * table, so the occurrence index j is 0-based within that block.
     * (An earlier version added offset[s] to the in-table index, which
     * belongs to a flat layout; it shifted every symbol after the first
     * onto uninitialised cells, so the encoder's next-state mapping for
     * those symbols was all zeros.) The scratch stores them per symbol
     * with the same j indexing, so the expansion below reads exactly
     * what the old intermediate table held. */
    {
        int used[256];
        memset(used, 0, sizeof(used));
        for (int i = 0; i < S; i++) {
            int s = dec->e[i].symbol;
            int j = used[s]++;
            struct zgec_fse_occ *o = &occ[(size_t)off[s] + (size_t)j];
            o->nb_bits = dec->e[i].nb_bits;
            o->new_state = (uint16_t)i;
            o->baseline = (uint16_t)dec->e[i].baseline;
        }
    }

    full_size = (size_t)nsym * (size_t)S * sizeof(zgec_fse_enc_entry);
    full = (zgec_fse_enc_table *)zgec_alloc(sizeof(zgec_fse_enc_table) + full_size, _Alignof(zgec_fse_enc_table));
    if (!full) { zgec_free(occ); return ZGEC_ERR_NOMEM; }
    /* Zero-fill is load-bearing, not just hygiene: a symbol with zero
     * occurrences owns a block of S cells that no occurrence writes, and the
     * encoder relies on those reading as (nb_bits = 0, new_state = 0) rather
     * than as uninitialised memory. Proving coverage below therefore only
     * covers symbols that actually occur.
     *
     * The zero-fill also supplies the encoder's presence marker. Every cell of
     * a symbol that does occur is written with pad = 1 below, so an all-zero
     * cell proves the symbol was absent from the table it is being asked to
     * encode -- an assumption the old comment stated but nothing checked, and
     * which zgec_fse_encode could not otherwise test because an untouched cell
     * and a legitimate zero transition are the same bits. zgec_fse_encode now
     * refuses such a symbol instead of writing a transition nobody chose
     * (review finding 1). `pad` is unused elsewhere and this table is built
     * per stream and never serialised, so this is not a format change. */
    memset(full, 0, sizeof(zgec_fse_enc_table) + full_size);
    full->e = (zgec_fse_enc_entry *)((uint8_t *)full + sizeof(zgec_fse_enc_table));
    full->al = al;
    full->nsym = nsym;

    /* Fill every cell, proving as we go that every cell of every symbol's
     * 2^al block is written exactly once. One symbol's occurrence ranges
     * tile [0, 2^al) (see the note in zgec_fse.h), so a gap would hand the
     * encoder a transition nobody chose and an overlap would leave two
     * occurrences disagreeing about the same cell. An overlap is always a
     * defect, so it is rejected outright. An apparent *gap* is not: it only
     * means something for a symbol that occurs, so the completeness test below
     * is guarded by counts[s] > 0 and the table keeps its zero-fill for the
     * blocks of symbols that never occur (fix.md entry 4, corrected).
     * `covered` is one bit per state -- 256 bytes, reused per symbol. */
    {
        uint8_t covered[(1u << ZGEC_MAX_AL) / 8u];   /* S <= 2048, so 256 bytes */
        /* Rounded up: the duplicate test below indexes with stprev >> 3, which
         * reaches byte (S - 1) / 8, so a truncated S / 8 leaves the last byte
         * uncleared -- and clears nothing at all for S < 8, where the OR test
         * would then compare against stack garbage. Harmless while
         * ZGEC_MIN_AL keeps S >= 32, but this ties the clear to the indices
         * the loop can reach rather than to that constant (review finding 3). */
        size_t covered_bytes = ((size_t)S + 7u) / 8u;

        for (int s = 0; s < nsym; s++) {
            int covered_cells = 0;
            memset(covered, 0, covered_bytes);
            for (int j = 0; j < counts[s]; j++) {
                const struct zgec_fse_occ *o = &occ[(size_t)off[s] + (size_t)j];
                uint8_t nb = o->nb_bits;
                uint16_t st = o->new_state;
                uint16_t bl = o->baseline;
                unsigned range;
                int lo, hi, stprev;

                /* Defensive only: nb comes from o->nb_bits, which
                 * zgec_fse_build_dec derives as al - highbit32(ns) and clamps
                 * to al, so nb <= al holds for every table that builder can
                 * produce and this branch is unreachable from here (review
                 * finding 4). It is kept so a future builder cannot widen a
                 * cell past its row unnoticed. */
                if ((unsigned)nb > (unsigned)al) {
                    zgec_free(occ); zgec_free(full); return ZGEC_ERR_FSE_COUNTS;
                }
                range = 1u << nb;
                lo = (int)bl;                        /* decoder next-state range is
                                                        [baseline, baseline+range-1] */
                hi = lo + (int)range - 1;
                for (stprev = lo; stprev <= hi; stprev++) {
                    size_t idx, byte;
                    uint8_t bit;
                    if (stprev < 0 || stprev >= S) continue;
                    byte = (size_t)stprev >> 3;
                    bit = (uint8_t)(1u << ((unsigned)stprev & 7u));
                    if (covered[byte] & bit) {       /* two occurrences, one cell */
                        zgec_free(occ); zgec_free(full); return ZGEC_ERR_FSE_COUNTS;
                    }
                    covered[byte] = (uint8_t)(covered[byte] | bit);
                    covered_cells++;
                    idx = (size_t)s * (size_t)S + (size_t)stprev;
                    full->e[idx].nb_bits = nb;
                    full->e[idx].new_state = st;
                    full->e[idx].baseline = bl;
                    full->e[idx].pad = (uint16_t)1;   /* symbol is present */
                }
            }
            /* Only a symbol that occurs has a partition to be complete: the
             * occurrence ranges of symbol s tile [0, S) exactly, but a symbol
             * with counts[s] == 0 runs this loop zero times and covers nothing,
             * which is not a gap -- its block is the zero-fill above. */
            if (counts[s] > 0 && covered_cells != S) {   /* a cell nobody wrote */
                zgec_free(occ); zgec_free(full); return ZGEC_ERR_FSE_COUNTS;
            }
        }
    }

    zgec_free(occ);
    *out = full;
    return ZGEC_OK;
}

void zgec_fse_free_enc(zgec_fse_enc_table *t)
{
    zgec_free(t);
}

/* ---- normalised-count serialisation (RFC 8878 section 4.1.1) ---- */

/* LSB-first bit reader over buf (bits consumed low bit first).
 *
 * The bits are now held in a 64-bit accumulator instead of being extracted
 * one bit at a time: this reader runs once per count field for every table
 * description in the file, and the old loop spent a bounds test, a load, a
 * shift and an or on each bit (fix.md entry 7). `bitpos` remains the
 * authoritative consumed-bit count -- the reported byte length is still
 * (bitpos + 7) / 8, and the bits returned are the same bits in the same
 * order -- while `next` is only the load cursor. The bounds test is on the
 * byte index rather than on `size * 8`, which could overflow size_t
 * (fix.md entry 6). */
typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t bitpos;   /* bits consumed so far */
    size_t next;     /* next byte to load into acc */
    uint64_t acc;    /* unconsumed bits; bit 0 is the next bit to consume */
    unsigned nacc;   /* unconsumed bits held in acc (0..64) */
    int ok;
} zgec_lsbr;

static int zgec_lsbr_ensure(zgec_lsbr *r, unsigned n)
{
    while (r->nacc < n && r->nacc <= 56u && r->next < r->size) {
        r->acc |= (uint64_t)r->buf[r->next] << r->nacc;
        r->next++;
        r->nacc += 8u;
    }
    return r->nacc >= n;
}

static uint32_t zgec_lsbr_read(zgec_lsbr *r, unsigned n)
{
    uint32_t v;
    if (n == 0u) return 0u;
    if (n > 32u || !zgec_lsbr_ensure(r, n)) { r->ok = 0; return 0u; }
    v = (n == 32u) ? (uint32_t)r->acc
                   : (uint32_t)(r->acc & (((uint64_t)1u << n) - 1u));
    r->acc >>= n;
    r->nacc -= n;
    r->bitpos += n;      /* only on success, so a failed read consumes nothing */
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
    r.next = 0;
    r.acc = 0;
    r.nacc = 0;
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
    /* Probe mode only wants the reader's verdict, so it no longer keeps a
     * 512-byte scratch copy of counts it would discard (fix.md entries 8
     * and 23); the destination is written only when there is one. */
    if (!probe) memset(counts, 0, (size_t)max_nsym * sizeof(int16_t));

    unsigned charnum = 0;
    int previous0 = 0;
    for (;;) {
        if (previous0) {
            /* Zero-run: 2-bit flags; 3 means continue with +3. */
            for (;;) {
                uint32_t v = zgec_lsbr_read(&r, 2);
                if (!r.ok) return 0;
                if (v == 3u) {
                    /* Subtraction form: the add cannot wrap, so a long run of
                     * continuation flags stops at the cap instead of around
                     * it (fix.md entry 9). */
                    if (charnum > cap || cap - charnum < 3u) return 0;
                    charnum += 3u;
                } else {
                    charnum += v;   /* v <= 2 and charnum <= cap here */
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
                if ((size_t)(oend - out) < 2u) return 0;
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
                if ((size_t)(oend - out) < 2u) return 0;
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
            if ((size_t)(oend - out) < 2u) return 0;
            out[0] = (uint8_t)bitStream;
            out[1] = (uint8_t)(bitStream >> 8);
            out += 2;
            bitStream >>= 16;
            bitCount -= 16;
        }
    }

    if (remaining != 1) return 0;
    if (symbol > alphabetSize) return 0;

    /* Only the remaining-byte count is inspected: `oend - 2` points before
     * the object when cap < 2, and forming or comparing it is undefined
     * (fix.md entry 2). The three old checks reduce to this one, which is
     * what they already amounted to. */
    size_t tail = (size_t)(bitCount + 7) / 8u;
    if ((size_t)(oend - out) < tail) return 0;
    if (tail >= 1u) out[0] = (uint8_t)bitStream;
    if (tail >= 2u) out[1] = (uint8_t)(bitStream >> 8);
    out += tail;
    return (size_t)(out - ostart);
}

/* ---- FSE stream decode / encode (section 8.4) ---- */

/* Take _nb bits into _dst from the local reader (acc/nacc/ptr/left).
 * Behaviour is exactly zgec_br_read's: bytes are refilled lazily in the
 * same order, a read that cannot be satisfied sets br->overflow, and a
 * zero-width read yields 0. Holding the reader in locals instead of in
 * the zgec_br struct is what lets the compiler keep it in registers:
 * reached through the struct, every one of the two reads per symbol went
 * to memory and back. `bad` is the function's error exit.
 *
 * Every width passed in is at most 32: t->al and e->nb_bits are bounded by
 * ZGEC_MAX_AL when the decode table is built, and nbits[sym] is checked by
 * the caller before the take (fix.md entry 1). That bound is this macro's
 * contract -- a width above 32 would make (64u - _k) underflow and
 * `acc <<= _k` undefined. */
#define ZGEC_BRF_TAKE(_nb, _dst) do { \
        unsigned _k = (unsigned)(_nb); \
        if (_k != 0u) { \
            if (nacc < _k) { \
                if (left >= 8u && nacc <= 56u) { \
                    /* Fast refill: one 8-byte load supplies every byte the \
                     * byte loop below would add. The load is little-endian, \
                     * so ptr sits at bit 56 and lands on bit (56 - nacc), \
                     * exactly where the loop puts it. Bytes past the last one \
                     * counted are ORed in at the positions a later refill \
                     * writes them at, with the same values, so they are \
                     * harmless; ptr and left advance exactly as the loop does. \
                     * (`left >= 8` guarantees [ptr-7, ptr] is inside the \
                     * stream, and nb8 <= 8 <= left, so nb8 bytes are always \
                     * available.) */ \
                    unsigned _nb8 = (56u - nacc) / 8u + 1u; \
                    uint64_t _w = zgec_rd64(ptr - 7); \
                    acc |= _w >> nacc; \
                    nacc += 8u * _nb8; \
                    left -= _nb8; \
                    ptr -= _nb8; \
                    if (left == 0u) ptr++; \
                } else { \
                    while (nacc <= 56u && left > 0u) { \
                        acc |= (uint64_t)(ptr[0]) << (56u - nacc); \
                        nacc += 8u; \
                        left--; \
                        if (left > 0u) ptr--; \
                    } \
                } \
            } \
            if (nacc < _k) { br->overflow = 1; goto bad; } \
            (_dst) = (uint32_t)(acc >> (64u - _k)); \
            acc <<= _k; \
            nacc -= _k; \
        } else { \
            (_dst) = 0u; \
        } \
    } while (0)

zgec_err zgec_fse_decode(const zgec_fse_dec_table *t, zgec_br *br,
                          uint32_t *out, uint8_t *syms, size_t n,
                          const uint32_t *base, const uint8_t *nbits)
{
    uint64_t acc;
    unsigned nacc;
    const uint8_t *ptr;
    size_t left;
    unsigned state;
    unsigned S;
    size_t i;
    zgec_err err = ZGEC_ERR_BITSTREAM;

    if (!t || !br) return ZGEC_ERR_INVAL;
    /* Validate the table object before indexing it (fix.md entry 25): al
     * fixes S below, nsym bounds the symbols read out of it, and e is
     * dereferenced in the loop. */
    if (t->al < ZGEC_MIN_AL || t->al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    if (t->nsym <= 0 || t->nsym > 256) return ZGEC_ERR_FSE_COUNTS;
    if (t->e == NULL) return ZGEC_ERR_FSE_COUNTS;
    if (n == 0) {
        if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
        return ZGEC_OK;
    }
    S = (unsigned)1 << (unsigned)t->al;
    acc = br->acc;
    nacc = br->nacc;
    ptr = br->ptr;
    left = br->left;

    ZGEC_BRF_TAKE(t->al, state);
    if (state >= S) goto bad;

    i = 0;
    /* Fast loop: while at least 16 bytes remain and this is not the final
     * symbol. Every refill is unconditional (one 8-byte load) and every bit
     * take is branchless, so the only branches left are the validity checks,
     * which are never taken on a valid stream. Bit order and the consumed bit
     * count are identical to the generic loop below; the loop hands over with
     * the reader state fully up to date.
     *
     * Each symbol needs at most 11 + 32 = 43 bits. `nb` tops the accumulator
     * up to 63 - ((63 - nacc) mod 8) bits, i.e. into 56..63, so one refill
     * always covers a whole symbol. nacc is at most 59 on entry (the take
     * above is preceded by a refill that tops out at 64 bits) and never
     * exceeds 63 afterwards, so `w >> nacc` is never a shift by 64. The
     * `left > 16u` test keeps nb <= 7 well inside the stream, so the 8-byte
     * load at ptr-7 never reads below the stream start. */
    while (i + 1 < n && left > 16u) {
        const zgec_fse_dec_entry *e = &t->e[state];
        unsigned sym;
        unsigned xb = 0u;
        uint32_t extra;
        unsigned nbq;
        uint32_t bits;
        int32_t next;
        unsigned nb;
        uint64_t w;
        if (e->symbol < 0 || e->symbol >= t->nsym) { err = ZGEC_ERR_FSE_SYMBOL; goto bad; }
        sym = (unsigned)e->symbol;
        if (nbits != NULL) xb = (unsigned)nbits[sym];
        if (xb > 32u) { err = ZGEC_ERR_FSE_SYMBOL; goto bad; }
        /* refill to 56..63 bits */
        nb = (63u - nacc) >> 3;
        w = zgec_rd64(ptr - 7);
        acc |= w >> nacc;
        nacc += 8u * nb;
        ptr -= nb;
        left -= nb;
        if (syms) syms[i] = (uint8_t)sym;
        /* extra bits: (acc >> 1) >> (63 - xb) is acc >> (64 - xb) for xb >= 1
         * and 0 for xb == 0, without a branch */
        extra = (uint32_t)((acc >> 1) >> (63u - xb));
        acc <<= xb;
        nacc -= xb;
        if (out) out[i] = (base ? base[sym] : (uint32_t)sym) + extra;
        nbq = e->nb_bits;
        bits = (uint32_t)((acc >> 1) >> (63u - nbq));
        acc <<= nbq;
        nacc -= nbq;
        next = e->baseline + (int32_t)bits;
        if (next < 0 || next >= (int32_t)S) { err = ZGEC_ERR_BITSTREAM; goto bad; }
        state = (unsigned)next;
        i++;
    }

    for (; i < n; i++) {
        const zgec_fse_dec_entry *e = &t->e[state];
        unsigned sym = 0;
        unsigned extra_bits = 0;
        uint32_t extra = 0;
        if (e->symbol < 0 || e->symbol >= t->nsym) {
            err = ZGEC_ERR_FSE_SYMBOL;
            goto bad;
        }
        sym = (unsigned)e->symbol;
        /* Validate the width before writing anything for this symbol. The
         * width comes from the caller's table and ZGEC_BRF_TAKE shifts by
         * (64 - k), so a width above 32 would be an invalid shift; the value
         * is only 32 bits wide in any case. `sym` is already known to be
         * below t->nsym here (fix.md entry 1).
         *
         * This is a fault in the table, not a short read, so it reports
         * ZGEC_ERR_FSE_SYMBOL: returning ZGEC_ERR_BITSTREAM made a malformed
         * table indistinguishable from truncated input, and doing it after
         * syms[i] was stored left half a result behind (review finding 2).
         * On any error the caller must ignore out and syms. */
        if (nbits != NULL) {
            extra_bits = (unsigned)nbits[sym];
            if (extra_bits > 32u) {
                err = ZGEC_ERR_FSE_SYMBOL;
                goto bad;
            }
        }
        if (syms) syms[i] = (uint8_t)sym;
        if (extra_bits > 0u) ZGEC_BRF_TAKE(extra_bits, extra);
        if (out) {
            out[i] = (base ? base[sym] : (uint32_t)sym) + extra;
        }

        if (i + 1 < n) {
            uint32_t bits = 0;
            int32_t next;
            ZGEC_BRF_TAKE(e->nb_bits, bits);
            next = e->baseline + (int32_t)bits;
            if (next < 0 || next >= (int32_t)S) goto bad;
            state = (unsigned)next;
        }
    }
    br->acc = acc;
    br->nacc = nacc;
    br->ptr = ptr;
    br->left = left;
    if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
    return ZGEC_OK;

bad:
    br->acc = acc;
    br->nacc = nacc;
    br->ptr = ptr;
    br->left = left;
    return err;
}

/* One symbol's extra bits: `value - base[sym]`, with the width and the range
 * both validated. The bit writer keeps only the low n bits of what it is
 * handed and reports nothing, so an extra field that did not fit -- or a
 * value below its base, which wraps -- used to be written out as a different
 * value and still counted as success (fix.md entry 5). A NULL width table
 * means "no extra bits", which is the case the old code skipped entirely.
 *
 * Precondition: `values[]` must have been derived from the section 8.1 code
 * table (zgec_seq_code_of), so that each value lies inside its symbol's
 * [base[sym], base[sym] + 2^nbits[sym]) range. Every in-tree caller does this
 * -- src/seq.c builds syms[i] from values[i] with that mapping -- so the
 * ZGEC_ERR_FSE_SYMBOL returns below mean "caller bug or damaged table", not
 * "unsupported input" (review finding 7). */
static zgec_err zgec_fse_symbol_extra(unsigned sym, uint32_t value,
                                      const uint32_t *base,
                                      const uint8_t *nbits,
                                      uint32_t *extra_out, unsigned *nb_out)
{
    uint32_t lo;
    unsigned nb;

    if (nbits == NULL) {
        *extra_out = 0u;
        *nb_out = 0u;
        return ZGEC_OK;
    }
    nb = (unsigned)nbits[sym];
    if (nb > 32u) return ZGEC_ERR_FSE_SYMBOL;
    lo = base ? base[sym] : (uint32_t)sym;
    if (nb == 0u) {
        /* The symbol carries no extra bits, so the only value it can stand
         * for is its base: any other value would be dropped silently. */
        if (value != lo) return ZGEC_ERR_FSE_SYMBOL;
        *extra_out = 0u;
        *nb_out = 0u;
        return ZGEC_OK;
    }
    if (value < lo) return ZGEC_ERR_FSE_SYMBOL;
    {
        uint32_t extra = value - lo;
        if (nb < 32u && extra >= (1u << nb)) return ZGEC_ERR_FSE_SYMBOL;
        *extra_out = extra;
        *nb_out = nb;
        return ZGEC_OK;
    }
}

zgec_err zgec_fse_encode(const zgec_fse_enc_table *t, zgec_bw *bw,
                          const uint32_t *values, const uint8_t *syms, size_t n,
                          const uint32_t *base, const uint8_t *nbits)
{
    zgec_err err;
    uint8_t last_s;
    uint32_t state;

    if (!t || !bw) return ZGEC_ERR_INVAL;
    /* Validate the table object before indexing it (fix.md entry 25). */
    if (t->al < ZGEC_MIN_AL || t->al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    if (t->nsym <= 0 || t->nsym > 256) return ZGEC_ERR_FSE_COUNTS;
    if (t->e == NULL) return ZGEC_ERR_FSE_COUNTS;
    if (n == 0) return ZGEC_OK;
    if (!values || !syms) return ZGEC_ERR_INVAL;

    last_s = syms[n - 1];
    if ((int)last_s >= t->nsym) return ZGEC_ERR_FSE_SYMBOL;
    /* Rows are 2^al entries and every new_state is an occurrence index below
     * 2^al (see zgec_fse_build_enc, which now proves that while filling), so
     * `state` stays inside its row. */
    /* A symbol absent from the table's histogram owns an all-zero row, so
     * encoding it would write a transition nobody chose and still report
     * success -- the decoder would then read whichever symbol owns state 0.
     * zgec_fse_build_enc marks every cell of a symbol that does occur, so an
     * unmarked cell proves absence (review finding 1). */
    if ((t->e + ((size_t)last_s << t->al))[0].pad == 0)
        return ZGEC_ERR_FSE_SYMBOL;
    state = (t->e + ((size_t)last_s << t->al))[0].new_state;

    {
        uint32_t extra = 0u;    /* written through the pointer; the
                                   initialisers keep -Wmaybe-uninitialized
                                   quiet in a -Werror build */
        unsigned nb = 0u;
        err = zgec_fse_symbol_extra(last_s, values[n - 1], base, nbits,
                                    &extra, &nb);
        if (err != ZGEC_OK) return err;
        if (nb > 0u) zgec_bw_write(bw, extra, nb);
    }

    for (size_t i = n - 1; i > 0; i--) {
        uint8_t s = syms[i - 1];
        const zgec_fse_enc_entry *row;
        zgec_fse_enc_entry entry;
        uint32_t bits;
        uint32_t extra = 0u;
        unsigned nb = 0u;

        if ((int)s >= t->nsym) return ZGEC_ERR_FSE_SYMBOL;
        /* Row base first, so the index multiplication is not repeated
         * (fix.md entry 13). */
        row = t->e + ((size_t)s << t->al);
        if (row[0].pad == 0) return ZGEC_ERR_FSE_SYMBOL;   /* absent symbol */
        entry = row[(size_t)state];
        bits = (state >= (uint32_t)entry.baseline)
                    ? (uint32_t)(state - (uint32_t)entry.baseline)
                    : 0u;
        zgec_bw_write(bw, bits, (unsigned)entry.nb_bits);
        err = zgec_fse_symbol_extra(s, values[i - 1], base, nbits, &extra, &nb);
        if (err != ZGEC_OK) return err;
        if (nb > 0u) zgec_bw_write(bw, extra, nb);
        state = entry.new_state;
    }

    /* The writer records an overflow in its own struct instead of returning
     * it, so a stream that did not fit used to be reported as encoded
     * (fix.md entry 14). The flag is sticky, so one test after the loop
     * covers every write above, and one more covers the state field below. */
    if (bw->overflow) return ZGEC_ERR_BITSTREAM_OVERFLOW;
    zgec_bw_write(bw, state, (unsigned)t->al);
    if (bw->overflow) return ZGEC_ERR_BITSTREAM_OVERFLOW;
    return ZGEC_OK;
}
