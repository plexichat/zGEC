#include <stdint.h>
#if defined(__AVX2__)
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
    for (int s = 0; s < ZGEC_NSYM_SEQ; s++) total += (double)hist[s];
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

/* Fixed fallback minimum non-repeat length for the
 * first implementation: 5, 6 beyond 256 KiB
 * (section 11.4). Repeat offsets keep minimum 4. */
static uint32_t parse_min_norep(size_t ip, zgec_tier tier)
{
    if (tier >= ZGEC_TIER_HIGH) return 5u;
    return (ip >= (size_t)262144) ? 6u : 5u;
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
static double parse_seq_cost(uint32_t ll, uint32_t ml, uint32_t offbase,
                              const double *l2tot,
                              const double *ll_base,
                              const double *ml_base,
                              const double *of_base)
{
    uint8_t nb_ll = 0, nb_ml = 0, nb_of = 0;
    uint8_t c_ll = zgec_seq_code_of(ll, &nb_ll);
    uint8_t c_ml = zgec_seq_code_of(ml >= 3 ? ml - 3 : 0, &nb_ml);
    uint8_t c_of = zgec_seq_code_of(offbase >= 1 ? offbase - 1 : 0, &nb_of);
    double cost = (double)(float)(l2tot[0] - ll_base[c_ll]) + (double)nb_ll;
    cost += (double)(float)(l2tot[1] - ml_base[c_ml]) + (double)nb_ml;
    cost += (double)(float)(l2tot[2] - of_base[c_of]) + (double)nb_of;
    return cost;
}

/* Price score of a candidate (section 11.4):
 * score = len * Lbar - cost(LL, ML, OF) * lscale.
 * Positive means the match saves bits over coding its len bytes as
 * literals. */
static double parse_score(uint32_t len, uint32_t ll, uint32_t offbase,
                          const double *l2tot,
                          const double *ll_base, const double *ml_base,
                          const double *of_base,
                          double lbar, double lscale)
{
    double cost = parse_seq_cost(ll, len, offbase, l2tot, ll_base, ml_base,
                                 of_base);
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
    /* The prior counts as probability mass, so the smoothed total t is
     * the denominator: bits accumulates c * (log2(t) - log2(c)) =
     * t*log2(t) - sum(c*log2(c)), and dividing by t gives the average
     * cost of one literal under the smoothed model (dividing by the
     * unsmoothed total would scale it up by t/total).
     *
     * log2, not log: this value is compared against sequence costs
     * measured in bits -- docs/spec.md:853, "Lbar is the running average
     * cost in bits of a literal". Natural logarithms understate it by
     * the log(2) factor, ~30%, which makes the price gate reject matches
     * that would have paid for themselves. */
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
#define ZGEC_FAST_SHIFT 5u
#endif

#ifndef ZGEC_FAST_SHORT_HASH_BYTES
#define ZGEC_FAST_SHORT_HASH_BYTES 5u
#endif
#ifndef ZGEC_FAST_INS_ALL
#define ZGEC_FAST_INS_ALL 1
#endif
#ifndef ZGEC_FAST_INS_STRIDE
#define ZGEC_FAST_INS_STRIDE 1
#endif
#ifndef ZGEC_FAST_INS_LONG
#define ZGEC_FAST_INS_LONG 1
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
#ifndef ZGEC_FAST_BITS_MIN
#define ZGEC_FAST_BITS_MIN 10u
#endif
/* Buckets wanted per indexed position, as a shift: 2 means one bucket per
 * four positions. */
#ifndef ZGEC_FAST_BITS_SHIFT
#define ZGEC_FAST_BITS_SHIFT 3u
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
static unsigned fast_table_bits(size_t positions)
{
    unsigned bits = ZGEC_FAST_SHORT_BITS;
    size_t want = positions >> ZGEC_FAST_BITS_SHIFT;
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



static inline uint32_t fast_match_len_tail(const uint8_t *a, const uint8_t *b, size_t cap)
{
    size_t len = 0;
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
    while (len + 8u <= cap) {
        uint64_t x = zgec_rd64(a + len) ^ zgec_rd64(b + len);
        if (x != 0u) {
            return (uint32_t)(len + ((unsigned)__builtin_ctzll(x) >> 3));
        }
        len += 8u;
    }
    while (len < cap && a[len] == b[len]) {
        len++;
    }
    return (uint32_t)len;
}

static zgec_err fast_parse(zgec_parse *p, const uint8_t *vb, size_t prefix, size_t vb_size)
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
    unsigned fbits = fast_table_bits(end - prefix);
    unsigned sshift = (unsigned)ZGEC_FAST_SHORT_BITS - fbits;
    unsigned lshift = (unsigned)ZGEC_FAST_LONG_BITS - fbits;

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
    memset(ts, 0, ((size_t)1u << fbits) * sizeof(uint32_t));
    memset(tl, 0, ((size_t)1u << fbits) * sizeof(uint32_t));

    /* Seed the tables with the dictionary / literal-reference prefix, sparsely. */
    for (pos = 0; pos + 8u <= prefix; pos += 4u) {
        uint64_t sv = zgec_rd64(vb + pos);
        ts[fast_hash_s_v(sv) >> sshift] = (uint32_t)pos + 1u;
        tl[fast_hash_l_v(sv) >> lshift] = (uint32_t)pos + 1u;
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
        uint32_t hl = fast_hash_l_v(cur8) >> lshift;
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
        uint32_t cs = ts[hs];
        uint32_t cl = tl[hl];
        size_t cap = end - ip;
        size_t step;

        /* 1. Repeat offsets rep0, rep1, rep2 (4-byte minimum). Probing the
         * whole chain costs two extra 4-byte compares on the common path and
         * recovers the ratio the repeat offsets carry on record, table and
         * source data, where a single long-distance match repeats its offset
         * many times. The move-to-front update below keeps the chain in step
         * with the decoder exactly as the main tier's matcher does. */
        {
            uint32_t ri;
            for (ri = 0; ri < 3u; ri++) {
                uint32_t r = reps.rep[ri];
                if (r == 0u || (size_t)r > ip) continue;
                uint64_t diff = zgec_rd64(vb + ip - r) ^ cur8;
                uint32_t len;
                if (diff != 0u) {
                    len = (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
                } else {
                    len = 8u + fast_match_len_tail(vb + ip - r + 8u, vb + ip + 8u, cap - 8u);
                }
                if (len >= 4u && len > best_len) {
                    best_len = len;
                    best_off = r;
                }
            }
        }
        /* 2. Long table (8-byte hash, minimum length 8). */
        if (cl != 0u && (size_t)(cl - 1u) < ip) {
            size_t d = ip - (size_t)(cl - 1u);
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
        /* 3. Short table (5-byte hash, minimum length 5). */
        if (cs != 0u && (size_t)(cs - 1u) < ip) {
            size_t d = ip - (size_t)(cs - 1u);
            if (best_len == 0u || (best_len <= cap && vb[ip - d + best_len] == vb[ip + best_len])) {
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
        /* Insert the visited position. */
        ts[hs] = (uint32_t)ip + 1u;
        tl[hl] = (uint32_t)ip + 1u;

#if ZGEC_FAST_LAZY
        if (best_len >= 4u && best_len < ZGEC_FAST_LAZY_MAX && ip + 9u <= end) {
            /* One-position lazy check: if the next position has a clearly
             * longer match, emit this position as a literal and move on. */
            size_t p1 = ip + 1u;
            uint32_t l1 = 0u;
            uint64_t v1 = zgec_rd64(vb + p1);
            uint32_t c1 = tl[fast_hash_l_v(v1) >> lshift];
            if (c1 != 0u && (size_t)(c1 - 1u) < p1) {
                size_t d1 = p1 - (size_t)(c1 - 1u);
                uint64_t diff1 = zgec_rd64(vb + p1 - d1) ^ v1;
                if (diff1 == 0u) {
                    uint32_t len1 = 8u + fast_match_len_tail(vb + p1 - d1 + 8u, vb + p1 + 8u, end - p1 - 8u);
                    if (len1 >= ZGEC_FAST_MIN_LONG) l1 = len1;
                }
            }
            c1 = ts[fast_hash_s_v(v1) >> sshift];
            if (c1 != 0u && (size_t)(c1 - 1u) < p1) {
                size_t d1 = p1 - (size_t)(c1 - 1u);
                uint64_t diff1 = zgec_rd64(vb + p1 - d1) ^ v1;
                if (diff1 != 0u) {
                    uint32_t len1 = (uint32_t)((unsigned)__builtin_ctzll(diff1) >> 3);
                    if (len1 >= ZGEC_FAST_MIN_SHORT && len1 > l1) l1 = len1;
                } else {
                    uint32_t len1 = 8u + fast_match_len_tail(vb + p1 - d1 + 8u, vb + p1 + 8u, end - p1 - 8u);
                    if (len1 >= ZGEC_FAST_MIN_SHORT && len1 > l1) l1 = len1;
                }
            }
            if (l1 > best_len + 1u) {
                ip = p1;
                continue;
            }
        }
#endif
        if (best_len >= 4u) {
            uint32_t ll = (uint32_t)(ip - anchor);
            uint32_t ob = zgec_reps_encode(&reps, best_off);
            size_t ins;
            (void)zgec_reps_resolve(&reps, ob);
            if (ll > 0u) {
                memcpy(p->lit + p->n_lit, vb + anchor, ll);
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
                size_t ins_end = ip + (size_t)best_len;
                if (ins_end + 8u > end) ins_end = (end >= 8u) ? end - 8u : 0u;
                ins = ip + 1u;
                for (; ins + 2u <= ins_end; ins += 2u) {
                    uint64_t iv0 = zgec_rd64(vb + ins);
                    uint64_t iv1 = zgec_rd64(vb + ins + 1u);
                    uint32_t hs0 = fast_hash_s_v(iv0) >> sshift;
                    uint32_t hl0 = fast_hash_l_v(iv0) >> lshift;
                    uint32_t hs1 = fast_hash_s_v(iv1) >> sshift;
                    uint32_t hl1 = fast_hash_l_v(iv1) >> lshift;
                    ts[hs0] = (uint32_t)ins + 1u;
#if ZGEC_FAST_INS_LONG
                    tl[hl0] = (uint32_t)ins + 1u;
#endif
                    ts[hs1] = (uint32_t)ins + 2u;
#if ZGEC_FAST_INS_LONG
                    tl[hl1] = (uint32_t)ins + 2u;
#endif
                }
                for (; ins < ins_end; ins++) {
                    uint64_t iv = zgec_rd64(vb + ins);
                    ts[fast_hash_s_v(iv) >> sshift] = (uint32_t)ins + 1u;
#if ZGEC_FAST_INS_LONG
                    tl[fast_hash_l_v(iv) >> lshift] = (uint32_t)ins + 1u;
#endif
                }
                ins = ip + (size_t)best_len;
            }
#else
            /* Sparse inserts inside the match: second and last position. */
            ins = ip + 1u;
            if (ins + 8u <= end) {
                uint64_t iv = zgec_rd64(vb + ins);
                ts[fast_hash_s_v(iv) >> sshift] = (uint32_t)ins + 1u;
                tl[fast_hash_l_v(iv) >> lshift] = (uint32_t)ins + 1u;
            }
            ins = ip + (size_t)best_len - 1u;
            if (ins + 8u <= end) {
                uint64_t iv = zgec_rd64(vb + ins);
                ts[fast_hash_s_v(iv) >> sshift] = (uint32_t)ins + 1u;
                tl[fast_hash_l_v(iv) >> lshift] = (uint32_t)ins + 1u;
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
#endif /* ZGEC_FASTDFAST_OFF */

zgec_err zgec_parse_block_ex(zgec_parse **out,
                             const uint8_t *src, size_t raw_size,
                             const uint8_t *dict, size_t dict_size,
                             const uint8_t *litref, size_t litref_size,
                             zgec_tier tier, double lambda)
{
    if (!out || !src) return ZGEC_ERR_INVAL;
    if (raw_size == 0) {
        zgec_parse *p = (zgec_parse *)zgec_alloc(sizeof(*p), _Alignof(zgec_parse));
        if (!p) return ZGEC_ERR_NOMEM;
        memset(p, 0, sizeof(*p));
        *out = p;
        return ZGEC_OK;
    }

    size_t prefix_size = dict_size + litref_size;
    if (zgec_add_overflows(prefix_size, raw_size)) return ZGEC_ERR_INVAL;
    size_t vb_size = prefix_size + raw_size;

    /* P24 limit check: dict_size + litref_size + raw_size <= 2^24 */
    if (vb_size > ((size_t)1 << 24)) return ZGEC_ERR_INVAL;

    uint8_t *vb = (uint8_t *)zgec_alloc(vb_size ? vb_size : 1, 64);
    zgec_parse *p = (zgec_parse *)zgec_alloc(sizeof(*p), _Alignof(zgec_parse));
    if (!vb || !p) {
        zgec_free(vb);
        zgec_free(p);
        return ZGEC_ERR_NOMEM;
    }
    memset(p, 0, sizeof(*p));
    p->raw_size = (uint32_t)raw_size;

    p->lit = (uint8_t *)zgec_alloc(raw_size, 64);
    size_t seq_cap = (raw_size / 8u) + 16u;
    if (parse_mul_overflows(seq_cap, sizeof(zgec_sequence))) {
        zgec_free(vb);
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }
    p->seq = (zgec_sequence *)zgec_alloc(seq_cap * sizeof(zgec_sequence), _Alignof(zgec_sequence));
    if (!p->lit || !p->seq) {
        zgec_free(vb);
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }

    if (dict && dict_size > 0) memcpy(vb, dict, dict_size);
    if (litref && litref_size > 0) memcpy(vb + dict_size, litref, litref_size);
    memcpy(vb + prefix_size, src, raw_size);

#ifndef ZGEC_FASTDFAST_OFF
    if (tier == ZGEC_TIER_FAST) {
        zgec_err err = fast_parse(p, vb, prefix_size, vb_size);
        zgec_free(vb);
        if (err != ZGEC_OK) {
            zgec_parse_free(p);
            return err;
        }
        *out = p;
        return ZGEC_OK;
    }
#endif

    /* Main / High tier match finding and parsing */
    zgec_matcher *m = zgec_matcher_create(tier, vb_size);
    if (!m) {
        zgec_free(vb);
        zgec_parse_free(p);
        return ZGEC_ERR_NOMEM;
    }
    zgec_matcher_reset(m, vb, vb_size, dict, dict_size);
    if (litref_size > 0) {
        size_t stride = (tier == ZGEC_TIER_HIGH) ? 1u : 2u;
        for (size_t pos = dict_size; pos < prefix_size; pos += stride) {
            zgec_matcher_insert(m, vb, pos);
        }
    }

    zgec_reps reps;
    zgec_reps_init(&reps);

    uint32_t ll_hist[ZGEC_NSYM_SEQ] = {0};
    uint32_t ml_hist[ZGEC_NSYM_SEQ] = {0};
    uint32_t of_hist[ZGEC_NSYM_SEQ] = {0};
    uint32_t lit_hist[256] = {0};
    size_t lit_total = 0;

    double ll_base[ZGEC_NSYM_SEQ];
    double ml_base[ZGEC_NSYM_SEQ];
    double of_base[ZGEC_NSYM_SEQ];
    double l2tot[3];

    for (int s = 0; s < ZGEC_NSYM_SEQ; s++) {
        ll_base[s] = log2(1.0);
        ml_base[s] = log2(1.0);
        of_base[s] = log2(1.0);
    }
    l2tot[0] = log2((double)ZGEC_NSYM_SEQ);
    l2tot[1] = log2((double)ZGEC_NSYM_SEQ);
    l2tot[2] = log2((double)ZGEC_NSYM_SEQ);

    double lscale = 1.0;
    if (lambda > 0.0) {
        lscale = 1.0 + lambda;
    }

    size_t ip = prefix_size;
    size_t anchor = prefix_size;
    size_t end = vb_size;

    while (ip + 4u <= end) {
        uint32_t rep_len[3] = {0};
        uint32_t min_len = parse_min_norep(ip - prefix_size, tier);
        uint32_t max_len = (uint32_t)(end - ip);

        zgec_match m0 = zgec_matcher_find_lens(m, vb, ip, reps.rep[0], reps.rep[1], reps.rep[2], min_len, max_len, rep_len);

        uint32_t best_len = m0.length;
        uint32_t best_off = m0.offset;
        uint32_t best_ob = (best_len >= 3) ? parse_offbase(best_off, &reps) : 0;
        uint32_t ll0 = (uint32_t)(ip - anchor);

        double lbar0 = parse_lbar(lit_hist, lit_total);
        double s0 = (best_len >= 3) ? parse_score(best_len, ll0, best_ob, l2tot, ll_base, ml_base, of_base, lbar0, lscale) : -1.0;

        /* Evaluate repeat offset price scores */
        for (int ri = 0; ri < 3; ri++) {
            uint32_t rlen = rep_len[ri];
            if (rlen >= 4) {
                uint32_t rob = (uint32_t)(ri + 1);
                double rs = parse_score(rlen, ll0, rob, l2tot, ll_base, ml_base, of_base, lbar0, lscale);
                if (rs > s0) {
                    s0 = rs;
                    best_len = rlen;
                    best_off = reps.rep[ri];
                    best_ob = rob;
                }
            }
        }

        /* Price gate threshold: score > 0 */
        if (s0 > 0.0 && best_len >= 3) {
            /* 1-position lazy lookahead check */
            if (best_len < PARSE_LAZY_MAX_LEN && ip + 1u + 4u <= end) {
                size_t p1 = ip + 1u;
                uint32_t max_len1 = (uint32_t)(end - p1);
                uint32_t rep_len1[3] = {0};
                zgec_match m1 = zgec_matcher_find_lens(m, vb, p1, reps.rep[0], reps.rep[1], reps.rep[2], min_len, max_len1, rep_len1);
                uint32_t l1_len = m1.length;
                uint32_t l1_off = m1.offset;
                uint32_t l1_ob = (l1_len >= 3) ? parse_offbase(l1_off, &reps) : 0;
                uint32_t ll1 = ll0 + 1u;
                double s1 = (l1_len >= 3) ? parse_score(l1_len, ll1, l1_ob, l2tot, ll_base, ml_base, of_base, lbar0, lscale) : -1.0;

                for (int ri = 0; ri < 3; ri++) {
                    uint32_t rlen1 = rep_len1[ri];
                    if (rlen1 >= 4) {
                        uint32_t rob1 = (uint32_t)(ri + 1);
                        double rs1 = parse_score(rlen1, ll1, rob1, l2tot, ll_base, ml_base, of_base, lbar0, lscale);
                        if (rs1 > s1) {
                            s1 = rs1;
                            l1_len = rlen1;
                            l1_off = reps.rep[ri];
                            l1_ob = rob1;
                        }
                    }
                }

                if (s1 > s0 + lbar0) {
                    /* Lazy step: emit current position byte as literal and step */
                    lit_hist[vb[ip]]++;
                    lit_total++;
                    zgec_matcher_insert(m, vb, ip);
                    ip++;
                    continue;
                }
            }

            /* Accept match `best_len` at `best_off` */
            uint32_t ll = (uint32_t)(ip - anchor);
            if (ll > 0) {
                memcpy(p->lit + p->n_lit, vb + anchor, ll);
                for (size_t k = 0; k < ll; k++) {
                    lit_hist[vb[anchor + k]]++;
                }
                lit_total += ll;
                p->n_lit += ll;
            }

            if (p->n_seq >= seq_cap) {
                size_t nc = seq_cap * 2u;
                zgec_sequence *ns = (zgec_sequence *)zgec_alloc(nc * sizeof(zgec_sequence), _Alignof(zgec_sequence));
                if (!ns) {
                    zgec_matcher_destroy(m);
                    zgec_free(vb);
                    zgec_parse_free(p);
                    return ZGEC_ERR_NOMEM;
                }
                memcpy(ns, p->seq, p->n_seq * sizeof(zgec_sequence));
                zgec_free(p->seq);
                p->seq = ns;
                seq_cap = nc;
            }

            p->seq[p->n_seq].ll = ll;
            p->seq[p->n_seq].ml = best_len;
            p->seq[p->n_seq].offbase = best_ob;
            p->n_seq++;

            /* Update histograms and running costs */
            uint8_t nb = 0;
            uint8_t c_ll = zgec_seq_code_of(ll, &nb);
            uint8_t c_ml = zgec_seq_code_of(best_len - 3u, &nb);
            uint8_t c_ob = zgec_seq_code_of(best_ob >= 1 ? best_ob - 1u : 0u, &nb);

            ll_hist[c_ll]++;
            ml_hist[c_ml]++;
            of_hist[c_ob]++;

            double tot_ll = 0.0, tot_ml = 0.0, tot_of = 0.0;
            for (int s = 0; s < ZGEC_NSYM_SEQ; s++) {
                tot_ll += (double)ll_hist[s];
                tot_ml += (double)ml_hist[s];
                tot_of += (double)of_hist[s];
            }
            ll_base[c_ll] = log2((double)ll_hist[c_ll] + 1.0);
            ml_base[c_ml] = log2((double)ml_hist[c_ml] + 1.0);
            of_base[c_ob] = log2((double)of_hist[c_ob] + 1.0);
            l2tot[0] = log2(tot_ll + (double)ZGEC_NSYM_SEQ);
            l2tot[1] = log2(tot_ml + (double)ZGEC_NSYM_SEQ);
            l2tot[2] = log2(tot_of + (double)ZGEC_NSYM_SEQ);

            zgec_reps_resolve(&reps, best_ob);
            zgec_matcher_insert_match(m, vb, ip, best_len);

            ip += best_len;
            anchor = ip;
        } else {
            /* Skip schedule */
            lit_hist[vb[ip]]++;
            lit_total++;
            zgec_matcher_insert(m, vb, ip);

            unsigned shift = parse_skip_shift(tier);
            size_t step = ((ip - anchor) >> shift) + 1u;
            if (step > PARSE_MAX_STEP) step = PARSE_MAX_STEP;
            ip += step;
        }
    }

    if (anchor < end) {
        size_t tail = end - anchor;
        memcpy(p->lit + p->n_lit, vb + anchor, tail);
        p->n_lit += tail;
    }

    zgec_matcher_destroy(m);
    zgec_free(vb);

    *out = p;
    return ZGEC_OK;
}
