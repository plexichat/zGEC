/*
 * Match finder per zGEC section 11.2 (informative).
 *
 * Bucket hash-chain match finder over a virtual
 * buffer. Three tiers select the bucket width and
 * the table size:
 *
 *   tier   short buckets   lanes   short size   long size   per-thread total
 *   fast   2^16            4       1 MiB        256 KiB     1.3125 MiB
 *   main   2^16            8       2 MiB        1 MiB       3.0625 MiB
 *   high   2^18            16      16 MiB       1 MiB       17.25 MiB
 *
 * A per-thread total counts the short table, the per-bucket ring heads
 * (2^16 B for fast/main, 2^18 B for high) and the long table, ignoring
 * allocator metadata and alignment waste; the earlier "2.25 MiB" figure
 * for main was written before the main long table moved to 2^18 buckets
 * and is 3.0625 MiB. Table sizes are the section 11.2 starting point
 * (2^16 buckets for fast/main, 2^18 for high, and measurement decides);
 * the main long table sits at 2^18 rather than the spec's 2^16 because
 * halving it costs ratio (see the geometry note under the macros).
 * Per-thread cap note: with one worker per core the per-thread total
 * SHOULD stay near 1-1.5 MiB so the sum across threads does not exceed
 * the shared L3; only the fast tier meets that budget (1.3125 MiB), the
 * main tier (3.0625 MiB) already exceeds it and the high tier
 * (17.25 MiB) far exceeds it, so a threaded build SHOULD shard workers,
 * use fewer match threads than cores, or fall back a tier.
 *
 * Two separate structures (section 11.2):
 *   short table: hashes 5 bytes (4 bytes for data
 *     classified as binary), nlanes entries per
 *     bucket, newest first.
 *   long table: hashes 8 bytes, one entry per
 *     bucket (overwrite newest).
 * Probe order: rep0, rep1 (main/high; fast checks
 * rep0 only), long table, short table, plus deeper
 * short hits for the high tier.
 *
 * Each entry packs a 24-bit virtual-buffer position
 * and an 8-bit tag from the spare hash bits. The
 * position is stored as pos + 1, so an all-zero
 * entry is unambiguously empty. The 24-bit field
 * (profile P24) makes three different quantities
 * easy to confuse, and the code below distinguishes
 * them explicitly:
 *   - the largest virtual-buffer byte count a
 *     matcher may be created for is ZGEC_MF_POS_LIMIT
 *     = 2^24 = 16 MiB;
 *   - the largest storable position is 2^24 - 2 =
 *     16,777,214, because the stored value pos + 1
 *     must fit 24 bits, and pos = 2^24 - 1 would wrap
 *     onto the empty sentinel;
 *   - the largest match offset is not a property of
 *     the field at all: it is bounded by the position
 *     of the query (off <= ip) and by vb_capacity.
 * zgec_matcher_create rejects a virtual buffer
 * capacity above 2^24 (16 MiB), and mf_insert_pos
 * drops positions at or above the storable limit
 * rather than packing them.
 *
 * Tables and buffers are allocated 2 MiB aligned
 * with zgec_alloc as the huge-page intent
 * (section 11.2); with 4 KiB pages the 2048-entry
 * L2 TLB covers only 8 MiB.
 */

#include "zgec_encode.h" /* zgec_tier */
#include "zgec_match.h"

#include <string.h>
#include <assert.h>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

/* Entry layout: 24-bit position (stored as pos + 1)
 * and an 8-bit tag from the spare hash bits (P24). */
#define ZGEC_MF_POS_BITS  24u
#define ZGEC_MF_POS_LIMIT (1u << ZGEC_MF_POS_BITS)
#define ZGEC_MF_POS_MASK  (ZGEC_MF_POS_LIMIT - 1u)
#define ZGEC_MF_TAG_SHIFT 24u
#define ZGEC_MF_TAG_MASK  0xFFu

/* Hash widths: short covers 5 bytes (4 for binary
 * data), long covers 8 bytes (section 11.2). */
#define ZGEC_MF_HASH_BYTES 5u
#define ZGEC_MF_HASH_BYTES_BIN 4u
#define ZGEC_MF_LONG_BYTES 8u

/* Table geometry (section 11.2: "begin with 2^16 entries for fast and
 * main, and 2^18 for high, and let measurement decide"). Measurement on
 * a 15.6 MB source tree says the short table's footprint, not its
 * retention, is what costs time: a 2^17-bucket main short table is
 * 4 MiB per worker, so every probe is an L3 round trip. Dropping it to
 * 2^16 buckets costs 0.2% of ratio and buys ~7% of encode wall time
 * (the probe count is unchanged, the miss latency is not), and a later
 * sweep on four corpora (14.9 MiB and 141 MiB source trees, a 20 MiB
 * text tar and an 86 MiB binary tree) moved the main tier down one more
 * step: 2^15 buckets, a 1 MiB table, costs 0.3-0.4% of ratio and buys
 * 12-15% of encode throughput, while 2^14 costs 0.6-0.9% for another
 * 10%, which is past where the ratio is worth trading. The long table
 * keeps 2^18 buckets: at 2^14 short, halving the long one to 2^17 was
 * measured to buy about as much throughput as the short-table step from
 * 2^15 to 2^14 but to cost more ratio than it. The short table hashes
 * 5 bytes into a bucket of `lanes` packed entries; the long table hashes
 * 8 bytes into one entry per bucket. Overridable so a build can
 * re-measure. */
#ifndef ZGEC_MF_FAST_BUCKETS
#define ZGEC_MF_FAST_BUCKETS (1u << 16)
#endif
#ifndef ZGEC_MF_MAIN_BUCKETS
#define ZGEC_MF_MAIN_BUCKETS (1u << 15)
#endif
#ifndef ZGEC_MF_HIGH_BUCKETS
#define ZGEC_MF_HIGH_BUCKETS (1u << 18)
#endif
#ifndef ZGEC_MF_FAST_LONG_BUCKETS
#define ZGEC_MF_FAST_LONG_BUCKETS (1u << 16)
#endif
#ifndef ZGEC_MF_MAIN_LONG_BUCKETS
#define ZGEC_MF_MAIN_LONG_BUCKETS (1u << 18)
#endif
#ifndef ZGEC_MF_HIGH_LONG_BUCKETS
#define ZGEC_MF_HIGH_LONG_BUCKETS (1u << 18)
#endif
#ifndef ZGEC_MF_FAST_LANES
#define ZGEC_MF_FAST_LANES   4u
#endif
#ifndef ZGEC_MF_MAIN_LANES
#define ZGEC_MF_MAIN_LANES   8u
#endif
#ifndef ZGEC_MF_HIGH_LANES
#define ZGEC_MF_HIGH_LANES   16u
#endif

