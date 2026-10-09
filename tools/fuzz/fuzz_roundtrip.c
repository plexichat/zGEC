/* Fuzz target: encoder/decoder round-trip, and decoder hardening.
 *
 * One core, two binaries, and no macro deciding between them:
 *
 *   zgec_fuzz            standalone and deterministic. Builds its own
 *                        inputs from a seeded xorshift64 PRNG, so it needs
 *                        no corpus and no build flags beyond the library.
 *                        This file, plus fuzz_standalone_main.c, which is
 *                        the only place `main` is defined.
 *   zgec_fuzz_libfuzzer  this file on its own, with Clang's
 *                        -fsanitize=fuzzer,address,undefined. libFuzzer
 *                        supplies the bytes and searches them -- and it
 *                        supplies `main` as well, which is why the
 *                        standalone main lives in another file instead of
 *                        behind a macro that every build has to remember.
 *
 * What it asserts (a failure is reported on stderr and turned into a
 * non-zero exit, or a libFuzzer crash):
 *
 *   1. Round-trip. For every parameter set the level presets can express,
 *      encoding valid source data must succeed and decoding the frame must
 *      reproduce the source byte for byte. The first input bytes seed the
 *      PRNG and the rest is the payload, so the fuzzer explores the
 *      parameter space and the data space together instead of only one.
 *
 *   2. Decoder hardening. Arbitrary bytes must never crash the decoder: the
 *      case drives zgec_decode_frame at both conformance levels, under
 *      randomised memory limits, through zgec_decode_block and
 *      zgec_decode_block_index. Every zgec_err is acceptable here; a crash,
 *      an ASan report or a leak is not.
 *
 *   3. Integrity. When the frame carries per-block checksums, a single-byte
 *      corruption or a truncated prefix must not decode to *different*
 *      data: the decode either fails or reproduces a prefix of the source.
 *      Blocks without checksums are covered by no integrity check at all,
 *      so for those the damaged cases assert memory safety only -- a
 *      corrupted literal can legally decode to different bytes.
 *
 * Sizes are capped so no single case can run away, and every encoder,
 * decoder and buffer is released on every path.
 */

#include "zgec.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A case never asks the encoder for more than this, and never synthesises a
 * larger standalone input. libFuzzer is additionally capped with -max_len by
 * the caller. */
#define FUZZ_MAX_SRC ((size_t)1u << 20)

/* The per-case parameter entropy is the first bytes of the input; the rest
 * is payload. */
#define FUZZ_HDR_BYTES 16u

/* How many corruptions and truncations to try per successful frame. */
#define FUZZ_CORRUPTIONS 4u
#define FUZZ_TRUNCATIONS 4u

static uint64_t g_rng;

/* Counters, printed by the standalone entry point and accumulated across
 * libFuzzer cases. */
static unsigned long long g_cases;
static unsigned long long g_roundtrips;
static unsigned long long g_rejected;
static unsigned long long g_encode_failures;
static unsigned long long g_failures;

/* ---- deterministic PRNG (xorshift64*), never rand() or time() ---- */

