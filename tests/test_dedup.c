#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "zgec.h"
#include "zgec_lit.h"
#include "zgec_seq.h"
#include "../src/zgec_internal.h"

/* Regression tests for the deduplicated shared helpers (see
 * docs/architecture.md). Same custom style as the other harnesses:
 * CHECK tallies failures, main returns 1 on any failure. */

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static void test_normalize(void)
{
    /* Empty histogram -> all mass on symbol 0, sums to S. */
    int16_t counts[66];
    uint32_t hist[66];
    memset(hist, 0, sizeof(hist));
    CHECK(zgec_normalize_counts(counts, hist, 66, 10) == 1, "normalize empty ok");
    {
        int sum = 0;
        for (int s = 0; s < 66; s++) sum += (counts[s] < 0) ? 1 : (int)counts[s];
        CHECK(sum == 1024, "normalize empty sums to S");
        CHECK(counts[0] == 1024, "normalize empty mass on 0");
    }
    /* Two symbols share evenly. */
    memset(hist, 0, sizeof(hist));
    hist[0] = 100;
    hist[1] = 100;
    CHECK(zgec_normalize_counts(counts, hist, 66, 10) == 1, "normalize even ok");
    {
        int sum = 0;
        for (int s = 0; s < 66; s++) sum += (counts[s] < 0) ? 1 : (int)counts[s];
        CHECK(sum == 1024, "normalize even sums to S");
        CHECK(counts[0] > 0 && counts[1] > 0, "normalize even nonzero");
        CHECK(counts[2] == 0, "normalize even unobserved zero");
    }
    /* More observed symbols than states -> no representation. */
    {
        uint32_t big[66];
        for (int s = 0; s < 66; s++) big[s] = 1;
        CHECK(zgec_normalize_counts(counts, big, 66, 5) == 0, "normalize overfull rejected");
    }
    /* Bad AL rejected. */
    CHECK(zgec_normalize_counts(counts, hist, 66, 4) == 0, "normalize AL low rejected");
    CHECK(zgec_normalize_counts(counts, hist, 66, 12) == 0, "normalize AL high rejected");
    printf("normalize ok\n");
}

static void test_lane_geom(void)
{
    /* Starts agree with the legacy helper and lanes partition Z. */
    for (size_t n = 0; n < 40; n++) {
        size_t s1[8], s2[8], len[8];
        zgec_lit_lane_starts(s1, n);
        zgec_lit_lane_geom(s2, len, n);
        CHECK(memcmp(s1, s2, sizeof(s1)) == 0, "lane starts agree");
        {
            size_t total = 0;
            for (unsigned lane = 0; lane < 8; lane++) total += len[lane];
            if (total != n) { CHECK(0, "lane lens sum to n"); break; }
        }
        for (unsigned lane = 1; lane < 8; lane++) {
            if (s2[lane] < s2[lane - 1] + len[lane - 1]) {
                CHECK(0, "lane contiguity");
                break;
            }
        }
    }
    /* NULL tolerance. */
    zgec_lit_lane_geom(NULL, NULL, 16);
    printf("lane ok\n");
}

static void test_sentinel(void)
{
    uint8_t ok[4] = { 1, 2, 3, 4 };
    uint8_t bad[4] = { 1, 2, 3, 0 };
    CHECK(zgec_check_stream_sentinel(ok, sizeof(ok)) == ZGEC_OK, "sentinel ok");
    CHECK(zgec_check_stream_sentinel(bad, sizeof(bad)) == ZGEC_ERR_BITSTREAM_SENTINEL,
          "sentinel zero rejected");
    CHECK(zgec_check_stream_sentinel(ok, 0) == ZGEC_ERR_STREAM_SIZE, "sentinel empty rejected");
    CHECK(zgec_check_stream_sentinel(NULL, 4) == ZGEC_ERR_TRUNCATED, "sentinel null rejected");
    printf("sentinel ok\n");
}

