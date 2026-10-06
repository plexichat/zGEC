#define _POSIX_C_SOURCE 199309L
#include "zgec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/*
 * Measurement harness.
 *
 *   zgec_bench [file] [reps]
 *
 * With no file argument a synthetic mixed corpus is generated (text-like
 * lines, a low-entropy run, and a pseudo-random region) instead of the
 * period-256 ramp this used to build, so the numbers mean something on
 * data with structure.
 *
 * Every figure is the BEST of `reps` (default 5) runs, not the mean: the
 * first run of a configuration always pays for page faults, table
 * zeroing and thread creation, and averaging that in understates the
 * steady-state rate for small inputs. One warm-up encode and decode of
 * each configuration runs before its timing loop, so no measured run
 * includes cold-start effects.
 *
 * Throughput is reported from the ORIGINAL (uncompressed) byte count in
 * both directions, so the encode and decode rates are directly
 * comparable with each other and with other tools.
 *
 * Time comes from CLOCK_MONOTONIC via _POSIX_C_SOURCE 199309L, the same
 * source this file already used: MinGW-w64 provides it through
 * winpthreads and the target builds and links with it, so no Windows
 * specific fallback is needed.
 */

#define BENCH_DEFAULT_SIZE ((size_t)10u * (size_t)1024u * (size_t)1024u)
#define BENCH_DEFAULT_REPS 5

static double bench_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double bench_mbps(size_t bytes, double sec)
{
    if (sec <= 0.0) return 0.0;
    return ((double)bytes / (1024.0 * 1024.0)) / sec;
}

/* xorshift64*: deterministic, so a run is reproducible. */
static uint64_t bench_rng(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static const char *const bench_words[16] = {
    "struct ", "return ", "value", "context", "static inline ",
    "unsigned ", "if (", ") {", "} else {", "/* note */ ",
    "zgec_", "buffer", "offset", "length", "segment", "sequence "
};

/* Three regions: text-like (repeating but varying, so not a fixed
 * period), a low-entropy run, then pseudo-random bytes. */
static void bench_gen_mixed(uint8_t *d, size_t n)
{
    size_t i = 0;
    size_t a = n / 3u;
    size_t b = (n / 3u) * 2u;
    uint64_t st = 0x9E3779B97F4A7C15ull;
    unsigned wi = 0;
    unsigned indent = 0;

    while (i < a && i < n) {
        const char *w = bench_words[wi & 15u];
        size_t len = strlen(w);
        size_t k;
        if (indent != 0u && (wi & 7u) == 0u) {
            size_t pad = (size_t)indent * 4u;
            for (k = 0; k < pad && i < a && i < n; k++) d[i++] = (uint8_t)' ';
        }
        for (k = 0; k < len && i < a && i < n; k++)
            d[i++] = (uint8_t)w[k];
        wi++;
        if ((wi & 31u) == 0u) {
            if (i < a && i < n) d[i++] = (uint8_t)'\n';
            indent = (unsigned)(bench_rng(&st) & 3u);
        } else if (i < a && i < n) {
            d[i++] = (uint8_t)' ';
        }
    }

    {
        uint8_t v = 0u;
        while (i < b && i < n) {
            size_t run = (size_t)(bench_rng(&st) & 63u) + 8u;
            size_t k;
            for (k = 0; k < run && i < b && i < n; k++) d[i++] = v;
            if ((bench_rng(&st) & 7u) == 0u) v = (uint8_t)(v + 1u);
        }
    }

    while (i < n) {
        uint64_t x = bench_rng(&st);
        unsigned k;
        for (k = 0; k < 8u && i < n; k++)
            d[i++] = (uint8_t)(x >> (8u * k));
    }
}

static uint8_t *bench_read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;
    size_t got;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = (uint8_t *)malloc((size_t)sz + 1u);
    if (!buf) { fclose(f); return NULL; }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *size_out = got;
    return buf;
}

/* Encode `reps` times with one configuration and keep the best time. The
 * frame from the warm-up run is returned for the decode side, so the two
 * directions measure the same compressed bytes. */
