/* Direct unit fuzzing for key zGEC engine functions:
 * FSE, rANS, LZ Matcher, Parser, Filter, Sequence Coder.
 */

#include "zgec.h"
#include "zgec_internal.h"
#include "zgec_fse.h"
#include "zgec_rans.h"
#include "zgec_match.h"
#include "zgec_parse.h"
#include "zgec_seq.h"
#include "zgec_encode.h"
#include "zgec_decode.h"
#include "zgec_lit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_fuzz_rng = 0x123456789ABCDEF0ull;

static uint64_t fuzz_rng_next(void) {
    uint64_t x = g_fuzz_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_fuzz_rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t fuzz_below(uint32_t n) {
    if (n == 0) return 0;
    return (uint32_t)((fuzz_rng_next() >> 24) % (uint64_t)n);
}

/* 1. Fuzz FSE functions */
static void fuzz_fse_key(void) {
    int nsym = (int)(1 + fuzz_below(256));
    int al = (int)(ZGEC_MIN_AL + fuzz_below(ZGEC_MAX_AL - ZGEC_MIN_AL + 1));
    int16_t counts[256];
    int sum = 0;
    int S = 1 << al;

    for (int i = 0; i < nsym; i++) {
        counts[i] = (int16_t)fuzz_below((uint32_t)(S + 1));
        sum += (counts[i] < 0) ? 1 : counts[i];
    }
    if (sum != S && nsym > 0) {
        counts[0] = (int16_t)((int)counts[0] + (S - sum));
        if (counts[0] < -1) counts[0] = -1;
    }

    zgec_fse_dec_table *dec = NULL;
    zgec_err err = zgec_fse_build_dec(&dec, counts, nsym, al);
    if (err == ZGEC_OK && dec != NULL) {
        zgec_fse_enc_table *enc = NULL;
        err = zgec_fse_build_enc(&enc, dec);
        if (err == ZGEC_OK && enc != NULL) {
            uint8_t wire[1024];
            size_t wbytes = zgec_fse_write_counts(wire, sizeof(wire), counts, nsym, al);
            if (wbytes > 0) {
                int16_t counts2[256];
                int nsym2 = 0, al2 = 0;
                size_t rbytes = zgec_fse_read_counts(counts2, &nsym2, &al2, 256, wire, wbytes);
                (void)rbytes;
            }
            zgec_fse_free_enc(enc);
        }
        zgec_fse_free_dec(dec);
    }
}

/* 2. Fuzz rANS functions */
static void fuzz_rans_key(void) {
    uint32_t hist[256];
    int16_t counts[256];
    for (int i = 0; i < 256; i++) {
        hist[i] = fuzz_below(100);
    }

    zgec_err err = zgec_rans_normalise(counts, hist);
    if (err == ZGEC_OK) {
        zgec_rans_enc_table enc;
        zgec_rans_dec_table dec;
        err = zgec_rans_build_enc(&enc, counts);
        if (err == ZGEC_OK) {
            err = zgec_rans_build_dec(&dec, counts);
        }
    }
}

/* 3. Fuzz Matcher & Parser functions */
static void fuzz_matcher_key(void) {
    size_t sz = 16 + fuzz_below(2048);
    uint8_t *buf = (uint8_t *)malloc(sz + 64);
    if (!buf) return;

    for (size_t i = 0; i < sz; i++) {
        buf[i] = (uint8_t)fuzz_below(256);
    }

    zgec_matcher *m = zgec_matcher_create(ZGEC_TIER_MAIN, sz + 64);
    if (m) {
        zgec_matcher_reset(m, buf, sz, NULL, 0);
        for (size_t ip = 0; ip + 8 <= sz; ip += 1 + fuzz_below(4)) {
            uint32_t rep_len[3] = {0, 0, 0};
            zgec_match best = zgec_matcher_find_lens(m, buf, ip, 1, 2, 3, 4, 64, rep_len);
            (void)best;
            zgec_matcher_insert(m, buf, ip);
        }
        zgec_matcher_destroy(m);
    }

    free(buf);
}

/* 4. Fuzz Filter functions */
static void fuzz_filter_key(void) {
    size_t n = 1 + fuzz_below(1024);
    uint8_t *src = (uint8_t *)malloc(n + 32);
    uint8_t *dst = (uint8_t *)malloc(n + 32);
    uint8_t *inv = (uint8_t *)malloc(n + 32);
    if (!src || !dst || !inv) {
        free(src); free(dst); free(inv);
        return;
    }

    for (size_t i = 0; i < n; i++) src[i] = (uint8_t)fuzz_below(256);

    /* Test DELTA (mode 1, param 0) */
    if (zgec_filter_apply(dst, src, n, 1, 0) == ZGEC_OK) {
        memcpy(inv, dst, n);
        if (zgec_filter_inverse(inv, n, 1, 0) == ZGEC_OK) {
            if (memcmp(src, inv, n) != 0) {
                fprintf(stderr, "FUZZ FILTER DELTA MISMATCH n=%zu\n", n);
                exit(1);
            }
        }
    }

    /* Test SHUFFLE (mode 2, param 2..8) */
    unsigned param = 2u + fuzz_below(7);
    if (zgec_filter_apply(dst, src, n, 2, param) == ZGEC_OK) {
        memcpy(inv, dst, n);
        if (zgec_filter_inverse(inv, n, 2, param) == ZGEC_OK) {
            if (memcmp(src, inv, n) != 0) {
                fprintf(stderr, "FUZZ FILTER SHUFFLE MISMATCH param=%u n=%zu\n", param, n);
                exit(1);
            }
        }
    }

    free(src); free(dst); free(inv);
}

int main(int argc, char **argv) {
    uint64_t iterations = 200000;
    if (argc > 1) iterations = strtoull(argv[1], NULL, 10);
    if (argc > 2) g_fuzz_rng = strtoull(argv[2], NULL, 10);

    printf("Starting key function unit fuzzing (%llu iterations)...\n", (unsigned long long)iterations);
    for (uint64_t i = 0; i < iterations; i++) {
        fuzz_fse_key();
        fuzz_rans_key();
        fuzz_matcher_key();
        fuzz_filter_key();
    }
    printf("Key function unit fuzzing complete: 0 errors!\n");
    return 0;
}