static void test_shuffle_count(void)
{
    /* n=10, N=3 -> rows=3, rem=1 -> lengths 4,3,3. */
    CHECK(zgec_filter_shuffle_count(3, 1, 0) == 4, "shuffle col 0");
    CHECK(zgec_filter_shuffle_count(3, 1, 1) == 3, "shuffle col 1");
    CHECK(zgec_filter_shuffle_count(3, 1, 2) == 3, "shuffle col 2");
    /* Filter round-trips still hold through the shared lengths. */
    {
        uint8_t src[16], fwd[16], back[16];
        for (unsigned i = 0; i < 16u; i++) src[i] = (uint8_t)(i * 7u + 1u);
        CHECK(zgec_filter_apply(fwd, src, sizeof(src), ZGEC_FILTER_SHUFFLE, 4) == ZGEC_OK,
              "shuffle apply");
        memcpy(back, fwd, sizeof(back));
        CHECK(zgec_filter_inverse(back, sizeof(back), ZGEC_FILTER_SHUFFLE, 4) == ZGEC_OK,
              "shuffle inverse");
        CHECK(memcmp(back, src, sizeof(src)) == 0, "shuffle round-trip");
    }
    printf("shuffle ok\n");
}

static void test_runstart(void)
{
    uint8_t rs[10];
    uint32_t ll[2] = { 3, 4 };
    /* 3 + 4 = 7, tail 3 -> starts at 0, 3, 7. */
    CHECK(zgec_lit_runstart(rs, 10, ll, 2) == ZGEC_OK, "runstart ok");
    CHECK(zgec_rs_get(rs, 0) == 1 && zgec_rs_get(rs, 3) == 1 &&
          zgec_rs_get(rs, 7) == 1, "runstart bits");
    CHECK(zgec_rs_get(rs, 1) == 0 && zgec_rs_get(rs, 4) == 0, "runstart gaps");
    /* Over-sum rejected, buffer untouched is checked by caller contract. */
    {
        uint32_t bad[1] = { 11 };
        CHECK(zgec_lit_runstart(rs, 10, bad, 1) == ZGEC_ERR_LL_SUM, "runstart over-sum");
    }
    /* Empty literals: all-zero lengths ok. */
    CHECK(zgec_lit_runstart(NULL, 0, NULL, 0) == ZGEC_OK, "runstart empty");
    {
        uint32_t nz[1] = { 1 };
        CHECK(zgec_lit_runstart(NULL, 0, nz, 1) == ZGEC_ERR_LL_SUM, "runstart empty nonzero");
    }
    printf("runstart ok\n");
}

static void test_class_map_shared(void)
{
    uint8_t map[64], packed[24], back[64];
    for (int i = 0; i < 64; i++) map[i] = (uint8_t)(i % 4);
    zgec_class_map_encode(packed, map);
    CHECK(zgec_class_map_decode(back, packed, 4) == ZGEC_OK, "class map round-trip");
    CHECK(memcmp(back, map, 64) == 0, "class map identical");
    /* Entry >= k rejected, same rule block params rely on. */
    packed[0] |= 0x07u;
    CHECK(zgec_class_map_decode(back, packed, 4) != ZGEC_OK, "class map >=k rejected");
    printf("classmap ok\n");
}

static void test_cpu_count(void)
{
    /* zgec_cpu_count has three implementations (GetSystemInfo on Windows,
     * sysctl on Darwin, sysconf elsewhere). This is the only in-tree
     * caller, so it is also what compiles and runs the Darwin and POSIX
     * arms on CI. */
    unsigned n = zgec_cpu_count();
    CHECK(n >= 1u, "cpu count >= 1");
    CHECK(n <= 1024u, "cpu count <= 1024");
    printf("cpucount ok (%u)\n", n);
}

static void test_reps_shared(void)
{
    zgec_reps r;
    zgec_reps_init(&r);
    /* Encode then resolve round-trips and applies the MTF update. */
    for (uint32_t d = 1; d < 20; d++) {
        uint32_t ob = zgec_reps_encode(&r, d);
        uint32_t got = zgec_reps_resolve(&r, ob);
        if (got != d) { CHECK(0, "reps round-trip"); break; }
    }
    printf("reps ok\n");
}

int main(void)
{
    test_normalize();
    test_lane_geom();
    test_sentinel();
    test_shuffle_count();
    test_runstart();
    test_class_map_shared();
    test_cpu_count();
    test_reps_shared();
    if (fails == 0) printf("ALL DEDUP CHECKS PASSED\n");
    else printf("FAILURES %d\n", fails);
    return fails ? 1 : 0;
}
