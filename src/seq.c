#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include "zgec_seq.h"
#include "zgec_internal.h"

#include <stddef.h>
#include <string.h>

/* ---- precomputed base and nbits tables (section 8.1) ---- */

const uint32_t zgec_seq_base[ZGEC_NSYM_SEQ] = {
    [0] = 0, [1] = 1, [2] = 2, [3] = 3, [4] = 4, [5] = 5, [6] = 6, [7] = 7,
    [8] = 8, [9] = 12, [10] = 16, [11] = 24, [12] = 32, [13] = 48, [14] = 64, [15] = 96,
    [16] = 128, [17] = 192, [18] = 256, [19] = 384, [20] = 512, [21] = 768, [22] = 1024, [23] = 1536,
    [24] = 2048, [25] = 3072, [26] = 4096, [27] = 6144, [28] = 8192, [29] = 12288, [30] = 16384, [31] = 24576,
    [32] = 32768, [33] = 49152, [34] = 65536, [35] = 98304, [36] = 131072, [37] = 196608, [38] = 262144, [39] = 393216,
    [40] = 524288, [41] = 786432, [42] = 1048576, [43] = 1572864, [44] = 2097152, [45] = 3145728, [46] = 4194304, [47] = 6291456,
    [48] = 8388608, [49] = 12582912, [50] = 16777216, [51] = 25165824, [52] = 33554432, [53] = 50331648, [54] = 67108864, [55] = 100663296,
    [56] = 134217728, [57] = 201326592, [58] = 268435456, [59] = 402653184, [60] = 536870912, [61] = 805306368, [62] = 1073741824, [63] = 1610612736,
    [64] = 2147483648u, [65] = 3221225472u
};

const uint8_t zgec_seq_nbits[ZGEC_NSYM_SEQ] = {
    [0] = 0, [1] = 0, [2] = 0, [3] = 0, [4] = 0, [5] = 0, [6] = 0, [7] = 0,
    [8] = 2, [9] = 2, [10] = 3, [11] = 3, [12] = 4, [13] = 4, [14] = 5, [15] = 5,
    [16] = 6, [17] = 6, [18] = 7, [19] = 7, [20] = 8, [21] = 8, [22] = 9, [23] = 9,
    [24] = 10, [25] = 10, [26] = 11, [27] = 11, [28] = 12, [29] = 12, [30] = 13, [31] = 13,
    [32] = 14, [33] = 14, [34] = 15, [35] = 15, [36] = 16, [37] = 16, [38] = 17, [39] = 17,
    [40] = 18, [41] = 18, [42] = 19, [43] = 19, [44] = 20, [45] = 20, [46] = 21, [47] = 21,
    [48] = 22, [49] = 22, [50] = 23, [51] = 23, [52] = 24, [53] = 24, [54] = 25, [55] = 25,
    [56] = 26, [57] = 26, [58] = 27, [59] = 27, [60] = 28, [61] = 28, [62] = 29, [63] = 29,
    [64] = 30, [65] = 30
};

/* ---- stream decode (tANS section 8.4, RLE section 8.5) ---- */