/* mf_bucket_hits() packs one hit bit per lane into a uint32_t, so no
 * tier may declare more lanes than this. */
#define ZGEC_MF_MAX_LANES 32u

/* Probes: how many tag hits of a short bucket are scored per find.
 *
 * Section 11.2 gives the fast tier two hits and the high tier "deeper";
 * for main it only names the candidates (rep0, rep1, long, short), and
 * 11.3 step 4 makes each probe a random memory access into the virtual
 * buffer, not a compare. Measurement on a source tree, where 62% of the
 * lanes of a probed bucket carry the queried tag (the corpus repeats
 * 5-byte fragments constantly, so a tag hit is a real candidate and not
 * noise), says the depth is where the ratio is: scoring the whole 8-lane
 * bucket instead of the two newest lanes is worth 3.2% of ratio for 25%
 * of the probe count. Main therefore scores the bucket; fast keeps the
 * spec's two hits. */
#ifndef ZGEC_MF_PROBE_DEPTH_FAST
#define ZGEC_MF_PROBE_DEPTH_FAST 2u
#endif
#ifndef ZGEC_MF_PROBE_DEPTH
#define ZGEC_MF_PROBE_DEPTH 8u
#endif
#ifndef ZGEC_MF_PROBE_DEPTH_HIGH
#define ZGEC_MF_PROBE_DEPTH_HIGH 8u
#endif

/* Positions sampled inside a match for the fast and main tiers.
 * Section 11.2: "Inside a match insert 2-3 sampled positions (fast and
 * main tiers); insert every position only in the high tier". Each sample
 * is two hashes and two random table stores. The spec's 2-3 is a sizeable
 * ratio sacrifice on a source tree, because the matches are short (about
 * seven bytes) so few samples already cover a whole match: measured at a
 * fixed 2^16-bucket main table, dropping the main count from 16 to 3
 * costs 7% of ratio and buys 12% of encode time. The dial stays at the
 * measured-better value; compile with -DZGEC_MF_MATCH_SAMPLES=3 to take
 * the spec's faster end. */
#ifndef ZGEC_MF_MATCH_SAMPLES
#define ZGEC_MF_MATCH_SAMPLES 16u
#endif
#ifndef ZGEC_MF_MATCH_SAMPLES_FAST
#define ZGEC_MF_MATCH_SAMPLES_FAST 3u
#endif

/* Minimum non-repeat match is 5; 4 is permitted for
 * repeat offsets (section 11.2). */
#define ZGEC_MF_MIN_HASH_MATCH 5u

/* The high tier inserts every position inside a match
 * until the match would need more samples than this. */
#define ZGEC_MF_HIGH_SAMPLES 2048u

/* The matcher struct stays 64-byte aligned; tables use the same
 * 64B cache-line alignment (huge-page intent removed: malloc never
 * provides MAP_HUGETLB, so 2MiB alignment only wasted ~1MiB/table). */
#define ZGEC_MF_ALIGN 64
#define ZGEC_MF_TABLE_ALIGN 64

struct zgec_matcher {
    zgec_tier      tier;
    uint32_t       nbuckets;      /* short buckets */
    uint32_t       nlanes;
    uint32_t       long_buckets;  /* long buckets (one entry each) */
    uint32_t      *short_tab;   /* nbuckets * nlanes entries */
    uint8_t       *short_head;  /* per-bucket next write lane (ring) */
    uint32_t      *long_tab;    /* long_buckets entries */
    int            is_binary;   /* short hash uses 4 bytes */
    /* Invariant: an attached buffer always satisfies vb_size <= vb_capacity.
     * zgec_matcher_create rejects capacity > 2^24 and reset refuses to attach
     * when vb_size > vb_capacity, so every offset d <= ip < vb_size also
     * satisfies d <= vb_capacity. Per-candidate capacity tests in find are
     * therefore dead in-tree; find keeps one hoisted validation plus asserts
     * and a single explicit check on the long-table path against an
     * adversarially mutated matcher. */
    size_t         vb_capacity; /* maximum referenceable offset */
    const uint8_t *vb;
    size_t         vb_size;
    size_t         c_ip[2];     /* hash cache: positions the cached hashes belong to */
    uint32_t       c_hs[2], c_hl[2]; /* cached short / long hashes */
    uint32_t       c_flags[2];  /* bit0: c_hs valid, bit1: c_hl valid */
};

static inline uint32_t mf_hash5_v(uint64_t v)
{
    uint32_t h = (uint32_t)v;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= (uint32_t)(v >> 8);
    h ^= h >> 13;
    return h;
}

static inline uint32_t mf_hash4_v(uint32_t v)
{
    uint32_t h = v;
    h ^= h >> 16;
    h *= 0x9E3779B1u;
    h ^= h >> 13;
    return h;
}

static inline uint32_t mf_hash8_v(uint64_t v)
{
    uint64_t x = v + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return (uint32_t)(x ^ (x >> 32));
}

/* Multiplicative hash of the 5 bytes at ip
 * (section 11.2). The caller must guarantee
 * ip + 5 <= vb_size. */
static uint32_t mf_hash5(const uint8_t *vb, size_t ip)
{
    uint32_t h = zgec_rd32(vb + ip);
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= zgec_rd32(vb + ip + 1);
    h ^= h >> 13;
    return h;
}

/* 4-byte variant for data classified as binary. */
static uint32_t mf_hash4(const uint8_t *vb, size_t ip)
{
    uint32_t h = zgec_rd32(vb + ip);
    h ^= h >> 16;
    h *= 0x9E3779B1u;
    h ^= h >> 13;
    return h;
}

/* 8-byte hash for the long table (one entry per
 * bucket). Xorshift/multiply/xorshift: the final shift xors the high
 * bits down, so the returned low 32 bits mix all eight input bytes.
 * The caller must guarantee ip + 8 <= vb_size. */
