#include <stdint.h>
#if defined(__AVX2__) || defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include "zgec_parse.h"
#include "zgec_seq.h"

#if defined(_MSC_VER)
#define ZGEC_ENC_TLS __declspec(thread)
#else
#define ZGEC_ENC_TLS _Thread_local
#endif

#define ZGEC_FAST_SCRATCH_BITS 18u
static ZGEC_ENC_TLS uint32_t fast_ts_scratch[1u << ZGEC_FAST_SCRATCH_BITS];
static ZGEC_ENC_TLS uint32_t fast_tl_scratch[1u << ZGEC_FAST_SCRATCH_BITS];

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- helpers ---- */

/* Checked multiply for this file's allocation sizes. sizeof() and the
 * capacity doubling both need guarding: a wrapped size allocates short
 * and is then written past its end. The add-side helper is in
 * zgec_common.h (zgec_add_overflows). */
static int parse_mul_overflows(size_t a, size_t b)
{
    return a != 0u && b > SIZE_MAX / a;
}

/* Entropy contribution of one symbol, in bits: count * -log2(p). The
 * result is positive for any counted symbol, so a sum over a histogram
 * is the non-negative cost of coding that histogram. */
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
    int s_idx = 0;
#if defined(__AVX2__)
    __m256i vsum = _mm256_setzero_si256();
    for (; s_idx + 8 <= ZGEC_NSYM_SEQ; s_idx += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(hist + s_idx));
        vsum = _mm256_add_epi32(vsum, v);
    }
    __m128i vlow = _mm256_castsi256_si128(vsum);
    __m128i vhigh = _mm256_extracti128_si256(vsum, 1);
    vlow = _mm_add_epi32(vlow, vhigh);
    vlow = _mm_hadd_epi32(vlow, vlow);
    vlow = _mm_hadd_epi32(vlow, vlow);
    total = (double)(uint32_t)_mm_cvtsi128_si32(vlow);
#endif
    for (; s_idx < ZGEC_NSYM_SEQ; s_idx++) total += (double)hist[s_idx];
    if (total == 0) return 0.0;
    double bits = 0.0;
    for (int s = 0; s < ZGEC_NSYM_SEQ; s++) bits += zgec_bits(total, (double)hist[s]);
    /* zgec_bits() already returns a positive contribution, so negating
     * the total made the "cost" negative and would invert every
     * comparison the value feeds. */
    return bits;
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
        } else {
            /* No histogram is not the same as a free symbol: charging
             * only the raw extra bits made every sequence look cheap
             * while the model is untrained. Price the code at the
             * uniform rate over the 66 codes instead. */
            cost += log2((double)ZGEC_NSYM_SEQ);
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

    /* Ceiling division without the additive form: n_seq + target - 1 can
     * overflow for a large caller-supplied target, and the wrapped
     * quotient would then feed the allocation below. */
    size_t n_seg = p->n_seq / target_seg_sequences;
    if (p->n_seq % target_seg_sequences != 0) n_seg++;
    if (n_seg == 0) n_seg = 1;
    if (n_seg > ZGEC_MAX_SEGMENTS) n_seg = ZGEC_MAX_SEGMENTS;

    /* n_seg <= ZGEC_MAX_SEGMENTS (4096) past this point, so
     * (n_seg + 1) * sizeof(size_t) cannot overflow. */
    size_t *b = (size_t *)zgec_alloc((n_seg + 1) * sizeof(size_t), _Alignof(size_t));
    if (!b) return ZGEC_ERR_NOMEM;

    size_t seqs_per_seg = p->n_seq / n_seg;
    if (p->n_seq % n_seg != 0) seqs_per_seg++;
    b[0] = 0;
    {
        /* Built incrementally rather than as i * seqs_per_seg: the
         * product has no bound the format guarantees. */
        size_t pos = 0;
        for (size_t i = 1; i < n_seg; i++) {
            pos += seqs_per_seg;
            if (pos > p->n_seq) pos = p->n_seq;
            b[i] = pos;
        }
    }
    b[n_seg] = p->n_seq;

    *bounds = b;
    *n_segments = n_seg;
    return ZGEC_OK;
}

/* Trusted-input helper. The interface is fixed by the header -- a void
 * return gives no way to report a malformed argument -- so the contract
 * is that seq_bounds comes from zgec_segment() or the encoder's greedy
 * merge, i.e. 0 = seq_bounds[0] <= ... <= seq_bounds[n_segments] with
 * the last equal to p->n_seq. The clamping below exists so that a bad
 * bound degrades into a wrong boundary rather than an out-of-range read
 * of p->seq; it does not validate the contract. */