zgec_err zgec_seq_stream_decode(uint32_t *out, size_t n,
                                const zgec_fse_dec_table *t,
                                int rle_symbol,
                                zgec_br *br,
                                const uint32_t *base,
                                const uint8_t *nbits)
{
    if (!out || !br || !base || !nbits) return ZGEC_ERR_INVAL;
    if (n == 0) {
        if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
        return ZGEC_OK;
    }
    if (rle_symbol >= 0) {
        /* RLE mode (section 8.5): no state, only extra bits in order
           over the same backward bitstream; exact consumption. */
        if ((unsigned)rle_symbol >= (unsigned)ZGEC_NSYM_SEQ)
            return ZGEC_ERR_FSE_SYMBOL;
        unsigned nb = (unsigned)nbits[rle_symbol];
        uint32_t bval = base[rle_symbol];
        for (size_t i = 0; i < n; i++) {
            uint32_t extra = (nb > 0) ? zgec_br_read(br, nb) : 0u;
            if (br->overflow) return ZGEC_ERR_BITSTREAM;
            out[i] = bval + extra;
        }
        if (!zgec_br_done(br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
        return ZGEC_OK;
    }

    if (!t) return ZGEC_ERR_INVAL;
    return zgec_fse_decode(t, br, out, NULL, n, base, nbits);
}

/* ---- stream encode ---- */

/* Validate that `value` lies in the range the symbol encodes and yield its
 * extra bits (section 8.1): base(sym) is the smallest value the symbol
 * encodes and its range is 2^nbits values wide. A value outside that range
 * makes the unsigned subtraction wrap or overflow nbits, and zgec_bw_write
 * keeps only the low n bits, so the stream would decode to a different value
 * (entries 1 and 5). nbits is validated here because it is a caller-supplied
 * array. */
static int zgec_seq_get_extra(uint32_t value, uint8_t symbol,
                              const uint32_t *base, const uint8_t *nbits,
                              uint32_t *extra_out)
{
    unsigned nb = (unsigned)nbits[symbol];
    uint32_t b = base[symbol];
    uint32_t extra;

    if (nb > 32) return 0;
    if (value < b) return 0;
    extra = value - b;
    if (nb < 32 && extra >= (UINT32_C(1) << nb)) return 0;
    *extra_out = extra;
    return 1;
}

size_t zgec_seq_stream_encode(const uint32_t *values, size_t n,
                              const zgec_fse_enc_table *t,
                              int rle_symbol,
                              zgec_bw *bw,
                              const uint32_t *base,
                              const uint8_t *nbits)
{
    if (!bw || !base || !nbits) return 0;
    if (n == 0) {
        if (zgec_bw_finish(bw) == 0) return 0;
        return (size_t)(bw->ptr - bw->start);
    }
    if (rle_symbol >= 0) {
        if ((unsigned)rle_symbol >= (unsigned)ZGEC_NSYM_SEQ) return 0;
        if (!values) return 0;
        unsigned nb = (unsigned)nbits[rle_symbol];
        /* Validate every value against rle_symbol before the first write, so
           a rejection leaves the caller's bitstream untouched rather than
           holding the n - k values already written with bw->ptr advanced
           (finding 3). Callers normally guarantee membership through
           zgec_seq_rle_symbol(), but the encoder does not rely on that
           contract (entry 1). */
        {
            uint32_t probe;
            for (size_t i = 0; i < n; i++) {
                if (!zgec_seq_get_extra(values[i], (uint8_t)rle_symbol,
                                        base, nbits, &probe))
                    return 0;
            }
        }
        /* Backward bitstream: consumption order is the reverse of write
           order, so values are written last-to-first; the decoder then
           reads them out[0..n-1] in order (section 8.5). */
        for (size_t i = n; i > 0; i--) {
            uint32_t extra;
            /* The pre-pass above already guarantees every value belongs to
               rle_symbol -- including the nb == 0 case, where a value outside
               the symbol's single base value used to be dropped without a
               trace (entry 1). Kept as a guard against future edits. */
            if (!zgec_seq_get_extra(values[i - 1], (uint8_t)rle_symbol,
                                    base, nbits, &extra))
                return 0;
            if (nb > 0) zgec_bw_write(bw, extra, nb);
            /* zgec_bw_write latches overflow and stops storing, so this is
               an early exit, not a safety net (entry 11). */
            if (bw->overflow) return 0;
        }
        if (zgec_bw_finish(bw) == 0) return 0;
        return (size_t)(bw->ptr - bw->start);
    }

    if (!t || !values) return 0;
    uint8_t *syms = (uint8_t *)zgec_alloc(n, 1);
    if (!syms) return 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t nb_dummy;
        syms[i] = zgec_seq_code_of(values[i], &nb_dummy);
    }

    zgec_err err = zgec_fse_encode(t, bw, values, syms, n, base, nbits);
    zgec_free(syms);
    if (err != ZGEC_OK) return 0;
    /* Flush the accumulator and append the sentinel byte (section 8.4),
     * exactly as the RLE path above does; the reader needs it (V5). */
    if (zgec_bw_finish(bw) == 0) return 0;
    return (size_t)(bw->ptr - bw->start);
}

/* ---- conditioned stream encode (section 8.6) ----
 * Exact inverse of the decoder's zgec_seq_decode_cond three-table path:
 * the class (and therefore the table) of each symbol is fixed by the ML
 * sequence, independent of the FSE state, so the reverse pass mirrors
 * zgec_fse_encode with a per-symbol table. */
size_t zgec_seq_stream_encode_cond(const uint32_t *values, size_t n,
                                   const uint32_t *ml, int use_prev,
                                   const zgec_fse_enc_table *const enc[3],
                                   zgec_bw *bw,
                                   const uint32_t *base,
                                   const uint8_t *nbits)
{
    size_t i;
    int al;
    unsigned S;
    int nsym;
    uint8_t last_s;
    unsigned last_cls;
    zgec_fse_enc_entry first_entry;
    uint32_t state;

    if (!values || !ml || !enc || !bw || !base || !nbits) return 0;
    if (!enc[0] || !enc[1] || !enc[2]) return 0;
    if (n == 0) {
        if (zgec_bw_finish(bw) == 0) return 0;
        return (size_t)(bw->ptr - bw->start);
    }
    /* One table's state indexes the next, so a damaged table must be
       rejected before it can drive a shift or an index out of range
       (entries 2, 3 and 4). al is bounded exactly as every other table
       check in the tree bounds it, which also makes 1u << al defined. */
    if (!enc[0]->e || !enc[1]->e || !enc[2]->e) return 0;
    al = enc[0]->al;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return 0;
    if (enc[1]->al != al || enc[2]->al != al) return 0;
    if (enc[1]->nsym != enc[0]->nsym || enc[2]->nsym != enc[0]->nsym)
        return 0;
    nsym = enc[0]->nsym;
    if (nsym <= 0 || nsym > ZGEC_NSYM_SEQ) return 0;
    S = 1u << (unsigned)al;

    /* The symbols are materialised in an n-byte array, as the plain path
       already does above: the validator pass stores each code, and the
       write loop below reuses it instead of deriving it a second time.
       Validation still runs to completion before the first write, so a
       failed encode leaves the bitstream untouched. */
    uint8_t *syms = (uint8_t *)zgec_alloc(n, 1);
    if (!syms) return 0;
    for (i = 0; i < n; i++) {
        uint8_t nb;
        uint8_t code = zgec_seq_code_of(values[i], &nb);
        uint32_t extra;
        if ((int)code >= nsym) { zgec_free(syms); return 0; }
        /* Bound the value against its symbol's range here too, not only in
           the write loop below: the range test can reject for a custom
           base/nbits pair, and doing it here is what keeps the promise that
           a failed encode leaves the bitstream untouched (finding 3). The
           accepted set is identical to the loop's, so this rejects nothing
           the loop would have accepted. */
        if (!zgec_seq_get_extra(values[i], code, base, nbits, &extra)) {
            zgec_free(syms);
            return 0;
        }
        syms[i] = code;
    }

    last_s = syms[n - 1];
    last_cls = use_prev ? ((n >= 2) ? zgec_mlclass(ml[n - 2]) : 0u)
                        : zgec_mlclass(ml[n - 1]);
    /* zgec_mlclass() is total on 0..2, but the index must not depend on
       that staying true (entry 3). */
    if (last_cls >= 3u) { zgec_free(syms); return 0; }
    first_entry = enc[last_cls]->e[(size_t)last_s * (size_t)S];
    state = first_entry.new_state;
    if (state >= S) { zgec_free(syms); return 0; }
    if (nbits[last_s] > 0) {
        uint32_t extra;
        if (!zgec_seq_get_extra(values[n - 1], last_s, base, nbits, &extra)) {
            zgec_free(syms);
            return 0;
        }
        zgec_bw_write(bw, extra, (unsigned)nbits[last_s]);
        if (bw->overflow) { zgec_free(syms); return 0; }
    }

    for (i = n - 1; i > 0; i--) {
        uint8_t s;
        unsigned c;
        zgec_fse_enc_entry entry;
        uint32_t bits;
        s = syms[i - 1];
        c = use_prev ? ((i >= 2) ? zgec_mlclass(ml[i - 2]) : 0u)
                     : zgec_mlclass(ml[i - 1]);
        if (c >= 3u) { zgec_free(syms); return 0; }
        if ((int)s >= enc[c]->nsym) { zgec_free(syms); return 0; }
        entry = enc[c]->e[(size_t)s * (size_t)S + (size_t)state];
        bits = (state >= (uint32_t)entry.baseline)
                   ? (uint32_t)(state - (uint32_t)entry.baseline)
                   : 0u;
        /* The transition is representable only if it fits nb_bits. For a
           table built by zgec_fse_build_enc the occurrence whose range covers
           `state` guarantees bits < 2^nb_bits, but that invariant is
           constructed and never checked, and zgec_bw_write() masks its value
           to the low n bits rather than reporting the ones it lost -- so an
           inconsistent baseline/nb_bits pair would emit a stream that decodes
           to a different value and still return a byte count as on success
           (finding 1). nb_bits < 32 so the shift below is defined. */
        if ((unsigned)entry.nb_bits < 32u &&
            bits >= (UINT32_C(1) << (unsigned)entry.nb_bits)) {
            zgec_free(syms);
            return 0;
        }
        zgec_bw_write(bw, bits, (unsigned)entry.nb_bits);
        if (nbits[s] > 0) {
            uint32_t extra;
            if (!zgec_seq_get_extra(values[i - 1], s, base, nbits, &extra)) {
                zgec_free(syms);
                return 0;
            }
            zgec_bw_write(bw, extra, (unsigned)nbits[s]);
        }
        if (bw->overflow) { zgec_free(syms); return 0; }
        state = entry.new_state;
        if (state >= S) { zgec_free(syms); return 0; }
    }

    zgec_free(syms);
    zgec_bw_write(bw, state, (unsigned)al);
    if (bw->overflow) return 0;
    if (zgec_bw_finish(bw) == 0) return 0;
    return (size_t)(bw->ptr - bw->start);
}

int zgec_seq_rle_symbol(const uint32_t *values, size_t n)
{
    if (n == 0 || values == NULL) return -1;
    uint8_t dummy;
    uint8_t code0 = zgec_seq_code_of(values[0], &dummy);
    size_t i = 1;
#if defined(__AVX2__)
    __m256i v0 = _mm256_set1_epi32((int)values[0]);
    while (i + 8 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(values + i));
        unsigned mask = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(v, v0)));
        if (mask != 0xFFu) {
            for (size_t k = 0; k < 8; k++) {
                uint8_t c = zgec_seq_code_of(values[i + k], &dummy);
                if (c != code0) return -1;
            }
        }
        i += 8;
    }