static uint64_t fuzz_rng_next(void)
{
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static uint32_t fuzz_below(uint32_t n)
{
    return (uint32_t)((fuzz_rng_next() >> 24) % (uint64_t)n);
}

static int fuzz_chance(uint32_t pct)
{
    return fuzz_below(100u) < pct;
}

/* ---- parameters ----
 *
 * The table mirrors the level presets of src/cli.c, which is what a user
 * gets from -l 4..10 and 12: tier and the feature switches, at the 2 MiB
 * section 12.3. Keeping a copy here means the fuzzer covers the shipped
 * ladder rather than an arbitrary set; the jitter below reaches the
 * combinations the ladder does not contain. */

typedef struct {
    int tier;
    int ctx;
    int sub;
    int cond;
    int litref;
    int dicts;
    int filter;
    int ck;
} fuzz_level;

static const fuzz_level fuzz_levels[8] = {
    { ZGEC_TIER_FAST, 0, 0, 0, 0, 0, 0, 0 },
    { ZGEC_TIER_FAST, 1, 0, 0, 0, 0, 0, 0 },
    { ZGEC_TIER_MAIN, 0, 0, 0, 0, 0, 0, 0 },
    { ZGEC_TIER_MAIN, 1, 0, 0, 0, 0, 0, 0 },
    { ZGEC_TIER_MAIN, 1, 0, 0, 1, 0, 0, 0 },
    { ZGEC_TIER_MAIN, 1, 0, 1, 1, 0, 0, 0 },
    { ZGEC_TIER_HIGH, 1, 1, 1, 1, 0, 0, 0 },
    { ZGEC_TIER_HIGH, 1, 1, 1, 1, 0, 0, 1 }
};

/* Build a parameter set from one level of the preset table plus a jitter
 * word. The dictionary knobs are varied as well: a small epoch and a small
 * dictionary cap reach the epoch and dictionary paths that a large block
 * would otherwise never enter. Every value stays inside the documented
 * ranges, so any error the encoder reports is a finding rather than bad
 * input. */
static void fuzz_build_params(zgec_params *p, uint32_t level, uint32_t jitter)
{
    const fuzz_level *l = &fuzz_levels[level % 8u];

    zgec_params_default(p);
    p->tier = (zgec_tier)l->tier;
    p->use_contexts = l->ctx;
    p->use_sublit = l->sub;
    p->use_conditioning = l->cond;
    p->use_litref = l->litref;
    p->use_dicts = l->dicts;
    p->use_filter = l->filter;
    p->block_checksums = l->ck;

    /* Off-ladder feature combinations. */
    if ((jitter & 0x01u) != 0u) p->use_filter = 1;
    if ((jitter & 0x02u) != 0u) p->use_dicts = 1;
    if ((jitter & 0x04u) != 0u) p->use_sublit = 1;
    if ((jitter & 0x08u) != 0u) p->use_conditioning = 1;
    if ((jitter & 0x10u) != 0u) p->use_contexts = 1;
    if ((jitter & 0x20u) != 0u) p->use_litref = 1;
    if ((jitter & 0x40u) != 0u) p->block_checksums = 1;
    if ((jitter & 0x80u) != 0u) p->tier = (zgec_tier)((jitter >> 8) % 3u);

    /* Block size inside the documented range: the small end gives a
     * multi-block frame, the large end a single block. */
    p->block_log2 = (int)((uint32_t)ZGEC_BLOCK_LOG2_MIN +
                          fuzz_below((uint32_t)(ZGEC_BLOCK_LOG2_MAX -
                                                ZGEC_BLOCK_LOG2_MIN + 1)));
    /* Epochs of 1..4 blocks and a 64 KiB..1 MiB dictionary cap, so epochs
     * and the size gate are exercised even on small inputs. */
    p->epoch_blocks = (int)(1u + fuzz_below(4u));
    p->max_dict_log2 = (int)(16u + fuzz_below(5u));
    p->n_threads = (int)(1u + fuzz_below(2u));
    p->lambda = (double)(jitter & 0xFFu) / 64.0;
}

/* ---- reporting helpers ---- */

static void fuzz_fail(const char *what)
{
    fprintf(stderr, "FUZZ FAILURE: %s\n", what);
    g_failures++;
}

static void fuzz_free(void *p)
{
    if (p != NULL) {
        zgec_free(p);
    }
}

/* ---- property 2: arbitrary bytes must not crash the decoder ---- */

static void fuzz_harden_decode(const uint8_t *data, size_t size)
{
    static const zgec_level levels[2] = { ZGEC_LEVEL_CORE,
                                          ZGEC_LEVEL_EXTENDED };
    size_t li;

    for (li = 0; li < 2u; li++) {
        zgec_limits lim;
        zgec_decoder *d;
        uint8_t *out = NULL;
        size_t out_size = 0;
        zgec_err err;

        memset(&lim, 0, sizeof(lim));
        /* Half the cases get explicit limits and half the defaults, which
         * is what makes the section 13 limit checks reachable. */
        if (fuzz_chance(50)) {
            lim.max_block_size = (size_t)1u << (unsigned)(16u + fuzz_below(11u));
        }
        if (fuzz_chance(50)) {
            lim.max_dict_size = (size_t)1u << (unsigned)(16u + fuzz_below(11u));
        }
        if (fuzz_chance(50)) {
            lim.max_total = (size_t)1u << (unsigned)(20u + fuzz_below(8u));
        }
        lim.n_threads = (int)(1u + fuzz_below(3u));

        d = zgec_decoder_create(levels[li], &lim);
        if (d == NULL) {
            continue; /* an allocation failure is not a finding here */
        }

        err = zgec_decode_frame(d, data, size, &out, &out_size);
        if (err != ZGEC_OK) {
            g_rejected++;
        } else if (out_size > 0u && out == NULL) {
            fuzz_fail("decode_frame reported bytes with no output buffer");
        }
        fuzz_free(out);
        out = NULL;
        out_size = 0;

        /* Random access, by offset and by index. The returned pointer is
         * internal to the decoder and must not be freed. */
        {
            const uint8_t *blk = NULL;
            size_t blk_size = 0;
            uint64_t off = fuzz_rng_next() & (((uint64_t)1u << 40) - 1u);

            err = zgec_decode_block(d, data, size, off, &blk, &blk_size);
            if (err == ZGEC_OK && (blk == NULL || blk_size == 0u)) {
                fuzz_fail("decode_block reported success with no block");
            }
            err = zgec_decode_block_index(d, data, size, fuzz_below(64u),
                                          &blk, &blk_size);
            if (err == ZGEC_OK && (blk == NULL || blk_size == 0u)) {
                fuzz_fail("decode_block_index reported success with no block");
            }
        }

        zgec_decoder_destroy(d);
    }
}

/* ---- properties 1 and 3: a valid frame round-trips, and stays honest
 * when damaged, as long as it carries checksums ---- */

/* Decode a frame and check it against the source. `checksums` says whether
 * the frame carries per-block checksums; when it does, a decode that
 * succeeds must reproduce the source, because every block's CRC-32C would
 * have to be wrong for anything else to pass. `truncated` relaxes that to
 * "a prefix of the source", which is what dropping a suffix of complete
 * blocks can honestly produce. */
static void fuzz_check_decode(const uint8_t *frame, size_t frame_size,
                              const uint8_t *src, size_t src_size,
                              int checksums, int truncated)
{
    zgec_limits lim;
    zgec_decoder *d;
    uint8_t *out = NULL;
    size_t out_size = 0;

    memset(&lim, 0, sizeof(lim));
    lim.n_threads = 1;
    d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
    if (d == NULL) {
        return;
    }
    if (zgec_decode_frame(d, frame, frame_size, &out, &out_size) == ZGEC_OK) {
        if (checksums &&
            (out == NULL || out_size > src_size ||
             memcmp(out, src, out_size) != 0)) {
            fuzz_fail(truncated
                          ? "truncated frame decoded to non-prefix data"
                          : "corrupted frame decoded to different data");
        }
        fuzz_free(out);
    }
    zgec_decoder_destroy(d);
}

/* Damage a frame we built ourselves, so the corruption starts from a
 * structure the decoder accepts. */
static void fuzz_damage_frame(const uint8_t *frame, size_t frame_size,
                              const uint8_t *src, size_t src_size,
                              int checksums)
{
    uint8_t *copy;
    size_t i;

    if (frame_size < 2u) {
        return;
    }

    /* Truncations: drop a suffix. */
    for (i = 0; i < (size_t)FUZZ_TRUNCATIONS; i++) {
        size_t cut;
        switch (i) {
        case 0: cut = 1u; break;
        case 1: cut = frame_size / 4u; break;
        case 2: cut = frame_size / 2u; break;
        default: cut = frame_size - 1u; break;
        }
        if (cut == 0u || cut >= frame_size) {
            continue;
        }
        fuzz_check_decode(frame, cut, src, src_size, checksums, 1);
    }

    /* Single-byte corruptions. The header CRC catches damage there, and a
     * checksummed block catches damage in its payload. */
    copy = (uint8_t *)malloc(frame_size);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, frame, frame_size);
    for (i = 0; i < (size_t)FUZZ_CORRUPTIONS; i++) {
        size_t pos = (i == 0u) ? 0u
                               : (size_t)fuzz_below((uint32_t)frame_size);
        uint8_t old = copy[pos];

        copy[pos] = (uint8_t)(old ^ (uint8_t)(1u << (unsigned)fuzz_below(8u)));
        fuzz_check_decode(copy, frame_size, src, src_size, checksums, 0);
        copy[pos] = old;
    }
    free(copy);
}