static uint32_t mf_hash8(const uint8_t *vb, size_t ip)
{
    uint64_t x = (uint64_t)zgec_rd64(vb + ip);
    x ^= x >> 33;
    x *= 0xD6E8FEB86659FD93ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

/* Short hash selected by the binary classification. */
static uint32_t mf_hash_short(const zgec_matcher *m, const uint8_t *vb, size_t ip)
{
    if (m->is_binary) {
        return mf_hash4(vb, ip);
    }
    return mf_hash5(vb, ip);
}

/* Classify the buffer as binary when more than ~30%
 * of a 4 KiB sample falls outside printable text
 * (section 11.2: 4-byte short hash for binary). */
static int mf_classify_binary(const uint8_t *vb, size_t vb_size)
{
    size_t sample = (vb_size < (size_t)4096) ? vb_size : (size_t)4096;
    size_t nontext = 0;
    size_t i;
    if (vb == NULL || sample == 0) {
        return 0;
    }
    for (i = 0; i < sample; i++) {
        uint8_t b = vb[i];
        if ((b < (uint8_t)32 && b != (uint8_t)9 && b != (uint8_t)10 && b != (uint8_t)13) ||
            b > (uint8_t)126) {
            nontext++;
        }
    }
    return (nontext * (size_t)10 > sample * (size_t)3) ? 1 : 0;
}

/* Pack a position and a tag into one 32-bit entry.
 * The position is stored as pos + 1 so that a zero
 * entry is the empty sentinel. Positions at or above
 * 2^24 - 1 cannot be packed distinctly and are
 * rejected by the caller. */
static uint32_t mf_pack(uint32_t pos, uint32_t tag)
{
    return ((pos + 1u) & ZGEC_MF_POS_MASK) | ((tag & ZGEC_MF_TAG_MASK) << ZGEC_MF_TAG_SHIFT);
}

/* Unpack the position from a non-empty entry. */
static uint32_t mf_pos(uint32_t entry)
{
    return (entry & ZGEC_MF_POS_MASK) - 1u;
}


/* Issue the hint a step ahead of the lookup, so the write-allocate fetch
 * overlaps the position's own work (section 11.3 step 1). */
static void mf_prefetch_bucket(const void *line)
{
    __builtin_prefetch(line, 0, 3);
}

/* Insert pos into the short bucket. The lanes are a ring: the next
 * write slot is short_head[bucket], so an insert is two stores rather
 * than an nlanes-wide shift. Lookup walks the ring newest first. */
static void mf_insert_short(zgec_matcher *m, uint32_t bucket, uint32_t entry)
{
    uint32_t *b = &m->short_tab[(size_t)bucket * (size_t)m->nlanes];
    uint32_t h = (uint32_t)m->short_head[bucket];
    /* No prefetch here. The store below needs the line immediately, so the
     * write-allocate fetch has to complete first and a hint issued one
     * instruction ahead cannot hide the latency; the lookup path keeps its
     * prefetch, where there is independent work to overlap. */
    b[h] = entry;
    m->short_head[bucket] = (uint8_t)((h + 1u) & (m->nlanes - 1u));
}

/* Insert pos into the long table: one entry per
 * bucket, newest overwrites. */
static void mf_insert_long(zgec_matcher *m, uint32_t bucket, uint32_t entry)
{
    m->long_tab[(size_t)bucket] = entry;
}

static void mf_insert_pos(zgec_matcher *m, const uint8_t *vb, size_t pos, int long_ok)
{
    uint32_t h;
    uint32_t bucket;
    uint32_t tag;

    if (pos >= (size_t)ZGEC_MF_POS_LIMIT - 1u) {
        return; /* position does not fit the 24-bit field */
    }
    if (m->is_binary) {
        if (pos + (size_t)ZGEC_MF_HASH_BYTES_BIN > m->vb_size) {
            return;
        }
    } else {
        if (pos + (size_t)ZGEC_MF_HASH_BYTES > m->vb_size) {
            return;
        }
    }
    /* The finder at this position has already computed both hashes; when it
     * hands the same position to insert, reuse them instead of recomputing.
     * Two parity-indexed slots keep the main probe at ip and the lazy probe
     * at ip + 1 live at once, so the speculative find does not evict the
     * hashes the insert below needs. */
    h = (m->c_ip[pos & 1u] == pos && (m->c_flags[pos & 1u] & 1u)) ? m->c_hs[pos & 1u] : mf_hash_short(m, vb, pos);
    bucket = h & (m->nbuckets - 1u);
    tag = (h >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
    mf_insert_short(m, bucket, mf_pack((uint32_t)pos, tag));

    if (long_ok && pos + (size_t)ZGEC_MF_LONG_BYTES <= m->vb_size) {
        uint32_t hl = (m->c_ip[pos & 1u] == pos && (m->c_flags[pos & 1u] & 2u)) ? m->c_hl[pos & 1u] : mf_hash8(vb, pos);
        uint32_t lb = hl & (m->long_buckets - 1u);
        uint32_t ltag = (hl >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        mf_insert_long(m, lb, mf_pack((uint32_t)pos, ltag));
    }
}

/* First-8-byte length via 8-byte XOR plus trailing
 * zero byte count (tzcnt >> 3, section 11.3 step 4).
 * Returns 8 when the first 8 bytes match. */
static inline __attribute__((always_inline)) uint32_t mf_len8(const uint8_t *src, const uint8_t *dst)
{
    uint64_t diff = (uint64_t)zgec_rd64(src) ^ (uint64_t)zgec_rd64(dst);
    if (diff == (uint64_t)0) {
        return 8u;
    }
    return (uint32_t)(((unsigned)__builtin_ctzll((unsigned long long)diff)) >> 3);
}

/* Length of the match at ip with offset d, capped at
 * cap. The caller guarantees d <= ip and
 * cap <= vb_size - ip, so neither side reads past the
 * virtual buffer. */
static uint32_t mf_match_len_slow(const uint8_t *src, const uint8_t *dst, uint32_t cap)
{
    uint32_t len = 8u;
#if defined(__AVX2__)
    /* Extend beyond the first 8 bytes 32 at a time; a 32-byte
     * movemask finds the first differing byte directly. */
    while (len + 32u <= cap) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(src + len));
        __m256i b32 = _mm256_loadu_si256((const __m256i *)(const void *)(dst + len));
        unsigned m32 = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, b32));
        if (m32 != 0xFFFFFFFFu) {
            return len + (uint32_t)__builtin_ctz(~m32);
        }
        len += 32u;
    }
