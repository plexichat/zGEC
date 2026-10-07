#define _POSIX_C_SOURCE 200809L
#include "zgec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Command-line front end.
 *
 *   zgec <c|d|t> [options] input output
 *
 * Options select the encoder dials of section 11 (level, tier, threads,
 * feature switches, lambda) and the external dictionaries of 5.8.
 * A level is a preset; an explicit switch overrides the corresponding
 * preset column, and the --no- forms turn one back off. */

#define CLI_MAX_DICTS 4

/* An external dictionary is a prefix region, so it competes for the per-worker
 * virtual buffer with the block itself (12.3, P24). The format's raw
 * dictionary maximum is 64 MiB; refusing a larger file before it is copied
 * keeps a mistyped path from costing that much resident memory. */
#define CLI_DICT_MAX_BYTES ((uint64_t)1 << 26)

/* No size limit at all, for the input file. */
#define CLI_NO_SIZE_LIMIT UINT64_MAX

/* The three commands, resolved once by exact string comparison and before any
 * file is read. A first-character test would accept "cat" as compress and
 * "destroy" as decompress, and would only reject them after the input and the
 * dictionaries had already been loaded. */
typedef enum {
    CLI_COMPRESS,
    CLI_DECOMPRESS,
    CLI_TEST
} cli_command;

static int cli_command_of(const char *s, cli_command *out)
{
    if (strcmp(s, "c") == 0 || strcmp(s, "C") == 0) {
        *out = CLI_COMPRESS;
        return 1;
    }
    if (strcmp(s, "d") == 0 || strcmp(s, "D") == 0) {
        *out = CLI_DECOMPRESS;
        return 1;
    }
    if (strcmp(s, "t") == 0 || strcmp(s, "T") == 0) {
        *out = CLI_TEST;
        return 1;
    }
    return 0;
}

/* Monotonic wall clock, for the throughput line of a c/d/t run.
 *
 * CLOCK_MONOTONIC is POSIX, so glibc hides it under the -std=c11 this
 * project builds with and the declaration above is what puts it back
 * (macOS likewise, where it needs __DARWIN_C_FULL). It is harmless on
 * Windows, which ignores the macro and takes the branch below. */
#if defined(_WIN32)
#include <windows.h>
/* The counter frequency is a property of the machine, not of the call, so it
 * is queried once and cached instead of on every timestamp. The cache is
 * plain static state, which is safe only because every timestamp in this file
 * is taken on the main thread -- the codec's workers never call this. If a
 * timestamp is ever taken around anything that spawns, this becomes a race
 * and the frequency has to be resolved once in main and passed down.
 *
 * 0.0 is the "clock unavailable" sentinel, not a time: cli_timing() reports
 * it as an absence instead of turning it into a fabricated 0.0 MiB/s. */
