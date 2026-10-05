#include "zgec_parse.h"
#include "zgec_seq.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- helpers ---- */

static double zgec_bits(double total, double count)
{
    if (count <= 0 || total <= 0) return 0.0;
    return -count * (log(count / total) / log(2.0));
}

void zgec_parse_free(zgec_parse *p)
{
    if (!p) return;
    zgec_free(p->seq);
    zgec_free(p->lit);
    zgec_free(p);
}

double zgec_seq_cost_bits(const uint32_t *hist)
{
    double total = 0.0;
    for (int s = 0; s < ZGEC_NSYM_SEQ; s++) total += (double)hist[s];
    if (total == 0) return 0.0;
    double bits = 0.0;
    for (int s = 0; s < ZGEC_NSYM_SEQ; s++) bits += zgec_bits(total, (double)hist[s]);
    return -bits;
}

double zgec_seq_triple_cost(uint32_t ll, uint32_t ml, uint32_t offbase,
                             const uint32_t *ll_hist, const uint32_t *ml_hist,
                             const uint32_t *of_hist, double lbar)
{
    /* Data-dependent cost: -log2 smoothed prob of each code + extra bits,
       plus literal cost ll*lbar. */
    uint8_t nb_ll = 0, nb_ml = 0, nb_of = 0;
    uint8_t c_ll = zgec_seq_code_of(ll, &nb_ll);
    uint8_t c_ml = zgec_seq_code_of(ml >= 3 ? ml - 3 : 0, &nb_ml);
    uint8_t c_of = zgec_seq_code_of(offbase >= 1 ? offbase - 1 : 0, &nb_of);
    double cost = 0.0;
    const uint32_t *hists[3] = { ll_hist, ml_hist, of_hist };
    uint8_t codes[3] = { c_ll, c_ml, c_of };
    uint8_t nbs[3] = { nb_ll, nb_ml, nb_of };
    for (int k = 0; k < 3; k++) {
        double total = 0.0;
        if (hists[k]) {
            for (int s = 0; s < ZGEC_NSYM_SEQ; s++) total += (double)hists[k][s];
            double c = hists[k] ? (double)hists[k][codes[k]] : 0.0;
            /* Laplace smoothing over 66 symbols. */
            double p = (c + 1.0) / (total + (double)ZGEC_NSYM_SEQ);
            if (p < 1e-12) p = 1e-12;
            cost += -(log(p) / log(2.0));
        }
        cost += (double)nbs[k]; /* extra bits written raw */
    }
    cost += (double)ll * lbar;
    return cost;
}

zgec_err zgec_segment(const zgec_parse *p,
                       size_t **bounds, size_t *n_segments,
                       size_t target_seg_sequences)
{
    if (!p || !bounds || !n_segments) return ZGEC_ERR_INVAL;
    if (target_seg_sequences == 0) target_seg_sequences = 16384;

    if (p->n_seq == 0) {
        size_t *b = (size_t *)zgec_alloc(2 * sizeof(size_t), _Alignof(size_t));
        if (!b) return ZGEC_ERR_NOMEM;
        b[0] = 0;
        b[1] = 0;
        *bounds = b;
        *n_segments = 1;
        return ZGEC_OK;
    }

    size_t n_seg = (p->n_seq + target_seg_sequences - 1) / target_seg_sequences;
    if (n_seg == 0) n_seg = 1;
    if (n_seg > ZGEC_MAX_SEGMENTS) n_seg = ZGEC_MAX_SEGMENTS;

    size_t *b = (size_t *)zgec_alloc((n_seg + 1) * sizeof(size_t), _Alignof(size_t));
    if (!b) return ZGEC_ERR_NOMEM;

    size_t seqs_per_seg = (p->n_seq + n_seg - 1) / n_seg;
    b[0] = 0;
    for (size_t i = 1; i < n_seg; i++) {
        size_t pos = i * seqs_per_seg;
        if (pos > p->n_seq) pos = p->n_seq;
        b[i] = pos;
    }
    b[n_seg] = p->n_seq;

    *bounds = b;
    *n_segments = n_seg;
    return ZGEC_OK;
}

