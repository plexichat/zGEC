/*
 * Match finder per zGEC section 11.2 (informative).
 *
 * Bucket hash-chain match finder over a virtual
 * buffer. Three tiers select the bucket width and
 * the table size:
 *
 *   tier   short buckets   lanes   short size   long size
 *   fast   2^16            4       1 MiB        256 KiB
 *   main   2^16            8       2 MiB        256 KiB
 *   high   2^18            16      16 MiB       1 MiB
 *
 * Table sizes are the section 11.2 starting point
 * (2^16 buckets for fast/main, 2^18 for high, and
 * measurement decides). Per-thread cap note: with
 * one worker per core the per-thread total SHOULD
 * stay near 1-1.5 MiB so the sum across threads
 * does not exceed the shared L3; the main tier
 * (2.25 MiB with both tables) already exceeds it
 * and the high tier (17 MiB) far exceeds it, so a
 * threaded build SHOULD shard workers, use fewer
 * match threads than cores, or fall back a tier.
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
 * entry is unambiguously empty. Because the position
 * field is 24 bits (profile P24), storable positions
 * are limited to 2^24 - 2 and zgec_matcher_create
 * rejects a virtual buffer capacity above 2^24
 * (16 MiB).
 *
 * Tables and buffers are allocated 2 MiB aligned
 * with zgec_alloc as the huge-page intent
 * (section 11.2); with 4 KiB pages the 2048-entry
 * L2 TLB covers only 8 MiB.
 */

#include "zgec_encode.h" /* zgec_tier */
#include "zgec_match.h"

#include <string.h>
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
 * main, and 2^18 for high, and let measurement decide"). Measurement
 * on source-tree data says the long-range retention of the smaller
 * tables, not probe cost, is what loses ratio, so the main tier's
 * long table is widened to 2^18 buckets and its short buckets to 2^17
 * lanes-equal rows. The short table hashes 5 bytes into a bucket of
 * `lanes` packed entries; the long table hashes 8 bytes into one
 * entry per bucket. Overridable so a build can re-measure. */
#ifndef ZGEC_MF_FAST_BUCKETS
#define ZGEC_MF_FAST_BUCKETS (1u << 16)
#endif
#ifndef ZGEC_MF_MAIN_BUCKETS
#define ZGEC_MF_MAIN_BUCKETS (1u << 17)
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

/* Probes: how many tag hits of a short bucket are scored. Section
 * 11.2 starts at 2 (fast/main) and 4 (high); measuring on source-tree
 * data, 8 probes win about 1% of ratio and cost ~5% of encode time,
 * because the deeper hits are exactly the long-range candidates that
 * the newest few lanes had pushed out. */
#ifndef ZGEC_MF_PROBE_DEPTH
#define ZGEC_MF_PROBE_DEPTH 8u
#endif
#ifndef ZGEC_MF_PROBE_DEPTH_HIGH
#define ZGEC_MF_PROBE_DEPTH_HIGH 8u
#endif

/* Positions sampled inside a match for the fast and main tiers.
 * Section 11.2 starts at 2-3; measurement says the table's memory of
 * older content, not probe cost, is what recovers long-range periodic
 * matches, so the default samples far more densely. Fast tier keeps
 * the spec's sparse form because it is the speed tier. */
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

/* The matcher struct stays 64-byte aligned; tables
 * use the 2 MiB huge-page alignment intent. */
#define ZGEC_MF_ALIGN 64
#define ZGEC_MF_TABLE_ALIGN ((size_t)2u * (size_t)1024u * (size_t)1024u)

struct zgec_matcher {
    zgec_tier      tier;
    uint32_t       nbuckets;      /* short buckets */
    uint32_t       nlanes;
    uint32_t       long_buckets;  /* long buckets (one entry each) */
    uint32_t      *short_tab;   /* nbuckets * nlanes entries */
    uint8_t       *short_head;  /* per-bucket next write lane (ring) */
    uint32_t      *long_tab;    /* long_buckets entries */
    int            is_binary;   /* short hash uses 4 bytes */
    size_t         vb_capacity; /* maximum referenceable offset */
    const uint8_t *vb;
    size_t         vb_size;
};

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
 * bucket). The caller must guarantee
 * ip + 8 <= vb_size. */