static double cli_now(void)
{
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    LARGE_INTEGER c;
    if (!have_freq) {
        QueryPerformanceFrequency(&freq);
        have_freq = (freq.QuadPart > 0);
    }
    if (!have_freq) return 0.0;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
static double cli_now(void)
{
    struct timespec ts;
    /* This branch needs no cached state, but a failed read uses the same 0.0
     * sentinel as the Windows one so both behave alike. The old code ignored
     * clock_gettime's result and returned whatever ts happened to hold. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
#endif

/* MiB per second over a wall-clock interval; 0 when the interval is empty.
 * The divisor is binary, so the label this file prints for the value is MiB/s
 * and never the decimal MB/s. That rule is this file's alone: tests/bench.c
 * and tools/bench/report.py divide identically but still label the result
 * MB/s, so the tree carries two unit names for one computation. */
static double cli_mbs(size_t bytes, double sec)
{
    if (sec <= 0.0) return 0.0;
    return ((double)bytes / 1048576.0) / sec;
}

/* Whether a wall-clock interval is a usable measurement.
 *
 * cli_now() returns 0.0 for a clock it could not read, and that is not a
 * duration: printing it produced "0.0 ms  0.0 MiB/s", a plausible-looking
 * throughput that was never measured and carried no sign that timing was
 * broken. Callers print the fields only when this is true, and otherwise say
 * the timing is unavailable. */
static int cli_timed(double sec)
{
    return sec > 0.0;
}

/* Read a whole file into a fresh buffer.
 *
 * Every step is checked: both seeks and the tell can fail, the size has to be
 * representable as a size_t, and the allocation is sized for exactly the bytes
 * that will be read. max_bytes bounds what is accepted, so a caller that
 * limits external dictionaries rejects an oversized file before allocating
 * for it; pass CLI_NO_SIZE_LIMIT for the input. */
static zgec_err zgec_read_file(const char *path, uint64_t max_bytes,
                               uint8_t **out, size_t *out_size)
{
    FILE *f;
    long sz;
    size_t want;
    size_t rd;
    uint8_t *buf;

    f = fopen(path, "rb");
    if (!f) return ZGEC_ERR_INVAL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return ZGEC_ERR_INVAL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return ZGEC_ERR_INVAL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return ZGEC_ERR_INVAL; }
    /* A long can be narrower than a size_t on some targets; reject a file
     * this build cannot address rather than truncating the conversion. A file
     * that is too large to *read* is an input fault, not an output one:
     * returning ZGEC_ERR_OUTPUT_SIZE sent a caller whose 64 MiB dictionary was
     * refused looking at the output end of the pipeline. zgec_err has no I/O
     * code, so the generic invalid-argument code is the honest one. */
    if ((uintmax_t)sz > (uintmax_t)SIZE_MAX ||
        (uintmax_t)sz > (uintmax_t)max_bytes) {
        fclose(f);
        return ZGEC_ERR_INVAL;
    }
    want = (size_t)sz;
    /* A zero-byte file still needs a non-NULL buffer to hand to the codec,
     * so the request is one byte for it. Sizing the allocation at exactly
     * `want` (not want + 1) also removes the overflow site the old
     * `(size_t)sz + 1` had on sz. */
    buf = (uint8_t *)zgec_alloc(want ? want : 1, 64);
    if (!buf) { fclose(f); return ZGEC_ERR_NOMEM; }
    rd = fread(buf, 1, want, f);
    if (rd < want) {
        /* Separate an I/O error from a file that shrank or was never that
         * long; both are fatal, but only the second is a truncation. */
        zgec_err e = ferror(f) ? ZGEC_ERR_INTERNAL : ZGEC_ERR_TRUNCATED;
        fclose(f);
        zgec_free(buf);
        return e;
    }
    if (fclose(f) != 0) { zgec_free(buf); return ZGEC_ERR_INTERNAL; }
    *out = buf;
    *out_size = rd;
    return ZGEC_OK;
}

/* Write a whole buffer, looping rather than trusting one fwrite.
 *
 * A failure here is an I/O failure (a full disk, a quota, a broken
 * destination), not an allocation failure, so it no longer reports NOMEM.
 * zgec_err has no I/O code, so the generic code is used. ferror() is tested
 * as well as the byte count, because an error can be raised on a write whose
 * count still looks complete, and the flush inside fclose() can raise one
 * after the last write, so fclose() is part of the result. */
static zgec_err zgec_write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f;
    size_t off = 0;

    f = fopen(path, "wb");
    if (!f) return ZGEC_ERR_INVAL;
    while (off < size) {
        size_t wr = fwrite(data + off, 1, size - off, f);
        if (wr == 0) { fclose(f); return ZGEC_ERR_INTERNAL; }
        off += wr;
    }
    if (ferror(f) != 0) { fclose(f); return ZGEC_ERR_INTERNAL; }
    if (fclose(f) != 0) return ZGEC_ERR_INTERNAL;
    return ZGEC_OK;
}