void zgec_lit_bounds(const zgec_parse *p,
                     const size_t *seq_bounds,
                     size_t n_segments,
                     size_t *lit_bounds)
{
    memset(lit_bounds, 0, (n_segments + 1) * sizeof(size_t));
    size_t lit_pos = 0;
    for (size_t s = 0; s < n_segments; s++) {
        size_t start_seq = seq_bounds[s];
        size_t end_seq = seq_bounds[s + 1];
        lit_bounds[s] = lit_pos;
        for (size_t i = start_seq; i < end_seq; i++) {
            lit_pos += p->seq[i].ll;
        }
    }
    lit_bounds[n_segments] = p->n_lit;
}

/* ---- parse helpers (sections 11.3-11.4) ---- */

/* Skip-schedule shift per tier (section 11.3 step 5):
 * step = ((ip - anchor) >> shift) + 1. Fast uses 5,
 * main 7, high 8 (spec ranges: fast 5-6, main 7-8,
 * high 8; 8 gives the best ratio, 5-6 is faster on
 * incompressible data). */
static unsigned parse_skip_shift(zgec_tier tier)
{
    switch (tier) {
    case ZGEC_TIER_FAST:
        return 5u;
    case ZGEC_TIER_MAIN:
        return 7u;
    case ZGEC_TIER_HIGH:
        return 8u;
    default:
        return 7u;
    }
}

/* Fixed fallback minimum non-repeat length for the
 * first implementation: 5, 6 beyond 256 KiB
 * (section 11.4). Repeat offsets keep minimum 4. */
static uint32_t parse_min_norep(size_t ip)
{
    return (ip >= (size_t)262144) ? 6u : 5u;
}

static uint32_t parse_offbase(uint32_t off, const zgec_reps *reps)
{
    if (off == reps->rep[0]) {
        return 1u;
    }
    if (off == reps->rep[1]) {
        return 2u;
    }
    if (off == reps->rep[2]) {
        return 3u;
    }
    return off + 3u;
}

/* Cost in bits of coding the three sequence fields alone, i.e. the
 * cost(LL, ML, OF) term of section 11.4. It contains no literal
 * term: the literals that precede the match are coded in either
 * case, so only the match bytes count as the saving. Each field
 * contributes the smoothed code cost from the running histograms
 * plus its raw extra bits; the offset extra-bit count is e - 1 with
 * e = floor(log2(v)), the leading-zero count of the distance value. */
static double parse_seq_cost(uint32_t ll, uint32_t ml, uint32_t offbase,
                             const uint32_t *ll_hist,
                             const uint32_t *ml_hist,
                             const uint32_t *of_hist)
{
    uint8_t nb_ll = 0, nb_ml = 0, nb_of = 0;
    uint8_t c_ll = zgec_seq_code_of(ll, &nb_ll);
    uint8_t c_ml = zgec_seq_code_of(ml >= 3 ? ml - 3 : 0, &nb_ml);
    uint8_t c_of = zgec_seq_code_of(offbase >= 1 ? offbase - 1 : 0, &nb_of);
    const uint32_t *hists[3] = { ll_hist, ml_hist, of_hist };
    uint8_t codes[3] = { c_ll, c_ml, c_of };
    uint8_t nbs[3] = { nb_ll, nb_ml, nb_of };
    double cost = 0.0;
    for (int k = 0; k < 3; k++) {
        double total = 0.0;
        if (hists[k]) {
            for (int s = 0; s < ZGEC_NSYM_SEQ; s++) total += (double)hists[k][s];
            /* Laplace smoothing over 66 symbols. */
            double p = ((double)hists[k][codes[k]] + 1.0) /
                       (total + (double)ZGEC_NSYM_SEQ);
            if (p < 1e-12) p = 1e-12;
            cost += -(log(p) / log(2.0));
        }
        cost += (double)nbs[k]; /* extra bits written raw */
    }
    return cost;
}

/* Price score of a candidate (section 11.4):
 * score = len * Lbar - cost(LL, ML, OF) * lscale.
 * Positive means the match saves bits over coding its len bytes as
 * literals. */
static double parse_score(uint32_t len, uint32_t ll, uint32_t offbase,
                          const uint32_t *ll_hist, const uint32_t *ml_hist,
                          const uint32_t *of_hist, double lbar, double lscale)
{
    double cost = parse_seq_cost(ll, len, offbase, ll_hist, ml_hist, of_hist);
    return (double)len * lbar - cost * lscale;
}

