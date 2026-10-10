#include "zgec_rans.h"

#include "zgec_lit.h"

#include <stdlib.h>
#include <string.h>

/* ---- context mode validation (Annex C) ----
 *
 * Section 9.3 lets a stream name one of five classification functions.
 * The decoder checks that before it classifies anything; the encoder paths
 * must apply the same test, because zgec_classify() answers 0 (ZGEC_CTX_NONE)
 * for a mode it does not know, so an unvalidated mode would silently build
 * and emit a model that does not match the mode named in the header. */
static int zgec_rans_ctx_mode_valid(int ctx_mode)
{
    return ctx_mode == ZGEC_CTX_NONE ||
           ctx_mode == ZGEC_CTX_LSB6 ||
           ctx_mode == ZGEC_CTX_MSB6 ||
           ctx_mode == ZGEC_CTX_TEXT ||
           ctx_mode == ZGEC_CTX_SIGNED;
}

/* Byte -> context-table index for one (mode, map, k), resolved once.
 * 0xFF marks a byte whose class maps outside the table set; callers
 * reject it when a literal follows it. k <= 8, so a real index never
 * collides with the sentinel. Same table the decoder builds inline. */
static void zgec_rans_build_idx_lut(uint8_t lut[256], int ctx_mode,
                                    const uint8_t *class_map, int k)
{
    unsigned b;
    for (b = 0; b < 256u; b++) {
        unsigned cls = zgec_classify(ctx_mode, (uint8_t)b);
        if (cls >= 64u || (int)class_map[cls] >= k) lut[b] = 0xFFu;
        else lut[b] = class_map[cls];
    }
}

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
    /* Pack once per build (Annex B.1 decode slots, one u32 per slot):
     *   bits 0..7   symbol
     *   bits 8..19  frequency (0 marks a slot that is not a valid symbol
     *               slot; it is rejected when first used, as before)
     *   bits 20..31 bias = slot - c[symbol]
     * The same validation the per-decode pack ran is done here, once.
     * Frequency is at most ZGEC_RANS_M (2048, 12 bits) and the bias is
     * below it, so both fields fit the widths above. */
    for (unsigned slot = 0; slot < (unsigned)ZGEC_RANS_M; slot++) {
        unsigned s = t->symbol_of_slot[slot];
        uint32_t f = (s < (unsigned)ZGEC_NSYM_LIT) ? t->f[s] : 0u;
        uint32_t c = (s < (unsigned)ZGEC_NSYM_LIT) ? t->c[s] : 0u;
        if (s >= (unsigned)ZGEC_NSYM_LIT || f == 0u || f > (uint32_t)ZGEC_RANS_M ||
            slot < c || slot >= c + f) {
            t->packed[slot] = 0u;   /* invalid: decodes to an error */
        } else {
            t->packed[slot] = (uint32_t)s | (f << 8) | ((uint32_t)(slot - c) << 20);
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
        if (!zgec_rans_ctx_mode_valid(ctx_mode)) return ZGEC_ERR_CTX_MODE;
    }
    if (stream == NULL) return ZGEC_ERR_TRUNCATED;
    if (stream_size < 32) return ZGEC_ERR_TRUNCATED;

    size_t start[8];
    size_t len[8];
    zgec_lit_lane_geom(start, len, n_lit);

    /* remaining is kept in step with cursor, so every bounds test is a
     * subtraction against a length rather than a comparison of a pointer
     * sum, which must not be formed past the end of the object. */
    const uint8_t *cursor = stream + 32;
    size_t remaining = stream_size - 32;
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

    /* Packed decode slots live in tables[t].packed (built once by
     * zgec_rans_build_dec): one u32 per slot (symbol | f<<8 | bias<<20,
     * f == 0 marks an invalid slot, rejected lazily below). REPEAT/cloned
     * segments memcpy the whole table, so the pack is reused, not rebuilt. */

    /* Context of each byte, resolved once. 0xFF marks a byte whose class
     * maps outside the table set; it is rejected when a literal follows it,
     * which is the behaviour of the reference form. k <= 8, so a real class
     * never maps to 0xFF. */
    uint8_t idx_lut[256];
    if (k > 1) {
        for (unsigned b = 0; b < 256u; b++) {
            unsigned cls = zgec_classify(ctx_mode, (uint8_t)b);
            if (cls >= 64u || (int)class_map[cls] >= k) idx_lut[b] = 0xFFu;
            else idx_lut[b] = class_map[cls];
        }
    }

    /* Lanes 0..r-1 hold q+1 symbols and the rest hold q (section 9.2, and
     * zgec_lit_lane_geom: len[lane] = q + (lane < r)), so every round has a
     * prefix of active lanes: full rounds run all eight, the tail round runs
     * the first `r`. Lane order within a round is the decode order the
     * renormalisation cursor depends on. */
    size_t full = len[7];
    size_t max_rounds = len[0];
    unsigned rem = 0u;
    for (unsigned lane = 0; lane < 8; lane++) if (len[lane] > full) rem++;
    zgec_err err = ZGEC_OK;
    if (k == 1) {
        /* k == 1 fast path: single table, no context/run-start logic. */
        for (size_t round = 0; round < max_rounds && err == ZGEC_OK; round++) {
            unsigned m = (round < full) ? 8u : rem;
            for (unsigned lane = 0; lane < m; lane++) {
                size_t j = start[lane] + round;
                uint32_t x = state[lane];
                uint32_t e = tables[0].packed[x & (uint32_t)(ZGEC_RANS_M - 1)];
                uint32_t f = (e >> 8) & 0xFFFu;
                if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                Z[j] = (uint8_t)e;
                x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                if (x < ZGEC_RANS_STATE_MIN) {
                    if (remaining < 2) { err = ZGEC_ERR_RANS_CURSOR; break; }
                    x = (x << 16) | zgec_rd16(cursor);
                    cursor += 2;
                    remaining -= 2;
                }
                state[lane] = x;
            }
        }
    } else {
        /* k > 1: lane-first symbols (round 0) always use the run-start
         * table, so they are peeled out and the steady-state loop only
         * tests runstart[j]. The runstart/no-runstart variants are
         * selected once per stream by a single bitmap scan. */
        int has_rs = 0;
        for (size_t i = 0; i < n_lit; i++) {
            if (zgec_rs_get(runstart, i)) { has_rs = 1; break; }
        }
        unsigned m0 = (0 < full) ? 8u : rem;
        for (unsigned lane = 0; lane < m0 && err == ZGEC_OK; lane++) {
            size_t j = start[lane];
            uint32_t x = state[lane];
            uint32_t e = tables[(unsigned)k].packed[x & (uint32_t)(ZGEC_RANS_M - 1)];
            uint32_t f = (e >> 8) & 0xFFFu;
            if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
            Z[j] = (uint8_t)e;
            x = f * (x >> ZGEC_RANS_L) + (e >> 20);
            if (x < ZGEC_RANS_STATE_MIN) {
                if (remaining < 2) { err = ZGEC_ERR_RANS_CURSOR; break; }
                x = (x << 16) | zgec_rd16(cursor);
                cursor += 2;
                remaining -= 2;
            }
            state[lane] = x;
        }
        if (err == ZGEC_OK) {
            if (has_rs) {
                for (size_t round = 1; round < max_rounds && err == ZGEC_OK; round++) {
                    unsigned m = (round < full) ? 8u : rem;
                    for (unsigned lane = 0; lane < m; lane++) {
                        size_t j = start[lane] + round;
                        unsigned ti;
                        if (zgec_rs_get(runstart, j)) {
                            ti = (unsigned)k;
                        } else {
                            uint8_t pidx = idx_lut[Z[j - 1]];
                            if (pidx == 0xFFu) { err = ZGEC_ERR_CLASS_MAP; break; }
                            ti = pidx;
                        }
                        uint32_t x = state[lane];
                        uint32_t e = tables[ti].packed[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        uint32_t f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        if (x < ZGEC_RANS_STATE_MIN) {
                            if (remaining < 2) { err = ZGEC_ERR_RANS_CURSOR; break; }
                            x = (x << 16) | zgec_rd16(cursor);
                            cursor += 2;
                            remaining -= 2;
                        }
                        state[lane] = x;
                    }
                }
            } else {
                for (size_t round = 1; round < max_rounds && err == ZGEC_OK; round++) {
                    unsigned m = (round < full) ? 8u : rem;
                    for (unsigned lane = 0; lane < m; lane++) {
                        size_t j = start[lane] + round;
                        uint8_t pidx = idx_lut[Z[j - 1]];
                        if (pidx == 0xFFu) { err = ZGEC_ERR_CLASS_MAP; break; }
                        unsigned ti = pidx;
                        uint32_t x = state[lane];
                        uint32_t e = tables[ti].packed[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        uint32_t f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        if (x < ZGEC_RANS_STATE_MIN) {
                            if (remaining < 2) { err = ZGEC_ERR_RANS_CURSOR; break; }
                            x = (x << 16) | zgec_rd16(cursor);
                            cursor += 2;
                            remaining -= 2;
                        }
                        state[lane] = x;
                    }
                }
            }
        }
    }

    if (err == ZGEC_OK) {
        for (unsigned lane = 0; lane < 8; lane++) {
            if (state[lane] != ZGEC_RANS_STATE_MIN) { err = ZGEC_ERR_RANS_STATE; break; }
        }
    }
    if (err == ZGEC_OK && remaining != 0) err = ZGEC_ERR_RANS_CURSOR;
    return err;
}

static inline int zgec_rans_encode_symbol(const zgec_rans_enc_table *tab, uint8_t s,
                                          uint32_t *state_ptr, uint16_t *words,
                                          size_t *nwords_ptr, size_t words_cap)
{
    uint64_t f = tab->f[s];
    if (f == 0) return -1;
    uint64_t xmax = f << 21;
    uint32_t x = *state_ptr;
    if ((uint64_t)x >= xmax) {
        if (*nwords_ptr >= words_cap) return -1;
        words[(*nwords_ptr)++] = (uint16_t)(x & 0xFFFFu);
        x >>= 16;
    }
    uint32_t recip = tab->f_recip[s];
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
    *state_ptr = (uint32_t)(((uint64_t)quo << ZGEC_RANS_L) + (uint64_t)rems + (uint64_t)tab->c[s]);
    return 0;
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
    if (k > 1 && !zgec_rans_ctx_mode_valid(ctx_mode)) return 0;
    if (stream == NULL) return 0;
    if (stream_cap < 32) return 0;
    /* words_cap is n_lit below, so n_lit * sizeof(uint16_t) is the size
     * that must not overflow. */
    if (n_lit > SIZE_MAX / sizeof(uint16_t)) return 0;

    size_t start[8];
    size_t len[8];
    zgec_lit_lane_geom(start, len, n_lit);

    /* At most one renormalisation word is emitted per symbol: after the
     * single shift x < 2^16, and 2^16 <= f << 21 = xmax for every f >= 1,
     * so one shift always restores the invariant. n_lit words are
     * therefore enough; the bound test in the loop still refuses to write
     * past the end if that reasoning is ever broken. */
    size_t words_cap = n_lit;
    uint16_t *words = (uint16_t *)zgec_alloc(words_cap * sizeof(uint16_t), 64);
    if (!words) return 0;
    size_t nwords = 0;

    uint32_t state[8];
    for (unsigned lane = 0; lane < 8; lane++) state[lane] = ZGEC_RANS_STATE_MIN;

    size_t max_rounds = 0;
    uint8_t idx_lut[256];
    for (unsigned lane = 0; lane < 8; lane++)
        if (len[lane] > max_rounds) max_rounds = len[lane];
    if (k > 1) zgec_rans_build_idx_lut(idx_lut, ctx_mode, class_map, k);
    if (k == 1) {
        const zgec_rans_enc_table *tab = &tables[0];
        size_t full = len[7];
        for (size_t round_p1 = max_rounds; round_p1 > 0; round_p1--) {
            size_t round = round_p1 - 1;
            if (round < full) {
                for (int lane = 7; lane >= 0; lane--) {
                    size_t j = start[lane] + round;
                    uint8_t s = Z[j];
                    if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                        zgec_free(words);
                        return 0;
                    }
                }
            } else {
                for (int lane = 7; lane >= 0; lane--) {
                    size_t j;
                    if (round >= len[lane]) continue;
                    j = start[lane] + round;
                    uint8_t s = Z[j];
                    if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                        zgec_free(words);
                        return 0;
                    }
                }
            }
        }
    } else {
        for (size_t round_p1 = max_rounds; round_p1 > 0; round_p1--) {
            size_t round = round_p1 - 1;
            for (int lane = 7; lane >= 0; lane--) {
                int table_idx = 0;
                size_t j;
                if (round >= len[lane]) continue;
                j = start[lane] + round;
                if (j >= n_lit) { zgec_free(words); return 0; }
                if (j == start[lane] || zgec_rs_get(runstart, j)) {
                    table_idx = k;
                } else {
                    uint8_t pidx = idx_lut[Z[j - 1]];
                    if (pidx == 0xFFu) { zgec_free(words); return 0; }
                    table_idx = (int)pidx;
                }
                if (table_idx < 0 || table_idx > k) { zgec_free(words); return 0; }
                const zgec_rans_enc_table *tab = &tables[table_idx];
                uint8_t s = Z[j];
                if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                    zgec_free(words);
                    return 0;
                }
            }
        }
    }

    /* Size the whole result -- header plus words -- before writing any of
     * it, so a capacity failure leaves the caller's buffer untouched. The
     * header used to be written first, which modified stream on failure. */
    if (nwords > (SIZE_MAX - 32u) / 2u) { zgec_free(words); return 0; }
    size_t needed = 32 + nwords * 2;
    if (needed > stream_cap) { zgec_free(words); return 0; }

    for (unsigned lane = 0; lane < 8; lane++) {
        zgec_wr32(stream + 4 * lane, state[lane]);
    }
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
    if (n_tables > 1 && !zgec_rans_ctx_mode_valid(ctx_mode)) return;
    if (n_tables > 1) {
        /* Prevalidate the whole map before a single count is written, so
         * an invalid map cannot leave a partly populated histogram set
         * that looks like a finished model. The loop below keeps its own
         * checks as a second line of defence, though this pass does not
         * make all of them unreachable: `cls` there comes from
         * zgec_classify(), not from class_map, so its `cls >= 64u` exit
         * still guards whatever classify() returns. */
        for (unsigned cls = 0; cls < 64u; cls++) {
            /* The bound is k, not n_tables, because index k is reserved for
             * the run-start table. include/zgec_rans.h:95 fixes the
             * relationship -- "n_tables is 1 (k == 1) or k + 1 (k > 1, last
             * table is run-start)" -- and src/encode.c:1496 is the only
             * assignment, `n_tables = (k <= 1) ? 1 : (k + 1)`, so with
             * n_tables > 1 established just above, k == n_tables - 1 here.
             * Both consumers of a class map refuse class_map[cls] >= k --
             * zgec_rans_decode() and zgec_rans_encode(), both above -- so
             * an entry of exactly k has to be refused here too: it would
             * otherwise be counted into the run-start table and only
             * rejected later by zgec_rans_encode(), leaving exactly the
             * half-built histogram set this guard exists to prevent. */
            if ((int)class_map[cls] >= n_tables - 1) return;
        }
    }

    if (n_tables == 1) {
        /* Single table: a plain linear histogram, four counts per step.
         * The lane geometry below only exists to interleave the table
         * streams; with one table it is pure overhead. Counts match the
         * lane loop byte for byte. */
        size_t i = 0;
        for (; i + 4 <= n_lit; i += 4) {
            tables_counts[Z[i]]++;
            tables_counts[Z[i + 1]]++;
            tables_counts[Z[i + 2]]++;
            tables_counts[Z[i + 3]]++;
        }
        for (; i < n_lit; i++) {
            tables_counts[Z[i]]++;
        }
        return;
    }

    size_t start[8];
    size_t len[8];
    uint8_t idx_lut[256];
    zgec_lit_lane_geom(start, len, n_lit);
    if (n_tables > 1)
        zgec_rans_build_idx_lut(idx_lut, ctx_mode, class_map, n_tables - 1);

    for (unsigned lane = 0; lane < 8; lane++) {
        for (size_t t = 0; t < len[lane]; t++) {
            size_t j = start[lane] + t;
            int table_idx = 0;
            if (n_tables > 1) {
                if (j >= n_lit) return;
                if (j == start[lane] || zgec_rs_get(runstart, j)) {
                    table_idx = n_tables - 1;
                } else {
                    uint8_t pidx = idx_lut[Z[j - 1]];
                    if (pidx == 0xFFu) return;
                    table_idx = (int)pidx;
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
        /* No information at all: spread M uniformly over the alphabet.
         * Equal frequencies are the maximum-entropy table, and it is the
         * only choice here that satisfies the builders' sum invariant.
         * The previous branch wrote -1 for every symbol; both builders
         * read -1 as effective frequency 1, so the table summed to
         * ZGEC_NSYM_LIT (256) instead of ZGEC_RANS_M (2048) and was
         * rejected with ZGEC_ERR_RANS_STATE. */
        uint32_t base = (uint32_t)ZGEC_RANS_M / (uint32_t)ZGEC_NSYM_LIT;
        uint32_t extra = (uint32_t)ZGEC_RANS_M % (uint32_t)ZGEC_NSYM_LIT;
        for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
            uint32_t add = ((uint32_t)s < extra) ? 1u : 0u;
            counts[s] = (int16_t)(base + add);
        }
        return ZGEC_OK;
    }
    if (total > (uint64_t)SIZE_MAX) return ZGEC_ERR_RANS_STATE;

    /* Scale to target 2048, giving each non-zero symbol at least 1. */
    uint64_t target = ZGEC_RANS_M;
    int min_per_symbol = 1;
    /* Every present symbol gets one unit, so the alphabet has to fit inside
     * the target. Enforced rather than clamped: with non_zero <= target the
     * per-symbol floors below can only undershoot, which is exactly what the
     * largest-remainder pass relies on -- it only hands spare units out and
     * never takes any back. Clamping to 0 instead would let remaining go
     * negative, make the floors overshoot, and leave a diff the pass cannot
     * correct. */
    if (non_zero > (int)ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
    int64_t remaining = (int64_t)(target - (uint64_t)non_zero * (uint64_t)min_per_symbol);

    uint64_t scaled_sum = 0;
    uint64_t remainder[ZGEC_NSYM_LIT];
    int order[ZGEC_NSYM_LIT];
    int n_order = 0;
    /* Both arrays are initialised over the whole alphabet first, so every
     * index the ordering step below reads is a defined value. */
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
        order[s] = s;
        remainder[s] = 0;
    }
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) {
        if (hist[s] == 0) {
            counts[s] = 0;
            continue;
        }
        /* One unit each, then the surviving mass in proportion. The
         * fractional part comes from the same numerator the floor used,
         * so it is a real remainder and not a second, differently scaled
         * quantity. */
        {
            uint64_t product = (uint64_t)hist[s] * (uint64_t)remaining;
            counts[s] = (int16_t)(min_per_symbol +
                                  (int)(product / (uint64_t)total));
            remainder[s] = product % (uint64_t)total;
        }
        scaled_sum += (uint64_t)counts[s];
        order[n_order] = s;
        n_order++;
    }

    /* The floors can only undershoot: the discarded remainders sum to less
     * than one per symbol, so 0 <= diff < n_order. Largest-remainder gives
     * each spare unit to the symbol whose fractional part is largest. The
     * previous code compared floor(hist[s] * M / total) and swapped
     * ascending, which handed the spare units to the *least* frequent
     * symbols instead. */
    int64_t diff = (int64_t)(target - scaled_sum);
    if (diff > 0) {
        for (int i = 1; i < n_order; i++) {
            int key = order[i];
            int j = i - 1;
            while (j >= 0 && remainder[order[j]] < remainder[key]) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = key;
        }
        for (int i = 0; diff > 0 && i < n_order; i++) {
            counts[order[i]]++;
            diff--;
        }
    }

    int final_sum = 0;
    for (int s = 0; s < ZGEC_NSYM_LIT; s++) final_sum += counts[s] < 0 ? 1 : counts[s];
    if (final_sum != ZGEC_RANS_M) return ZGEC_ERR_RANS_STATE;
    return ZGEC_OK;
}