void zgec_lit_bounds(const zgec_parse *p,
                     const size_t *seq_bounds,
                     size_t n_segments,
                     size_t *lit_bounds)
{
    if (!lit_bounds) return;
    if (!p || !seq_bounds || n_segments == 0) {
        /* Nothing to derive. Define the whole array rather than only the
         * boundary the n_segments == 0 caller reads: a NULL seq_bounds with
         * n_segments > 0 would otherwise leave lit_bounds[1..n_segments]
         * indeterminate. The write is the caller's own n_segments+1 array,
         * and the same size the main path memsets below. With n_segments == 0
         * this is exactly the old lit_bounds[0] = p->n_lit result. */
        memset(lit_bounds, 0, (n_segments + 1) * sizeof(size_t));
        lit_bounds[n_segments] = p ? p->n_lit : 0;
        return;
    }
    memset(lit_bounds, 0, (n_segments + 1) * sizeof(size_t));
    size_t lit_pos = 0;
    for (size_t s = 0; s < n_segments; s++) {
        size_t start_seq = seq_bounds[s];
        size_t end_seq = seq_bounds[s + 1];
        if (start_seq > p->n_seq) start_seq = p->n_seq;
        if (end_seq > p->n_seq) end_seq = p->n_seq;
        if (end_seq < start_seq) end_seq = start_seq;
        lit_bounds[s] = lit_pos;
        size_t i = start_seq;
#if defined(__AVX2__)
        __m256i vsum = _mm256_setzero_si256();
        while (i + 8 <= end_seq) {
            const zgec_sequence *q = &p->seq[i];
            __m256i v_ll = _mm256_set_epi32((int)q[7].ll, (int)q[6].ll, (int)q[5].ll, (int)q[4].ll,
                                            (int)q[3].ll, (int)q[2].ll, (int)q[1].ll, (int)q[0].ll);
            vsum = _mm256_add_epi32(vsum, v_ll);
            i += 8;
        }
        __m128i vlow = _mm256_castsi256_si128(vsum);
        __m128i vhigh = _mm256_extracti128_si256(vsum, 1);
        vlow = _mm_add_epi32(vlow, vhigh);
        vlow = _mm_hadd_epi32(vlow, vlow);
        vlow = _mm_hadd_epi32(vlow, vlow);
        lit_pos += (size_t)(uint32_t)_mm_cvtsi128_si32(vlow);
#endif
        for (; i < end_seq; i++) {
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

/* Minimum non-repeat length (section 11.4). A 5-byte short hash cannot
 * find a 4-byte match, so 5 is the floor the format's hashing imposes;
 * 4 stays available to repeat offsets. The old fallback raised the main
 * tier's floor to 6 past 256 KiB, but that discarded the profitable
 * 5-byte matches the fast tier keeps, and left the main tier *worse*
 * than the fast one on binary data once its literals were priced in
 * bits (mr 2.69 vs 2.81 at level 6). Keep 5 everywhere: the price gate
 * above, not a length floor, decides whether a match pays. */
static uint32_t parse_min_norep(size_t ip, zgec_tier tier)
{
    (void)ip;
    (void)tier;
    return 5u;
}

/* Offbase selection is the shared zgec_reps_encode helper (section 8.2);
 * kept as a thin wrapper so the call sites read unchanged. */
static uint32_t parse_offbase(uint32_t off, const zgec_reps *reps)
{
    return zgec_reps_encode(reps, off);
}

/* Cost in bits of coding the three sequence fields alone, i.e. the
 * cost(LL, ML, OF) term of section 11.4. It contains no literal
 * term: the literals that precede the match are coded in either
 * case, so only the match bytes count as the saving. Each field
 * contributes the smoothed code cost from the running histograms
 * plus its raw extra bits; the offset extra-bit count is e - 1 with
 * e = floor(log2(v)), the leading-zero count of the distance value. */
/* The price gate is evaluated once per candidate match (millions of
 * times per block), so the per-call cost must stay tiny. The Laplace
 * cost of one code is
 *
 *     -log2((count + 1) / (total + 66))
 *       = log2(total + 66) - log2(count + 1),
 *
 * and the three totals are carried incrementally by the parser, so only
 * the three tiny per-symbol terms remain. The per-symbol term comes from
 * the `*_base[]` tables, which hold log2(count + 1) per code, and
 * `l2tot[]` holds log2(total + 66) for LL/ML/OF: a cost is one
 * subtraction and one float narrowing, which is the same value the
 * earlier materialised `*_cost[]` tables held. Materialising all 66
 * entries of a cost table on every accepted sequence cost three
 * 66-iteration log loops to move two numbers -- one histogram bin and
 * one total -- so the tables are updated at those two points instead. */
static inline float parse_ll_cost(uint32_t ll, float l2tot0, const float *ll_base)
{
    uint8_t nb_ll = 0;
    uint8_t c_ll = zgec_seq_code_of(ll, &nb_ll);
    return (l2tot0 - ll_base[c_ll]) + (float)nb_ll;
}

/* Price score of a candidate (section 11.4):
 * score = len * Lbar - cost(LL, ML, OF) * lscale.
 * Positive means the match saves bits over coding its len bytes as
 * literals. */
static inline float parse_score_fast(uint32_t len, float ll_cost, uint32_t offbase,
                                     const float *l2tot,
                                     const float *ml_base, const float *of_base,
                                     float lbar, float lscale)
{
    uint8_t nb_ml = 0, nb_of = 0;
    uint8_t c_ml = zgec_seq_code_of(len >= 3u ? len - 3u : 0u, &nb_ml);
    uint8_t c_of;
    if (offbase <= 3u) {
        c_of = (uint8_t)(offbase >= 1u ? offbase - 1u : 0u);
        nb_of = 0;
    } else {
        c_of = zgec_seq_code_of(offbase - 1u, &nb_of);
    }
    float cost = ll_cost;
    cost += (l2tot[1] - ml_base[c_ml]) + (float)nb_ml;
    cost += (l2tot[2] - of_base[c_of]) + (float)nb_of;
    return (float)len * lbar - cost * lscale;
}

static inline float parse_score_rep_fast(uint32_t len, float ll_cost, unsigned ri,
                                         const float *l2tot,
                                         const float *ml_base, const float *of_base,
                                         float lbar, float lscale)
{
    uint8_t nb_ml = 0;
    uint8_t c_ml = zgec_seq_code_of(len >= 3u ? len - 3u : 0u, &nb_ml);
    float cost = ll_cost;
    cost += (l2tot[1] - ml_base[c_ml]) + (float)nb_ml;
    cost += (l2tot[2] - of_base[ri]);
    return (float)len * lbar - cost * lscale;
}

/* Running estimate of the average cost in bits of one literal
 * (section 11.4): the order-0 entropy of the literals seen so far,
 * lightly smoothed so early estimates stay finite, clamped to the
 * [2, 8] band. The prior is uniform, so an empty histogram returns
 * the 6-bit initial estimate. */
#define PARSE_LBAR_PRIOR 0.25f
static float parse_lbar(const uint32_t *lit_hist, size_t total)
{
    float t;
    float bits = 0.0f;
    float per;
    int s = 0;
    if (total < 64) return 6.0f;
    t = (float)total + PARSE_LBAR_PRIOR * 256.0f;
    float log2_t = log2f(t);
#if defined(__AVX2__)
    __m256 vprior = _mm256_set1_ps(PARSE_LBAR_PRIOR);
    for (; s + 8 <= 256; s += 8) {
        __m256i vhist = _mm256_loadu_si256((const __m256i *)(const void *)(lit_hist + s));
        __m256 vc = _mm256_add_ps(_mm256_cvtepi32_ps(vhist), vprior);
        float c_arr[8];
        _mm256_storeu_ps(c_arr, vc);
        for (int k = 0; k < 8; k++) {
            bits += c_arr[k] * (log2_t - log2f(c_arr[k]));
        }
    }
#endif
    for (; s < 256; s++) {
        float c = (float)lit_hist[s] + PARSE_LBAR_PRIOR;
        bits += c * (log2_t - log2f(c));
    }
    per = bits / (float)total;
    if (per < 2.0f) per = 2.0f;
    if (per > 8.0f) per = 8.0f;
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

/* Table-latency prefetch hooks in the main parse loop. Measurement on a
 * source tree says they cost more than they save: each hook is an extra
 * hash of the input (two 8-byte loads and a mix each) and on the fast and
 * main tiers the tables are small enough that the miss they hide is
 * shorter than the work to issue the hint. Off by default; compile with
 * -DZGEC_PARSE_PREFETCH=1 to re-measure. */
#ifndef ZGEC_PARSE_PREFETCH
#define ZGEC_PARSE_PREFETCH 0
#endif

/* How many literals of a pending run are folded into lit_hist at a time.
 * Folding on acceptance only left Lbar describing the literals before the
 * last accepted match, which is a stale model during a long literal run
 * (and permanently stale on data where no match is ever accepted). */
#define PARSE_LIT_FOLD 256u

/* Lazy evaluation is worthwhile only for the short matches that a
 * one-byte lookahead can plausibly beat. The step is a second full
 * match-finder call, which on a source tree is the single most expensive
 * thing the parser does, so the ceiling is where measurement stops
 * earning: raising it from 16 to 64 (i.e. running the second probe for
 * two-thirds more accepted matches) gains 0.3% of ratio for 8% of encode
 * wall time. Matches longer than the ceiling are kept outright. */
#ifndef PARSE_LAZY_MAX_LEN
#define PARSE_LAZY_MAX_LEN 16u
#endif

/* ---- actual parse with match finder ---- */

zgec_err zgec_parse_block(zgec_parse **out,
                          const uint8_t *src, size_t raw_size,
                          const uint8_t *dict, size_t dict_size,
                          zgec_tier tier, double lambda)
{
    return zgec_parse_block_ex(out, src, raw_size, dict, dict_size,
                              NULL, 0, tier, lambda);
}

#ifndef ZGEC_FASTDFAST_OFF
/* ---- Fast tier (dfast-style) prototype -------------------------------------
 * Two single-entry tables (5-byte short hash, 8-byte long hash), one entry per
 * bucket, positions stored +1 so 0 means empty. Per visited position: rep0
 * compare, then one probe of each table, take the longest acceptable match.
 * No tags, no ring buffer, no lazy step beyond the one-position check below.
 * Skip schedule step = ((ip - anchor) >> shift) + 1. Compiled only with
 * ZGEC_FASTDFAST (disable with -DZGEC_FASTDFAST_OFF).
 *
 * Measured and rejected: folding the two tables into one 5-byte-keyed table
 * of two-entry buckets. It halves the hashes and the cache lines touched per
 * position at unchanged memory, but it also gives up the 8-byte key, and the
 * long table was earning rather more than the extra candidate recovers: at
 * level 1 it lost 3.8% of ratio on llvm_small.tar and 4.4% on text.tar for
 * -1% and +3% of encode throughput (the two tables are separate arrays and
 * their probes already overlap in the out-of-order window, so the saved line
 * touch was worth almost nothing). Keep the two keys. */
#ifndef ZGEC_FAST_SHORT_BITS
#define ZGEC_FAST_SHORT_BITS 20u
#endif
#ifndef ZGEC_FAST_LONG_BITS
#define ZGEC_FAST_LONG_BITS 20u
#endif
#ifndef ZGEC_FAST_MIN_SHORT
#define ZGEC_FAST_MIN_SHORT 5u
#endif
#ifndef ZGEC_FAST_MIN_LONG
#define ZGEC_FAST_MIN_LONG 8u
#endif
#ifndef ZGEC_FAST_SHIFT
#define ZGEC_FAST_SHIFT 6u
#endif

#ifndef ZGEC_FAST_SHORT_HASH_BYTES
#define ZGEC_FAST_SHORT_HASH_BYTES 5u
#endif
#ifndef ZGEC_FAST_INS_ALL
#define ZGEC_FAST_INS_ALL 1
#endif
#ifndef ZGEC_FAST_INS_LONG
#define ZGEC_FAST_INS_LONG 1
#endif
/* Cap on interior positions indexed per accepted match. The interior-insert
 * loop below walks the whole match two positions at a time, so a long match
 * costs O(best_len) hash-and-store pairs: on periodic data where matches run
 * kilobytes, that write stream dominates the accept path. 0 restores the
 * unbounded walk.
 *
 * Measured and left off: the cap is a large speed win where matches are long
 * (nci +50% encode throughput at span 8, vs +13-18% on webster/samba in an
 * earlier sweep) and the ratio proxy barely moves -- but the proxy prices
 * sequences at a flat 3 bytes and so is blind to the offset distribution the
 * cap changes, and end-to-end output at -l 4 grows +0.6% (dickens) to +1.7%
 * (nci). That is past the line a speed dial may trade, so the default keeps
 * every interior position indexed. Revisit if the fast tier ever gains a
 * price-gated accept, which would pay the cap back in rejected candidates. */
#ifndef ZGEC_FAST_INS_SPAN
#define ZGEC_FAST_INS_SPAN 0u
#endif
#ifndef ZGEC_FAST_PREFETCH
#define ZGEC_FAST_PREFETCH 0
#endif
#ifndef ZGEC_FAST_LAZY
#define ZGEC_FAST_LAZY 1
#endif
#ifndef ZGEC_FAST_LAZY_MAX
#define ZGEC_FAST_LAZY_MAX 12u
#endif
/* When the lazy lookahead runs, probe only the long (8-byte) table at ip+1,
 * not the short (5-byte) one as well. zstd's dfast lookahead likewise tests
 * only the long candidate it already has. The short-table probe finds matches
 * a few bytes long that rarely beat the current best_len+1 gate, so it costs a
 * hash and a probe per visited position for little decision change.
 *
 * Measured on Silesia (fast tier, 2 MiB blocks, median of 3 interleaved
 * rounds) against the post-Bolt fast_parse: long-only gives +2-4% encode
 * throughput (dickens +4.3%, xml +4.0%, samba +2.9%, nci +2.3%) at
 * +0.02-0.16pp of ratio proxy cost, and +0.03-0.13% end-to-end output at
 * -l 4. With the earlier LAZY_MAX=64 the win was larger (+4-10%), since the
 * skipped probe ran on every short match; at 12 it runs less often but still
 * pays for itself. */
#ifndef ZGEC_FAST_LAZY_LONG_ONLY
#define ZGEC_FAST_LAZY_LONG_ONLY 1
#endif
/* Table entries pack the position in the low 24 bits and a per-block
 * generation in the high 8. The P24 contract (vb_size <= 2^24, enforced by
 * zgec_parse_block_ex before the parse runs) makes the position field exact,
 * so the generation is free to serve as the validity tag: an entry left by an
 * earlier block reads as empty. The two 1 MiB tables therefore no longer need
 * a memset per block -- they are cleared only when the generation wraps, i.e.
 * once every 255 blocks -- which removes about one byte of clearing per input
 * byte at the 2 MiB presets. With ZGEC_FAST_GEN=0 the tables are cleared every
 * block and the tag is pinned to 1, which reproduces the pre-tag parse. */
/* Measured at level 4 on the 211.9 MB Silesia concatenation (single thread,
 * best of 5 interleaved rounds): the tag alone is 2.7% SLOWER than clearing
 * the tables per block, at identical output. The per-block memset is not pure
 * overhead -- it also leaves both tables resident, so a block's first touches
 * miss less. Kept behind the macro for block sizes and levels where the
 * clearing is a larger share of the block. */
#ifndef ZGEC_FAST_GEN
#define ZGEC_FAST_GEN 0
#endif
/* Carry the long table's candidate for the next visited position across the
 * loop, as zstd's dfast does: the hash and the table load for that position
 * are issued an iteration before the compare that consumes them, so the miss
 * overlaps this position's rep and candidate compares. The carried value is
 * kept equal to what a fresh load would return (see the store site). */
/* Measured at level 4 on Silesia (single thread, best of 5): with the tag on,
 * the carried candidate is 2.4% SLOWER than loading it fresh, at identical
 * output. The extra hash and load per position cost more than the memory
 * latency they hide -- the same result the table-prefetch probe reached. */
#ifndef ZGEC_FAST_PIPE
#define ZGEC_FAST_PIPE 0
#endif
/* Extend an accepted match backwards into the pending literals while the byte
 * before it still agrees, as zstd's dfast and lazy accept paths do. */
#ifndef ZGEC_FAST_BACKEXT
#define ZGEC_FAST_BACKEXT 1
#endif
#ifndef ZGEC_FAST_BITS_MIN
#define ZGEC_FAST_BITS_MIN 10u
#endif
/* Buckets wanted per indexed position, as a shift: 2 means one bucket per
 * four positions. */
#ifndef ZGEC_FAST_BITS_SHIFT
#define ZGEC_FAST_BITS_SHIFT 2u
#endif

/* Bucket count for the fast tier's two tables, chosen from the number of
 * positions the block can index. The tables were a fixed 2^20 entries each
 * whatever the block: 8 MiB of table for a 2 MiB block, more than the block
 * itself, so nearly every probe was an L3 miss, and a 64 KiB block paid for
 * the same 8 MiB. The rule below sizes the tables from the block and clamps
 * both ends. One bucket per four positions measured best in the ratio-
 * preserving range: on a 2 MiB block it picks 2^19 and gives up 0.4% of
 * ratio for 4-7% of encode throughput, where one per eight positions gave up
 * 0.9-1.6% for 3-11% (the two measured within noise of each other on the
 * source tree and only text separated them), and a 2^22 table gained 0.02%
 * of ratio and cost 27% of throughput. */
static unsigned fast_table_bits(size_t positions, unsigned extra_bits)
{
    unsigned bits = ZGEC_FAST_SHORT_BITS;
    size_t want = positions >> (ZGEC_FAST_BITS_SHIFT + extra_bits);
    if (ZGEC_FAST_LONG_BITS < (unsigned)bits) bits = ZGEC_FAST_LONG_BITS;
    while (bits > ZGEC_FAST_BITS_MIN && ((size_t)1u << bits) > want) bits--;
    return bits;
}

/* Both tables hash the same eight bytes at a position, so every caller
 * loads them once and hands the value to both hashes. Splitting the load
 * out is a pure load elimination: the hashes are bit-identical to the older
 * pair that each read the bytes themselves, so the parse, the ratio and the
 * byte stream do not move. */
static inline uint32_t fast_hash_s_v(uint64_t v)
{
    return (uint32_t)(((v << (64u - 8u * ZGEC_FAST_SHORT_HASH_BYTES)) * 0x9E3779B97F4A7C15ULL) >> (64u - ZGEC_FAST_SHORT_BITS));
}

static inline uint32_t fast_hash_l_v(uint64_t v)
{
    return (uint32_t)((v * 0xD6E8FEB86659FD93ULL) >> (64u - ZGEC_FAST_LONG_BITS));
}

/* Entry packing for the two fast-tier tables. See the ZGEC_FAST_GEN comment. */
#if ZGEC_FAST_GEN
static ZGEC_ENC_TLS unsigned fast_scratch_gen;
#endif

static inline uint32_t fast_pack(uint32_t gen, size_t pos)
{
    return (gen << 24) | (uint32_t)pos;
}

static inline int fast_live(uint32_t e, uint32_t gen)
{
    return (e >> 24) == gen;
}

static inline size_t fast_pos_of(uint32_t e)
{
    return (size_t)(e & 0xFFFFFFu);
}



static inline __attribute__((always_inline))
uint32_t fast_match_len_tail(const uint8_t *a, const uint8_t *b, size_t cap)
{
    size_t len = 0;
    if (cap >= 8u) {
        uint64_t x = zgec_rd64(a) ^ zgec_rd64(b);
        if (x != 0u) {
            return (uint32_t)((unsigned)__builtin_ctzll(x) >> 3);
        }
        len = 8u;
        if (cap >= 16u) {
            uint64_t x2 = zgec_rd64(a + 8) ^ zgec_rd64(b + 8);
            if (x2 != 0u) {
                return (uint32_t)(8u + ((unsigned)__builtin_ctzll(x2) >> 3));
            }
            len = 16u;
        }
    }
#if defined(__AVX2__)
    while (len + 32u <= cap) {
        __m256i va = _mm256_loadu_si256((const __m256i *)(const void *)(a + len));
        __m256i vb256 = _mm256_loadu_si256((const __m256i *)(const void *)(b + len));
        unsigned m32 = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(va, vb256));
        if (m32 != 0xFFFFFFFFu) {
            return (uint32_t)(len + (unsigned)__builtin_ctz(~m32));
        }
        len += 32u;
    }
#endif
#if defined(__AVX2__) || defined(__SSE2__)
    while (len + 16u <= cap) {
        __m128i va16 = _mm_loadu_si128((const __m128i *)(const void *)(a + len));
        __m128i vb16 = _mm_loadu_si128((const __m128i *)(const void *)(b + len));
        unsigned m16 = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(va16, vb16));
        if (m16 != 0xFFFFu) {
            return (uint32_t)(len + (unsigned)__builtin_ctz(~m16));
        }
        len += 16u;
    }
#endif
    while (len + 8u <= cap) {
        uint64_t x = zgec_rd64(a + len) ^ zgec_rd64(b + len);
        if (x != 0u) {
            return (uint32_t)(len + ((unsigned)__builtin_ctzll(x) >> 3));
        }
        len += 8u;
    }
    if (len < cap) {
        if (cap >= 8u) {
            uint64_t x = zgec_rd64(a + cap - 8u) ^ zgec_rd64(b + cap - 8u);
            if (x != 0u) {
                return (uint32_t)(cap - 8u + ((unsigned)__builtin_ctzll(x) >> 3));
            }
            return (uint32_t)cap;
        }
        while (len < cap && a[len] == b[len]) {
            len++;
        }
    }
    return (uint32_t)len;
}

/* Out-of-line form of the match-length scan for the lean core. Inlining
 * the AVX2 loop into fast_parse_lite's position loop costs more than the
 * call: the many live values of the accept path force `anchor` and the
 * table base to spill and reload on every position, including the ~9 of
 * 10 positions that find no candidate at all. Keeping the scan behind a
 * call leaves the position loop's register set small. The scan itself is
 * unchanged, so the parse (and the byte stream) is identical. */
__attribute__((noinline))
static uint32_t fast_match_len_out(const uint8_t *a, const uint8_t *b, size_t cap)
{
    return fast_match_len_tail(a, b, cap);
}

/* Lean-core table entries carry a hash tag above the 24-bit position:
 * entry = (tag << 24) | pos, with tag >= 1 (0 marks an empty slot). The tag
 * holds the hash bits the bucket index does not use, so a bucket hit whose
 * full hash differs is rejected with a register compare instead of the random
 * (cache-missing) candidate load. It cannot change the parse: equal 8-byte
 * keys hash equally, so a tag mismatch implies different bytes, which the
 * byte compare would have rejected anyway. */
static inline uint32_t fast_lite_tag(uint32_t h, unsigned tagbits)
{
    return (uint32_t)(((h & ((1u << tagbits) - 1u)) << (8u - tagbits)) | 1u);
}

/* Sparse interior inserts for the lean core's accept path: a few positions at
 * fixed offsets, constant work per sequence instead of the O(best_len) walk in
 * fast_parse (which, at the fast rung, touched nearly as many positions as the
 * main loop did). Only the long table exists here, so every offset goes into
 * tl. Out of line so the pinned levels, which never call this, keep the exact
 * hot-path code shape they had before this option existed. */
__attribute__((noinline))
static void fast_insert_sparse_long(uint32_t *tl, const uint8_t *vb, size_t end,
                                    size_t ip, uint32_t best_len,
                                    unsigned lshift, unsigned tagbits)
{
    size_t ie = ip + (size_t)best_len;
    if (ip + 2u + 8u <= end) {
        size_t p = ip + 2u;
        uint32_t h = fast_hash_l_v(zgec_rd64(vb + p));
        tl[h >> lshift] = (fast_lite_tag(h, tagbits) << 24) | (uint32_t)p;
    }
    if (ie >= 2u && ie - 2u + 8u <= end) {
        size_t p = ie - 2u;
        uint32_t h = fast_hash_l_v(zgec_rd64(vb + p));
        tl[h >> lshift] = (fast_lite_tag(h, tagbits) << 24) | (uint32_t)p;
    }
    if (ie >= 1u && ie - 1u + 8u <= end) {
        size_t p = ie - 1u;
        uint32_t h = fast_hash_l_v(zgec_rd64(vb + p));
        tl[h >> lshift] = (fast_lite_tag(h, tagbits) << 24) | (uint32_t)p;
    }
}

/* Copy `ll` pending literals into the block's literal buffer.
 *
 * The literal buffer keeps ZGEC_LIT_SLACK spare bytes past the block and the
 * virtual buffer keeps 64 bytes past its end, so a short run can move a fixed
 * 8- or 16-byte block: the bytes past ll are overwritten by the next literal
 * copy (or by the tail copy) before the literal region is read, and both the
 * load and the store stay inside their allocations (anchor <= ip <= end - 8
 * in both fast loops). Most runs between matches are only a few bytes, and a
 * variable-length memcpy call cost more than the copy itself. The main/high
 * parser keeps the plain memcpy: its literal runs are longer and its per-match
 * work already dominates. */
static inline __attribute__((always_inline))
void parse_copy_lit(uint8_t *dst, const uint8_t *src, uint32_t ll)
{
    if (ll <= 8u) {
        memcpy(dst, src, 8u);
    } else if (ll <= 16u) {
        memcpy(dst, src, 16u);
    } else {
        memcpy(dst, src, ll);
    }
}

static zgec_err fast_parse(zgec_parse *p, const uint8_t *vb, size_t prefix, size_t vb_size,
                           int dial)
{
    uint32_t *ts;
    uint32_t *tl;
    zgec_sequence *seq;
    size_t seq_cap;
    size_t ip;
    size_t anchor;
    size_t end = vb_size;
    zgec_reps reps;
    size_t pos;
    /* Speed dial (fast_dial): 0 is the pinned default. 1..2 shrink the two
     * hash tables (extra_bits doubles the bucket-per-position stride), which
     * is the single biggest envelope win because the working set is
     * table-latency bound; 3 additionally drops the repeat-offset probes and
     * the short-table probe, the two optional per-position passes. Every
     * rung trades ratio for throughput and is only ever set by the new fast
     * levels 1-3. Dial 4 does not reach this function: zgec_parse_block_dial
     * routes it to fast_parse_lite() below. */
    unsigned extra_bits = (dial >= 2) ? 4u : ((dial >= 1) ? 2u : 0u);
    const int no_short = (dial >= 3);
    const int no_reps  = (dial >= 3);
    unsigned fbits = fast_table_bits(end - prefix, extra_bits);
    unsigned sshift = (unsigned)ZGEC_FAST_SHORT_BITS - fbits;
    unsigned lshift = (unsigned)ZGEC_FAST_LONG_BITS - fbits;
    uint32_t gen = 1u;      /* validity tag written into every entry */
#if ZGEC_FAST_PIPE
    size_t pipe_ip = (size_t)-1; /* position the carried candidate belongs to */
    uint32_t pipe_hl = 0u;
    uint32_t pipe_cl = 0u;
#endif

    int ts_owned = 0;
    if (fbits <= ZGEC_FAST_SCRATCH_BITS) {
        ts = fast_ts_scratch;
        tl = fast_tl_scratch;
    } else {
        ts = (uint32_t *)zgec_alloc(((size_t)1u << fbits) * sizeof(uint32_t), 64);
        tl = (uint32_t *)zgec_alloc(((size_t)1u << fbits) * sizeof(uint32_t), 64);
        ts_owned = 1;
    }
    seq_cap = (end - prefix) / 8u + 16u;
    seq = (zgec_sequence *)zgec_alloc(seq_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
    if (!ts || !tl || !seq) {
        if (ts_owned) { zgec_free(ts); zgec_free(tl); }
        zgec_free(seq);
        return ZGEC_ERR_NOMEM;
    }
#if ZGEC_FAST_GEN
    if (ts_owned) {
        /* A fresh allocation is not zeroed, so clear it and pin the tag. */
        memset(ts, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
        memset(tl, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
        gen = 1u;
    } else {
        /* Scratch tables: bump the tag, clearing only when it wraps. */
        gen = (fast_scratch_gen % 255u) + 1u;
        if (gen == 1u) {
            memset(ts, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
            memset(tl, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
        }
        fast_scratch_gen = gen;
    }
#else
    memset(ts, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
    memset(tl, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
    gen = 1u;
#endif

    /* Seed the tables with the dictionary / literal-reference prefix, sparsely. */
    for (pos = 0; pos + 8u <= prefix; pos += 4u) {
        uint64_t sv = zgec_rd64(vb + pos);
        ts[fast_hash_s_v(sv) >> sshift] = fast_pack(gen, pos);
        tl[fast_hash_l_v(sv) >> lshift] = fast_pack(gen, pos);
    }

    zgec_reps_init(&reps);
    ip = prefix;
    anchor = prefix;
    p->n_seq = 0;
    p->n_lit = 0;

    while (ip + 8u <= end) {
        uint32_t best_len = 0u;
        uint32_t best_off = 0u;
        uint64_t cur8 = zgec_rd64(vb + ip);
        (void)cur8;
        uint32_t hs = fast_hash_s_v(cur8) >> sshift;
        uint32_t hl;
        uint32_t cl;
#if ZGEC_FAST_PIPE
        if (pipe_ip == ip) {
            hl = pipe_hl;
            cl = pipe_cl;
        } else {
            hl = fast_hash_l_v(cur8) >> lshift;
            cl = tl[hl];
        }
#else
        hl = fast_hash_l_v(cur8) >> lshift;
        cl = tl[hl];
#endif
#if ZGEC_FAST_PREFETCH
        {
            /* Prefetch the buckets of the position the skip schedule visits
             * next if no match is taken here; the probes below then overlap
             * the miss instead of waiting for it. */
            size_t nxt = ip + (((ip - anchor) >> ZGEC_FAST_SHIFT) + 1u);
            if (nxt + 8u <= end) {
                uint64_t nv = zgec_rd64(vb + nxt);
                __builtin_prefetch(&ts[fast_hash_s_v(nv) >> sshift], 1, 3);
                __builtin_prefetch(&tl[fast_hash_l_v(nv) >> lshift], 1, 3);
            }
        }
#endif
        uint32_t cs;
        if (no_short) {
            /* A dead tag no live entry can carry, so every short-table probe
             * below short-circuits: fast_live(~0u, gen) is false for every
             * gen. Output is exactly the long-table-only parse. */
            cs = ~0u;
        } else {
            cs = ts[hs];
        }
        size_t cap = end - ip;
        size_t step = ((ip - anchor) >> ZGEC_FAST_SHIFT) + 1u;
#if ZGEC_FAST_PIPE
        {
            size_t pnext = ip + step;
            if (pnext + 8u <= end) {
                uint64_t nv = zgec_rd64(vb + pnext);
                pipe_hl = fast_hash_l_v(nv) >> lshift;
                pipe_cl = tl[pipe_hl];
                pipe_ip = pnext;
            } else {
                pipe_ip = (size_t)-1;
            }
        }
#endif

        /* 1. Repeat offsets rep0, rep1, rep2 (4-byte minimum). Probing the
         * whole chain costs two extra 4-byte compares on the common path and
         * recovers the ratio the repeat offsets carry on record, table and
         * source data, where a single long-distance match repeats its offset
         * many times. The move-to-front update below keeps the chain in step
         * with the decoder exactly as the main tier's matcher does.
         * Each probe first tests only the low 32 bits of the xor: a differing
         * byte among the first four already means the match cannot reach the
         * 4-byte repeat minimum, so the length scan and the update branches
         * run only when the probe can actually win. Output is unchanged. */
        if (!no_reps) {
        uint32_t r0 = reps.rep[0];
        uint32_t r1 = reps.rep[1];
        uint32_t r2 = reps.rep[2];

        if (r0 != 0u && (size_t)r0 <= ip) {
            if (best_len < 4u || (best_len < cap && vb[ip - r0 + best_len] == vb[ip + best_len])) {
                uint64_t diff = zgec_rd64(vb + ip - r0) ^ cur8;
                if ((uint32_t)diff == 0u) {
                    uint32_t len;
                    if (diff == 0u) {
                        len = 8u + fast_match_len_tail(vb + ip - r0 + 8u, vb + ip + 8u, cap - 8u);
                    } else {
                        len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_off = r0;
                    }
                }
            }
        }
        if (r1 != 0u && r1 != r0 && (size_t)r1 <= ip) {
            if (best_len < 4u || (best_len < cap && vb[ip - r1 + best_len] == vb[ip + best_len])) {
                uint64_t diff = zgec_rd64(vb + ip - r1) ^ cur8;
                if ((uint32_t)diff == 0u) {
                    uint32_t len;
                    if (diff == 0u) {
                        len = 8u + fast_match_len_tail(vb + ip - r1 + 8u, vb + ip + 8u, cap - 8u);
                    } else {
                        len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_off = r1;
                    }
                }
            }
        }
        if (r2 != 0u && r2 != r0 && r2 != r1 && (size_t)r2 <= ip) {
            if (best_len < 4u || (best_len < cap && vb[ip - r2 + best_len] == vb[ip + best_len])) {
                uint64_t diff = zgec_rd64(vb + ip - r2) ^ cur8;
                if ((uint32_t)diff == 0u) {
                    uint32_t len;
                    if (diff == 0u) {
                        len = 8u + fast_match_len_tail(vb + ip - r2 + 8u, vb + ip + 8u, cap - 8u);
                    } else {
                        len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                    }
                    if (len > best_len) {
                        best_len = len;
                        best_off = r2;
                    }
                }
            }
        }
        }

        /* 2 & 3. Table probes (short table cs and long table cl).
         * When cs == cl, both tables point to the same candidate position,
         * so a single combined probe evaluates both 5-byte short and 8-byte
         * long match criteria without re-reading memory or re-calculating tail lengths. */
        if (cs == cl && fast_live(cl, gen)) {
            if (fast_pos_of(cl) < ip) {
                size_t d = ip - fast_pos_of(cl);
                size_t check_idx_combo = (best_len >= ZGEC_FAST_MIN_SHORT) ? (size_t)best_len : (size_t)ZGEC_FAST_MIN_SHORT - 1u;
                if (d != (size_t)best_off && check_idx_combo < cap && vb[ip - d + check_idx_combo] == vb[ip + check_idx_combo]) {
                    uint64_t diff = zgec_rd64(vb + ip - d) ^ cur8;
                    if (diff == 0u) {
                        uint32_t len = 8u + fast_match_len_tail(vb + ip - d + 8u, vb + ip + 8u, cap - 8u);
                        if (len > best_len) {
                            best_len = len;
                            best_off = (uint32_t)d;
                        }
                    } else {
                        uint32_t len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                        if (len >= ZGEC_FAST_MIN_SHORT && len > best_len) {
                            best_len = len;
                            best_off = (uint32_t)d;
                        }
                    }
                }
            }
        } else {
            /* 2. Long table (8-byte hash, minimum length 8). */
            if (fast_live(cl, gen) && fast_pos_of(cl) < ip) {
                size_t d = ip - fast_pos_of(cl);
                if (d != (size_t)best_off) {
                    uint64_t diff = zgec_rd64(vb + ip - d) ^ cur8;
                    if (diff == 0u) {
                        if (best_len <= 8u || (best_len <= cap && vb[ip - d + best_len] == vb[ip + best_len])) {
                            uint32_t len = 8u + fast_match_len_tail(vb + ip - d + 8u, vb + ip + 8u, cap - 8u);
                            if (len > best_len) {
                                best_len = len;
                                best_off = (uint32_t)d;
                            }
                        }
                    }
                }
            }
            /* 3. Short table (5-byte hash, minimum length 5). */
            if (fast_live(cs, gen) && fast_pos_of(cs) < ip) {
                size_t d = ip - fast_pos_of(cs);
                size_t check_idx_s = (best_len >= ZGEC_FAST_MIN_SHORT) ? (size_t)best_len : (size_t)ZGEC_FAST_MIN_SHORT - 1u;
                if (d != (size_t)best_off && check_idx_s < cap && vb[ip - d + check_idx_s] == vb[ip + check_idx_s]) {
                    uint64_t diff = zgec_rd64(vb + ip - d) ^ cur8;
                    if (diff != 0u) {
                        uint32_t len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                        if (len >= ZGEC_FAST_MIN_SHORT && len > best_len) {
                            best_len = len;
                            best_off = (uint32_t)d;
                        }
                    } else {
                        uint32_t len = 8u + fast_match_len_tail(vb + ip - d + 8u, vb + ip + 8u, cap - 8u);
                        if (len > best_len) {
                            best_len = len;
                            best_off = (uint32_t)d;
                        }
                    }
                }
            }
        }
        /* Insert the visited position. */
        if (!no_short) {
            ts[hs] = fast_pack(gen, ip);
        }
        tl[hl] = fast_pack(gen, ip);
#if ZGEC_FAST_PIPE
        /* The lookahead read above may have taken this very bucket before the
         * store; the carried value must agree with a fresh load. */
        if (pipe_ip != (size_t)-1 && pipe_hl == hl) pipe_cl = fast_pack(gen, ip);
#endif

#if ZGEC_FAST_LAZY
        if (best_len >= 4u && best_len < ZGEC_FAST_LAZY_MAX && ip + 9u <= end) {
            /* One-position lazy check: if the next position has a clearly
             * longer match, emit this position as a literal and move on. */
            size_t p1 = ip + 1u;
            uint32_t l1 = 0u;
            uint64_t v1 = zgec_rd64(vb + p1);
            uint32_t c1 = tl[fast_hash_l_v(v1) >> lshift];
            if (fast_live(c1, gen) && fast_pos_of(c1) < p1) {
                size_t d1 = p1 - fast_pos_of(c1);
                uint64_t diff1 = zgec_rd64(vb + p1 - d1) ^ v1;
                if (diff1 == 0u) {
                    uint32_t len1 = 8u + fast_match_len_tail(vb + p1 - d1 + 8u, vb + p1 + 8u, end - p1 - 8u);
                    if (len1 >= ZGEC_FAST_MIN_LONG) l1 = len1;
                }
            }
#if !ZGEC_FAST_LAZY_LONG_ONLY
            c1 = ts[fast_hash_s_v(v1) >> sshift];
            if (fast_live(c1, gen) && fast_pos_of(c1) < p1) {
                size_t d1 = p1 - fast_pos_of(c1);
                uint64_t diff1 = zgec_rd64(vb + p1 - d1) ^ v1;
                if (diff1 != 0u) {
                    uint32_t len1 = (uint32_t)((unsigned)__builtin_ctzll(diff1) >> 3);
                    if (len1 >= ZGEC_FAST_MIN_SHORT && len1 > l1) l1 = len1;
                } else {
                    uint32_t len1 = 8u + fast_match_len_tail(vb + p1 - d1 + 8u, vb + p1 + 8u, end - p1 - 8u);
                    if (len1 >= ZGEC_FAST_MIN_SHORT && len1 > l1) l1 = len1;
                }
            }
#endif
            if (l1 > best_len + 1u) {
                ip = p1;
                continue;
            }
        }
#endif
        if (best_len >= 4u) {
#if ZGEC_FAST_BACKEXT
            /* Extend the match backwards into the pending literals while the
             * byte before the match still agrees, as zstd's dfast and lazy
             * accept paths do. The match end does not move, so the offset
             * stays valid and the sequence only sheds literals. */
            while (ip > anchor && (size_t)best_off < ip &&
                   vb[ip - 1u] == vb[ip - (size_t)best_off - 1u]) {
                ip--;
                best_len++;
            }
#endif
#if ZGEC_FAST_PIPE
            /* An accepted match jumps ip and rewrites the tables below, so the
             * carried candidate is no longer what a fresh load would return. */
            pipe_ip = (size_t)-1;
#endif
            uint32_t ll = (uint32_t)(ip - anchor);
            uint32_t ob = zgec_reps_encode(&reps, best_off);
            (void)zgec_reps_resolve(&reps, ob);
            if (ll > 0u) {
                parse_copy_lit(p->lit + p->n_lit, vb + anchor, ll);
                p->n_lit += ll;
            }
            if (p->n_seq >= seq_cap) {
                size_t nc = seq_cap * 2u;
                zgec_sequence *ns = (zgec_sequence *)zgec_alloc(nc * sizeof(zgec_sequence), _Alignof(zgec_sequence));
                if (!ns) {
                    zgec_free(ts); zgec_free(tl); zgec_free(seq);
                    return ZGEC_ERR_NOMEM;
                }
                memcpy(ns, seq, p->n_seq * sizeof(zgec_sequence));
                zgec_free(seq);
                seq = ns;
                seq_cap = nc;
            }
            seq[p->n_seq].ll = ll;
            seq[p->n_seq].ml = best_len;
            seq[p->n_seq].offbase = ob;
            p->n_seq++;
#if ZGEC_FAST_INS_ALL
            {
                size_t ins;
                size_t ins_end = ip + (size_t)best_len;
                if (ins_end + 8u > end) ins_end = (end >= 8u) ? end - 8u : 0u;
#if ZGEC_FAST_INS_SPAN > 0u
                if (ins_end > ip + (size_t)ZGEC_FAST_INS_SPAN)
                    ins_end = ip + (size_t)ZGEC_FAST_INS_SPAN;
#endif
                ins = ip + 1u;
                for (; ins + 2u <= ins_end; ins += 2u) {
                    uint64_t iv0 = zgec_rd64(vb + ins);
                    uint64_t iv1 = zgec_rd64(vb + ins + 1u);
                    uint32_t hs0 = fast_hash_s_v(iv0) >> sshift;
                    uint32_t hl0 = fast_hash_l_v(iv0) >> lshift;
                    uint32_t hs1 = fast_hash_s_v(iv1) >> sshift;
                    uint32_t hl1 = fast_hash_l_v(iv1) >> lshift;
                    ts[hs0] = fast_pack(gen, ins);
#if ZGEC_FAST_INS_LONG
                    tl[hl0] = fast_pack(gen, ins);
#endif
                    ts[hs1] = fast_pack(gen, ins + 1u);
#if ZGEC_FAST_INS_LONG
                    tl[hl1] = fast_pack(gen, ins + 1u);
#endif
                }
                for (; ins < ins_end; ins++) {
                    uint64_t iv = zgec_rd64(vb + ins);
                    ts[fast_hash_s_v(iv) >> sshift] = fast_pack(gen, ins);
#if ZGEC_FAST_INS_LONG
                    tl[fast_hash_l_v(iv) >> lshift] = fast_pack(gen, ins);
#endif
                }
                ins = ip + (size_t)best_len;
            }
#else
            /* Sparse inserts inside the match, mirroring zstd's dfast accept
             * path: four positions at fixed offsets, constant work per
             * sequence instead of the O(best_len) walk above. The position two
             * past the match start goes into both tables, and the last two
             * positions into the table whose key can still reach them. */
            {
                size_t i2 = ip + 2u;
                size_t ie = ip + (size_t)best_len;
                if (i2 + 8u <= end) {
                    uint64_t iv = zgec_rd64(vb + i2);
                    ts[fast_hash_s_v(iv) >> sshift] = fast_pack(gen, i2);
                    tl[fast_hash_l_v(iv) >> lshift] = fast_pack(gen, i2);
                }
                if (ie >= 2u && ie - 2u + 8u <= end) {
                    size_t i1 = ie - 2u;
                    uint64_t iv = zgec_rd64(vb + i1);
                    tl[fast_hash_l_v(iv) >> lshift] = fast_pack(gen, i1);
                }
                if (ie >= 1u && ie - 1u + 8u <= end) {
                    size_t i1 = ie - 1u;
                    uint64_t iv = zgec_rd64(vb + i1);
                    ts[fast_hash_s_v(iv) >> sshift] = fast_pack(gen, i1);
                }
            }
#endif
            ip += best_len;
            anchor = ip;
        } else {
            step = ((ip - anchor) >> ZGEC_FAST_SHIFT) + 1u;
            ip += step;
        }
    }

    if (anchor < end) {
        size_t tail = end - anchor;
        memcpy(p->lit + p->n_lit, vb + anchor, tail);
        p->n_lit += tail;
    }

    zgec_free(p->seq);
    p->seq = seq;
    if (ts_owned) {
        zgec_free(ts);
        zgec_free(tl);
    }
    return ZGEC_OK;
}

/* Level-1 core: one long table, no repeat-offset probes, no short table, no
 * lazy check, sparse interior inserts. This is the from-scratch single-table
 * rung: it keeps only the mandatory per-position body (one hash, one table
 * load, one store) plus a candidate probe, dropping the two optional passes
 * and the short/long fan-out that fast_parse carries for the pinned levels.
 * Measured -15% encode time for +1.9% size against the dial-3 rung it
 * replaces as level 1, so the fast ladder's top step is this core and
 * fast_parse itself stays untouched for the pinned levels (its byte stream
 * and throughput do not move). */
static zgec_err fast_parse_lite(zgec_parse *p, const uint8_t *vb, size_t prefix,
                                size_t vb_size)
{
    uint32_t *tl;
    zgec_sequence *seq;
    size_t seq_cap;
    size_t ip;
    size_t anchor;
    size_t end = vb_size;
    zgec_reps reps;
    size_t pos;
    unsigned fbits = fast_table_bits(end - prefix, 4u);
    unsigned lshift = (unsigned)ZGEC_FAST_LONG_BITS - fbits;
    unsigned tagbits = (lshift < 8u) ? lshift : 8u;
    int tl_owned = 0;

    if (fbits <= ZGEC_FAST_SCRATCH_BITS) {
        tl = fast_tl_scratch;
    } else {
        tl = (uint32_t *)zgec_alloc(((size_t)1u << fbits) * sizeof(uint32_t), 64);
        tl_owned = 1;
    }
    seq_cap = (end - prefix) / 8u + 16u;
    seq = (zgec_sequence *)zgec_alloc(seq_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
    if (!tl || !seq) {
        if (tl_owned) zgec_free(tl);
        zgec_free(seq);
        return ZGEC_ERR_NOMEM;
    }
    memset(tl, 0, ((size_t)1u << fbits) * sizeof(uint32_t));

    /* Seed with the dictionary / literal-reference prefix, sparsely. */
    for (pos = 0; pos + 8u <= prefix; pos += 4u) {
        uint32_t h = fast_hash_l_v(zgec_rd64(vb + pos));
        tl[h >> lshift] = (fast_lite_tag(h, tagbits) << 24) | (uint32_t)pos;
    }

    zgec_reps_init(&reps);
    ip = prefix;
    anchor = prefix;
    p->n_seq = 0;
    p->n_lit = 0;

    while (ip + 8u <= end) {
        uint64_t cur8 = zgec_rd64(vb + ip);
        uint32_t h = fast_hash_l_v(cur8);
        uint32_t hl = h >> lshift;
        uint32_t tag = fast_lite_tag(h, tagbits);
        uint32_t e = tl[hl];
        tl[hl] = (tag << 24) | (uint32_t)ip;

        uint32_t best_len = 0u;
        uint32_t best_off = 0u;
        size_t cap = end - ip;
        /* The tag covers the hash bits the bucket index does not, so a hit
         * whose tag differs cannot be an 8-byte match: skip the candidate
         * load. On a tag match the full word is still compared, which also
         * absorbs the rare same-bucket hash collision. */
        if (e != 0u && (e >> 24) == tag) {
            size_t cpos = (size_t)(e & 0xFFFFFFu);
            if (cpos < ip) {
                size_t d = ip - cpos;
                if (zgec_rd64(vb + ip - d) == cur8) {
                    best_len = 8u + fast_match_len_out(vb + ip - d + 8u, vb + ip + 8u, cap - 8u);
                    best_off = (uint32_t)d;
                }
            }
        }

        if (best_len >= ZGEC_FAST_MIN_LONG) {
            /* Back-extend into the pending literals; the match end does not
             * move, so the offset stays valid and only literals are shed. */
            while (ip > anchor && (size_t)best_off < ip &&
                   vb[ip - 1u] == vb[ip - (size_t)best_off - 1u]) {
                ip--;
                best_len++;
            }
            uint32_t ll = (uint32_t)(ip - anchor);
            uint32_t ob = zgec_reps_encode(&reps, best_off);
            (void)zgec_reps_resolve(&reps, ob);
            if (ll > 0u) {
                parse_copy_lit(p->lit + p->n_lit, vb + anchor, ll);
                p->n_lit += ll;
            }
            if (p->n_seq >= seq_cap) {
                size_t nc = seq_cap * 2u;
                zgec_sequence *ns = (zgec_sequence *)zgec_alloc(nc * sizeof(zgec_sequence), _Alignof(zgec_sequence));
                if (!ns) {
                    if (tl_owned) zgec_free(tl);
                    zgec_free(seq);
                    return ZGEC_ERR_NOMEM;
                }
                memcpy(ns, seq, p->n_seq * sizeof(zgec_sequence));
                zgec_free(seq);
                seq = ns;
                seq_cap = nc;
            }
            seq[p->n_seq].ll = ll;
            seq[p->n_seq].ml = best_len;
            seq[p->n_seq].offbase = ob;
            p->n_seq++;
            fast_insert_sparse_long(tl, vb, end, ip, best_len, lshift, tagbits);
            ip += best_len;
            anchor = ip;
        } else {
            ip += ((ip - anchor) >> ZGEC_FAST_SHIFT) + 1u;
        }
    }

    if (anchor < end) {
        size_t tail = end - anchor;
        memcpy(p->lit + p->n_lit, vb + anchor, tail);
        p->n_lit += tail;
    }

    zgec_free(p->seq);
    p->seq = seq;
    if (tl_owned) zgec_free(tl);
    return ZGEC_OK;
}
#endif /* ZGEC_FASTDFAST_OFF */

zgec_err zgec_parse_block_dial(zgec_parse **out,
                          const uint8_t *src, size_t raw_size,
                          const uint8_t *dict, size_t dict_size,
                          const uint8_t *litref, size_t litref_size,
                          zgec_tier tier, double lambda, int fast_dial, int lazy_max)
{
    if (!out) return ZGEC_ERR_INVAL;
    /* Never leave a stale pointer behind on the failure paths below. */
    *out = NULL;
    if (fast_dial < 0) fast_dial = 0;
    else if (fast_dial > 4) fast_dial = 4;
    if ((!src && raw_size > 0)) return ZGEC_ERR_INVAL;
    /* A size without a pointer would leave the [dict) region of the
     * virtual buffer below uninitialised and exposed to the matcher, so
     * reject the pair instead of copying nothing into it. */
    if (!dict && dict_size > 0) return ZGEC_ERR_INVAL;
    if ((!litref && litref_size > 0)) return ZGEC_ERR_INVAL;
    /* raw_size is recorded in 32 bits, so a larger input would be
     * truncated into every allocation and bounds decision that uses it.
     * Guarded on SIZE_MAX because on a 32-bit target size_t cannot exceed
     * UINT32_MAX, which makes the comparison always false and trips gcc's
     * -Wtype-limits in this -Werror build. No cast in the #if: the
     * preprocessor evaluates type names as 0. */
#if SIZE_MAX > UINT32_MAX
    if (raw_size > (size_t)UINT32_MAX) return ZGEC_ERR_INVAL;
#endif

    /* Lambda is the speed/ratio dial (section 11.7):
     * 0 maximises ratio, larger values raise the
     * price-gate bar slightly towards fewer, longer
     * matches (faster decode). Clamped small so the
     * effect stays a slight threshold scale. */
    /* A non-finite lambda survives the clamp below unchanged, because
     * every comparison against NaN is false: lscale and every score
     * would be NaN, the "cur_score > 0.0" gate would reject every match,
     * and the block would come out as pure literals. Fall back to the
     * ratio endpoint rather than silently disabling match emission. */
    if (!isfinite(lambda)) lambda = 0.0;
    double lam = (lambda < 0.0) ? 0.0 : ((lambda > 4.0) ? 4.0 : lambda);
    float lscale = 1.0f + 0.25f * (float)lam;
    uint32_t lazy_max_cap = (lazy_max > 0) ? (uint32_t)lazy_max : PARSE_LAZY_MAX_LEN;

    /* Sequence capacity from the input rather than a flat 65536 records:
     * at 12 bytes a record that is ~768 KiB, allocated even for a 64-byte
     * block. The shortest match is 4 bytes, so raw_size/16 is a safe
     * under-estimate that the growth path below extends on demand. */
    size_t seq_cap = raw_size / 16u + 16u;
    if (seq_cap < 256u) seq_cap = 256u;
    if (seq_cap > 65536u) seq_cap = 65536u;
    if (parse_mul_overflows(seq_cap, sizeof(zgec_sequence)))
        return ZGEC_ERR_NOMEM;

    zgec_parse *p = (zgec_parse *)zgec_alloc(sizeof(*p), _Alignof(zgec_parse));
    if (!p) return ZGEC_ERR_NOMEM;
    memset(p, 0, sizeof(*p));

    /* p->seq allocation is deferred past the ZGEC_TIER_FAST check because
     * fast_parse allocates its own seq array and would immediately free p->seq. */

    /* Checked before allocating: a wrapped size allocates short, and the
     * literal copies below then run past its end. */
    if (zgec_add_overflows(raw_size, (size_t)ZGEC_LIT_SLACK + 64u)) {
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }
    p->lit = (uint8_t *)zgec_alloc(raw_size + (size_t)ZGEC_LIT_SLACK + 64u, 64);
    if (!p->lit) { zgec_parse_free(p); return ZGEC_ERR_NOMEM; }
    p->raw_size = (uint32_t)raw_size;
    p->n_seq = 0;
    p->n_lit = 0;

    if (raw_size == 0) {
        *out = p;
        return ZGEC_OK;
    }

    /* Virtual buffer [dict][litref][src] (section 6.1). Every size here
     * is checked before use: an unchecked sum can wrap to a small value,
     * pass the P24 guard below, and be used as a memcpy length. */
    if (zgec_add_overflows(dict_size, litref_size)) {
        zgec_parse_free(p);
        return ZGEC_ERR_INVAL;
    }
    size_t prefix = dict_size + litref_size;
    if (prefix > (size_t)(1u << 24)) {
        zgec_parse_free(p);
        return ZGEC_ERR_INVAL;
    }
    if (zgec_add_overflows(prefix, raw_size)) {
        zgec_parse_free(p);
        return ZGEC_ERR_INVAL;
    }
    size_t vb_size = prefix + raw_size;
    /* P24 position limit (section 6.1): the caller contract is that the whole
     * virtual buffer -- dict + litref + src -- fits inside 2^24 positions,
     * because every offset the parse emits is a position in it. The check on
     * prefix above only bounds the two dictionary regions, so on its own it
     * lets vb_size exceed P24 by up to raw_size; bound the full extent. */
    if (vb_size > (size_t)1 << 24) {
        zgec_parse_free(p);
        return ZGEC_ERR_INVAL;
    }
    uint8_t *vb;
    zgec_matcher *m;
    if (zgec_add_overflows(vb_size, 64u)) {
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }
    vb = (uint8_t *)zgec_alloc(vb_size + 64u, 64);
    if (!vb) { zgec_parse_free(p); return ZGEC_ERR_NOMEM; }
    if (dict && dict_size > 0) {
        memcpy(vb, dict, dict_size);
    }
    if (litref && litref_size > 0) {
        memcpy(vb + dict_size, litref, litref_size);
    }
    memcpy(vb + prefix, src, raw_size);

#ifndef ZGEC_FASTDFAST_OFF
    if (tier == ZGEC_TIER_FAST) {
        zgec_err fe = (fast_dial >= 4)
            ? fast_parse_lite(p, vb, prefix, vb_size)
            : fast_parse(p, vb, prefix, vb_size, fast_dial);
        zgec_free(vb);
        if (fe != ZGEC_OK) { zgec_parse_free(p); return fe; }
        *out = p;
        return ZGEC_OK;
    }
#endif

    p->seq = (zgec_sequence *)zgec_alloc(seq_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
    if (!p->seq) { zgec_free(vb); zgec_parse_free(p); return ZGEC_ERR_NOMEM; }

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
    /* Per-code log2(count + 1): the per-symbol half of a sequence cost.
     * A histogram bin moves by one per accepted sequence, so only that
     * bin changes here; see parse_score_fast. This subsumes the throttled
     * cost-table refresh main used for the same reason: with no
     * materialised table there is nothing to throttle, and a cost is
     * derived from the live total and bin at query time, which is the
     * value the un-throttled table held. */
    float ll_base[ZGEC_NSYM_SEQ];
    float ml_base[ZGEC_NSYM_SEQ];
    float of_base[ZGEC_NSYM_SEQ];
    uint32_t ll_tot = 0, ml_tot = 0, of_tot = 0;   /* running code totals */
    float l2tot[3];           /* log2(total + 66) for LL/ML/OF */
    size_t lit_total = 0;      /* literals tallied into lit_hist */
    size_t lit_eval = 0;       /* lit_total at the last lbar estimate */
    size_t lit_step = 256u;    /* literals before the next estimate */
    float lbar = 6.0f;
    unsigned shift = parse_skip_shift(tier);
    memset(ll_hist, 0, sizeof(ll_hist));
    memset(ml_hist, 0, sizeof(ml_hist));
    memset(of_hist, 0, sizeof(of_hist));
    memset(lit_hist, 0, sizeof(lit_hist));
    /* Every bin starts at count 0, so log2(count + 1) is log2(1) = 0 for
     * all 66 codes, which is what the old initial cost table held. */
    memset(ll_base, 0, sizeof(ll_base));
    memset(ml_base, 0, sizeof(ml_base));
    memset(of_base, 0, sizeof(of_base));
    l2tot[0] = zgec_fast_log2_u32_f(ZGEC_NSYM_SEQ);
    l2tot[1] = l2tot[0];
    l2tot[2] = l2tot[0];

    size_t ip = prefix;
    size_t end = prefix + raw_size;
    size_t anchor = ip;
    int has_deferred_match = 0;
    zgec_match deferred_match;
    uint32_t deferred_rep_len[3] = { 0u, 0u, 0u };
    deferred_match.offset = 0;
    deferred_match.length = 0;

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
        float cur_score = 0.0f;
        uint32_t cur_len = 0;
        uint32_t cur_off = 0;
        zgec_match match;

        /* Prefetch the upcoming input line; the
         * finder prefetches the hash bucket lines of
         * each visited position internally. */
        if (ip + (size_t)64 <= end) {
            __builtin_prefetch((const void *)(vb + ip + (size_t)64), 0, 3);
        }
#if ZGEC_PARSE_PREFETCH
        {
            /* Hide table latency: prefetch the buckets of the position the
             * skip schedule visits next (and ip + 1 for the lazy probe)
             * before spending a whole find on ip. */
            size_t nstep = ((ip - anchor) >> shift) + (size_t)1;
            if (nstep > (size_t)PARSE_MAX_STEP) nstep = (size_t)PARSE_MAX_STEP;
            zgec_matcher_prefetch(m, vb, ip + nstep);
            if (nstep != (size_t)1) zgec_matcher_prefetch(m, vb, ip + (size_t)1);
        }
#endif
        uint32_t rep_len[3];
        if (has_deferred_match) {
            match = deferred_match;
            rep_len[0] = deferred_rep_len[0];
            rep_len[1] = deferred_rep_len[1];
            rep_len[2] = deferred_rep_len[2];
            has_deferred_match = 0;
        } else {
            match = zgec_matcher_find_lens(m, vb, ip, reps.rep[0], reps.rep[1],
                                          reps.rep[2], 4u, (uint32_t)(end - ip),
                                          rep_len);
        }
        ll = (uint32_t)(ip - anchor);
        float ll_cost = parse_ll_cost(ll, l2tot[0], ll_base);
        if (match.length >= 4u) {
            is_rep = (match.offset == reps.rep[0] || match.offset == reps.rep[1] ||
                      match.offset == reps.rep[2]) ? 1u : 0u;
            need = (is_rep != 0u) ? 4u : parse_min_norep(ip, tier);
            if (match.length >= need) {
                offbase = parse_offbase(match.offset, &reps);
                cur_score = parse_score_fast(match.length, ll_cost, offbase,
                                             l2tot, ml_base, of_base,
                                             lbar, lscale);
                if (cur_score > 0.0f) {
                    cur_len = match.length;
                    cur_off = match.offset;
                }
            }
        }
        /* Evaluate repeat offset candidates (offbase 1, 2, 3) directly.
         * zgec_matcher_find selects candidates solely based on match length,
         * discarding repeat offsets when a non-repeat match is 1 byte longer.
         * However, repeat offsets carry 0 extra bits for offset coding, so
         * a repeat match can yield a significantly higher parse_score (net bit
         * savings) than a longer non-repeat match with a large offset.
         *
         * The lengths are the ones find's rep scan already computed and
         * returned in rep_len[], probed with the same reps and the same
         * cap at this exact ip, so re-scanning them here would repeat the
         * most expensive half of that work: a slot is 0 when the rep was
         * not probeable there or the first four bytes differed, both of
         * which cannot clear the rlen >= 4 gate below. */
        {
            size_t cap = end - ip;
            unsigned nreps = (tier == ZGEC_TIER_FAST) ? 1u : 3u;
            for (unsigned ri = 0u; ri < nreps; ri++) {
                uint32_t roff = reps.rep[ri];
                if (roff != 0u && (size_t)roff <= ip && (size_t)roff <= (1u << 24)) {
                    if (cap >= 4u && rep_len[ri] >= 4u) {
                        /* A repeat's score is rlen * lbar minus a
                         * non-negative sequence cost, so it can never
                         * exceed rlen * lbar: when that does not clear
                         * cur_score the rep cannot win and the score is
                         * skipped rather than computed and discarded.
                         * The test is exact, so the choice is unchanged. */
                        if ((float)rep_len[ri] * lbar > cur_score) {
                            uint32_t rlen = rep_len[ri];
                            float rscore = parse_score_rep_fast(rlen, ll_cost, ri,
                                                                l2tot, ml_base, of_base,
                                                                lbar, lscale);
                            if (rscore > cur_score) {
                                cur_score = rscore;
                                cur_len = rlen;
                                cur_off = roff;
                            }
                        }
                    }
                }
            }
        }

        /* Lazy evaluation at ip + 1, driven by the
         * same price score and selected with a
         * conditional move on the score. */
        if (lazy_max_cap > 0u && cur_len > 0u &&
            cur_len <= lazy_max_cap &&
            tier >= ZGEC_TIER_MAIN && ip + (size_t)5 <= end) {
            uint32_t min_lazy_len = (uint32_t)(cur_score / lbar) + 1u;
            if (min_lazy_len < 4u) min_lazy_len = 4u;
            uint32_t nrep_len[3] = { 0u, 0u, 0u };
            zgec_match nm = zgec_matcher_find_lens(m, vb, ip + (size_t)1,
                                                   reps.rep[0], reps.rep[1], reps.rep[2],
                                                   min_lazy_len,
                                                   (uint32_t)(end - (ip + (size_t)1)),
                                                   nrep_len);
            uint32_t nll = (uint32_t)((ip + (size_t)1) - anchor);
            uint32_t nis_rep = (nm.length >= 4u &&
                                (nm.offset == reps.rep[0] || nm.offset == reps.rep[1] ||
                                 nm.offset == reps.rep[2])) ? 1u : 0u;
            uint32_t nneed = (nis_rep != 0u) ? 4u : parse_min_norep(ip + (size_t)1, tier);
            float base = cur_score;
            float nscore = -1.0f;
            uint32_t defer;
            /* The lazy probe has to beat `base` to be worth deferring to,
             * and a candidate's score never exceeds its length priced as
             * literals, so a length that does not clear base ends the
             * comparison before any score is computed. Exact either way:
             * the branch below only runs when cur_len > 0, and only then
             * is base > 0, so the -1.0 sentinel loses the same comparison
             * the real score would. */
            float nll_cost = parse_ll_cost(nll, l2tot[0], ll_base);
            if (nm.length >= nneed && (float)nm.length * lbar > base) {
                uint32_t noffbase = parse_offbase(nm.offset, &reps);
                nscore = parse_score_fast(nm.length, nll_cost, noffbase,
                                          l2tot, ml_base, of_base,
                                          lbar, lscale);
            }
            {
                size_t nip = ip + 1u;
                size_t ncap = end - nip;
                unsigned nreps = (tier == ZGEC_TIER_FAST) ? 1u : 3u;
                for (unsigned ri = 0u; ri < nreps; ri++) {
                    uint32_t roff = reps.rep[ri];
                    if (roff != 0u && (size_t)roff <= nip && (size_t)roff <= (1u << 24)) {
                        if (ncap >= 4u && nrep_len[ri] >= 4u) {
                            if ((float)nrep_len[ri] * lbar > base) {
                                uint32_t rlen = nrep_len[ri];
                                float rscore = parse_score_rep_fast(rlen, nll_cost, ri,
                                                                    l2tot, ml_base, of_base,
                                                                    lbar, lscale);
                                if (rscore > nscore) {
                                    nscore = rscore;
                                }
                            }
                        }
                    }
                }
            }
            /* cmov-style: the score comparison feeds a
             * 0/1 select, not a length branch. */
            defer = (nscore > base) ? 1u : 0u;
            if (defer != 0u) {
                zgec_matcher_insert(m, vb, ip);
                has_deferred_match = 1;
                deferred_match = nm;
                deferred_rep_len[0] = nrep_len[0];
                deferred_rep_len[1] = nrep_len[1];
                deferred_rep_len[2] = nrep_len[2];
                ip += (size_t)1;
                continue;
            }
        }

        if (cur_len > 0u) {
            uint32_t ml = cur_len;
            uint32_t off = cur_off;
            uint8_t nb = 0;
            /* Shared MTF update (section 8.2): encode then resolve applies
             * the same move-to-front step the decoder performs. */
            uint32_t ob = zgec_reps_encode(&reps, off);
            (void)zgec_reps_resolve(&reps, ob);
#if ZGEC_PARSE_PREFETCH
            /* Issue the table prefetches for the insert samples and for the next
             * position now; the sequence bookkeeping below is independent
             * work that overlaps the misses. */
            zgec_matcher_prefetch_match(m, vb, ip, ml);
            zgec_matcher_prefetch(m, vb, ip + (size_t)ml);
#endif

            if (ll > 0) {
                uint32_t u;
                for (u = 0; u < ll; u++) lit_hist[vb[anchor + (size_t)u]]++;
                memcpy(p->lit + p->n_lit, vb + anchor, ll);
                p->n_lit += ll;
                lit_total += ll;
            }

            if (p->n_seq >= seq_cap) {
                size_t new_cap;
                zgec_sequence *new_seq;
                /* The doubling and the byte size are both checked: a
                 * wrapped capacity allocates short and the writes past it
                 * corrupt the heap. sizeof(zgec_sequence) > 1, so the
                 * product test also rules the doubling itself out. */
                if (parse_mul_overflows(seq_cap, 2u * sizeof(zgec_sequence))) {
                    zgec_matcher_destroy(m);
                    zgec_free(vb);
                    zgec_parse_free(p);
                    return ZGEC_ERR_NOMEM;
                }
                new_cap = seq_cap * 2u;
                new_seq = (zgec_sequence *)zgec_alloc(new_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
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
             * running statistics for the price gate.
             * Two numbers move per accepted sequence -- one histogram bin
             * in each of the three tables and the three totals -- so that
             * is all this updates; parse_score_fast derives every cost from
             * these. */
            {
                uint8_t c_ll_acc = zgec_seq_code_of(ll, &nb);
                uint8_t c_ml_acc = zgec_seq_code_of(ml >= 3u ? ml - 3u : 0u, &nb);
                uint8_t c_of_acc = zgec_seq_code_of(ob >= 1u ? ob - 1u : 0u, &nb);
                ll_hist[c_ll_acc]++;
                ml_hist[c_ml_acc]++;
                of_hist[c_of_acc]++;
                ll_tot++;
                ml_tot++;
                of_tot++;
                l2tot[0] = zgec_fast_log2_u32_f(ll_tot + (uint32_t)ZGEC_NSYM_SEQ);
                l2tot[1] = zgec_fast_log2_u32_f(ml_tot + (uint32_t)ZGEC_NSYM_SEQ);
                l2tot[2] = zgec_fast_log2_u32_f(of_tot + (uint32_t)ZGEC_NSYM_SEQ);
                ll_base[c_ll_acc] = zgec_fast_log2_u32_f(ll_hist[c_ll_acc] + 1u);
                ml_base[c_ml_acc] = zgec_fast_log2_u32_f(ml_hist[c_ml_acc] + 1u);
                of_base[c_of_acc] = zgec_fast_log2_u32_f(of_hist[c_of_acc] + 1u);
            }
            /* Re-estimate Lbar when the literal count has grown by a
             * growing margin. parse_lbar walks all 256 symbols and takes
             * two logarithms of each, so a fixed 256-literal step
             * recomputes it thousands of times per block for a statistic
             * that moves smoothly: on a 1 MiB block of text that was over
             * 4000 estimates, and the estimate itself changed the parse
             * no more than the geometric schedule below does. */
            if (lit_total >= lit_eval + lit_step) {
                lbar = parse_lbar(lit_hist, lit_total);
                lit_eval = lit_total;
                lit_step = lit_total >> 4;
                if (lit_step < 256u) lit_step = 256u;
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

/* Public form (section 6.1): the pinned fast parser, byte-identical to every
 * level the ladder shipped with. New levels 1-3 call zgec_parse_block_dial
 * with a non-zero dial instead. */
zgec_err zgec_parse_block_ex(zgec_parse **out,
                          const uint8_t *src, size_t raw_size,
                          const uint8_t *dict, size_t dict_size,
                          const uint8_t *litref, size_t litref_size,
                          zgec_tier tier, double lambda)
{
    return zgec_parse_block_dial(out, src, raw_size, dict, dict_size,
                                 litref, litref_size, tier, lambda, 0, 0);
}