/* ---- the shared case body ---- */

static int fuzz_run_case(const uint8_t *data, size_t size)
{
    unsigned long long before = g_failures + g_encode_failures;
    zgec_params p;
    zgec_encoder *e;
    uint8_t *frame = NULL;
    size_t frame_size = 0;
    const uint8_t *src;
    size_t src_size;
    size_t hdr;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    zgec_err err;
    size_t k;

    g_cases++;

    /* Property 2 is unconditional: even an empty input is decoder input. */
    if (size > 0u) {
        fuzz_harden_decode(data, size);
    }

    hdr = (size < (size_t)FUZZ_HDR_BYTES) ? size : (size_t)FUZZ_HDR_BYTES;
    for (k = 0; k < hdr; k++) {
        seed = (seed ^ ((uint64_t)data[k] + 0x9E3779B97F4A7C15ull)) *
               0x100000001B3ull;
    }
    g_rng = seed | 1u;

    src = data + hdr;
    src_size = size - hdr;
    if (src_size > FUZZ_MAX_SRC) {
        src_size = FUZZ_MAX_SRC;
    }
    if (src_size == 0u) {
        /* Nothing to encode: the frame would have no blocks. */
        return (g_failures + g_encode_failures == before) ? 0 : 1;
    }

    fuzz_build_params(&p, fuzz_below(9u), (uint32_t)fuzz_rng_next());

    e = zgec_encoder_create(&p);
    if (e == NULL) {
        return (g_failures + g_encode_failures == before) ? 0 : 1;
    }
    err = zgec_encode_frame(e, src, src_size, &frame, &frame_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) {
        g_encode_failures++;
        fprintf(stderr,
                "FUZZ FAILURE: encode of %zu valid bytes failed: %s "
                "(block_log2=%d tier=%d ctx=%d sub=%d cond=%d litref=%d "
                "dicts=%d filter=%d ck=%d)\n",
                src_size, zgec_strerror(err), p.block_log2, (int)p.tier,
                p.use_contexts, p.use_sublit, p.use_conditioning,
                p.use_litref, p.use_dicts, p.use_filter, p.block_checksums);
        fuzz_free(frame);
        return 1;
    }
    if (frame == NULL || frame_size == 0u) {
        fuzz_fail("encode reported success with no frame");
        fuzz_free(frame);
        return 1;
    }

    /* The round-trip itself (property 1), plus one random-access read that
     * must agree with it. */
    {
        zgec_limits lim;
        zgec_decoder *d;
        uint8_t *out = NULL;
        size_t out_size = 0;

        memset(&lim, 0, sizeof(lim));
        lim.n_threads = 1;
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
        if (d == NULL) {
            fuzz_free(frame);
            return (g_failures + g_encode_failures == before) ? 0 : 1;
        }
        err = zgec_decode_frame(d, frame, frame_size, &out, &out_size);
        if (err != ZGEC_OK) {
            fprintf(stderr, "FUZZ FAILURE: decode of own frame failed: %s\n",
                    zgec_strerror(err));
            g_failures++;
        } else if (out_size != src_size || memcmp(out, src, src_size) != 0) {
            fprintf(stderr,
                    "FUZZ FAILURE: round-trip mismatch (%zu bytes in, "
                    "%zu bytes out)\n",
                    src_size, out_size);
            g_failures++;
        } else {
            g_roundtrips++;
        }
        fuzz_free(out);

        {
            const uint8_t *blk = NULL;
            size_t blk_size = 0;
            err = zgec_decode_block_index(d, frame, frame_size, 0u, &blk,
                                          &blk_size);
            if (err != ZGEC_OK || blk == NULL || blk_size == 0u ||
                blk_size > src_size || memcmp(blk, src, blk_size) != 0) {
                fuzz_fail("random access disagrees with the sequential scan");
            }
        }
        zgec_decoder_destroy(d);
    }

    /* Property 3. */
    fuzz_damage_frame(frame, frame_size, src, src_size, p.block_checksums);

    fuzz_free(frame);
    return (g_failures + g_encode_failures == before) ? 0 : 1;
}