static void cli_usage(FILE *f, const char *argv0)
{
    fprintf(f,
        "usage: %s <c|d> [options] input output\n"
        "       %s  t   [options] input\n"
        "\n"
        "commands:\n"
        "  c   compress input into output\n"
        "  d   decompress input into output\n"
        "  t   compress, decompress and verify (writes no file, so it\n"
        "      takes no output path)\n"
        "\n"
        "options:\n"
        "  -l, --level N       1 (fastest) .. 9 (best ratio); default 3\n"
        "  -T, --threads N     worker threads (0 = one per core); default 0\n"
        "      --tier T        match finder tier: fast, main, high\n"
        "      --block-log2 N  block size log2, 16..26 (default: the level\n"
        "                      preset: 21, i.e. 2 MiB, section 12.3)\n"
        "      --lambda X      speed/ratio dial (11.7)\n"
        "      --contexts      learned literal contexts (11.6)\n"
        "      --sub-lit       sub-literal segments (9.6)\n"
        "      --conditioning  sequence conditioning (8.6)\n"
        "      --litref        literal references (6.3); keeps blocks\n"
        "                      plain, so it replaces sub-literals and\n"
        "                      sequence conditioning (6.3)\n"
        "      --dicts         epoch dictionaries (5.5, 5.7)\n"
        "      --filter        sampled block pre-filter (11.11)\n"
        "      --checksums     per-block CRC32C\n"
        "      --no-<feature> turn a feature the level preset enabled back\n"
        "                      off (e.g. --no-filter)\n"
        "  -D, --dict FILE     external dictionary (5.8); repeatable when\n"
        "                      compressing, required to decode such frames.\n"
        "                      Ids follow the argument order, so decompress\n"
        "                      with the same files in the same order.\n"
        "      --core          decode at CORE level (default EXTENDED);\n"
        "                      applies to the d command only\n"
        "      --quiet         suppress the size summary\n"
        "  -h, --help          this text\n"
        "  -V, --version       version string\n",
        argv0, argv0);
}

/* Level presets: a level is the previous one plus what measured better on
 * the corpus, so the compressed size never rises with the number. The speed
 * levels use the fast tier (11.2); lambda stays 0 because on the corpus a
 * positive lambda cost ratio without a measurable decode gain. */
typedef struct {
    zgec_tier tier;
    double    lambda;
    int       ctx;
    int       sub;
    int       cond;
    int       litref;
    int       dicts;
    int       filter;
    int       ck;
    int       block_log2;
} cli_level;

/* A level adds the feature that measured best on the reference corpus, so
 * the compressed size never rises with the level.
 *
 * Two orderings matter. A literal-reference level carries its own bit set,
 * which suppresses conditioning (6.3), so conditioning is added after
 * literal references rather than before them; the level that adds it
 * re-states the flag rather than dropping one.
 *
 * Every preset uses the 2 MiB block of section 12.3 (block_log2 21). A
 * larger block is not free: blocks are the unit of parallelism (10.6), so a
 * 16 MiB block leaves a short input with a single worker and no scaling at
 * all, and section 12.3 caps the working set per worker at a size that
 * spills the shared L3 long before this machine's 32 MiB is exhausted.
 * 2 MiB measured both smaller and faster than 16 MiB everywhere in the
 * level ladder. The block also has to hold the P24 prefix: a dictionary or
 * literal-reference region shares the 16 MiB virtual buffer with the block,
 * and the encoder shrinks the prefix to fit rather than the block.
 *
 * Level 8 adds sub-literals; level 9 adds the per-block CRC-32C, which is a
 * fixed-width field and so does not move the size.
 *
 * Two features are deliberately in no preset: the sampled pre-filter and
 * epoch dictionaries. The preset rule is measured, so a feature keeps a
 * level only while it is a net win on the reference corpus at the 2 MiB
 * block, and neither is. The filter's sampled gate accepts, and a block it
 * accepts cannot set LIT_EXPORTABLE (6.3, V11), which breaks the
 * literal-reference chain the levels above 5 rely on: 254 KB, 1.0% of a
 * 26 MB frame, which made level 9 come out larger than level 7. The epoch
 * dictionary is a 186 byte net loss on the same frame, because one epoch's
 * block is encoded with a dictionary that does not pay for itself there and
 * the encoder does not re-encode to prove each per-block choice. At 16 MiB
 * blocks neither one showed, which is why the earlier ladder did not. Both
 * stay available as flags for the input where they do pay (binary, columnar
 * and heterogeneous data), and a caller that wants a dictionary prefix on a
 * small block is the reason --block-log2 is still an option. */