static int bench_encode(const uint8_t *src, size_t n, int n_threads, int reps,
                        int block_log2,
                        uint8_t **frame_out, size_t *frame_size_out,
                        double *best_sec_out)
{
    zgec_params p;
    zgec_encoder *e;
    uint8_t *frame = NULL;
    size_t frame_size = 0;
    double best = 0.0;
    int i;

    zgec_params_default(&p);
    p.n_threads = n_threads;
    if (block_log2 != 0) p.block_log2 = block_log2;
    e = zgec_encoder_create(&p);
    if (!e) { fprintf(stderr, "bench: encoder allocation failed\n"); return 0; }

    /* Warm-up, out of band: allocates tables, faults in pages, spawns
     * the thread pool. Its frame is the one the decoder will read. */
    if (zgec_encode_frame(e, src, n, &frame, &frame_size) != ZGEC_OK) {
        fprintf(stderr, "bench: encode failed\n");
        zgec_encoder_destroy(e);
        return 0;
    }

    for (i = 0; i < reps; i++) {
        uint8_t *tmp = NULL;
        size_t tmp_size = 0;
        double t0, t1, sec;
        t0 = bench_now_sec();
        if (zgec_encode_frame(e, src, n, &tmp, &tmp_size) != ZGEC_OK) {
            fprintf(stderr, "bench: encode failed\n");
            zgec_free(tmp);
            zgec_free(frame);
            zgec_encoder_destroy(e);
            return 0;
        }
        t1 = bench_now_sec();
        sec = t1 - t0;
        if (i == 0 || sec < best) best = sec;
        zgec_free(tmp);
    }
    zgec_encoder_destroy(e);
    *frame_out = frame;
    *frame_size_out = frame_size;
    *best_sec_out = best;
    return 1;
}

static int bench_decode(const uint8_t *frame, size_t frame_size,
                        size_t expect, int n_threads, int reps,
                        double *best_sec_out, uint8_t *verify_buf)
{
    zgec_limits lim;
    zgec_decoder *d;
    double best = 0.0;
    int i;

    memset(&lim, 0, sizeof(lim));
    lim.n_threads = n_threads;
    d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
    if (!d) { fprintf(stderr, "bench: decoder allocation failed\n"); return 0; }

    /* Warm-up, out of band. */
    {
        uint8_t *out = NULL;
        size_t out_size = 0;
        if (zgec_decode_frame(d, frame, frame_size, &out, &out_size) !=
            ZGEC_OK) {
            fprintf(stderr, "bench: decode failed\n");
            zgec_decoder_destroy(d);
            return 0;
        }
        if (out_size != expect) {
            fprintf(stderr, "bench: decode size %zu, expected %zu\n",
                    out_size, expect);
            zgec_free(out);
            zgec_decoder_destroy(d);
            return 0;
        }
        if (verify_buf != NULL) memcpy(verify_buf, out, out_size);
        zgec_free(out);
    }

    for (i = 0; i < reps; i++) {
        uint8_t *out = NULL;
        size_t out_size = 0;
        double t0, t1, sec;
        t0 = bench_now_sec();
        if (zgec_decode_frame(d, frame, frame_size, &out, &out_size) !=
            ZGEC_OK) {
            fprintf(stderr, "bench: decode failed\n");
            zgec_free(out);
            zgec_decoder_destroy(d);
            return 0;
        }
        t1 = bench_now_sec();
        sec = t1 - t0;
        if (i == 0 || sec < best) best = sec;
        zgec_free(out);
    }
    zgec_decoder_destroy(d);
    *best_sec_out = best;
    return 1;
}

static int bench_parse_reps(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (s == NULL || *s == '\0') return 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return 0;
    if (v < 1 || v > 1000) return 0;
    *out = (int)v;
    return 1;
}

/* Optional block size, so the block-size question can be measured rather
 * than argued: the default is whatever the level preset says. */
