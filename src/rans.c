#if defined(__AVX2__)
#include <immintrin.h>
#endif
#include "zgec_rans.h"

#include "zgec_lit.h"

#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__) || defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

/* Branchless renormalisation for the scalar decoder loops.
 *
 * The renorm decision (x < 2^16) is roughly a one-in-three coin flip per
 * literal, so a data-dependent branch here mispredicts far more often
 * than it costs.  The two-byte load is clamped to the last safe window
 * when fewer than two stream bytes remain; the value is then discarded
 * unless `need` is set, and a set `need` with fewer than two bytes left
 * is exactly the cursor error the caller reports (returns 0). */
static inline int zgec_rans_renorm(uint32_t *xp, const uint8_t **cursorp,
                                   size_t *remainingp,
                                   const uint8_t *safe_end2)
{
    uint32_t x = *xp;
    uint32_t need = (x < ZGEC_RANS_STATE_MIN) ? 1u : 0u;
    uint16_t w;
    if ((need & (uint32_t)(*remainingp < 2)) != 0u) return 0;
    w = zgec_rd16((*remainingp >= 2) ? *cursorp : safe_end2);
    {
        uint32_t xr = (x << 16) | (uint32_t)w;
        *xp = need ? xr : x;
    }
    *cursorp += (size_t)need * 2u;
    *remainingp -= (size_t)need * 2u;
    return 1;
}

/* Unguarded renormalisation for the fast rounds: the caller guarantees at
 * least two readable bytes at *cursorp, so the cursor bound test and the
 * clamped-load select of zgec_rans_renorm are dead and dropped. Same
 * arithmetic, same cursor/remaining updates; the load stays unconditional
 * (a data-dependent branch on `need` would mispredict about a third of the
 * time and costs far more than the always-taken load). */