static const cli_level cli_levels[9] = {
    /* 1 */ { ZGEC_TIER_FAST, 0.0, 0, 0, 0, 0, 0, 0, 0, 21 },
    /* 2 */ { ZGEC_TIER_FAST, 0.0, 1, 0, 0, 0, 0, 0, 0, 21 },
    /* 3 */ { ZGEC_TIER_MAIN, 0.0, 0, 0, 0, 0, 0, 0, 0, 21 },
    /* 4 */ { ZGEC_TIER_MAIN, 0.0, 1, 0, 0, 0, 0, 0, 0, 21 },
    /* 5 */ { ZGEC_TIER_MAIN, 0.0, 1, 0, 0, 1, 0, 0, 0, 21 },
    /* 6 */ { ZGEC_TIER_MAIN, 0.0, 1, 0, 1, 1, 0, 0, 0, 21 },
    /* 7 */ { ZGEC_TIER_HIGH, 0.0, 1, 0, 1, 1, 0, 0, 0, 21 },
    /* 8 */ { ZGEC_TIER_HIGH, 0.0, 1, 1, 1, 1, 0, 0, 0, 21 },
    /* 9 */ { ZGEC_TIER_HIGH, 0.0, 1, 1, 1, 1, 0, 0, 1, 21 }
};

static void cli_apply_level(zgec_params *p, int level)
{
    const cli_level *l;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    l = &cli_levels[level - 1];
    p->block_log2 = l->block_log2;
    p->tier = l->tier;
    p->lambda = l->lambda;
    p->use_contexts = l->ctx;
    p->use_sublit = l->sub;
    p->use_conditioning = l->cond;
    p->use_litref = l->litref;
    p->use_dicts = l->dicts;
    p->use_filter = l->filter;
    p->block_checksums = l->ck;
}

static int cli_tier(const char *s, zgec_tier *out)
{
    if (strcmp(s, "fast") == 0) { *out = ZGEC_TIER_FAST; return 1; }
    if (strcmp(s, "main") == 0) { *out = ZGEC_TIER_MAIN; return 1; }
    if (strcmp(s, "high") == 0) { *out = ZGEC_TIER_HIGH; return 1; }
    return 0;
}