#endif
    for (; i < n; i++) {
        uint8_t c = zgec_seq_code_of(values[i], &dummy);
        if (c != code0) return -1;
    }
    return (int)code0;
}

/* Normalise a histogram to Annex A counts summing to 2^al. Returns 0 when
 * no valid distribution exists. `al` is bounded first so that both the shift
 * and the int16_t counts are exact: ZGEC_MAX_AL is 11, so S <= 2048 and the
 * cast below cannot overflow (entry 7's al == 15 is unreachable). Every
 * observed symbol needs at least one state, so a histogram with more
 * observed symbols than states has no representation at all (entry 6); that
 * case used to produce effective counts exceeding S, which only
 * zgec_fse_build_dec() rejected later. Canonical shared implementation
 * (see zgec_internal.h); previously duplicated in encode.c. */
int zgec_normalize_counts(int16_t *counts, const uint32_t *hist,
                          int nsym, int al)
{
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return 0;
    int S = 1 << al;
    uint64_t total = 0;
    int n_non_zero = 0;
    for (int s = 0; s < nsym; s++) {
        total += hist[s];
        if (hist[s] > 0) n_non_zero++;
    }
    if (n_non_zero > S) return 0;
    if (total == 0 || n_non_zero == 0) {
        counts[0] = (int16_t)S;
        for (int s = 1; s < nsym; s++) counts[s] = 0;
        return 1;
    }

    int remaining = S;
    for (int s = 0; s < nsym; s++) {
        if (hist[s] == 0) {
            counts[s] = 0;
        } else {
            uint64_t scaled = ((uint64_t)hist[s] * (uint64_t)S) / total;
            if (scaled == 0) {
                counts[s] = -1;
                remaining -= 1;
            } else {
                counts[s] = (int16_t)scaled;
                remaining -= (int)scaled;
            }
        }
    }

    while (remaining != 0) {
        int best_s = -1;
        uint32_t max_h = 0;
        for (int s = 0; s < nsym; s++) {
            if (counts[s] > 1 && hist[s] > max_h) {
                max_h = hist[s];
                best_s = s;
            }
        }
        if (best_s < 0) {
            for (int s = 0; s < nsym; s++) {
                if (counts[s] > 0) { best_s = s; break; }
            }
        }
        if (best_s < 0) break;
        if (remaining > 0) {
            counts[best_s]++;
            remaining--;
        } else {
            if (counts[best_s] > 1) {
                counts[best_s]--;
                remaining++;
            } else {
                break;
            }
        }
    }
    /* The correction loop above can exit with `remaining != 0` (it has two
       breaks for the cases where no candidate can absorb the surplus), so the
       contract is enforced here by checking the sum rather than by asserting
       it. Effective counts are the normalised ones, with -1 meaning "one
       state" -- the same convention zgec_fse_build_dec() reads. */
    {
        int sum = 0;
        for (int s = 0; s < nsym; s++)
            sum += (counts[s] < 0) ? 1 : (int)counts[s];
        return sum == S;
    }
}