/* ---- entry points ---- */

/* libFuzzer's entry point. It is present in both shapes: the standalone
 * harness simply never calls it. Nothing here defines main, so compiling
 * this file alone and linking the libFuzzer runtime -- which brings its own
 * main -- produces one definition of each entry point. */

/* Declared before its definition so -Wmissing-prototypes stays quiet. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (fuzz_run_case(data, size) != 0) {
        /* Turn a finding into a libFuzzer crash, so the input is saved and
         * the run reports the exact bytes that provoked it. */
        abort();
    }
    return 0;
}

/* ---- standalone harness ---- */

/* Synthesise a case payload: runs (RLE and long repeats), word soup (the
 * match finder's best case), incompressible bytes (RAW and filter paths),
 * or a mix of literal runs and matches. */
static void fuzz_gen(uint8_t *buf, size_t n)
{
    size_t i = 0;

    switch (fuzz_below(4u)) {
    case 0u:
        while (i < n) {
            uint8_t b = (uint8_t)fuzz_below(3u);
            size_t run = 1u + (size_t)fuzz_below(512u);
            if (run > n - i) {
                run = n - i;
            }
            memset(buf + i, (int)b, run);
            i += run;
        }
        break;
    case 1u:
        while (i < n) {
            static const char *const words[8] = {
                "struct ", "node {", " int key; ", "return 0;\n",
                "#include ", "static ", "size_t ", "};\n"
            };
            const char *w = words[fuzz_below(8u)];
            size_t wl = strlen(w);
            if (wl > n - i) {
                wl = n - i;
            }
            memcpy(buf + i, w, wl);
            i += wl;
        }
        break;
    case 2u:
        while (i < n) {
            buf[i++] = (uint8_t)(fuzz_rng_next() >> 56);
        }
        break;
    default:
        while (i < n) {
            size_t run = 1u + (size_t)fuzz_below(64u);
            if (run > n - i) {
                run = n - i;
            }
            if (fuzz_chance(50)) {
                memset(buf + i, (int)(uint8_t)fuzz_below(256u), run);
            } else {
                size_t j;
                for (j = 0; j < run; j++) {
                    buf[i + j] = (uint8_t)(fuzz_rng_next() >> 56);
                }
            }
            i += run;
        }
        break;
    }
}

