#include "zgec_rans.h"

#include "zgec_lit.h"

#include <stdlib.h>
#include <string.h>

/* ---- decoder tables (Annex B.1) ---- */

zgec_err zgec_rans_build_dec(zgec_rans_dec_table *t, const int16_t *counts)
{
    if (t == NULL || counts == NULL) return ZGEC_ERR_INVAL;
    memset(t, 0, sizeof(*t));
    {
        uint32_t sum = 0;
        for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
            int c = (int)counts[s];
            uint32_t eff = (c < 0) ? 1u : (uint32_t)c;
            if (c < -1) return ZGEC_ERR_RANS_STATE;
            if (eff > (uint32_t)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
            t->f[s] = (uint16_t)eff;
            t->c[s] = (uint16_t)sum;
            sum += eff;
        }
        if (sum != (uint32_t)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
    }
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
        uint32_t c = (uint32_t)t->c[s];
        uint32_t f = (uint32_t)t->f[s];
        for (uint32_t slot = c; slot < c + f; slot++) {
            t->symbol_of_slot[slot] = (uint16_t)s;
        }
    }
    return ZGEC_OK;
}

/* ---- encoder tables (Annex B.2) ---- */

zgec_err zgec_rans_build_enc(zgec_rans_enc_table *t, const int16_t *counts)
{
    if (t == NULL || counts == NULL) return ZGEC_ERR_INVAL;
    memset(t, 0, sizeof(*t));
    {
        uint32_t sum = 0;
        for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
            int c = (int)counts[s];
            uint32_t eff = (c < 0) ? 1u : (uint32_t)c;
            if (c < -1) return ZGEC_ERR_RANS_STATE;
            if (eff > (uint32_t)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
            t->f[s] = (uint16_t)eff;
            t->c[s] = (uint16_t)sum;
            sum += eff;
            /* Precompute a biased reciprocal for the encode loop's
             * division by f[s]. floor(2^32 / f) + 1 is the bias that
             * makes the multiply-shift below off by at most one, which
             * the encode loop corrects exactly. f == 1 would need
             * 2^32 + 1, so it is stored as 0 and takes a branch there. */
            if (eff > 1u) {
                uint64_t recip = ((uint64_t)1 << 32) / (uint64_t)eff + 1u;
                t->f_recip[s] = (uint32_t)recip;
            } else {
                t->f_recip[s] = 0u;   /* eff == 0 unused; eff == 1 branched */
            }
        }
        if (sum != (uint32_t)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
    }
    return ZGEC_OK;
}

/* ---- 8-lane rANS decode (section 9.5) ---- */

zgec_err zgec_rans_decode(uint8_t *Z, size_t n_lit,
                           const uint8_t *stream, size_t stream_size,
                           const zgec_rans_dec_table *tables, int k,
                           int ctx_mode, const uint8_t *class_map,
                           const uint8_t *runstart)
{
    /* Section 9.5: n_lit == 0 is the empty stream (lit_size == 0).
     * No header is read; stream may be NULL. */
    if (tables == NULL) return ZGEC_ERR_INVAL;
    if (k != 1 && k != 2 && k != 4 && k != 8) return ZGEC_ERR_CTX_COUNT;
    if (n_lit == 0) {
        return ZGEC_OK;
    }
    if (Z == NULL) return ZGEC_ERR_INVAL;
    if (k > 1) {
        if (class_map == NULL || runstart == NULL) return ZGEC_ERR_INVAL;
        if (ctx_mode != ZGEC_CTX_NONE && ctx_mode != ZGEC_CTX_LSB6 &&
            ctx_mode != ZGEC_CTX_MSB6 && ctx_mode != ZGEC_CTX_TEXT &&
            ctx_mode != ZGEC_CTX_SIGNED)
            return ZGEC_ERR_CTX_MODE;
    }
    if (stream == NULL) return ZGEC_ERR_TRUNCATED;
    if (stream_size < 32) return ZGEC_ERR_TRUNCATED;

    size_t q = n_lit / 8;
    size_t r = n_lit % 8;
    size_t start[8];
    size_t len[8];
    for (unsigned lane = 0; lane < 8; lane++) {
        start[lane] = lane * q + (lane < r ? lane : r);
        len[lane] = q + (lane < r ? 1 : 0);
    }

    const uint8_t *cursor = stream + 32;
    uint32_t state[8];
    for (unsigned lane = 0; lane < 8; lane++) {
        state[lane] = zgec_rd32(stream + 4 * lane);
        if (len[lane] == 0) {
            /* Unused lane (0 < n_lit < 8): header state must be 2^16. */
            if (state[lane] != ZGEC_RANS_STATE_MIN) return ZGEC_ERR_RANS_STATE;
        } else {
            if (state[lane] < ZGEC_RANS_STATE_MIN) return ZGEC_ERR_RANS_STATE;
        }
    }

    for (size_t round = 0; round < q + (r > 0 ? 1 : 0); round++) {
        for (unsigned lane = 0; lane < 8; lane++) {
            if (round >= len[lane]) continue;
            size_t j = start[lane] + round;
            int table_idx = 0;
            if (k > 1) {
                if (j >= n_lit) return ZGEC_ERR_RANS_STATE;
                if (j == start[lane] || runstart[j]) {
                    table_idx = k;  /* run-start table */
                } else {
                    unsigned cls = zgec_classify(ctx_mode, Z[j - 1]);
                    if (cls >= 64u) return ZGEC_ERR_CLASS_MAP;
                    if ((int)class_map[cls] >= k) return ZGEC_ERR_CLASS_MAP;
                    table_idx = (int)class_map[cls];
                }
                if (table_idx < 0 || table_idx > k) return ZGEC_ERR_CLASS_MAP;
            }
            const zgec_rans_dec_table *tab = &tables[table_idx];
            unsigned slot = (unsigned)(state[lane] & (uint32_t)(ZGEC_RANS_M - 1));
            if (slot >= (unsigned)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
            uint16_t s = tab->symbol_of_slot[slot];
            if ((unsigned)s >= (unsigned)ZGEC_NSYM_LIT) return ZGEC_ERR_RANS_STATE;
            Z[j] = (uint8_t)s;
            if (tab->f[s] == 0) return ZGEC_ERR_RANS_STATE;
            if (slot < (unsigned)tab->c[s] ||
                slot >= (unsigned)tab->c[s] + (unsigned)tab->f[s])
                return ZGEC_ERR_RANS_STATE;
            uint64_t x = state[lane];
            x = (uint64_t)tab->f[s] * (x >> ZGEC_RANS_L) + (uint64_t)slot - (uint64_t)tab->c[s];
            state[lane] = (uint32_t)x;
            if (state[lane] < ZGEC_RANS_STATE_MIN) {
                if (cursor + 2 > stream + stream_size) return ZGEC_ERR_RANS_CURSOR;
                uint16_t w = zgec_rd16(cursor);
                cursor += 2;
                state[lane] = (state[lane] << 16) | w;
            }
        }
    }

    for (unsigned lane = 0; lane < 8; lane++) {
        if (state[lane] != ZGEC_RANS_STATE_MIN) return ZGEC_ERR_RANS_STATE;
    }
    if (cursor != stream + stream_size) return ZGEC_ERR_RANS_CURSOR;
    return ZGEC_OK;
}

/* ---- 8-lane rANS encode (section 9.5) ---- */

size_t zgec_rans_encode(const uint8_t *Z, size_t n_lit,
                        uint8_t *stream, size_t stream_cap,
                        const zgec_rans_enc_table *tables, int k,
                        int ctx_mode, const uint8_t *class_map,
                        const uint8_t *runstart)
{
    /* Section 9.5: n_lit == 0 emits the empty stream (0 bytes). */
    if (tables == NULL) return 0;
    if (k != 1 && k != 2 && k != 4 && k != 8) return 0;
    if (n_lit == 0) {
        return 0;
    }
    if (Z == NULL) return 0;
    if (k > 1 && (class_map == NULL || runstart == NULL)) return 0;
    if (stream == NULL) return 0;
    if (stream_cap < 32) return 0;
    if (n_lit > (SIZE_MAX - 64u) / sizeof(uint16_t)) return 0;

    size_t q = n_lit / 8;
    size_t r = n_lit % 8;
    size_t start[8];
    size_t len[8];
    for (unsigned lane = 0; lane < 8; lane++) {
        start[lane] = lane * q + (lane < r ? lane : r);
        len[lane] = q + (lane < r ? 1 : 0);
    }

    size_t words_cap = n_lit + 64;
    uint16_t *words = (uint16_t *)zgec_alloc(words_cap * sizeof(uint16_t), 64);
    if (!words) return 0;
    size_t nwords = 0;

    uint32_t state[8];
    for (unsigned lane = 0; lane < 8; lane++) state[lane] = ZGEC_RANS_STATE_MIN;

    size_t max_rounds = q + (r > 0 ? 1 : 0);
    for (size_t round_p1 = max_rounds; round_p1 > 0; round_p1--) {
        size_t round = round_p1 - 1;
        for (int lane = 7; lane >= 0; lane--) {
            if (round >= len[lane]) continue;
            size_t j = start[lane] + round;
            int table_idx = 0;
            if (k > 1) {
                if (j >= n_lit) { zgec_free(words); return 0; }
                if (j == start[lane] || runstart[j]) {
                    table_idx = k;
                } else {
                    unsigned cls = zgec_classify(ctx_mode, Z[j - 1]);
                    if (cls >= 64u) { zgec_free(words); return 0; }
                    if ((int)class_map[cls] >= k) { zgec_free(words); return 0; }
                    table_idx = (int)class_map[cls];
                }
                if (table_idx < 0 || table_idx > k) { zgec_free(words); return 0; }
            }
            const zgec_rans_enc_table *tab = &tables[table_idx];
            uint8_t s = Z[j];
            uint64_t f = tab->f[s];
            if (f == 0) { zgec_free(words); return 0; }
            uint64_t xmax = f << 21;
            uint32_t x = state[lane];
            if ((uint64_t)x >= xmax) {
                if (nwords >= words_cap) { zgec_free(words); return 0; }
                words[nwords++] = (uint16_t)(x & 0xFFFFu);
                x >>= 16;
            }
            uint32_t recip = tab->f_recip[s];
            /* Annex B.2: x = (x / f) << 11 | (x % f) + c.
             * A hardware divide per literal was the most expensive
             * instruction in this loop. After the renormalisation above,
             * x < f << 21 <= 2^32 and f <= ZGEC_RANS_M, so with
             * recip = floor(2^32 / f) + 1 the value floor(x * recip /
             * 2^32) is either floor(x/f) or floor(x/f) + 1; one
             * correction step makes quo and rems exact. f == 1 has no
             * 32-bit reciprocal and is handled directly. */
            uint32_t quo;
            int64_t rems;
            if (f == 1u) {
                quo = x;
                rems = 0;
            } else {
                uint64_t prod = (uint64_t)x * (uint64_t)recip;
                quo = (uint32_t)(prod >> 32);
                rems = (int64_t)x - (int64_t)quo * (int64_t)f;
                if (rems < 0) {
                    quo--;
                    rems += (int64_t)f;
                }
            }
            x = (uint32_t)(((uint64_t)quo << ZGEC_RANS_L) + (uint64_t)rems + (uint64_t)tab->c[s]);
            state[lane] = x;
        }
    }

    for (unsigned lane = 0; lane < 8; lane++) {
        zgec_wr32(stream + 4 * lane, state[lane]);
    }

    size_t needed = 32 + nwords * 2;
    if (needed > stream_cap) { zgec_free(words); return 0; }
    for (size_t i = 0; i < nwords; i++) {
        zgec_wr16(stream + 32 + 2 * i, words[nwords - 1 - i]);
    }
    zgec_free(words);
    return needed;
}

/* ---- histograms (section 9.3) ---- */

void zgec_rans_histograms(uint32_t *tables_counts, int n_tables,
                          const uint8_t *Z, size_t n_lit,
                          int ctx_mode, const uint8_t *class_map,
                          const uint8_t *runstart)
{
    if (tables_counts == NULL || n_tables <= 0) return;
    if (n_tables > 9) return;
    memset(tables_counts, 0, (size_t)n_tables * (size_t)ZGEC_NSYM_LIT * sizeof(uint32_t));
    if (n_lit == 0) return;
    if (Z == NULL) return;
    if (n_tables > 1 && (class_map == NULL || runstart == NULL)) return;

    size_t q = n_lit / 8;
    size_t r = n_lit % 8;
    size_t start[8];
    size_t len[8];
    for (unsigned lane = 0; lane < 8; lane++) {
        start[lane] = lane * q + (lane < r ? lane : r);
        len[lane] = q + (lane < r ? 1 : 0);
    }

    for (unsigned lane = 0; lane < 8; lane++) {
        for (size_t t = 0; t < len[lane]; t++) {
            size_t j = start[lane] + t;
            int table_idx = 0;
            if (n_tables > 1) {
                if (j >= n_lit) return;
                if (j == start[lane] || runstart[j]) {
                    table_idx = n_tables - 1;
                } else {
                    unsigned cls = zgec_classify(ctx_mode, Z[j - 1]);
                    if (cls >= 64u) return;
                    table_idx = (int)class_map[cls];
                }
                if (table_idx < 0 || table_idx >= n_tables) return;
            }
            tables_counts[(size_t)table_idx * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j]]++;
        }
    }
}

/* ---- normalise histogram to sum 2048 ---- */

zgec_err zgec_rans_normalise(int16_t *counts, const uint32_t *hist)
{
    if (counts == NULL || hist == NULL) return ZGEC_ERR_INVAL;
    uint64_t total = 0;
    int non_zero = 0;
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
        total += hist[s];
        if (hist[s] > 0) non_zero++;
    }
    if (total == 0) {
        for (int s = 0; s < ZGEC_NSYM_LIT; s++) counts[s] = -1;
        return ZGEC_OK;
    }
    if (total > (uint64_t)SIZE_MAX) return ZGEC_ERR_RANS_STATE;

    /* Scale to target 2048, giving each non-zero symbol at least 1. */
    uint64_t target = ZGEC_RANS_M;
    int min_per_symbol = 1;
    int64_t remaining = (int64_t)(target - (uint64_t)non_zero * (uint64_t)min_per_symbol);
    if (remaining < 0) remaining = 0;

    uint64_t scaled_sum = 0;
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
        if (hist[s] == 0) {
            counts[s] = 0;
        } else {
            uint64_t v = (uint64_t)hist[s] * (uint64_t)remaining;
            v /= (uint64_t)total;
            counts[s] = (int16_t)(min_per_symbol + (int)v);
            scaled_sum += (uint64_t)counts[s];
        }
    }

    /* Adjust to exactly target. */
    int64_t diff = (int64_t)(target - scaled_sum);
    if (diff != 0) {
        /* Find symbols with largest/smallest fractional parts.
         * fscaled[s] is hist[s] * target / total, which the comparison
         * below used to recompute with a 64-bit division on both sides of
         * every one of the ~32768 inner-loop iterations. It depends only
         * on the symbol, so it is computed once here; the comparison, the
         * swap rule and the resulting selection order are unchanged. */
        uint64_t fscaled[ZGEC_NSYM_LIT];
        int order[ZGEC_NSYM_LIT];
        for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
            order[s] = s;
            fscaled[s] = (total > 0) ? (uint64_t)hist[s] * target / total : 0;
        }
        /* Sort by hist[s] * target / total - counts[s] descending if diff > 0. */
        for (int i = 0; i < ZGEC_NSYM_LIT - 1; i++) {
            for (int j = i + 1; j < ZGEC_NSYM_LIT; j++) {
                uint64_t fi = fscaled[order[i]];
                uint64_t fj = fscaled[order[j]];
                int swap = 0;
                if (diff > 0 && fi > fj) swap = 1;
                else if (diff < 0 && fi < fj) swap = 1;
                else if (hist[order[i]] > hist[order[j]]) swap = 1;
                if (swap) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
            }
        }
        for (int i = 0; diff != 0 && i < ZGEC_NSYM_LIT; i++) {
            int s = order[i];
            if (diff > 0 && counts[s] > 0) { counts[s]++; diff--; }
            else if (diff < 0 && counts[s] > 1) { counts[s]--; diff++; }
        }
    }

    int final_sum = 0;
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) final_sum += counts[s] < 0 ? 1 : counts[s];
    if (final_sum != ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
    return ZGEC_OK;
}