static uint32_t mf_hash8(const uint8_t *vb, size_t ip)
{
    uint64_t x = (uint64_t)zgec_rd64(vb + ip);
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return (uint32_t)(x ^ (x >> 32));
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

/* Prefetch a bucket line for a visited position
 * (section 11.3 step 1). */
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
    mf_prefetch_bucket((const void *)b);
    b[h] = entry;
    m->short_head[bucket] = (uint8_t)((h + 1u) & (m->nlanes - 1u));
}

/* Insert pos into the long table: one entry per
 * bucket, newest overwrites. */
static void mf_insert_long(zgec_matcher *m, uint32_t bucket, uint32_t entry)
{
    m->long_tab[(size_t)bucket] = entry;
}

static void mf_insert_pos(zgec_matcher *m, const uint8_t *vb, size_t pos)
{
    uint32_t h;
    uint32_t bucket;
    uint32_t tag;
    uint32_t entry;

    if (pos + 1u >= (size_t)ZGEC_MF_POS_LIMIT) {
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
    h = mf_hash_short(m, vb, pos);
    bucket = h & (m->nbuckets - 1u);
    tag = (h >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
    entry = mf_pack((uint32_t)pos, tag);
    mf_insert_short(m, bucket, entry);

    if (pos + (size_t)ZGEC_MF_LONG_BYTES <= m->vb_size) {
        uint32_t hl = mf_hash8(vb, pos);
        uint32_t lb = hl & (m->long_buckets - 1u);
        uint32_t ltag = (hl >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        mf_insert_long(m, lb, mf_pack((uint32_t)pos, ltag));
    }
}

/* First-8-byte length via 8-byte XOR plus trailing
 * zero byte count (tzcnt >> 3, section 11.3 step 4).
 * Returns 8 when the first 8 bytes match. */
static uint32_t mf_len8(const uint8_t *src, const uint8_t *dst)
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
static uint32_t mf_match_len(const uint8_t *vb, size_t ip, uint32_t d, uint32_t cap)
{
    const uint8_t *src = vb + ip - (size_t)d;
    const uint8_t *dst = vb + ip;
    uint32_t len = 0;
    if (cap >= 8u) {
        uint32_t first = mf_len8(src, dst);
        if (first < 8u) {
            return first;
        }
        len = 8u;
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
            uint64_t d1 = (uint64_t)zgec_rd64(src + (size_t)len + (size_t)8) ^
                          (uint64_t)zgec_rd64(dst + (size_t)len + (size_t)8);
            uint64_t d2 = (uint64_t)zgec_rd64(src + (size_t)len + (size_t)16) ^
                          (uint64_t)zgec_rd64(dst + (size_t)len + (size_t)16);
            uint64_t d3 = (uint64_t)zgec_rd64(src + (size_t)len + (size_t)24) ^
                          (uint64_t)zgec_rd64(dst + (size_t)len + (size_t)24);
            if ((d0 | d1 | d2 | d3) != (uint64_t)0) {
                break;
            }
            len += 32u;
        }
#endif
        while (len + 8u <= cap) {
            if (zgec_rd64(src + (size_t)len) != zgec_rd64(dst + (size_t)len)) {
                break;
            }
            len += 8u;
        }
    }
    while (len < cap && src[len] == dst[len]) {
        len++;
    }
    return len;
}

/* Hit mask of the short bucket: bit `lane` is set when entry `lane`
 * carries the wanted 8-bit tag and is not the empty sentinel. AVX2
 * compares eight lanes at once (fast/main/high use 4/8/16); the
 * remaining lanes stay scalar. */
static uint32_t mf_bucket_hits(const uint32_t *b, uint32_t tag, uint32_t nlanes)
{
    uint32_t mask = 0u;
    uint32_t lane = 0u;
#if defined(__AVX2__)
    __m256i tagv = _mm256_set1_epi32((int)(tag << ZGEC_MF_TAG_SHIFT));
    __m256i tagmask = _mm256_set1_epi32((int)(ZGEC_MF_TAG_MASK << ZGEC_MF_TAG_SHIFT));
    __m256i posmask = _mm256_set1_epi32((int)ZGEC_MF_POS_MASK);
    __m256i zero = _mm256_setzero_si256();
    for (; lane + 8u <= nlanes; lane += 8u) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(b + lane));
        __m256i eq = _mm256_cmpeq_epi32(_mm256_and_si256(v, tagmask), tagv);
        __m256i pos = _mm256_and_si256(v, posmask);
        __m256i nz = _mm256_cmpeq_epi32(pos, zero);
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
    short_n = (size_t)m->nbuckets * (size_t)m->nlanes;
    long_n = (size_t)m->long_buckets;
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
    m->vb = vb;
    m->vb_size = vb_size;
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
    /* Section 11.8 intent: hash the dictionary once
     * into a table snapshot and copy it (memcpy)
     * into each worker's table at block start; the
     * loop below is the per-worker fill from that
     * snapshot layout. */
    m->is_binary = mf_classify_binary(vb, vb_size);
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
        mf_insert_pos(m, vb, pos);
    }
}