#else
    /* Extend beyond 8 bytes with wide compares in
     * a loop that normally runs once. */
    while (len + 32u <= cap) {
        uint64_t d0 = (uint64_t)zgec_rd64(src + (size_t)len) ^
                      (uint64_t)zgec_rd64(dst + (size_t)len);
        if (d0 != 0u) return len + ((uint32_t)__builtin_ctzll(d0) >> 3);
        uint64_t d1 = (uint64_t)zgec_rd64(src + (size_t)len + 8u) ^
                      (uint64_t)zgec_rd64(dst + (size_t)len + 8u);
        if (d1 != 0u) return len + 8u + ((uint32_t)__builtin_ctzll(d1) >> 3);
        uint64_t d2 = (uint64_t)zgec_rd64(src + (size_t)len + 16u) ^
                      (uint64_t)zgec_rd64(dst + (size_t)len + 16u);
        if (d2 != 0u) return len + 16u + ((uint32_t)__builtin_ctzll(d2) >> 3);
        uint64_t d3 = (uint64_t)zgec_rd64(src + (size_t)len + 24u) ^
                      (uint64_t)zgec_rd64(dst + (size_t)len + 24u);
        if (d3 != 0u) return len + 24u + ((uint32_t)__builtin_ctzll(d3) >> 3);
        len += 32u;
    }
#endif
    while (len + 8u <= cap) {
        uint64_t diff = (uint64_t)zgec_rd64(src + (size_t)len) ^
                        (uint64_t)zgec_rd64(dst + (size_t)len);
        if (diff != 0u) {
            return len + ((uint32_t)__builtin_ctzll(diff) >> 3);
        }
        len += 8u;
    }
    while (len < cap && src[len] == dst[len]) {
        len++;
    }
    return len;
}

/* Length of the match at ip with offset d, capped at cap. The first eight
 * bytes are the overwhelmingly common case -- a source tree's matches are
 * about seven bytes long -- so they are compared here, inline, and only a
 * match that survives them pays for the wide-compare loop above. The
 * precondition is d <= ip and cap <= vb_size - ip. */
static inline __attribute__((always_inline))
uint32_t mf_match_len(const uint8_t *vb, size_t ip, uint32_t d, uint32_t cap)
{
    const uint8_t *src = vb + ip - (size_t)d;
    const uint8_t *dst = vb + ip;
    if (cap >= 8u) {
        uint32_t first = mf_len8(src, dst);
        if (first < 8u) {
            return first;
        }
        return mf_match_len_slow(src, dst, cap);
    }
    uint32_t len = 0;
    while (len < cap && src[len] == dst[len]) {
        len++;
    }
    return len;
}

/* Hit mask of the short bucket: bit `lane` is set when entry `lane`
 * carries the wanted 8-bit tag and is not the empty sentinel. AVX2
 * compares eight lanes at once (fast/main/high use 4/8/16); the
 * remaining lanes stay scalar. `want` is the caller's probe depth and
 * `newest` is the newest-lane index ((head - 1) & (nlanes - 1)): both
 * only gate work, the accessed range stays b[0 .. nlanes) and the hit
 * semantics are unchanged. */