static inline uint32_t zgec_rans_renorm_unguarded(uint32_t x,
                                                  const uint8_t **cursorp,
                                                  size_t *remainingp)
{
    uint32_t need = (x < ZGEC_RANS_STATE_MIN) ? 1u : 0u;
    uint32_t w = (uint32_t)zgec_rd16(*cursorp);
    *cursorp += (size_t)need * 2u;
    *remainingp -= (size_t)need * 2u;
    return need ? ((x << 16) | w) : x;
}

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
            /* Renormalise while x >= f << 21. For f == ZGEC_RANS_M the
             * true threshold is 2^32, which overflows a 32-bit xmax;
             * zgec_rans_encode_symbol handles that single-symbol case
             * before it ever reads xmax, so saturate here rather than
             * store 0. */
            t->xmax[s] = (eff >= (uint32_t)ZGEC_RANS_M)
                             ? 0xFFFFFFFFu
                             : (eff << 21u);
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
    const uint8_t *safe_end2 = stream + stream_size - 2u;
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

    /* Context of each byte, resolved to the table itself, once: the steady
     * loops used to pay two dependent loads per symbol (output byte ->
     * class index -> packed pointer). NULL marks a byte whose class maps
     * outside the table set; it is rejected when a literal follows it,
     * which is the behaviour of the reference form. k <= 8, so a real
     * class never maps to a NULL table. */
    const uint32_t *ctx_tab[256];
    if (k > 1) {
        for (unsigned b = 0; b < 256u; b++) {
            unsigned cls = zgec_classify(ctx_mode, (uint8_t)b);
            if (cls >= 64u || (int)class_map[cls] >= k) ctx_tab[b] = NULL;
            else ctx_tab[b] = tables[class_map[cls]].packed;
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
        /* k == 1 fast path: single table, no context/run-start logic.
         * The loop is scalar with branchless renormalisation: measured
         * against the old eight-lane AVX2 gather loop, this is ~40%
         * faster, because the gather plus the per-lane renorm branches
         * cost more than the branchless scalar lane update. */
        const uint32_t *packed0 = tables[0].packed;
        size_t round = 0;
        /* Unguarded rounds: while more than 15 bytes remain at the top of
         * a full round, every lane's branchless 2-byte renorm load is in
         * bounds (a round consumes at most 16), so the cursor bound test
         * and the clamped-load select of zgec_rans_renorm are dead here
         * and are dropped; the guarded loop below covers the tail. */
        for (; round < full && remaining > 15u; round++) {
            for (unsigned lane = 0; lane < 8; lane++) {
                size_t j = start[lane] + round;
                uint32_t x = state[lane];
                uint32_t e = packed0[x & (uint32_t)(ZGEC_RANS_M - 1)];
                uint32_t f = (e >> 8) & 0xFFFu;
                if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                Z[j] = (uint8_t)e;
                x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                x = zgec_rans_renorm_unguarded(x, &cursor, &remaining);
                state[lane] = x;
            }
            if (err != ZGEC_OK) break;
        }
        for (; round < max_rounds && err == ZGEC_OK; round++) {
            unsigned m = (round < full) ? 8u : rem;
            for (unsigned lane = 0; lane < m; lane++) {
                size_t j = start[lane] + round;
                uint32_t x = state[lane];
                uint32_t e = packed0[x & (uint32_t)(ZGEC_RANS_M - 1)];
                uint32_t f = (e >> 8) & 0xFFFu;
                if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                Z[j] = (uint8_t)e;
                x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                if (!zgec_rans_renorm(&x, &cursor, &remaining, safe_end2)) {
                    err = ZGEC_ERR_RANS_CURSOR;
                    break;
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
        const uint32_t *rst = tables[(unsigned)k].packed;
        unsigned m0 = (0 < full) ? 8u : rem;
        for (unsigned lane = 0; lane < m0 && err == ZGEC_OK; lane++) {
            size_t j = start[lane];
            uint32_t x = state[lane];
            uint32_t e = rst[x & (uint32_t)(ZGEC_RANS_M - 1)];
            uint32_t f = (e >> 8) & 0xFFFu;
            if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
            Z[j] = (uint8_t)e;
            x = f * (x >> ZGEC_RANS_L) + (e >> 20);
            if (!zgec_rans_renorm(&x, &cursor, &remaining, safe_end2)) {
                err = ZGEC_ERR_RANS_CURSOR;
                break;
            }
            state[lane] = x;
        }
        if (err == ZGEC_OK) {
            if (has_rs) {
                size_t round = 1;
                /* Unguarded rounds as for k == 1 above: the renorm guards
                 * are deferred to the steady-state loop below. */
                for (; round < full && remaining > 15u; round++) {
                    for (unsigned lane = 0; lane < 8; lane++) {
                        size_t j = start[lane] + round;
                        const uint32_t *tp;
                        uint32_t x;
                        uint32_t e;
                        uint32_t f;
                        if (zgec_rs_get(runstart, j)) {
                            tp = rst;
                        } else {
                            tp = ctx_tab[Z[j - 1]];
                            if (tp == NULL) { err = ZGEC_ERR_CLASS_MAP; break; }
                        }
                        x = state[lane];
                        e = tp[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        x = zgec_rans_renorm_unguarded(x, &cursor, &remaining);
                        state[lane] = x;
                    }
                    if (err != ZGEC_OK) break;
                }
                for (; round < max_rounds && err == ZGEC_OK; round++) {
                    unsigned m = (round < full) ? 8u : rem;
                    for (unsigned lane = 0; lane < m; lane++) {
                        size_t j = start[lane] + round;
                        const uint32_t *tp;
                        if (zgec_rs_get(runstart, j)) {
                            tp = rst;
                        } else {
                            tp = ctx_tab[Z[j - 1]];
                            if (tp == NULL) { err = ZGEC_ERR_CLASS_MAP; break; }
                        }
                        uint32_t x = state[lane];
                        uint32_t e = tp[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        uint32_t f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        if (!zgec_rans_renorm(&x, &cursor, &remaining, safe_end2)) {
                            err = ZGEC_ERR_RANS_CURSOR;
                            break;
                        }
                        state[lane] = x;
                    }
                }
            } else {
                size_t round = 1;
                for (; round < full && remaining > 15u; round++) {
                    for (unsigned lane = 0; lane < 8; lane++) {
                        size_t j = start[lane] + round;
                        const uint32_t *tp = ctx_tab[Z[j - 1]];
                        uint32_t x;
                        uint32_t e;
                        uint32_t f;
                        if (tp == NULL) { err = ZGEC_ERR_CLASS_MAP; break; }
                        x = state[lane];
                        e = tp[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        x = zgec_rans_renorm_unguarded(x, &cursor, &remaining);
                        state[lane] = x;
                    }
                    if (err != ZGEC_OK) break;
                }
                for (; round < max_rounds && err == ZGEC_OK; round++) {
                    unsigned m = (round < full) ? 8u : rem;
                    for (unsigned lane = 0; lane < m; lane++) {
                        size_t j = start[lane] + round;
                        const uint32_t *tp = ctx_tab[Z[j - 1]];
                        if (tp == NULL) { err = ZGEC_ERR_CLASS_MAP; break; }
                        uint32_t x = state[lane];
                        uint32_t e = tp[x & (uint32_t)(ZGEC_RANS_M - 1)];
                        uint32_t f = (e >> 8) & 0xFFFu;
                        if (f == 0u) { err = ZGEC_ERR_RANS_STATE; break; }
                        Z[j] = (uint8_t)e;
                        x = f * (x >> ZGEC_RANS_L) + (e >> 20);
                        if (!zgec_rans_renorm(&x, &cursor, &remaining, safe_end2)) {
                            err = ZGEC_ERR_RANS_CURSOR;
                            break;
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

/* Branchless per-symbol encode step.
 *
 * The renormalisation test (x >= f<<21) is a data-dependent coin flip that
 * mispredicts far too often to branch on: the decoder made the same change
 * long ago (see zgec_rans_renorm). Here the word store is unconditional and
 * the commit is a conditional add, and the reciprocal correction is a cmov.
 * This requires the caller to reserve one word of slack past the commit
 * point (the uncommitted store lands in that slack and is overwritten); the
 * caller sizes `words` with that slack, so no per-write bound test is needed
 * (a committed word can only appear once per symbol, and there are n_lit
 * symbols, so nwords <= n_lit <= words_cap). */
static inline int zgec_rans_encode_symbol(const zgec_rans_enc_table *tab, uint8_t s,
                                          uint32_t *state_ptr, uint16_t *words,
                                          size_t *nwords_ptr, size_t words_cap)
{
    (void)words_cap;
    uint32_t f = tab->f[s];
    if (f == 0u) return -1;
    /* A single-symbol table (f == ZGEC_RANS_M, c == 0, probability 1)
     * is an exact identity on the state: x/f<<L + x%f + c == x, and the
     * renormalisation threshold f<<21 is 2^32, which does not fit the
     * 32-bit xmax field (it wraps to 0 and would renormalise forever).
     * Handle it before the threshold test so it neither renormalises nor
     * depends on xmax. */
    if (f == (uint32_t)ZGEC_RANS_M) return 0;
    uint32_t x = *state_ptr;
    uint32_t need = (x >= tab->xmax[s]) ? 1u : 0u;
    size_t nw = *nwords_ptr;
    words[nw] = (uint16_t)x;
    *nwords_ptr = nw + (size_t)need;
    x = need ? (x >> 16) : x;
    uint32_t quo;
    uint32_t rems;
    if (f == 1u) {
        quo = x;
        rems = 0u;
    } else {
        uint64_t prod = (uint64_t)x * (uint64_t)tab->f_recip[s];
        quo = (uint32_t)(prod >> 32);
        rems = x - quo * f;
        uint32_t adj = (rems >= f) ? 1u : 0u;
        quo -= adj;
        rems += adj * f;
    }
    *state_ptr = (quo << ZGEC_RANS_L) + rems + (uint32_t)tab->c[s];
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
     * so one shift always restores the invariant. The branchless encoder
     * also performs one uncommitted store at the current position for every
     * symbol, so eight words of slack are reserved past the n_lit committed
     * words: the slack absorbs the store that a symbol with no
     * renormalisation does at index nwords, and is then overwritten. */
    if (n_lit > SIZE_MAX - 8u) return 0;
    size_t words_cap = n_lit + 8u;
    uint16_t stack_words[2048];
    uint16_t *words = NULL;
    int words_owned = 0;

    if (words_cap <= sizeof(stack_words) / sizeof(stack_words[0])) {
        words = stack_words;
    } else {
        words = (uint16_t *)zgec_alloc(words_cap * sizeof(uint16_t), 64);
        if (!words) return 0;
        words_owned = 1;
    }
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
                size_t j7 = start[7] + round;
                size_t j6 = start[6] + round;
                size_t j5 = start[5] + round;
                size_t j4 = start[4] + round;
                size_t j3 = start[3] + round;
                size_t j2 = start[2] + round;
                size_t j1 = start[1] + round;
                size_t j0 = start[0] + round;
                if (zgec_rans_encode_symbol(tab, Z[j7], &state[7], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j6], &state[6], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j5], &state[5], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j4], &state[4], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j3], &state[3], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j2], &state[2], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j1], &state[1], words, &nwords, words_cap) != 0 ||
                    zgec_rans_encode_symbol(tab, Z[j0], &state[0], words, &nwords, words_cap) != 0) {
                    if (words_owned) zgec_free(words);
                    return 0;
                }
            } else {
                for (int lane = 7; lane >= 0; lane--) {
                    size_t j;
                    if (round >= len[lane]) continue;
                    j = start[lane] + round;
                    uint8_t s = Z[j];
                    if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                        if (words_owned) zgec_free(words);
                        return 0;
                    }
                }
            }
        }
    } else {
        size_t full = len[7];
        for (size_t round_p1 = max_rounds; round_p1 > 0; round_p1--) {
            size_t round = round_p1 - 1;
            if (round < full) {
                for (int lane = 7; lane >= 0; lane--) {
                    size_t j = start[lane] + round;
                    int table_idx = (j == start[lane] || zgec_rs_get(runstart, j))
                                      ? k
                                      : (int)idx_lut[Z[j - 1]];
                    const zgec_rans_enc_table *tab = &tables[table_idx];
                    uint8_t s = Z[j];
                    if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                        if (words_owned) zgec_free(words);
                        return 0;
                    }
                }
            } else {
                for (int lane = 7; lane >= 0; lane--) {
                    if (round >= len[lane]) continue;
                    size_t j = start[lane] + round;
                    if (j >= n_lit) { if (words_owned) zgec_free(words); return 0; }
                    int table_idx = (j == start[lane] || zgec_rs_get(runstart, j))
                                      ? k
                                      : (int)idx_lut[Z[j - 1]];
                    if (table_idx < 0 || table_idx > k) { if (words_owned) zgec_free(words); return 0; }
                    const zgec_rans_enc_table *tab = &tables[table_idx];
                    uint8_t s = Z[j];
                    if (zgec_rans_encode_symbol(tab, s, &state[lane], words, &nwords, words_cap) != 0) {
                        if (words_owned) zgec_free(words);
                        return 0;
                    }
                }
            }
        }
    }

    /* Size the whole result -- header plus words -- before writing any of
     * it, so a capacity failure leaves the caller's buffer untouched. The
     * header used to be written first, which modified stream on failure. */
    if (nwords > (SIZE_MAX - 32u) / 2u) { if (words_owned) zgec_free(words); return 0; }
    size_t needed = 32 + nwords * 2;
    if (needed > stream_cap) { if (words_owned) zgec_free(words); return 0; }

    for (unsigned lane = 0; lane < 8; lane++) {
        zgec_wr32(stream + 4 * lane, state[lane]);
    }
    for (size_t i = 0; i < nwords; i++) {
        zgec_wr16(stream + 32 + 2 * i, words[nwords - 1 - i]);
    }
    if (words_owned) zgec_free(words);
    return needed;
}

/* ---- histograms (section 9.3) ---- */

/* Clear and fill n_tables literal histograms of 256 counts each from Z
 * (section 9.3). A single table counts all literals; with multiple tables,
 * lane starts and runstart bits select the last table, and other literals
 * use the preceding byte's context class mapped through class_map.
 * Multi-table input requires a valid ctx_mode, class_map and runstart. */
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
         * that looks like a finished model. */
        for (unsigned cls = 0; cls < 64u; cls++) {
            if ((int)class_map[cls] >= n_tables - 1) return;
        }
    }

    if (n_tables == 1) {
        /* Single table: multi-accumulator histogram unrolled 8-wide to break
         * store-forwarding dependencies on consecutive repeating bytes. */
        size_t i = 0;
        uint32_t c0[ZGEC_NSYM_LIT] = {0};
        uint32_t c1[ZGEC_NSYM_LIT] = {0};
        uint32_t c2[ZGEC_NSYM_LIT] = {0};
        uint32_t c3[ZGEC_NSYM_LIT] = {0};
        uint32_t c4[ZGEC_NSYM_LIT] = {0};
        uint32_t c5[ZGEC_NSYM_LIT] = {0};
        uint32_t c6[ZGEC_NSYM_LIT] = {0};
        uint32_t c7[ZGEC_NSYM_LIT] = {0};
        for (; i + 8 <= n_lit; i += 8) {
            c0[Z[i]]++;
            c1[Z[i + 1]]++;
            c2[Z[i + 2]]++;
            c3[Z[i + 3]]++;
            c4[Z[i + 4]]++;
            c5[Z[i + 5]]++;
            c6[Z[i + 6]]++;
            c7[Z[i + 7]]++;
        }
        for (; i < n_lit; i++) {
            c0[Z[i]]++;
        }
#if defined(__AVX2__)
        for (size_t s = 0; s < ZGEC_NSYM_LIT; s += 8) {
            __m256i v0 = _mm256_loadu_si256((const __m256i *)(const void *)(c0 + s));
            __m256i v1 = _mm256_loadu_si256((const __m256i *)(const void *)(c1 + s));
            __m256i v2 = _mm256_loadu_si256((const __m256i *)(const void *)(c2 + s));
            __m256i v3 = _mm256_loadu_si256((const __m256i *)(const void *)(c3 + s));
            __m256i v4 = _mm256_loadu_si256((const __m256i *)(const void *)(c4 + s));
            __m256i v5 = _mm256_loadu_si256((const __m256i *)(const void *)(c5 + s));
            __m256i v6 = _mm256_loadu_si256((const __m256i *)(const void *)(c6 + s));
            __m256i v7 = _mm256_loadu_si256((const __m256i *)(const void *)(c7 + s));
            __m256i sum01 = _mm256_add_epi32(v0, v1);
            __m256i sum23 = _mm256_add_epi32(v2, v3);
            __m256i sum45 = _mm256_add_epi32(v4, v5);
            __m256i sum67 = _mm256_add_epi32(v6, v7);
            __m256i sum = _mm256_add_epi32(_mm256_add_epi32(sum01, sum23), _mm256_add_epi32(sum45, sum67));
            _mm256_storeu_si256((__m256i *)(tables_counts + s), sum);
        }
#else
        for (size_t s = 0; s < ZGEC_NSYM_LIT; s++) {
            tables_counts[s] = c0[s] + c1[s] + c2[s] + c3[s] + c4[s] + c5[s] + c6[s] + c7[s];
        }
#endif
        return;
    }

    size_t start[8];
    size_t len[8];
    uint8_t idx_lut[256];
    zgec_lit_lane_geom(start, len, n_lit);
    zgec_rans_build_idx_lut(idx_lut, ctx_mode, class_map, n_tables - 1);

    int rs_empty = zgec_rs_is_empty(runstart, n_lit);

    if (rs_empty) {
        /* When runstart has no bits set, skip testing zgec_rs_get() per literal. */
        for (unsigned lane = 0; lane < 8; lane++) {
            size_t lane_len = len[lane];
            size_t lane_start = start[lane];
            if (lane_len > 0) {
                tables_counts[(size_t)(n_tables - 1) * (size_t)ZGEC_NSYM_LIT + (size_t)Z[lane_start]]++;
                size_t t = 1;
                for (; t + 4 <= lane_len; t += 4) {
                    size_t j = lane_start + t;
                    uint8_t pidx0 = idx_lut[Z[j - 1]];
                    if (pidx0 == 0xFFu) return;
                    tables_counts[(size_t)pidx0 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j]]++;

                    uint8_t pidx1 = idx_lut[Z[j]];
                    if (pidx1 == 0xFFu) return;
                    tables_counts[(size_t)pidx1 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 1]]++;

                    uint8_t pidx2 = idx_lut[Z[j + 1]];
                    if (pidx2 == 0xFFu) return;
                    tables_counts[(size_t)pidx2 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 2]]++;

                    uint8_t pidx3 = idx_lut[Z[j + 2]];
                    if (pidx3 == 0xFFu) return;
                    tables_counts[(size_t)pidx3 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 3]]++;
                }
                for (; t < lane_len; t++) {
                    size_t j = lane_start + t;
                    uint8_t pidx = idx_lut[Z[j - 1]];
                    if (pidx == 0xFFu) return;
                    tables_counts[(size_t)pidx * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j]]++;
                }
            }
        }
    } else {
        for (unsigned lane = 0; lane < 8; lane++) {
            size_t lane_len = len[lane];
            size_t lane_start = start[lane];
            if (lane_len > 0) {
                tables_counts[(size_t)(n_tables - 1) * (size_t)ZGEC_NSYM_LIT + (size_t)Z[lane_start]]++;
                size_t t = 1;
                for (; t + 4 <= lane_len; t += 4) {
                    size_t j = lane_start + t;
                    int table_idx0 = zgec_rs_get(runstart, j) ? (n_tables - 1) : (int)idx_lut[Z[j - 1]];
                    if (table_idx0 < 0 || table_idx0 >= n_tables) return;
                    tables_counts[(size_t)table_idx0 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j]]++;

                    int table_idx1 = zgec_rs_get(runstart, j + 1) ? (n_tables - 1) : (int)idx_lut[Z[j]];
                    if (table_idx1 < 0 || table_idx1 >= n_tables) return;
                    tables_counts[(size_t)table_idx1 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 1]]++;

                    int table_idx2 = zgec_rs_get(runstart, j + 2) ? (n_tables - 1) : (int)idx_lut[Z[j + 1]];
                    if (table_idx2 < 0 || table_idx2 >= n_tables) return;
                    tables_counts[(size_t)table_idx2 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 2]]++;

                    int table_idx3 = zgec_rs_get(runstart, j + 3) ? (n_tables - 1) : (int)idx_lut[Z[j + 2]];
                    if (table_idx3 < 0 || table_idx3 >= n_tables) return;
                    tables_counts[(size_t)table_idx3 * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j + 3]]++;
                }
                for (; t < lane_len; t++) {
                    size_t j = lane_start + t;
                    int table_idx;
                    if (zgec_rs_get(runstart, j)) {
                        table_idx = n_tables - 1;
                    } else {
                        uint8_t pidx = idx_lut[Z[j - 1]];
                        if (pidx == 0xFFu) return;
                        table_idx = (int)pidx;
                    }
                    tables_counts[(size_t)table_idx * (size_t)ZGEC_NSYM_LIT + (size_t)Z[j]]++;
                }
            }
        }
    }
}