zgec_match zgec_matcher_find(zgec_matcher *m, const uint8_t *vb, size_t ip,
                             uint32_t rep0, uint32_t rep1,
                             uint32_t min_len, uint32_t max_len)
{
    zgec_match best;
    uint32_t min_match;
    uint32_t hash_min;
    uint32_t cap;
    uint32_t nreps;
    uint32_t i;
    size_t avail;
    uint32_t reps[2];

    best.offset = 0;
    best.length = 0;
    if (!m || !vb || ip >= m->vb_size) {
        return best;
    }

    min_match = (min_len < 3u) ? 3u : min_len;
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

    /* 1. Repeat offsets, checked first (fast checks
     * rep0 only; main/high check rep0 and rep1). */
    reps[0] = rep0;
    reps[1] = rep1;
    nreps = (m->tier == ZGEC_TIER_FAST) ? 1u : 2u;
    for (i = 0; i < nreps; i++) {
        uint32_t d = reps[i];
        uint32_t len;
        if (d == 0u || (size_t)d > ip || (size_t)d > m->vb_capacity) {
            continue;
        }
        len = mf_match_len(vb, ip, d, cap);
        if (len >= min_match) {
            mf_candidate(d, len, &best);
        }
    }

    /* 2. Long table: 8-byte hash, one entry per
     * bucket, tag-checked before any length work. */
    if (ip + (size_t)ZGEC_MF_LONG_BYTES <= m->vb_size) {
        uint32_t hl = mf_hash8(vb, ip);
        uint32_t lb = hl & (m->long_buckets - 1u);
        const uint32_t *le = &m->long_tab[(size_t)lb];
        uint32_t e;
        uint32_t ltag;
        uint32_t etag;
        uint32_t pos;
        uint32_t d;
        uint32_t len;
        mf_prefetch_bucket((const void *)le);
        e = *le;
        ltag = (hl >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        etag = (e >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
        if (e != 0u && etag == ltag) {
            pos = mf_pos(e);
            if ((size_t)pos < ip) {
                d = (uint32_t)(ip - (size_t)pos);
                if ((size_t)d <= m->vb_capacity && hash_min <= cap) {
                    len = mf_match_len(vb, ip, d, cap);
                    if (len >= hash_min) {
                        mf_candidate(d, len, &best);
                    }
                }
            }
        }
    }

    /* 3. Short bucket probe, newest entry first. The
     * tags compare as one SIMD operation (16 tags in
     * one 128-bit compare on x86); here the scalar
     * loop builds the hit mask the same way. The
     * first hits are extracted with tzcnt plus
     * clear-lowest-bit; an empty mask yields zero
     * hits through the loop bound, so a padded dummy
     * slot is selected implicitly and no
     * data-dependent branch is needed. */
    {
        size_t need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                                   : (size_t)ZGEC_MF_HASH_BYTES;
        if (ip + need <= m->vb_size && hash_min <= cap) {
            uint32_t h = mf_hash_short(m, vb, ip);
            uint32_t bucket = h & (m->nbuckets - 1u);
            const uint32_t *b = &m->short_tab[(size_t)bucket * (size_t)m->nlanes];
            uint32_t tag = (h >> ZGEC_MF_TAG_SHIFT) & ZGEC_MF_TAG_MASK;
            uint32_t mask;
            uint32_t head;
            uint32_t nmatch;
            uint32_t want;
            uint32_t nhit;
            uint32_t k;
            mf_prefetch_bucket((const void *)b);
            mask = mf_bucket_hits(b, tag, m->nlanes);
            nmatch = (uint32_t)__builtin_popcount((unsigned)mask);
            want = (m->tier == ZGEC_TIER_HIGH) ? ZGEC_MF_PROBE_DEPTH_HIGH
                                               : ZGEC_MF_PROBE_DEPTH;
            nhit = (nmatch < want) ? nmatch : want;
            head = (uint32_t)m->short_head[bucket];
            /* The live entries walk backwards from the newest lane: the
             * ring head points one past the most recent write. */
            for (k = 0; k < m->nlanes && nhit > 0; k++) {
                uint32_t lane = (head + m->nlanes - 1u - k) & (m->nlanes - 1u);
                uint32_t entry;
                uint32_t pos;
                uint32_t d;
                uint32_t len;
                if (!(mask & (1u << lane))) continue;
                nhit--;
                entry = b[lane];
                pos = mf_pos(entry);
                if ((size_t)pos >= ip) {
                    continue;
                }
                d = (uint32_t)(ip - (size_t)pos);
                if ((size_t)d > m->vb_capacity) {
                    continue;
                }
                len = mf_match_len(vb, ip, d, cap);
                if (len >= hash_min) {
                    mf_candidate(d, len, &best);
                }
            }
        }
    }
    return best;
}

void zgec_matcher_insert(zgec_matcher *m, const uint8_t *vb, size_t ip)
{
    size_t need;
    if (!m || !vb || !m->short_tab || !m->long_tab) {
        return;
    }
    /* The position must fit the 24-bit field; the
     * short hash reads 5 bytes (4 for binary). */
    need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN : (size_t)ZGEC_MF_HASH_BYTES;
    if (ip + need > m->vb_size || ip >= (size_t)ZGEC_MF_POS_LIMIT) {
        return;
    }
    mf_insert_pos(m, vb, ip);
}

void zgec_matcher_insert_match(zgec_matcher *m, const uint8_t *vb, size_t start, size_t len)
{
    if (!m || !vb || !m->short_tab || !m->long_tab || len == 0) {
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
     * parse drives inserts); inside a match the fast
     * and main tiers insert 2-3 sampled positions,
     * the high tier inserts every position. */
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
            mf_insert_pos(m, vb, pos);
        }
    } else {
        /* Sampled: evenly spaced, distinct positions across the match
         * (never more than the match length, so no duplicate inserts),
         * all within the virtual buffer. */
        size_t nsamp = (m->tier == ZGEC_TIER_FAST)
                           ? (size_t)ZGEC_MF_MATCH_SAMPLES_FAST
                           : (size_t)ZGEC_MF_MATCH_SAMPLES;
        size_t t;
        size_t need = m->is_binary ? (size_t)ZGEC_MF_HASH_BYTES_BIN
                                   : (size_t)ZGEC_MF_HASH_BYTES;
        if (nsamp > len) nsamp = len;
        if (nsamp < 1u) nsamp = 1u;
        for (t = 0; t < nsamp; t++) {
            size_t pos = start + (len - 1u) * t /
                                    (nsamp > 1u ? nsamp - 1u : 1u);
            if (pos < m->vb_size && pos + need <= m->vb_size) {
                mf_insert_pos(m, vb, pos);
            }
        }
    }
}