/* Running estimate of the average cost in bits of one literal
 * (section 11.4): the order-0 entropy of the literals seen so far,
 * lightly smoothed so early estimates stay finite, clamped to the
 * [2, 8] band. The prior is uniform, so an empty histogram returns
 * the 6-bit initial estimate. */
#define PARSE_LBAR_PRIOR 0.25
static double parse_lbar(const uint32_t *lit_hist, size_t total)
{
    double t;
    double bits = 0.0;
    double per;
    int s;
    if (total < 64) return 6.0;
    t = (double)total + PARSE_LBAR_PRIOR * 256.0;
    for (s = 0; s < 256; s++) {
        double c = (double)lit_hist[s] + PARSE_LBAR_PRIOR;
        bits += c * (log(t) - log(c));
    }
    per = bits / (double)total;
    if (per < 2.0) per = 2.0;
    if (per > 8.0) per = 8.0;
    return per;
}

/* Upper bound on the skip schedule (section 11.3). The schedule
 * step grows with the current literal run so incompressible data is
 * scanned coarsely, but an unbounded step lets one long run of
 * literals stride the scan past a whole compressible region (a
 * random prefix before source text, say) and lose its matches. The
 * cap keeps every position visible to within PARSE_MAX_STEP bytes at
 * a cost that is negligible even on pure noise (2 MiB / 32 probes). */
#define PARSE_MAX_STEP 32u

/* Lazy evaluation is worthwhile only for the short matches that a
 * one-byte lookahead can plausibly beat. Skipping it for a long match
 * removes a second full match-finder probe for most of a source tree
 * at no measurable ratio cost. */
#define PARSE_LAZY_MAX_LEN 64u

/* ---- actual parse with match finder ---- */

zgec_err zgec_parse_block(zgec_parse **out,
                          const uint8_t *src, size_t raw_size,
                          const uint8_t *dict, size_t dict_size,
                          zgec_tier tier, double lambda)
{
    return zgec_parse_block_ex(out, src, raw_size, dict, dict_size,
                              NULL, 0, tier, lambda);
}