/* The standalone entry point. main itself is in fuzz_standalone_main.c so
 * that this file can be compiled on its own for libFuzzer. */
int zgec_fuzz_standalone_main(int argc, char **argv);

int zgec_fuzz_standalone_main(int argc, char **argv)
{
    unsigned long long iterations = 20000ull;
    unsigned long long seed = 1ull;
    unsigned long long it;
    uint8_t *buf;

    if (argc > 1) {
        iterations = strtoull(argv[1], NULL, 10);
    }
    if (argc > 2) {
        seed = strtoull(argv[2], NULL, 10);
    }
    if (iterations == 0ull) {
        iterations = 1ull;
    }

    buf = (uint8_t *)malloc(FUZZ_MAX_SRC);
    if (buf == NULL) {
        fprintf(stderr, "fuzz: out of memory\n");
        return 2;
    }

    g_rng = seed | 1ull;

    for (it = 0; it < iterations; it++) {
        size_t n;

        /* Most cases stay small so a full run is seconds rather than
         * minutes; one in ten is large enough to cross several blocks at
         * the smallest block size. The buffer always carries the parameter
         * header plus at least one payload byte. */
        n = fuzz_chance(90) ? (size_t)fuzz_below(1024u)
                            : (size_t)fuzz_below(65536u);
        if (n < (size_t)FUZZ_HDR_BYTES + 1u) {
            n = (size_t)FUZZ_HDR_BYTES + 1u;
        }
        fuzz_gen(buf, n);
        (void)fuzz_run_case(buf, n);
    }

    free(buf);

    printf("fuzz: cases=%llu round-trips=%llu decode-rejections=%llu "
           "encode-failures=%llu failures=%llu\n",
           g_cases, g_roundtrips, g_rejected, g_encode_failures, g_failures);
    return (g_failures != 0ull || g_encode_failures != 0ull) ? 1 : 0;
}