/* Integer option value: the whole string must be a number in [lo, hi]. */
static int cli_int(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v;
    if (s == NULL || *s == '\0') return 0;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return 0;
    if (v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

/* Real option value in [lo, hi]: rejects NaN (the comparison is false)
 * and non-numeric text, because a NaN lambda would make every 11.7 score
 * comparison false. */
static int cli_double(const char *s, double lo, double hi, double *out)
{
    char *end = NULL;
    double v;
    if (s == NULL || *s == '\0') return 0;
    v = strtod(s, &end);
    if (end == s || *end != '\0') return 0;
    if (!(v >= lo)) return 0;
    if (!(v <= hi)) return 0;
    *out = v;
    return 1;
}

int main(int argc, char **argv)
{
    int level = 3;
    int threads = 0;
    int threads_set = 0;
    int tier_set = 0;
    int block_log2 = 0;
    int lambda_set = 0;
    int core = 0;
    int quiet = 0;
    int f_ctx = -1, f_sub = -1, f_cond = -1, f_litref = -1;
    int f_dicts = -1, f_filter = -1, f_ck = -1;
    const char *dict_paths[CLI_MAX_DICTS];
    uint16_t dict_ids[CLI_MAX_DICTS];
    size_t dict_sizes[CLI_MAX_DICTS];
    uint8_t *dict_data[CLI_MAX_DICTS];
    int n_dicts = 0;
    zgec_tier tier = ZGEC_TIER_MAIN;
    double lambda = 0.0;
    const char *pos[3];
    int npos = 0;
    int i;
    int rc = 1;
    uint8_t *in = NULL;
    size_t in_size = 0;
    uint8_t *out = NULL;
    size_t out_size = 0;
    /* Initialised so that no path can read it before cli_command_of()
     * has set it, even though every path that reaches a use has a value. */
    cli_command cmd = CLI_COMPRESS;
    const char *cmd_name;
    const char *inpath;
    const char *outpath = NULL;
    zgec_err err;
    double enc_sec = 0.0;
    double dec_sec = 0.0;
    double dec_t0 = 0.0;

    /* Every dictionary slot starts empty so an early error can free the
     * whole array safely. */
    for (i = 0; i < CLI_MAX_DICTS; i++) {
        dict_paths[i] = NULL;
        dict_ids[i] = 0;
        dict_sizes[i] = 0;
        dict_data[i] = NULL;
    }
    if (argc < 2) {
        cli_usage(stderr, argv[0]);
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != '\0') {
            /* Boolean feature switches, including the --no- forms that
             * turn off what the selected level preset enabled. */
            struct cli_flag {
                const char *name;
                int *slot;
                int value;
            };
            const struct cli_flag flags[] = {
                { "--contexts", &f_ctx, 1 },
                { "--no-contexts", &f_ctx, 0 },
                { "--sub-lit", &f_sub, 1 },
                { "--no-sub-lit", &f_sub, 0 },
                { "--conditioning", &f_cond, 1 },
                { "--no-conditioning", &f_cond, 0 },
                { "--litref", &f_litref, 1 },
                { "--no-litref", &f_litref, 0 },
                { "--dicts", &f_dicts, 1 },
                { "--no-dicts", &f_dicts, 0 },
                { "--filter", &f_filter, 1 },
                { "--no-filter", &f_filter, 0 },
                { "--checksums", &f_ck, 1 },
                { "--no-checksums", &f_ck, 0 }
            };
            size_t fi;
            int matched = 0;
            long num = 0;
            for (fi = 0; fi < sizeof(flags) / sizeof(flags[0]); fi++) {
                if (strcmp(a, flags[fi].name) == 0) {
                    *flags[fi].slot = flags[fi].value;
                    matched = 1;
                    break;
                }
            }
            if (matched) continue;
            if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
                cli_usage(stdout, argv[0]);
                return 0;
            } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
                printf("zgec %s\n", zgec_version());
                return 0;
            } else if (strcmp(a, "-l") == 0 || strcmp(a, "--level") == 0) {
                if (++i >= argc) goto bad_value;
                if (!cli_int(argv[i], 1, 9, &num)) {
                    fprintf(stderr, "zgec: level must be 1..9\n");
                    return 1;
                }
                level = (int)num;
            } else if (strcmp(a, "-T") == 0 || strcmp(a, "--threads") == 0) {
                if (++i >= argc) goto bad_value;
                if (!cli_int(argv[i], 0, 1024, &num)) {
                    fprintf(stderr, "zgec: thread count must be 0..1024\n");
                    return 1;
                }
                threads = (int)num;
                threads_set = 1;
            } else if (strcmp(a, "--tier") == 0) {
                if (++i >= argc) goto bad_value;
                if (!cli_tier(argv[i], &tier)) {
                    fprintf(stderr, "zgec: tier must be fast, main or high\n");
                    return 1;
                }
                tier_set = 1;
            } else if (strcmp(a, "--block-log2") == 0) {
                if (++i >= argc) goto bad_value;
                if (!cli_int(argv[i], ZGEC_BLOCK_LOG2_MIN,
                             ZGEC_BLOCK_LOG2_MAX, &num)) {
                    fprintf(stderr, "zgec: block-log2 must be %d..%d\n",
                            (int)ZGEC_BLOCK_LOG2_MIN,
                            (int)ZGEC_BLOCK_LOG2_MAX);
                    return 1;
                }
                block_log2 = (int)num;
            } else if (strcmp(a, "--lambda") == 0) {
                if (++i >= argc) goto bad_value;
                if (!cli_double(argv[i], 0.0, 1.0e9, &lambda)) {
                    fprintf(stderr, "zgec: lambda must be a number >= 0\n");
                    return 1;
                }
                lambda_set = 1;
            } else if (strcmp(a, "--core") == 0) {
                core = 1;
            } else if (strcmp(a, "--quiet") == 0) {
                quiet = 1;
            } else if (strcmp(a, "-D") == 0 || strcmp(a, "--dict") == 0) {
                if (++i >= argc) goto bad_value;
                if (n_dicts >= CLI_MAX_DICTS) {
                    fprintf(stderr, "zgec: at most %d dictionaries\n",
                            CLI_MAX_DICTS);
                    return 1;
                }
                dict_paths[n_dicts] = argv[i];
                n_dicts++;
            } else {
                fprintf(stderr, "zgec: unknown option %s\n", a);
                cli_usage(stderr, argv[0]);
                return 1;
            }
        } else if (npos < 3) {
            pos[npos++] = a;
        } else {
            fprintf(stderr, "zgec: too many arguments\n");
            cli_usage(stderr, argv[0]);
            return 1;
        }
    }

    if (npos < 2) {
        cli_usage(stderr, argv[0]);
        return 1;
    }
    cmd_name = pos[0];
    if (!cli_command_of(cmd_name, &cmd)) {
        fprintf(stderr, "zgec: unknown command %s\n", cmd_name);
        cli_usage(stderr, argv[0]);
        return 1;
    }
    inpath = pos[1];
    /* c and d write a file, so they take exactly one output path; t writes
     * nothing, so an output path there is a mistake rather than an argument
     * to silently ignore. */
    if (cmd == CLI_TEST) {
        if (npos > 2) {
            fprintf(stderr, "zgec: t takes no output path\n");
            cli_usage(stderr, argv[0]);
            return 1;
        }
    } else if (npos < 3) {
        fprintf(stderr, "zgec: %s needs an output path\n", cmd_name);
        cli_usage(stderr, argv[0]);
        return 1;
    } else {
        outpath = pos[2];
    }

    err = zgec_read_file(inpath, CLI_NO_SIZE_LIMIT, &in, &in_size);
    if (err != ZGEC_OK) {
        fprintf(stderr, "zgec: cannot read %s\n", inpath);
        return 1;
    }

    /* External dictionaries are read once and handed to both ends. */
    for (i = 0; i < n_dicts; i++) {
        dict_ids[i] = (uint16_t)(i + 1);
        dict_sizes[i] = 0;
        dict_data[i] = NULL;
        err = zgec_read_file(dict_paths[i], CLI_DICT_MAX_BYTES,
                             &dict_data[i], &dict_sizes[i]);
        if (err != ZGEC_OK) {
            /* Name the limit as well as the code. A dictionary that is too
             * large and a dictionary that is missing share one code, and the
             * size limit (5.8) is the reason a real file gets refused, so
             * the message has to say which limit was hit. */
            fprintf(stderr,
                    "zgec: cannot read dictionary %s: %s (limit %llu MiB, 5.8)\n",
                    dict_paths[i], zgec_strerror(err),
                    (unsigned long long)(CLI_DICT_MAX_BYTES >> 20));
            goto done;
        }
    }

    if (cmd == CLI_COMPRESS || cmd == CLI_TEST) {
        zgec_params p;
        zgec_encoder *e;
        zgec_params_default(&p);
        cli_apply_level(&p, level);
        if (threads_set) p.n_threads = threads;
        if (tier_set) p.tier = tier;
        if (block_log2) p.block_log2 = block_log2;
        if (lambda_set) p.lambda = lambda;
        if (f_ctx >= 0) p.use_contexts = f_ctx;
        if (f_sub >= 0) p.use_sublit = f_sub;
        if (f_cond >= 0) p.use_conditioning = f_cond;
        if (f_litref >= 0) p.use_litref = f_litref;
        if (f_dicts >= 0) p.use_dicts = f_dicts;
        if (f_filter >= 0) p.use_filter = f_filter;
        if (f_ck >= 0) p.block_checksums = f_ck;

        /* Literal references keep their predecessor block plain and
         * unconditioned (6.3), so --litref replaces --sub-lit and
         * --conditioning. The encoder suppresses those two itself, so
         * clearing the flags here is belt-and-braces, not what makes the run
         * correct, and it is not what makes the reported configuration true
         * either (neither flag is reported anywhere).
         *
         * The warning is therefore keyed on the user's own request (f_sub and
         * f_cond, which are -1 when the flag was never given), not on the
         * resolved parameters. Testing the parameters fired on every higher
         * level, because the presets enable sub-literals and conditioning on
         * their own: `zgec c -l 9 --litref in out` announced that its options
         * had been replaced when the preset had chosen them. */
        if (p.use_litref != 0) {
            if (!quiet && (f_sub == 1 || f_cond == 1))
                fprintf(stderr, "zgec: --litref replaces sub-literals and "
                                "sequence conditioning (6.3)\n");
            p.use_sublit = 0;
            p.use_conditioning = 0;
        }

        /* A block is the unit of parallelism (10.6), so more workers than
         * the input has blocks only multiplies per-worker state for no gain.
         * The ceiling division is written without an add so that it cannot
         * overflow for an input close to SIZE_MAX. */
        if (p.n_threads > 1 && in_size > 0 && p.block_log2 > 0) {
            size_t block_size = (size_t)1 << p.block_log2;
            size_t n_blocks = in_size / block_size
                            + (in_size % block_size != 0 ? 1u : 0u);
            if ((size_t)p.n_threads > n_blocks) {
                /* Reported even under --quiet: -T is a measurement input, and
                 * the benchmark that varies it labels each row with the count
                 * it requested (tools/bench/run_bench.py:452-464), so a silent
                 * clamp would publish a thread-scaling curve that was never
                 * measured. Both numbers are on the line, so the effective
                 * count is recoverable from stderr alone. */
                fprintf(stderr, "zgec: threads requested %d, using %zu "
                                "(one per block)\n",
                        p.n_threads, n_blocks);
                p.n_threads = (int)n_blocks;
            }
        }
        /* No separate banner here. It was one more unconditional stderr line
         * on every non-quiet run, and it duplicated information that is
         * already reported: the input size appears in the encode summary
         * below, and the worker count appears there and in the clamp notice
         * above whenever it differs from what was requested. */
        e = zgec_encoder_create(&p);
        if (e == NULL) {
            fprintf(stderr, "zgec: out of memory\n");
            goto done;
        }
        for (i = 0; i < n_dicts; i++) {
            err = zgec_encoder_set_external_dict(e, dict_ids[i],
                                                 dict_data[i], dict_sizes[i]);
            if (err != ZGEC_OK) {
                fprintf(stderr, "zgec: dictionary %s rejected: %s\n",
                        dict_paths[i], zgec_strerror(err));
                zgec_encoder_destroy(e);
                goto done;
            }
        }
        {
            double t0 = cli_now();
            err = zgec_encode_frame(e, in, in_size, &out, &out_size);
            enc_sec = cli_now() - t0;
        }
        zgec_encoder_destroy(e);
        if (err != ZGEC_OK) {
            fprintf(stderr, "zgec: compress failed: %s\n", zgec_strerror(err));
            goto done;
        }
        if (!quiet) {
            double ratio = (out_size > 0)
                ? (double)in_size / (double)out_size : 0.0;
            /* enc_sec is zgec_encode_frame() alone: context creation,
             * dictionary registration and worker creation are outside the
             * interval, so the figure is the frame encode. */
            if (cli_timed(enc_sec))
                fprintf(stderr,
                        "zgec: level %d, %zu -> %zu bytes (%.4fx)  "
                        "%.1f ms  %.1f MiB/s\n",
                        level, in_size, out_size, ratio, enc_sec * 1000.0,
                        cli_mbs(in_size, enc_sec));
            else
                fprintf(stderr,
                        "zgec: level %d, %zu -> %zu bytes (%.4fx)  "
                        "timing unavailable\n",
                        level, in_size, out_size, ratio);
        }
    }

    if (cmd == CLI_COMPRESS) {
        err = zgec_write_file(outpath, out, out_size);
        if (err != ZGEC_OK) {
            fprintf(stderr, "zgec: cannot write %s\n", outpath);
            goto done;
        }
    } else if (cmd == CLI_DECOMPRESS) {
        zgec_limits lim;
        zgec_decoder *d;
        memset(&lim, 0, sizeof(lim));
        lim.n_threads = threads_set ? threads : 0;
        d = zgec_decoder_create(core ? ZGEC_LEVEL_CORE : ZGEC_LEVEL_EXTENDED,
                                &lim);
        if (d == NULL) {
            fprintf(stderr, "zgec: out of memory\n");
            goto done;
        }
        for (i = 0; i < n_dicts; i++) {
            err = zgec_decoder_add_external_dict(d, dict_ids[i],
                                                 dict_data[i], dict_sizes[i]);
            if (err != ZGEC_OK) {
                fprintf(stderr, "zgec: dictionary %s rejected: %s\n",
                        dict_paths[i], zgec_strerror(err));
                zgec_decoder_destroy(d);
                goto done;
            }
        }
        dec_t0 = cli_now();
        err = zgec_decode_frame(d, in, in_size, &out, &out_size);
        dec_sec = cli_now() - dec_t0;
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK) {
            fprintf(stderr, "zgec: decompress failed: %s\n",
                    zgec_strerror(err));
            goto done;
        }
        if (!quiet) {
            /* dec_sec is zgec_decode_frame() alone, as for the encode. */
            if (cli_timed(dec_sec))
                fprintf(stderr,
                        "zgec: %zu -> %zu bytes  %.1f ms  %.1f MiB/s\n",
                        in_size, out_size, dec_sec * 1000.0,
                        cli_mbs(out_size, dec_sec));
            else
                fprintf(stderr,
                        "zgec: %zu -> %zu bytes  timing unavailable\n",
                        in_size, out_size);
        }
        err = zgec_write_file(outpath, out, out_size);
        if (err != ZGEC_OK) {
            fprintf(stderr, "zgec: cannot write %s\n", outpath);
            goto done;
        }
    } else if (cmd == CLI_TEST) {
        /* Compress and decompress in memory, then compare. */
        zgec_limits lim;
        zgec_decoder *d;
        uint8_t *back = NULL;
        size_t back_size = 0;
        memset(&lim, 0, sizeof(lim));
        lim.n_threads = threads_set ? threads : 0;
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
        if (d == NULL) {
            fprintf(stderr, "zgec: out of memory\n");
            goto done;
        }
        for (i = 0; i < n_dicts; i++) {
            err = zgec_decoder_add_external_dict(d, dict_ids[i],
                                                 dict_data[i], dict_sizes[i]);
            if (err != ZGEC_OK) {
                fprintf(stderr, "zgec: dictionary %s rejected: %s\n",
                        dict_paths[i], zgec_strerror(err));
                zgec_decoder_destroy(d);
                goto done;
            }
        }
        dec_t0 = cli_now();
        err = zgec_decode_frame(d, out, out_size, &back, &back_size);
        dec_sec = cli_now() - dec_t0;
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK) {
            fprintf(stderr, "zgec: t decompress failed: %s\n",
                    zgec_strerror(err));
            zgec_free(back);
            goto done;
        }
        if (back_size != in_size || memcmp(back, in, in_size) != 0) {
            fprintf(stderr, "zgec: round-trip mismatch\n");
            zgec_free(back);
            goto done;
        }
        zgec_free(back);
        if (!quiet) {
            /* Test mode times its decode step too, so one run reports the
             * encode and the decode speed side by side. */
            if (cli_timed(dec_sec))
                fprintf(stderr, "zgec: decode %zu -> %zu bytes  %.1f ms  "
                                "%.1f MiB/s\n",
                        out_size, back_size, dec_sec * 1000.0,
                        cli_mbs(back_size, dec_sec));
            else
                fprintf(stderr, "zgec: decode %zu -> %zu bytes  "
                                "timing unavailable\n",
                        out_size, back_size);
        }
        printf("round-trip ok\n");
    }

    rc = 0;

done:
    for (i = 0; i < n_dicts; i++) zgec_free(dict_data[i]);
    zgec_free(in);
    zgec_free(out);
    return rc;

bad_value:
    fprintf(stderr, "zgec: option %s needs a value\n", argv[i - 1]);
    return 1;
}