static uint32_t mf_bucket_hits(const uint32_t *b, uint32_t tag, uint32_t nlanes,
                               uint32_t want, uint32_t newest)
{
    uint32_t e0;
    uint32_t mask = 0u;
    uint32_t lane = 0u;
    if (want == 0u) {
        return 0u;
    }
    /* Newest-lane gate before any vector work: table memory starts zeroed
     * and a packed entry is never zero (pos + 1 keeps a low bit set), so
     * an empty newest lane means the bucket was never written and the
     * full compare could only produce an empty mask. Otherwise pre-test
     * the newest tag: with want == 1 only the newest hit is scored, so a
     * newest hit returns its single bit without the vector compare; a
     * newest miss still falls through to the full compare. */
    e0 = b[newest];
    if (e0 == 0u) {
        return 0u;
    }
    if (want == 1u && (((e0 >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK) == tag)) {
        return (uint32_t)1u << newest;
    }
    if (nlanes < 8u) {
        for (; lane < nlanes; lane++) {
            uint32_t e = b[lane];
            uint32_t etag = (e >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
            mask |= (uint32_t)((e != 0u && etag == tag) ? 1u : 0u) << lane;
        }
        return mask;
    }
#if defined(__AVX2__)
    __m256i tagv = _mm256_set1_epi32((int)(tag << ZGEC_MF_TAG_SHIFT));
    __m256i tagmask = _mm256_set1_epi32((int)(ZGEC_MF_TAG_MASK << ZGEC_MF_TAG_SHIFT));
    __m256i zero = _mm256_setzero_si256();
    for (; lane + 8u <= nlanes; lane += 8u) {
        __m256i v = _mm256_load_si256((const __m256i *)(const void *)(b + lane));
        __m256i eq = _mm256_cmpeq_epi32(_mm256_and_si256(v, tagmask), tagv);
        __m256i nz = _mm256_cmpeq_epi32(v, zero);
        __m256i hit = _mm256_andnot_si256(nz, eq);
        mask |= (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(hit)) << lane;
    }
#endif
    for (; lane < nlanes; lane++) {
        uint32_t e = b[lane];
        uint32_t etag = (e >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        mask |= (uint32_t)((e != 0u && etag == tag) ? 1u : 0u) << lane;
    }
    return mask;
}

/* Record a candidate if it beats the current best:
 * longer, or equal length with a smaller offset. */
static void mf_candidate(uint32_t d, uint32_t len, zgec_match *best)
{
    if (len > best->length || (len == best->length && d < best->offset)) {
        best->offset = d;
        best->length = len;
    }
}

/* The geometry macros above are overridable, so the invariants the bucket
 * and lane masks rely on are checked at create time rather than assumed. */
static int mf_is_pow2_u32(uint32_t x)
{
    return x != 0u && (x & (x - 1u)) == 0u;
}

zgec_matcher *zgec_matcher_create(zgec_tier tier, size_t vb_capacity)
{
    zgec_matcher *m;
    size_t short_n;
    size_t long_n;

    /* The 24-bit position field limits the virtual
     * buffer to 16 MiB (v1 documented limit); larger
     * capacities are rejected. */
    if (vb_capacity == 0 || vb_capacity > (size_t)ZGEC_MF_POS_LIMIT) {
        return NULL;
    }
    m = (zgec_matcher *)zgec_alloc(sizeof(zgec_matcher), ZGEC_MF_ALIGN);
    if (!m) {
        return NULL;
    }
    m->tier = tier;
    m->vb_capacity = vb_capacity;
    m->vb = NULL;
    m->vb_size = 0;
    m->is_binary = 0;
    m->short_tab = NULL;
    m->short_head = NULL;
    m->long_tab = NULL;
    switch (tier) {
    case ZGEC_TIER_FAST:
        m->nbuckets = ZGEC_MF_FAST_BUCKETS;
        m->nlanes = ZGEC_MF_FAST_LANES;
        m->long_buckets = ZGEC_MF_FAST_LONG_BUCKETS;
        break;
    case ZGEC_TIER_MAIN:
        m->nbuckets = ZGEC_MF_MAIN_BUCKETS;
        m->nlanes = ZGEC_MF_MAIN_LANES;
        m->long_buckets = ZGEC_MF_MAIN_LONG_BUCKETS;
        break;
    case ZGEC_TIER_HIGH:
        m->nbuckets = ZGEC_MF_HIGH_BUCKETS;
        m->nlanes = ZGEC_MF_HIGH_LANES;
        m->long_buckets = ZGEC_MF_HIGH_LONG_BUCKETS;
        break;
    default:
        zgec_free(m);
        return NULL;
    }
    /* Buckets and lanes must be nonzero powers of two for the `- 1u` masks
     * and the ring indexing, and nlanes must fit the 32-bit lane hit mask. */
    if (!mf_is_pow2_u32(m->nbuckets) || !mf_is_pow2_u32(m->long_buckets) ||
        !mf_is_pow2_u32(m->nlanes) || m->nlanes > ZGEC_MF_MAX_LANES) {
        zgec_free(m);
        return NULL;
    }
    /* Overflow-proof size products, for the short table and the long one. */
    if ((size_t)m->nlanes > SIZE_MAX / (size_t)m->nbuckets) {
        zgec_free(m);
        return NULL;
    }
    short_n = (size_t)m->nbuckets * (size_t)m->nlanes;
    long_n = (size_t)m->long_buckets;
    if (short_n > SIZE_MAX / sizeof(uint32_t) ||
        long_n > SIZE_MAX / sizeof(uint32_t)) {
        zgec_free(m);
        return NULL;
    }
    m->short_tab = (uint32_t *)zgec_alloc(short_n * sizeof(uint32_t), ZGEC_MF_TABLE_ALIGN);
    if (!m->short_tab) {
        zgec_free(m);
        return NULL;
    }
    m->long_tab = (uint32_t *)zgec_alloc(long_n * sizeof(uint32_t), ZGEC_MF_TABLE_ALIGN);
    if (!m->long_tab) {
        zgec_free(m->short_tab);
        zgec_free(m);
        return NULL;
    }
    m->short_head = (uint8_t *)zgec_alloc(m->nbuckets, 64);
    if (!m->short_head) {
        zgec_free(m->long_tab);
        zgec_free(m->short_tab);
        zgec_free(m);
        return NULL;
    }
    memset(m->short_tab, 0, short_n * sizeof(uint32_t));
    memset(m->short_head, 0, m->nbuckets);
    memset(m->long_tab, 0, long_n * sizeof(uint32_t));
    /* No position has cached hashes yet. */
    m->c_ip[0] = (size_t)-1;
    m->c_ip[1] = (size_t)-1;
    m->c_hs[0] = 0u;
    m->c_hs[1] = 0u;
    m->c_hl[0] = 0u;
    m->c_hl[1] = 0u;
    m->c_flags[0] = 0u;
    m->c_flags[1] = 0u;
    return m;
}

void zgec_matcher_destroy(zgec_matcher *m)
{
    if (!m) {
        return;
    }
    zgec_free(m->short_tab);
    zgec_free(m->short_head);
    zgec_free(m->long_tab);
    zgec_free(m);
}

void zgec_matcher_reset(zgec_matcher *m, const uint8_t *vb, size_t vb_size,
                        const uint8_t *dict, size_t dict_size)
{
    size_t stride;
    size_t pos;
    size_t short_n;
    size_t long_n;

    if (!m) {
        return;
    }
    /* Clear the tables *before* deciding whether to attach, so that a
     * detached matcher is also an empty one. Returning early with the
     * previous block's packed positions still in short_tab/short_head/
     * long_tab left "detached" and "empty" as two states differing in
     * memory contents, and the safety of every entry point below then rested
     * entirely on the identity test running first: any future entry point
     * that checked only the size (which passes, since vb_size == 0 admits
     * only ip == 0) would read positions into a buffer no longer attached. */
    short_n = (size_t)m->nbuckets * (size_t)m->nlanes;
    long_n = (size_t)m->long_buckets;
    if (m->short_tab) {
        memset(m->short_tab, 0, short_n * sizeof(uint32_t));
    }
    if (m->short_head) {
        memset(m->short_head, 0, m->nbuckets);
    }
    if (m->long_tab) {
        memset(m->long_tab, 0, long_n * sizeof(uint32_t));
    }
    /* The cache describes the previous block's buffer, so drop it: a stale
     * hash could otherwise be reused for the same position in an unrelated
     * buffer. */
    m->c_ip[0] = (size_t)-1;
    m->c_ip[1] = (size_t)-1;
    m->c_flags[0] = 0u;
    m->c_flags[1] = 0u;
    /* This interface returns void, so inconsistent state is rejected by
     * refusing to attach a buffer rather than by reporting an error. Every
     * entry point below starts with a vb_size bound check, so a detached
     * matcher performs no reads at all: a null buffer with a nonzero size,
     * or a size above the capacity this matcher was created for, leaves the
     * matcher unusable instead of hashable. The detach is unreachable from
     * the only in-tree caller, so it buys safety against a caller that does
     * not exist yet -- and it still does so silently, because the interface
     * has no way to report it. */
    if ((vb_size != 0 && vb == NULL) || vb_size > m->vb_capacity) {
        m->vb = NULL;
        m->vb_size = 0;
        m->is_binary = 0;
        return;
    }
    m->vb = vb;
    m->vb_size = vb_size;
    /* Classify the block, not a dictionary prefix: the caller lays vb out as
     * [dictionary][literal references][block], and a dictionary is usually
     * longer than the 4 KiB sample, so sampling from vb[0] would describe
     * the dictionary instead of the data being compressed. Only the
     * dictionary offset is known here (the literal-reference region is
     * not), so the sample starts after the dictionary.
     *
     * `dict_size`, not `dict`, is the layout fact this keys off: the
     * dictionary bytes are vb[0 .. dict_size) whatever the `dict` pointer
     * says, and the caller passes dict_size unconditionally while passing
     * `dict` only when both are nonzero, so testing the pointer would sample
     * the dictionary if that pair ever arrived as (NULL, >0).
     *
     * What is still not fixed: cls_len spans to the end of vb, so it covers
     * the literal-reference region -- another block's literals -- and
     * whenever that region exceeds the 4 KiB sample this still classifies
     * data the block does not contain. Narrowing it needs the
     * literal-reference length, which the frozen signature cannot carry.
     * So the change removes the dictionary case and leaves the litref case,
     * which is the same defect behind a different prefix and is not
     * marginal. Because is_binary selects the short-hash width (4 vs 5
     * bytes) and which positions are insertable, this flips the emitted
     * matches for every dictionary-compressed block: the read is provably
     * inside [dict_size, vb_size), so it is safe, but the direction of the
     * ratio change is unmeasured and needs a corpus before it is called an
     * improvement rather than a difference. */
    {
        size_t cls_off = 0;
        size_t cls_len = vb_size;
        if (dict_size != 0 && dict_size < vb_size) {
            cls_off = dict_size;
            cls_len = vb_size - dict_size;
        }
        m->is_binary = mf_classify_binary(vb ? vb + cls_off : NULL, cls_len);
    }
    /* Section 11.8 intent: hash the dictionary once
     * into a table snapshot and copy it (memcpy)
     * into each worker's table at block start; the
     * loop below is the per-worker fill from that
     * snapshot layout. */
    /* `dict` is only a non-null flag: the dictionary bytes are the first
     * dict_size bytes of vb, so there is no second pointer to read through
     * and the argument is redundant with dict_size. It stays in the
     * interface because include/zgec_match.h declares it. */
    if (!vb || vb_size == 0 || !dict || dict_size == 0) {
        return;
    }
    if (dict_size > vb_size) {
        dict_size = vb_size;
    }
    /* The caller has laid out vb as [dictionary][block].
     * Insert the dictionary positions so matches can
     * reference the dictionary: every position in the
     * high tier, every 2nd position in the others. */
    stride = (m->tier == ZGEC_TIER_HIGH) ? (size_t)1 : (size_t)2;
    for (pos = 0; pos < dict_size; pos += stride) {
        if (m->is_binary) {
            if (pos + (size_t)ZGEC_MF_HASH_BYTES_BIN > vb_size) {
                break;
            }
        } else {
            if (pos + (size_t)ZGEC_MF_HASH_BYTES > vb_size) {
                break;
            }
        }
        mf_insert_pos(m, vb, pos, 1);
    }
}

zgec_match zgec_matcher_find(zgec_matcher *m, const uint8_t *vb, size_t ip,
                             uint32_t rep0, uint32_t rep1, uint32_t rep2,
                             uint32_t min_len, uint32_t max_len)
{
    zgec_match best;
    uint32_t min_match;
    uint32_t hash_min;
    uint32_t cap;
    uint32_t nreps;
    uint32_t i;
    size_t avail;
    uint32_t reps[3];
    uint32_t cur4;

    best.offset = 0;
    best.length = 0;
    /* The position bounds below are checked against m->vb_size, so the reads
     * must be of the very buffer those bounds describe: a caller that passes
     * a shorter or unrelated buffer gets no match rather than an
     * out-of-bounds read. */
    if (!m || !vb || vb != m->vb || ip >= m->vb_size) {
        return best;
    }
    /* Hoisted capacity validation: vb_size <= vb_capacity holds for every
     * attached buffer (see the struct invariant), so the per-candidate
     * d <= vb_capacity tests below are dead. One check here covers all
     * candidates; the assert documents the invariant in debug builds. */
    assert(m->vb_size <= m->vb_capacity);
    if (m->vb_size > m->vb_capacity) {
        return best;
    }

    /* Section 11.2: a 5-byte short hash cannot find a 4-byte match, so 5 is
     * the minimum for hash (non-repeat) candidates. The floor of 4 is for
     * repeat offsets, and rests on stated policy rather than on the hash
     * argument: include/zgec_match.h says "The minimum non-repeat match is
     * 5, with 4 permitted for repeat offsets", and docs/spec.md:836 says the
     * same. Three is not a legal matcher length in either case, even though
     * the format's decoder rule V3 only requires ML >= 3, which is laxer
     * than the encoder policy applied here. */
    min_match = (min_len < 4u) ? 4u : min_len;
    if (max_len < min_match) {
        return best;
    }
    avail = m->vb_size - ip;
    cap = (avail < (size_t)max_len) ? (uint32_t)avail : max_len;
    if (cap < min_match) {
        return best;
    }
    /* Hash candidates need the non-repeat minimum of
     * 5 (section 11.2); repeat offsets may use 4. */
    hash_min = (min_match < ZGEC_MF_MIN_HASH_MATCH) ? ZGEC_MF_MIN_HASH_MATCH : min_match;
    if (cap < hash_min && min_match < hash_min) {
        /* Only repeat offsets can still qualify. */
        hash_min = cap + 1u; /* disables hash probes below */
    }

    reps[0] = rep0;
    reps[1] = rep1;
    reps[2] = rep2;
    nreps = (m->tier == ZGEC_TIER_FAST) ? 1u : 3u;
    int has_v8 = (ip + 8u <= m->vb_size);
    uint64_t v8 = has_v8 ? zgec_rd64(vb + ip) : 0u;
    cur4 = has_v8 ? (uint32_t)v8 : zgec_rd32(vb + ip);   /* cap >= min_match >= 4 here */
    for (i = 0; i < nreps; i++) {
        uint32_t d = reps[i];
        uint32_t len;
        if (d == 0u || (size_t)d > ip) {
            continue;
        }
        /* Assert-style: d <= ip < vb_size <= vb_capacity (hoisted check). */
        assert((size_t)d <= m->vb_capacity);
        /* min_match >= 4, so a repeat whose first four bytes differ cannot
         * qualify; one 4-byte compare replaces the length scan. */
        if (zgec_rd32(vb + ip - (size_t)d) != cur4) {
            continue;
        }
        if (has_v8) {
            uint64_t diff = zgec_rd64(vb + ip - (size_t)d) ^ v8;
            len = (diff == 0u) ? mf_match_len_slow(vb + ip - (size_t)d, vb + ip, cap)
                               : (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
        } else {
            len = mf_match_len(vb, ip, d, cap);
        }
        if (len >= min_match) {
            mf_candidate(d, len, &best);
        }
    }

    /* The hashes this find computes for ip are the ones an insert of ip
     * needs, so leave them behind for mf_insert_pos. The slot is parity
     * indexed, so the lazy probe at ip + 1 lands in the other slot and
     * leaves this position's hashes intact for the main probe's insert. */
    m->c_ip[ip & 1u] = ip;
    m->c_flags[ip & 1u] = 0u;
    /* 2. Long table: 8-byte hash, one entry per
     * bucket, tag-checked before any length work. */
    if (has_v8) {
        uint32_t hl = mf_hash8_v(v8);
        uint32_t lb = hl & (m->long_buckets - 1u);
        const uint32_t *le = &m->long_tab[(size_t)lb];
        uint32_t e = *le;
        uint32_t ltag = (hl >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        uint32_t etag = (e >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        uint32_t pos;
        uint32_t d;
        uint32_t len;
        m->c_hl[ip & 1u] = hl;
        m->c_flags[ip & 1u] |= 2u;
        mf_prefetch_bucket((const void *)le);
        e = *le;
        ltag = (hl >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        etag = (e >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        if (e != 0u && etag == ltag) {
            pos = mf_pos(e);
            if ((size_t)pos < ip) {
                d = (uint32_t)(ip - (size_t)pos);
                /* Retained explicit capacity check on the long-table path:
                 * the hoisted validation above already covers it in-tree. */
                if ((size_t)d <= m->vb_capacity && hash_min <= cap) {
                    /* A candidate can only beat `best` if it matches at
                     * best.length - 1 as well; one byte compare there skips
                     * the length scan for the candidates that cannot. */
                    if (best.length == 0u || best.length > cap ||
                        vb[ip - (size_t)d + (size_t)best.length - 1u] ==
                        vb[ip + (size_t)best.length - 1u]) {
                        if (has_v8) {
            uint64_t diff = zgec_rd64(vb + ip - (size_t)d) ^ v8;
            len = (diff == 0u) ? mf_match_len_slow(vb + ip - (size_t)d, vb + ip, cap)
                               : (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
        } else {
            len = mf_match_len(vb, ip, d, cap);
        }
                        if (len >= hash_min) {
                            mf_candidate(d, len, &best);
                        }
                    }
                }
            }
        }
    }

    /* 3. Short bucket probe, newest entry first. Tags are compared by
     * mf_bucket_hits(): on AVX2 that is 32-bit-lane compares, two 256-bit
     * vectors for a 16-lane high-tier bucket, with any leftover lanes
     * scalar, so there is no 128-bit tag compare here. The returned hit
     * mask is then rotated so a count-leading-zeros walk visits the hit
     * lanes in newest-first order, and an empty mask simply ends the loop
     * after zero iterations.
     *
     * A rep or long-table match of 32 bytes or more already captures the
     * bulk of the available gain; scoring the short bucket then rarely
     * changes the parse, so it is skipped. */
    if (best.length < 32u) {
    {
        size_t need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                                   : (size_t)ZGEC_MF_HASH_BYTES;
        if (ip + need <= m->vb_size && hash_min <= cap) {
            uint32_t h = m->is_binary ? mf_hash4_v((uint32_t)cur4)
                                      : (has_v8 ? mf_hash5_v(v8) : mf_hash_short(m, vb, ip));
            uint32_t bucket = h & (m->nbuckets - 1u);
            const uint32_t *b = &m->short_tab[(size_t)bucket * (size_t)m->nlanes];
            uint32_t tag = (h >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
            uint32_t mask;
            uint32_t head;
            uint32_t nmatch;
            uint32_t want;
            uint32_t nhit;
            uint32_t cur4s;
            m->c_hs[ip & 1u] = h;
            m->c_flags[ip & 1u] |= 1u;
            mf_prefetch_bucket((const void *)b);
            want = (m->tier == ZGEC_TIER_HIGH) ? ZGEC_MF_PROBE_DEPTH_HIGH
                 : (m->tier == ZGEC_TIER_FAST) ? ZGEC_MF_PROBE_DEPTH_FAST
                                               : ZGEC_MF_PROBE_DEPTH;
            head = (uint32_t)m->short_head[bucket];
            mask = mf_bucket_hits(b, tag, m->nlanes, want,
                                  (head - 1u) & (m->nlanes - 1u));
            nmatch = (uint32_t)__builtin_popcount((unsigned)mask);
            nhit = (nmatch < want) ? nmatch : want;
            cur4s = zgec_rd32(vb + ip); /* need >= 4 above, so ip + 4 <= vb_size */
            /* The live entries walk backwards from the newest lane: the
             * ring head points one past the most recent write.
             *
             * Visit only the lanes that hit, newest first. Rotating the hit
             * mask so that lane head-1 lands in the top bit lets a
             * count-leading-zeros walk yield lanes in newest-first order
             * while skipping the empty ones: the loop runs once per hit
             * instead of once per lane. `s == 0` (head == 0) is the
             * unrotated mask, which also keeps each shift below in range,
             * and the mask is truncated to nlanes bits for nlanes < 32. */
            {
                uint32_t n = m->nlanes;
                uint32_t s = (n - head) & (n - 1u);
                uint32_t rm;
                if (s == 0u) {
                    rm = mask;
                } else {
                    rm = (mask << s) | (mask >> (n - s));
                }
                if (n < 32u) {
                    rm &= (uint32_t)(((uint64_t)1u << n) - 1u);
                }
                while (rm != 0u && nhit > 0) {
                    uint32_t bpos = 31u - (uint32_t)__builtin_clz(rm);
                    uint32_t lane = (head + bpos) & (n - 1u);
                    uint32_t entry;
                    uint32_t pos;
                    uint32_t d;
                    uint32_t len;
                    rm &= ~(1u << bpos);
                    nhit--;
                    entry = b[lane];
                    pos = mf_pos(entry);
                    if ((size_t)pos >= ip) {
                        continue;
                    }
                    d = (uint32_t)(ip - (size_t)pos);
                    /* Assert-style: d <= ip < vb_size <= vb_capacity. */
                    assert((size_t)d <= m->vb_capacity);
                    /* A len >= 5 match implies a first-4-byte match, so the
                     * 4-byte screen the rep path uses skips hopeless hash
                     * candidates before any length work. The pos + 4 guard
                     * keeps the read in bounds locally. */
                    if ((size_t)pos + (size_t)4 <= m->vb_size &&
                        zgec_rd32(vb + (size_t)pos) != cur4s) {
                        continue;
                    }
                    /* A candidate can only beat `best` if it matches at
                     * best.length - 1 as well; one byte compare there skips
                     * the length scan for the candidates that cannot. */
                    if (best.length != 0u && best.length <= cap &&
                        vb[ip - (size_t)d + (size_t)best.length - 1u] !=
                        vb[ip + (size_t)best.length - 1u]) {
                        continue;
                    }
                    if (has_v8) {
            uint64_t diff = zgec_rd64(vb + ip - (size_t)d) ^ v8;
            len = (diff == 0u) ? mf_match_len_slow(vb + ip - (size_t)d, vb + ip, cap)
                               : (uint32_t)((unsigned)__builtin_ctzll(diff) >> 3);
        } else {
            len = mf_match_len(vb, ip, d, cap);
        }
                    if (len >= hash_min) {
                        mf_candidate(d, len, &best);
                    }
                }
            }
        }
    }
    }
    return best;
}

/* Prefetch the table lines a find at ip will touch (section 11.3 step 1).
 * Issued a step ahead of the skip schedule, so the bucket load overlaps the
 * position's own work instead of stalling on it. */
void zgec_matcher_prefetch(zgec_matcher *m, const uint8_t *vb, size_t ip)
{
    size_t need;
    if (!m || !vb || vb != m->vb || !m->short_tab || !m->long_tab) {
        return;
    }
    need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                        : (size_t)ZGEC_MF_HASH_BYTES;
    if (ip + need > m->vb_size) {
        return;
    }
    {
        uint32_t h = mf_hash_short(m, vb, ip);
        uint32_t bucket = h & (m->nbuckets - 1u);
        __builtin_prefetch((const void *)&m->short_tab[(size_t)bucket * (size_t)m->nlanes], 1, 3);
        __builtin_prefetch((const void *)&m->short_head[bucket], 1, 3);
    }
    if (ip + (size_t)ZGEC_MF_LONG_BYTES <= m->vb_size) {
        uint32_t hl = mf_hash8(vb, ip);
        __builtin_prefetch((const void *)&m->long_tab[(size_t)(hl & (m->long_buckets - 1u))], 1, 3);
    }
}

/* Prefetch the lines insert_match(start, len) will write: every position in
 * the high tier, the same sample grid the other tiers insert. */
void zgec_matcher_prefetch_match(zgec_matcher *m, const uint8_t *vb, size_t start, size_t len)
{
    if (!m || !vb || vb != m->vb || !m->short_tab || !m->long_tab || len == 0 || start >= m->vb_size) {
        return;
    }
    if (len > m->vb_size - start) {
        len = m->vb_size - start;
    }
    if (m->tier == ZGEC_TIER_HIGH) {
        size_t stride = (size_t)1;
        size_t pos;
        if (len > (size_t)ZGEC_MF_HIGH_SAMPLES) {
            stride = (len + (size_t)ZGEC_MF_HIGH_SAMPLES - 1u) / (size_t)ZGEC_MF_HIGH_SAMPLES;
        }
        for (pos = start; pos < start + len; pos += stride) {
            zgec_matcher_prefetch(m, vb, pos);
        }
    } else {
        size_t nsamp = (m->tier == ZGEC_TIER_FAST) ? (size_t)ZGEC_MF_MATCH_SAMPLES_FAST
                                                   : (size_t)ZGEC_MF_MATCH_SAMPLES;
        size_t t;
        if (nsamp > len) nsamp = len;
        if (nsamp < 1u) nsamp = 1u;
        for (t = 0; t < nsamp; t++) {
            uint64_t off = (nsamp > 1u)
                               ? ((uint64_t)(len - 1u) * (uint64_t)t /
                                  (uint64_t)(nsamp - 1u))
                               : 0u;
            zgec_matcher_prefetch(m, vb, start + (size_t)off);
        }
    }
}

void zgec_matcher_insert(zgec_matcher *m, const uint8_t *vb, size_t ip)
{
    size_t need;
    if (!m || !vb || vb != m->vb || !m->short_tab || !m->long_tab) {
        return;
    }
    /* The position must fit the 24-bit field; the
     * short hash reads 5 bytes (4 for binary). */
    need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                        : (size_t)ZGEC_MF_HASH_BYTES;
    if (ip >= (size_t)ZGEC_MF_POS_LIMIT || ip > m->vb_size || need > m->vb_size - ip) {
        return;
    }
    mf_insert_pos(m, vb, ip, 1);
}

void zgec_matcher_insert_match(zgec_matcher *m, const uint8_t *vb, size_t start, size_t len)
{
    if (!m || !vb || vb != m->vb || !m->short_tab || !m->long_tab || len == 0) {
        return;
    }
    if (start >= m->vb_size) {
        return;
    }
    /* Clamp the range to the virtual buffer. */
    if (len > m->vb_size - start) {
        len = m->vb_size - start;
    }
    /* Insertion policy: visited positions only (the
     * parse drives inserts); the high tier inserts
     * every position. Section 11.2 names 2-3 sampled
     * positions for fast and main, but main samples
     * ZGEC_MF_MATCH_SAMPLES (16 by default, see the
     * note at that macro) because the spec's figure
     * measured worse in ratio here; the fast tier
     * keeps 3 through ZGEC_MF_MATCH_SAMPLES_FAST. */
    if (m->tier == ZGEC_TIER_HIGH) {
        /* Every position in the range, sampled if the
         * match is very long. */
        size_t stride = (size_t)1;
        size_t pos;
        if (len > (size_t)ZGEC_MF_HIGH_SAMPLES) {
            stride = (len + (size_t)ZGEC_MF_HIGH_SAMPLES - 1u) / (size_t)ZGEC_MF_HIGH_SAMPLES;
        }
        for (pos = start; pos < start + len; pos += stride) {
            size_t need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                                       : (size_t)ZGEC_MF_HASH_BYTES;
            if (pos + need > m->vb_size) {
                break;
            }
            mf_insert_pos(m, vb, pos, 1);
        }
    } else {
        /* Sampled: evenly spaced, distinct positions across the match
         * (never more than the match length, so no duplicate inserts),
         * all within the virtual buffer. uint64_t math: no 32-bit wrap,
         * no division per sample. */
        size_t nsamp = (m->tier == ZGEC_TIER_FAST)
                           ? (size_t)ZGEC_MF_MATCH_SAMPLES_FAST
                           : (size_t)ZGEC_MF_MATCH_SAMPLES;
        size_t t;
        size_t need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                                   : (size_t)ZGEC_MF_HASH_BYTES;
        /* Adaptive sampling: short matches need few inserts to cover the
         * range, so cap the sample count at 2 + (len >> 2). Matches < 8
         * bytes insert ~2-3 positions; the full count applies only for
         * len >= ~56. */
        {
            size_t cap = (size_t)2 + (len >> 2);
            if (nsamp > cap) nsamp = cap;
        }
        if (nsamp > len) nsamp = len;
        if (nsamp < 1u) nsamp = 1u;
        for (t = 0; t < nsamp; t++) {
            uint64_t off = (nsamp > 1u)
                               ? ((uint64_t)(len - 1u) * (uint64_t)t /
                                  (uint64_t)(nsamp - 1u))
                               : 0u;
            size_t pos = start + (size_t)off;
            if (pos < m->vb_size && need <= m->vb_size - pos) {
                int long_ok = (t == 0u || t + 1u == nsamp) ? 1 : 0;
                mf_insert_pos(m, vb, pos, long_ok);
            }
        }
    }
}