static int bench_parse_log2(const char *s, int *out)
{
    char *end = NULL;
    long v;
    if (s == NULL || *s == '\0') return 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return 0;
    if (v < (long)ZGEC_BLOCK_LOG2_MIN || v > (long)ZGEC_BLOCK_LOG2_MAX)
        return 0;
    *out = (int)v;
    return 1;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : NULL;
    int reps = BENCH_DEFAULT_REPS;
    int block_log2 = 0;   /* 0: the level preset decides */
    size_t in_size = 0;
    uint8_t *in = NULL;
    uint8_t *frame = NULL;   /* library buffer: zgec_free */
    uint8_t *frame1 = NULL;  /* library buffer: zgec_free */
    size_t frame_size = 0;
    size_t frame1_size = 0;
    uint8_t *verify = NULL;  /* harness buffer: free */
    double enc1 = 0.0, encN = 0.0, dec1 = 0.0, decN = 0.0;
    int ok = 1;

    if (argc > 2 && !bench_parse_reps(argv[2], &reps)) {
        fprintf(stderr, "bench: reps must be 1..1000\n");
        return 1;
    }
    if (argc > 3 && !bench_parse_log2(argv[3], &block_log2)) {
        fprintf(stderr, "bench: block-log2 must be %d..%d\n",
                (int)ZGEC_BLOCK_LOG2_MIN, (int)ZGEC_BLOCK_LOG2_MAX);
        return 1;
    }

    if (path != NULL) {
        in = bench_read_file(path, &in_size);
        if (!in) {
            fprintf(stderr, "bench: cannot read %s\n", path);
            return 1;
        }
    } else {
        in_size = BENCH_DEFAULT_SIZE;
        in = (uint8_t *)malloc(in_size);
        if (!in) { fprintf(stderr, "bench: allocation failed\n"); return 1; }
        bench_gen_mixed(in, in_size);
    }

    printf("zGEC benchmark\n");
    printf("  input      : %s (%zu bytes)\n",
           (path != NULL) ? path : "synthetic mixed corpus", in_size);
    printf("  reps       : best of %d\n", reps);
    printf("  block log2 : %d\n", block_log2);

    verify = (uint8_t *)malloc(in_size);
    if (!verify) { fprintf(stderr, "bench: allocation failed\n"); free(in); return 1; }

    /* The multi-thread encode runs first: its frame is the one both
     * decode configurations read, so one compression feeds all three
     * timings and the reported size is a single frame's. */
    if (!bench_encode(in, in_size, 0, reps, block_log2, &frame, &frame_size,
                      &encN)) {
        ok = 0;
        goto done;
    }
    if (!bench_decode(frame, frame_size, in_size, 0, reps, &decN, verify)) {
        ok = 0;
        goto done;
    }
    if (memcmp(verify, in, in_size) != 0) {
        printf("  round-trip : MISMATCH\n");
        ok = 0;
        goto done;
    }
    printf("  round-trip : ok\n");
    printf("  compressed : %zu bytes   ratio %.4f\n",
           frame_size, ((double)in_size) / ((double)frame_size));

    if (!bench_decode(frame, frame_size, in_size, 1, reps, &dec1, NULL)) {
        ok = 0;
        goto done;
    }
    if (!bench_encode(in, in_size, 1, reps, block_log2, &frame1, &frame1_size,
                      &enc1)) {
        ok = 0;
        goto done;
    }
    if (frame1_size != frame_size) {
        printf("  encode     : size differs with thread count (%zu vs %zu)\n",
               frame1_size, frame_size);
        ok = 0;
        goto done;
    }

    printf("  encode 1T  : %9.2f ms  %9.1f MB/s (input bytes)\n",
           enc1 * 1000.0, bench_mbps(in_size, enc1));
    printf("  encode NT  : %9.2f ms  %9.1f MB/s (input bytes)\n",
           encN * 1000.0, bench_mbps(in_size, encN));
    printf("  decode 1T  : %9.2f ms  %9.1f MB/s (input bytes)\n",
           dec1 * 1000.0, bench_mbps(in_size, dec1));
    printf("  decode NT  : %9.2f ms  %9.1f MB/s (input bytes)\n",
           decN * 1000.0, bench_mbps(in_size, decN));
    printf("enc 1T: %.1f MB/s   enc NT: %.1f MB/s   "
           "dec 1T: %.1f MB/s   dec NT: %.1f MB/s   ratio %.4f\n",
           bench_mbps(in_size, enc1), bench_mbps(in_size, encN),
           bench_mbps(in_size, dec1), bench_mbps(in_size, decN),
           ((double)in_size) / ((double)frame_size));

done:
    zgec_free(frame1);
    zgec_free(frame);
    free(verify);
    free(in);
    return ok ? 0 : 1;
}