/* ---- normalise histogram to sum 2048 ---- */

zgec_err zgec_rans_normalise(int16_t *counts, const uint32_t *hist)
{
    if (counts == NULL || hist == NULL) return ZGEC_ERR_INVAL;
    uint64_t total = 0;
    int non_zero = 0;
    int s_idx = 0;
#if defined(__AVX2__)
    __m256i vsum = _mm256_setzero_si256();
    __m256i vzero = _mm256_setzero_si256();
    __m256i vcount = _mm256_setzero_si256();
    for (; s_idx + 8 <= ZGEC_NSYM_LIT; s_idx += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(hist + s_idx));
        vsum = _mm256_add_epi32(vsum, v);
        __m256i vgt = _mm256_cmpgt_epi32(v, vzero);
        vcount = _mm256_sub_epi32(vcount, vgt); /* vgt is -1 for >0 */
    }
    __m128i vlow = _mm256_castsi256_si128(vsum);
    __m128i vhigh = _mm256_extracti128_si256(vsum, 1);
    vlow = _mm_add_epi32(vlow, vhigh);
    vlow = _mm_hadd_epi32(vlow, vlow);
    vlow = _mm_hadd_epi32(vlow, vlow);
    total = (uint64_t)(uint32_t)_mm_cvtsi128_si32(vlow);

    __m128i vclow = _mm256_castsi256_si128(vcount);
    __m128i vchigh = _mm256_extracti128_si256(vcount, 1);
    vclow = _mm_add_epi32(vclow, vchigh);
    vclow = _mm_hadd_epi32(vclow, vclow);
    vclow = _mm_hadd_epi32(vclow, vclow);
    non_zero = _mm_cvtsi128_si32(vclow);
#endif
    for (; s_idx < ZGEC_NSYM_LIT; s_idx++) {
        total += hist[s_idx];
        if (hist[s_idx] > 0) non_zero++;
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