zgec_err zgec_parse_block_ex(zgec_parse **out,
                          const uint8_t *src, size_t raw_size,
                          const uint8_t *dict, size_t dict_size,
                          const uint8_t *litref, size_t litref_size,
                          zgec_tier tier, double lambda)
{
    if (!out || (!src && raw_size > 0)) return ZGEC_ERR_INVAL;
    if ((!litref && litref_size > 0)) return ZGEC_ERR_INVAL;

    /* Lambda is the speed/ratio dial (section 11.7):
     * 0 maximises ratio, larger values raise the
     * price-gate bar slightly towards fewer, longer
     * matches (faster decode). Clamped small so the
     * effect stays a slight threshold scale. */
    double lam = (lambda < 0.0) ? 0.0 : ((lambda > 4.0) ? 4.0 : lambda);
    double lscale = 1.0 + 0.25 * lam;

    zgec_parse *p = (zgec_parse *)zgec_alloc(sizeof(*p), _Alignof(zgec_parse));
    if (!p) return ZGEC_ERR_NOMEM;
    memset(p, 0, sizeof(*p));

    size_t seq_cap = 65536;
    p->seq = (zgec_sequence *)zgec_alloc(seq_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
    if (!p->seq) { zgec_parse_free(p); return ZGEC_ERR_NOMEM; }

    p->lit = (uint8_t *)zgec_alloc(raw_size + ZGEC_LIT_SLACK + 64, 64);
    if (!p->lit) { zgec_parse_free(p); return ZGEC_ERR_NOMEM; }
    p->raw_size = (uint32_t)raw_size;
    p->n_seq = 0;
    p->n_lit = 0;

    if (raw_size == 0) {
        *out = p;
        return ZGEC_OK;
    }

    /* Virtual buffer [dict][litref][src] (section 6.1). */
    size_t prefix = dict_size + litref_size;
    size_t vb_size = prefix + raw_size;
    uint8_t *vb;
    zgec_matcher *m;
    if (prefix > (size_t)(1u << 24)) {
        zgec_parse_free(p);
        return ZGEC_ERR_INVAL;
    }
    vb = (uint8_t *)zgec_alloc(vb_size + 64, 64);
    if (!vb) { zgec_parse_free(p); return ZGEC_ERR_NOMEM; }
    if (dict && dict_size > 0) {
        memcpy(vb, dict, dict_size);
    }
    if (litref && litref_size > 0) {
        memcpy(vb + dict_size, litref, litref_size);
    }
    memcpy(vb + prefix, src, raw_size);

    m = zgec_matcher_create(tier, vb_size);
    if (!m) {
        zgec_free(vb);
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }
    zgec_matcher_reset(m, vb, vb_size, (dict && dict_size > 0) ? dict : NULL,
                       dict_size);
    if (litref_size > 0) {
        /* Seed the LITREF region too, with the tier's priming stride, so
         * matches may reach back into the predecessors' literals. */
        size_t stride = (tier == ZGEC_TIER_HIGH) ? (size_t)1 : (size_t)2;
        size_t pos;
        for (pos = dict_size; pos < prefix; pos += stride) {
            zgec_matcher_insert(m, vb, pos);
        }
    }

    zgec_reps reps;
    zgec_reps_init(&reps);

    /* Price-gate state (section 11.4): running code
     * histograms stand in for the previous segment's
     * statistics (Laplace-smoothed inside the cost
     * function, so the first segment starts uniform),
     * and Lbar is the running average literal cost in
     * bits with a 512-byte prior at the 6-bit initial
     * estimate. */
    uint32_t ll_hist[ZGEC_NSYM_SEQ];
    uint32_t ml_hist[ZGEC_NSYM_SEQ];
    uint32_t of_hist[ZGEC_NSYM_SEQ];
    uint32_t lit_hist[ZGEC_NSYM_LIT];
    size_t lit_total = 0;      /* literals tallied into lit_hist */
    size_t lit_eval = 0;       /* lit_total at the last lbar estimate */
    double lbar = 6.0;
    unsigned shift = parse_skip_shift(tier);
    memset(ll_hist, 0, sizeof(ll_hist));
    memset(ml_hist, 0, sizeof(ml_hist));
    memset(of_hist, 0, sizeof(of_hist));
    memset(lit_hist, 0, sizeof(lit_hist));

    size_t ip = prefix;
    size_t end = prefix + raw_size;
    size_t anchor = ip;

    /* Per-position order (section 11.3): hash plus
     * bucket prefetch for visited positions, rep
     * checks, long table then short table with SIMD
     * tag compare, 8-byte XOR plus tzcnt length, best
     * by price score, lazy check at ip + 1 by score
     * with conditional-move selection. Minimum
     * non-repeat 5 (6 beyond 256 KiB fallback),
     * repeat 4. */
    while (ip + (size_t)4 <= end) {
        uint32_t ll;
        uint32_t need;
        uint32_t is_rep;
        uint32_t offbase;
        double cur_score;
        uint32_t cur_len = 0;
        uint32_t cur_off = 0;
        zgec_match match;

        /* Prefetch the upcoming input line; the
         * finder prefetches the hash bucket lines of
         * each visited position internally. */
        if (ip + (size_t)64 <= end) {
            __builtin_prefetch((const void *)(vb + ip + (size_t)64), 0, 3);
        }
        match = zgec_matcher_find(m, vb, ip, reps.rep[0], reps.rep[1], 4u,
                                  (uint32_t)(end - ip));
        ll = (uint32_t)(ip - anchor);
        if (match.length >= 4u) {
            is_rep = (match.offset == reps.rep[0] || match.offset == reps.rep[1] ||
                      match.offset == reps.rep[2]) ? 1u : 0u;
            need = (is_rep != 0u) ? 4u : parse_min_norep(ip);
            if (match.length >= need) {
                offbase = parse_offbase(match.offset, &reps);
                cur_score = parse_score(match.length, ll, offbase,
                                        ll_hist, ml_hist, of_hist, lbar, lscale);
                if (cur_score > 0.0) {
                    cur_len = match.length;
                    cur_off = match.offset;
                }
            }
        }

        /* Lazy evaluation at ip + 1, driven by the
         * same price score and selected with a
         * conditional move on the score. */
        if (cur_len > 0u && cur_len <= PARSE_LAZY_MAX_LEN &&
            tier >= ZGEC_TIER_MAIN && ip + (size_t)5 <= end) {
            zgec_match nm = zgec_matcher_find(m, vb, ip + (size_t)1,
                                              reps.rep[0], reps.rep[1], 4u,
                                              (uint32_t)(end - (ip + (size_t)1)));
            uint32_t nll = (uint32_t)((ip + (size_t)1) - anchor);
            uint32_t nis_rep = (nm.length >= 4u &&
                                (nm.offset == reps.rep[0] || nm.offset == reps.rep[1] ||
                                 nm.offset == reps.rep[2])) ? 1u : 0u;
            uint32_t nneed = (nis_rep != 0u) ? 4u : parse_min_norep(ip + (size_t)1);
            double base = cur_score;
            double nscore = -1.0;
            uint32_t defer;
            if (nm.length >= nneed) {
                uint32_t noffbase = parse_offbase(nm.offset, &reps);
                nscore = parse_score(nm.length, nll, noffbase,
                                     ll_hist, ml_hist, of_hist, lbar, lscale);
            }
            /* cmov-style: the score comparison feeds a
             * 0/1 select, not a length branch. */
            defer = (nscore > base) ? 1u : 0u;
            if (defer != 0u) {
                zgec_matcher_insert(m, vb, ip);
                ip += (size_t)1;
                continue;
            }
        }

        if (cur_len > 0u) {
            uint32_t ml = cur_len;
            uint32_t off = cur_off;
            uint32_t ob;
            uint8_t nb = 0;

            if (off == reps.rep[0]) {
                ob = 1;
            } else if (off == reps.rep[1]) {
                ob = 2;
                reps.rep[1] = reps.rep[0];
                reps.rep[0] = off;
            } else if (off == reps.rep[2]) {
                ob = 3;
                reps.rep[2] = reps.rep[1];
                reps.rep[1] = reps.rep[0];
                reps.rep[0] = off;
            } else {
                ob = off + 3u;
                reps.rep[2] = reps.rep[1];
                reps.rep[1] = reps.rep[0];
                reps.rep[0] = off;
            }

            if (ll > 0) {
                uint32_t u;
                for (u = 0; u < ll; u++) lit_hist[vb[anchor + (size_t)u]]++;
                memcpy(p->lit + p->n_lit, vb + anchor, ll);
                p->n_lit += ll;
                lit_total += ll;
            }

            if (p->n_seq >= seq_cap) {
                size_t new_cap = seq_cap * 2;
                zgec_sequence *new_seq = (zgec_sequence *)zgec_alloc(new_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
                if (!new_seq) {
                    zgec_matcher_destroy(m);
                    zgec_free(vb);
                    zgec_parse_free(p);
                    return ZGEC_ERR_NOMEM;
                }
                memcpy(new_seq, p->seq, p->n_seq * sizeof(zgec_sequence));
                zgec_free(p->seq);
                p->seq = new_seq;
                seq_cap = new_cap;
            }

            p->seq[p->n_seq].ll = ll;
            p->seq[p->n_seq].ml = ml;
            p->seq[p->n_seq].offbase = ob;
            p->n_seq++;

            /* Fold the accepted triple into the
             * running statistics for the price gate. */
            ll_hist[zgec_seq_code_of(ll, &nb)]++;
            ml_hist[zgec_seq_code_of(ml >= 3u ? ml - 3u : 0u, &nb)]++;
            of_hist[zgec_seq_code_of(ob >= 1u ? ob - 1u : 0u, &nb)]++;
            if (lit_total >= lit_eval + 256u) {
                lbar = parse_lbar(lit_hist, lit_total);
                lit_eval = lit_total;
            }

            zgec_matcher_insert_match(m, vb, ip, ml);
            ip += ml;
            anchor = ip;
        } else {
            /* Skip schedule: visit every position on
             * compressible data, stride out on runs
             * of literals (section 11.3). Only
             * visited positions are inserted. */
            size_t step;
            zgec_matcher_insert(m, vb, ip);
            step = ((ip - anchor) >> shift) + (size_t)1;
            if (step > (size_t)PARSE_MAX_STEP) step = (size_t)PARSE_MAX_STEP;
            ip += step;
        }
    }

    if (anchor < end) {
        size_t tail_len = end - anchor;
        memcpy(p->lit + p->n_lit, vb + anchor, tail_len);
        p->n_lit += tail_len;
    }

    zgec_matcher_destroy(m);
    zgec_free(vb);

    *out = p;
    return ZGEC_OK;
}