zgec_err zgec_seq_build_tables_counts(zgec_fse_dec_table **dec,
                                               zgec_fse_enc_table **enc,
                                               const uint32_t *hist, int al,
                                               int16_t *counts_out)
{
    if (!dec || !enc || !hist) return ZGEC_ERR_INVAL;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    int16_t counts[ZGEC_NSYM_SEQ];
    int16_t *counts_use = counts_out ? counts_out : counts;
    /* No valid distribution: either more observed symbols than the state
       space has states, or its normalised counts do not sum to 2^al (entry 6;
       the sum is verified in zgec_normalize_counts() itself).
       ZGEC_ERR_FSE_AL is the nearest code the enum offers -- the true fault is
       "accuracy log too small for this alphabet", which is not the same as a
       log outside [ZGEC_MIN_AL, ZGEC_MAX_AL] that the same code otherwise
       reports. A distinct member would have to be added to
       include/zgec_common.h (outside this slice). */
    if (!zgec_normalize_counts(counts_use, hist, ZGEC_NSYM_SEQ, al))
        return ZGEC_ERR_FSE_AL;

    zgec_err err = zgec_fse_build_dec(dec, counts_use, ZGEC_NSYM_SEQ, al);
    if (err != ZGEC_OK) return err;
    err = zgec_fse_build_enc(enc, *dec);
    if (err != ZGEC_OK) { zgec_fse_free_dec(*dec); *dec = NULL; return err; }
    return ZGEC_OK;
}

zgec_err zgec_seq_build_tables(zgec_fse_dec_table **dec,
                               zgec_fse_enc_table **enc,
                               const uint32_t *hist, int al)
{
    return zgec_seq_build_tables_counts(dec, enc, hist, al, NULL);
}
