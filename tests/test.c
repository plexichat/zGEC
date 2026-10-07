#include "zgec.h"
#include "zgec_block.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int zgec_test_fse_round_trip(void)
{
    /* Sum must equal 2^11 = 2048 (the AL=11 probability scale). */
    int16_t counts[66];
    memset(counts, 0, sizeof(counts));
    for (int s = 0; s < 66; s++) counts[s] = (int16_t)(s < 64 ? 31 : 32);
    int sum = 0;
    for (int s = 0; s < 66; s++) sum += counts[s];
    if (sum != 2048) return 0;

    zgec_fse_dec_table *dec = NULL;
    zgec_fse_enc_table *enc = NULL;
    if (zgec_fse_build_dec(&dec, counts, 66, 11) != ZGEC_OK) return 0;
    if (zgec_fse_build_enc(&enc, dec) != ZGEC_OK) { zgec_fse_free_dec(dec); return 0; }

    uint8_t syms[1024];
    uint32_t values[1024];
    for (int i = 0; i < 1024; i++) {
        syms[i] = (uint8_t)(i % 66);
        values[i] = zgec_seq_base[syms[i]];
    }

    uint8_t buf[4096];
    zgec_bw bw;
    zgec_bw_init(&bw, buf, sizeof(buf));
    if (zgec_fse_encode(enc, &bw, values, syms, 1024, zgec_seq_base, zgec_seq_nbits) != ZGEC_OK) {
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); return 0;
    }
    size_t enc_size = zgec_bw_finish(&bw);

    zgec_br br;
    zgec_br_init(&br, buf, enc_size);
    uint32_t decoded[1024];
    if (zgec_fse_decode(dec, &br, decoded, NULL, 1024, zgec_seq_base, zgec_seq_nbits) != ZGEC_OK) {
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); return 0;
    }
    if (!zgec_br_done(&br)) {
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); return 0;
    }

    for (int i = 0; i < 1024; i++) {
        if (decoded[i] != values[i]) {
            zgec_fse_free_enc(enc); zgec_fse_free_dec(dec);
            return 0;
        }
    }

    zgec_fse_free_enc(enc);
    zgec_fse_free_dec(dec);
    return 1;
}

static int zgec_test_rans_round_trip(void)
{
    /* Sum must equal 2^11 = 2048; only symbols 0..127 occur. */
    int16_t counts[256];
    memset(counts, -1, sizeof(counts));
    for (int s = 0; s < 256; s++) counts[s] = (int16_t)(s < 128 ? 16 : 0);
    int sum = 0;
    for (int s = 0; s < 256; s++) sum += counts[s] < 0 ? 1 : counts[s];
    if (sum != 2048) return 0;

    zgec_rans_dec_table dec;
    zgec_rans_enc_table enc;
    if (zgec_rans_build_dec(&dec, counts) != ZGEC_OK) return 0;
    if (zgec_rans_build_enc(&enc, counts) != ZGEC_OK) return 0;

    uint8_t lit[256];
    for (int i = 0; i < 256; i++) lit[i] = (uint8_t)(i & 127);

    uint8_t stream[4096];
    size_t stream_size = zgec_rans_encode(lit, 256, stream, sizeof(stream), &enc, 1, ZGEC_CTX_NONE, NULL, NULL);
    if (stream_size == 0) return 0;

    uint8_t decoded[256 + ZGEC_LIT_SLACK];
    if (zgec_rans_decode(decoded, 256, stream, stream_size, &dec, 1, ZGEC_CTX_NONE, NULL, NULL) != ZGEC_OK) return 0;

    for (int i = 0; i < 256; i++) {
        if (decoded[i] != lit[i]) return 0;
    }
    return 1;
}

static int zgec_test_frame_round_trip(void)
{
    uint8_t buf[64];
    zgec_frame_header fh;
    memset(&fh, 0, sizeof(fh));
    fh.version_major = 1;
    fh.version_minor = 0;
    fh.flags = ZGEC_FLAG_HAS_FOOTER;
    fh.block_log2 = 21;
    fh.epoch_blocks = 10;
    fh.max_dict_log2 = 20;
    fh.seg_hint_log2 = 18;
    fh.content_size = 1024;
    fh.block_count = 2;

    zgec_frame_header_emit(buf, &fh);
    zgec_frame_header parsed;
    if (zgec_frame_header_parse(&parsed, buf) != ZGEC_OK) return 0;
    if (parsed.version_major != 1) return 0;
    if (parsed.block_log2 != 21) return 0;
    if (parsed.content_size != 1024) return 0;
    return 1;
}

static int zgec_test_classify(void)
{
    /* LSB6 keeps the low six bits (0xAB & 63 == 0x2B). */
    if (zgec_classify(ZGEC_CTX_LSB6, 0xAB) != (0xAB & 63)) return 0;
    if (zgec_classify(ZGEC_CTX_MSB6, 0xAB) != (0xAB >> 2)) return 0;
    if (zgec_classify(ZGEC_CTX_NONE, 0xAB) != 0) return 0;
    int8_t s = -10;
    if (zgec_classify(ZGEC_CTX_SIGNED, (uint8_t)s) != 22) return 0;
    return 1;
}

static int zgec_test_runstart(void)
{
    uint8_t runstart[16];
    uint32_t ll[] = {5, 3, 2};
    if (zgec_lit_runstart(runstart, 10, ll, 3) != ZGEC_OK) return 0;
    if (!runstart[0]) return 0;
    if (!runstart[5]) return 0;
    if (!runstart[8]) return 0;
    return 1;
}

static int zgec_test_lane_starts(void)
{
    size_t start[8];
    zgec_lit_lane_starts(start, 0);
    for (int i = 0; i < 8; i++) if (start[i] != 0) return 0;
    zgec_lit_lane_starts(start, 8);
    if (start[0] != 0 || start[7] != 7) return 0;
    /* n = 15: q = 1, r = 7, start(k) = k*q + min(k, r). */
    zgec_lit_lane_starts(start, 15);
    if (start[0] != 0 || start[6] != 12 || start[7] != 14) return 0;
    return 1;
}

static int zgec_test_class_map_round_trip(void)
{
    uint8_t map[64];
    uint8_t packed[24];
    for (int i = 0; i < 64; i++) map[i] = (uint8_t)(i % 4);
    zgec_class_map_encode(packed, map);
    uint8_t unpacked[64];
    if (zgec_class_map_decode(unpacked, packed, 4) != ZGEC_OK) return 0;
    for (int i = 0; i < 64; i++) {
        if (unpacked[i] != map[i]) return 0;
    }
    return 1;
}

static int zgec_test_dict_cache(void)
{
    zgec_dict_cache *c = zgec_dict_cache_create(2);
    if (!c) return 0;
    zgec_dict d1;
    memset(&d1, 0, sizeof(d1));
    d1.dict_id = 1;
    d1.raw_size = 10;
    d1.data = (uint8_t *)zgec_alloc(10, 1);
    if (!d1.data) { zgec_dict_cache_destroy(c); return 0; }
    zgec_dict_cache_put(c, &d1);
    const zgec_dict *got = zgec_dict_cache_get(c, 1);
    if (!got || got->dict_id != 1) { zgec_dict_cache_destroy(c); return 0; }
    zgec_dict_cache_destroy(c);
    return 1;
}

static int zgec_test_round_trip(void)
{
    uint8_t src[1024];
    for (int i = 0; i < 1024; i++) src[i] = (uint8_t)(i * 3 + 1);

    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    if (zgec_compress(src, sizeof(src), &cmp, &cmp_size) != ZGEC_OK) return 0;

    uint8_t *out = NULL;
    size_t out_size = 0;
    if (zgec_decompress(cmp, cmp_size, &out, &out_size) != ZGEC_OK) {
        zgec_free(cmp); return 0;
    }

    int ok = (out_size == sizeof(src) && memcmp(src, out, sizeof(src)) == 0) ? 1 : 0;
    zgec_free(cmp);
    zgec_free(out);
    return ok;
}

static int zgec_test_fse_seq_round_trip(void)
{
    /* Non-RLE sequence stream with several symbols per stream: exercises
     * zgec_seq_build_tables, zgec_fse_encode, the sentinel finish and
     * zgec_fse_decode end to end. */
    uint32_t hist[ZGEC_NSYM_SEQ];
    uint32_t val[300];
    uint32_t out[300];
    unsigned s = 99;
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < 300; i++) {
        s = s * 1103515245u + 12345u;
        int sym = (int)((s >> 16) % 17u);
        uint8_t nb = zgec_seq_nbits[sym];
        uint32_t extra = 0;
        if (nb > 0) {
            s = s * 1103515245u + 12345u;
            extra = (s >> 8) & ((1u << nb) - 1u);
        }
        val[i] = zgec_seq_base[sym] + extra;
        hist[sym]++;
    }
    zgec_fse_dec_table *dec = NULL;
    zgec_fse_enc_table *enc = NULL;
    if (zgec_seq_build_tables(&dec, &enc, hist, 10) != ZGEC_OK) return 0;
    uint8_t buf[8192];
    zgec_bw bw;
    zgec_bw_init(&bw, buf, sizeof(buf));
    size_t sz = zgec_seq_stream_encode(val, 300, enc, -1, &bw,
                                       zgec_seq_base, zgec_seq_nbits);
    if (sz == 0) { zgec_fse_free_dec(dec); zgec_fse_free_enc(enc); return 0; }
    zgec_br br;
    zgec_br_init(&br, buf, sz);
    zgec_err e = zgec_seq_stream_decode(out, 300, dec, -1, &br,
                                        zgec_seq_base, zgec_seq_nbits);
    zgec_fse_free_dec(dec);
    zgec_fse_free_enc(enc);
    if (e != ZGEC_OK) return 0;
    for (int i = 0; i < 300; i++) {
        if (out[i] != val[i]) return 0;
    }
    return 1;
}

static int zgec_test_filter_transform(void)
{
    /* Section 7.5 forward/inverse transforms and descriptor validation. */
    static const unsigned strides[6] = { 2, 4, 8, 16, 32, 64 };
    uint8_t a[257], b[257];
    for (int i = 0; i < 257; i++) a[i] = (uint8_t)(i * 7 + 3);
    if (zgec_filter_apply(b, a, 257, ZGEC_FILTER_DELTA, 0) != ZGEC_OK) return 0;
    if (zgec_filter_inverse(b, 257, ZGEC_FILTER_DELTA, 0) != ZGEC_OK) return 0;
    if (memcmp(a, b, 257) != 0) return 0;
    for (int k = 0; k < 6; k++) {
        if (zgec_filter_apply(b, a, 257, ZGEC_FILTER_SHUFFLE,
                              strides[k]) != ZGEC_OK) return 0;
        if (zgec_filter_inverse(b, 257, ZGEC_FILTER_SHUFFLE,
                                strides[k]) != ZGEC_OK) return 0;
        if (memcmp(a, b, 257) != 0) return 0;
    }
    if (zgec_filter_validate(ZGEC_FILTER_DELTA, 0) != ZGEC_OK) return 0;
    if (zgec_filter_validate(ZGEC_FILTER_DELTA, 1) != ZGEC_ERR_RESERVED) return 0;
    if (zgec_filter_validate(ZGEC_FILTER_SHUFFLE, 1) != ZGEC_ERR_RESERVED) return 0;
    if (zgec_filter_validate(ZGEC_FILTER_SHUFFLE, 65) != ZGEC_ERR_RESERVED) return 0;
    if (zgec_filter_validate(ZGEC_FILTER_NONE, 0) != ZGEC_ERR_RESERVED) return 0;
    return 1;
}

static int zgec_test_filter_frame(void)
{
    /* Columnar 32-bit data: the sampled gate (11.11) should accept a
     * shuffle and the FILTERED round-trip must reproduce the input. */
    size_t n = 200000;
    uint8_t *src = (uint8_t *)calloc(n, 1);
    if (!src) return 0;
    for (size_t i = 0; i + 3 < n; i += 4) src[i] = (uint8_t)((i / 4) & 0xFFu);

    zgec_params p;
    zgec_params_default(&p);
    p.use_filter = 1;
    p.block_log2 = 16;
    p.block_checksums = 1;
    zgec_encoder *e = zgec_encoder_create(&p);
    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    zgec_err err = zgec_encode_frame(e, src, n, &cmp, &cmp_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) { free(src); return 0; }

    int nfiltered = 0;
    {
        zgec_trailer tr;
        zgec_footer f;
        memset(&f, 0, sizeof(f));
        if (zgec_trailer_parse(&tr, cmp + cmp_size - 16) == ZGEC_OK &&
            zgec_footer_parse(&f, cmp + tr.footer_offset,
                              tr.footer_size) == ZGEC_OK) {
            for (uint32_t i = 0; i < f.block_count; i++) {
                if (f.blocks[i].rflags & ZGEC_RFLAG_FILTERED) nfiltered++;
            }
            zgec_footer_free(&f);
        }
    }

    zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
    uint8_t *out = NULL;
    size_t out_size = 0;
    err = zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
    zgec_decoder_destroy(d);
    int ok = (err == ZGEC_OK && out_size == n && nfiltered > 0 &&
              memcmp(src, out, n) == 0) ? 1 : 0;
    zgec_free(cmp);
    zgec_free(out);
    free(src);
    return ok;
}

static uint32_t t_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Walk a frame's COMPRESSED records and count the literal/sequence coder
 * choices actually emitted (section 11.7). */
static void zgec_count_coders(const uint8_t *frame, size_t n,
                              int *rans, int *sub, int *cond)
{
    zgec_frame_header fh;
    size_t off = 32;
    *rans = 0;
    *sub = 0;
    *cond = 0;
    if (n < 56 || zgec_frame_header_parse(&fh, frame) != ZGEC_OK) return;
    while (off + 24u <= n) {
        uint8_t rt = frame[off];
        uint32_t psz = t_rd32(frame + off + 8);
        uint32_t sc = t_rd32(frame + off + 12);
        if (off + 24u + (size_t)psz > n) break;
        if (rt == 0 && sc > 0 && sc <= 4096) {
            zgec_block_params bp;
            size_t pl;
            memset(&bp, 0, sizeof(bp));
            pl = zgec_block_params_parse(&bp, frame + off + 24, psz);
            if (pl > 0 && pl + (size_t)sc * 8u <= psz) {
                size_t base = off + 24 + pl;
                size_t soff = base + (size_t)sc * 8u;
                uint32_t i;
                for (i = 0; i < sc; i++) {
                    uint32_t clen = t_rd32(frame + base + (size_t)i * 8u);
                    zgec_seg_header sh;
                    int lit_form;
                    int k;
                    if (clen == 0u || soff + clen > off + 24u + psz) break;
                    lit_form = (int)(frame[soff] & 1u);
                    k = (int)(lit_form ? bp.sub.ctx_count
                                       : bp.plain.ctx_count);
                    memset(&sh, 0, sizeof(sh));
                    if (zgec_seg_header_parse_ex(&sh, frame + soff, clen,
                                                 NULL, 0, NULL, k) != 0) {
                        if ((sh.segment_flags >> 1) & 1u) (*rans)++;
                        if (sh.segment_flags & 1u) (*sub)++;
                        if (sh.segment_flags & 0x08u) (*cond)++;
                    }
                    soff += clen;
                }
            }
        }
        off += 24 + psz;
    }
}

/* Build one test texture. Each texture is chosen because it gives the
 * per-segment coder selection (section 11.7) a reason to prefer a
 * different family; the test then requires that every family is emitted
 * somewhere across the set. */
#define ZGEC_TEST_TEXTURES 4
static void zgec_test_texture(int kind, uint8_t *src, size_t n)
{
    const char *w = "the quick brown fox jumps over the lazy dog 0123456789 ";
    size_t wl = strlen(w);
    uint32_t r = 12345u;
    for (size_t i = 0; i < n; i++) {
        r = r * 1664525u + 1013904223u;
        switch (kind) {
        case 0: /* text with sporadic noise: rANS + contexts + cond */
            src[i] = (uint8_t)((i % 251u) == 0u ? (uint8_t)(i * 7u)
                                                : w[i % wl]);
            break;
        case 1: /* constant-delta ramp: sub-literal residuals vanish */
            src[i] = (uint8_t)((i % 97u) == 0u ? (uint8_t)((r >> 9) & 0x1Fu)
                                               : (uint8_t)(i * 7u));
            break;
        case 2: /* low-entropy binary: rANS over a skewed alphabet */
            src[i] = (uint8_t)((r >> 8) & 0x0Fu);
            break;
        default: /* mixed text and columnar structure */
            src[i] = (uint8_t)((i & 7u) == 0u ? (uint8_t)((i >> 3) & 0x3Fu)
                                              : w[(i + (i >> 4)) % wl]);
            break;
        }
    }
}

/* Section 11.7: the encoder must be able to emit rANS literals, learned
 * context tables, sub-literals and OF conditioning, and every emitted
 * frame must round-trip. */
static int zgec_test_coder_paths(void)
{
    static const int combos[5][3] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 1, 1 }
    };
    size_t n = 400000;
    uint8_t *src = (uint8_t *)malloc(n);
    int tot_rans = 0;
    int tot_sub = 0;
    int tot_cond = 0;
    int tot_ctx = 0;
    int ok = 1;
    int c;
    int kind;
    if (!src) return 0;
    for (kind = 0; kind < ZGEC_TEST_TEXTURES && ok; kind++) {
        zgec_test_texture(kind, src, n);
        for (c = 0; c < 5 && ok; c++) {
            zgec_params p;
            zgec_encoder *e;
            uint8_t *cmp = NULL;
            size_t cmp_size = 0;
            zgec_err err;
            zgec_params_default(&p);
            p.use_contexts = combos[c][0];
            p.use_sublit = combos[c][1];
            p.use_conditioning = combos[c][2];
            e = zgec_encoder_create(&p);
            if (!e) { ok = 0; break; }
            err = zgec_encode_frame(e, src, n, &cmp, &cmp_size);
            zgec_encoder_destroy(e);
            if (err != ZGEC_OK) { ok = 0; break; }
            {
                zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
                uint8_t *out = NULL;
                size_t out_size = 0;
                err = zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
                zgec_decoder_destroy(d);
                if (err != ZGEC_OK || out_size != n ||
                    memcmp(src, out, n) != 0)
                    ok = 0;
                zgec_free(out);
            }
            {
                int r = 0;
                int s = 0;
                int co = 0;
                zgec_count_coders(cmp, cmp_size, &r, &s, &co);
                tot_rans += r;
                tot_sub += s;
                tot_cond += co;
                if (p.use_contexts) tot_ctx += r;
            }
            zgec_free(cmp);
        }
    }
    free(src);
    printf("coders: rans=%d sub=%d cond_of=%d\n", tot_rans, tot_sub, tot_cond);
    /* All three coder families must actually be emitted somewhere in
     * this set of inputs, and every frame must round-trip. */
    if (!ok || tot_rans <= 0 || tot_sub <= 0 || tot_cond <= 0) return 0;
    (void)tot_ctx;
    return 1;
}

/* Parse a frame's header, footer and trailer. */
static int t_frame_parts(const uint8_t *frame, size_t n, zgec_frame_header *fh,
                         zgec_footer *f)
{
    zgec_trailer tr;
    uint64_t off;
    if (n < 56u) return 0;
    if (zgec_frame_header_parse(fh, frame) != ZGEC_OK) return 0;
    if (zgec_trailer_parse(&tr, frame + n - 16) != ZGEC_OK) return 0;
    off = tr.footer_offset;
    if (off + (uint64_t)tr.footer_size + 16u != (uint64_t)n) return 0;
    if (zgec_footer_parse(f, frame + off, tr.footer_size) != ZGEC_OK) return 0;
    return 1;
}

/* A repetitive but non-trivial body: many literal runs and matches, and
 * enough blocks for the literal-reference chain to span several of them. */
static void t_body(uint8_t *src, size_t n)
{
    const char *w = "int foo(void) { return bar(1, 2) + baz(3); } /* note */\n";
    size_t wl = strlen(w);
    for (size_t i = 0; i < n; i++) src[i] = (uint8_t)w[i % wl];
    for (size_t i = 0; i + 7 < n; i += 4099) {
        src[i] = (uint8_t)(i >> 3);
        src[i + 1] = (uint8_t)(i * 7u);
    }
}

/* Largest segment_count over the frame's COMPRESSED records. Used to
 * prove a test really exercises multi-segment blocks (the serial decode
 * path walks the segment directory). */
static uint32_t t_max_segments(const uint8_t *frame, size_t n)
{
    zgec_frame_header fh;
    zgec_trailer tr;
    size_t off = 32;
    uint32_t best = 0;
    if (n < 56u) return 0;
    if (zgec_frame_header_parse(&fh, frame) != ZGEC_OK) return 0;
    if (zgec_trailer_parse(&tr, frame + n - 16) != ZGEC_OK) return 0;
    while (off + 24u <= (size_t)tr.footer_offset) {
        uint8_t rt = frame[off];
        uint32_t psz = t_rd32(frame + off + 8);
        if (rt == ZGEC_REC_COMPRESSED) {
            uint32_t sc = t_rd32(frame + off + 12);
            if (sc > best) best = sc;
        }
        off += 24u + (size_t)psz;
    }
    return best;
}

/* Line-based source-like body with varied lengths: many short matches
 * and many sequences, so a block splits into several segments. */
static void t_text_body(uint8_t *p, size_t n, uint32_t seed)
{
    static const char *wd[12] = { "static", "int", "return", "if", "else",
                                  "for", "while", "struct", "node", "value",
                                  "count", "buffer" };
    uint32_t s = seed ? seed : 7u;
    size_t k = 0;
    while (k < n) {
        uint32_t x;
        unsigned i;
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        x = s;
        for (i = 0; i < x % 9u && k < n; i++) p[k++] = ' ';
        {
            unsigned nw = 3u + ((x >> 8) % 8u);
            for (i = 0; i < nw && k < n; i++) {
                const char *q;
                s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                q = wd[s % 12u];
                for (; *q != '\0' && k < n; q++) p[k++] = (uint8_t)(unsigned char)*q;
                if (k < n) p[k++] = ((i + 1u) == nw) ? ';' : ' ';
            }
        }
        if (k < n) p[k++] = '\n';
    }
}

/* Deterministic body with a given percentage of incompressible bytes,
 * so blocks keep large literal buffers. The structural part is shared
 * between buffers; `seed` controls the noise so two buffers are not
 * byte-identical. */
static void t_noisy_body(uint8_t *p, size_t n, int noise_pct, uint32_t seed)
{
    const char *w = "struct node { int key; struct node *left, *right; };\n";
    size_t wl = strlen(w);
    /* xorshift, not an LCG: the low bits of an LCG are strongly
     * structured, so an LCG "noise" byte is still compressible. */
    uint32_t s = seed ? seed : 1u;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        if (noise_pct > 0 && (int)(s % 100u) < noise_pct)
            p[i] = (uint8_t)((s >> 24) ^ (s >> 8));
        else
            p[i] = (uint8_t)w[i % wl];
    }
}

/* Section 5.8: an external dictionary becomes a footer entry of kind 1,
 * a block may reference it, and the decoder must demand the same bytes. */
static int zgec_test_external_dict(void)
{
    size_t n = 700000;
    size_t dn = 262144;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *dict = (uint8_t *)malloc(dn);
    uint8_t *bad = (uint8_t *)malloc(dn);
    uint8_t *plain = NULL, *ext = NULL;
    size_t plain_size = 0, ext_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_err err;
    int ok = 0;
    int used = 0;
    zgec_frame_header fh;
    zgec_footer f;
    uint16_t id = 7;
    size_t i;

    if (!src || !dict || !bad) { free(src); free(dict); free(bad); return 0; }
    t_body(src, n);
    memcpy(dict, src, dn);
    memcpy(bad, dict, dn);
    bad[dn / 2] ^= 0x5Au;
    memset(&f, 0, sizeof(f));

    zgec_params_default(&p);
    p.block_log2 = 16;
    p.n_threads = 2;

    /* Rejections at registration (5.8). */
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    if (zgec_encoder_set_external_dict(e, 0, dict, dn) != ZGEC_ERR_INVAL) {
        zgec_encoder_destroy(e);
        goto out;
    }
    if (zgec_encoder_set_external_dict(e, id, dict, 0) != ZGEC_ERR_INVAL) {
        zgec_encoder_destroy(e);
        goto out;
    }
    if (zgec_encoder_set_external_dict(e, id, dict, dn) != ZGEC_OK) {
        zgec_encoder_destroy(e);
        goto out;
    }
    if (zgec_encoder_set_external_dict(e, id, bad, dn) != ZGEC_OK) {
        zgec_encoder_destroy(e);
        goto out;
    }
    if (zgec_encoder_set_external_dict(e, id, dict, dn) != ZGEC_OK) {
        zgec_encoder_destroy(e);
        goto out;
    }
    zgec_encoder_destroy(e);

    /* Baseline without a dictionary. */
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encode_frame(e, src, n, &plain, &plain_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;

    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encoder_set_external_dict(e, id, dict, dn);
    if (err != ZGEC_OK) { zgec_encoder_destroy(e); goto out; }
    err = zgec_encode_frame(e, src, n, &ext, &ext_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    if (ext_size >= plain_size) goto out; /* 5.5: only kept when smaller */

    if (!t_frame_parts(ext, ext_size, &fh, &f)) goto out;
    if ((fh.flags & ZGEC_FLAG_EXTERNAL_DICT) == 0) goto out;
    /* Footer: one kind-1 entry per external dictionary, offset 0, hash. */
    {
        int found = 0;
        for (i = 0; i < f.dict_count; i++) {
            if (f.dicts[i].dict_id != id) continue;
            if (f.dicts[i].kind != 1) goto out;
            if (f.dicts[i].offset != 0) goto out;
            if (f.dicts[i].raw_size != (uint32_t)dn) goto out;
            if (f.dicts[i].content_hash != zgec_xxh64(dict, dn, 0)) goto out;
            found = 1;
        }
        if (!found) goto out;
        /* Exactly one entry per registered external dictionary. */
        if (f.dict_count != 1) goto out;
    }
    for (i = 0; i < f.block_count; i++) {
        if (f.blocks[i].dict_id == id) used++;
    }
    if (used == 0) goto out; /* a block must actually reference it */
    zgec_footer_free(&f);
    memset(&f, 0, sizeof(f));

    /* Decode with the same bytes; without them; with wrong bytes. */
    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decoder_add_external_dict(d, id, dict, dn);
        if (err != ZGEC_OK) { zgec_decoder_destroy(d); goto out; }
        err = zgec_decode_frame(d, ext, ext_size, &out, &out_size);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || out_size != n ||
            memcmp(out, src, n) != 0) {
            zgec_free(out);
            goto out;
        }
        zgec_free(out);
    }
    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decode_frame(d, ext, ext_size, &out, &out_size);
        zgec_decoder_destroy(d);
        zgec_free(out);
        if (err != ZGEC_ERR_DICT_NOT_FOUND) goto out;
    }
    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decoder_add_external_dict(d, id, bad, dn);
        if (err == ZGEC_OK) {
            err = zgec_decode_frame(d, ext, ext_size, &out, &out_size);
            zgec_decoder_destroy(d);
            zgec_free(out);
            if (err != ZGEC_ERR_DICT_HASH) goto out;
        } else {
            zgec_decoder_destroy(d);
            if (err != ZGEC_ERR_DICT_HASH) goto out;
        }
    }

    /* Clearing the registry removes the flag from the next frame. */
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    if (zgec_encoder_set_external_dict(e, id, dict, dn) != ZGEC_OK) {
        zgec_encoder_destroy(e);
        goto out;
    }
    zgec_encoder_clear_external_dicts(e);
    {
        uint8_t *again = NULL;
        size_t again_size = 0;
        zgec_frame_header gh;
        zgec_footer g;
        memset(&g, 0, sizeof(g));
        err = zgec_encode_frame(e, src, n, &again, &again_size);
        zgec_encoder_destroy(e);
        if (err != ZGEC_OK) { zgec_free(again); goto out; }
        if (again_size != plain_size) { zgec_free(again); goto out; }
        if (!t_frame_parts(again, again_size, &gh, &g)) {
            zgec_free(again);
            goto out;
        }
        /* The cleared registry must leave no trace in the frame. */
        if ((gh.flags & ZGEC_FLAG_EXTERNAL_DICT) != 0 || g.dict_count != 0) {
            zgec_free(again);
            zgec_footer_free(&g);
            goto out;
        }
        zgec_footer_free(&g);
        zgec_free(again);
    }
    ok = 1;
out:
    zgec_footer_free(&f);
    zgec_free(plain);
    zgec_free(ext);
    free(src);
    free(dict);
    free(bad);
    return ok;
}

/* Section 6.3: a frame may carry literal references; the decoder needs
 * EXTENDED, random access still works, and Core must refuse the frame. */
static int zgec_test_literal_refs(void)
{
    size_t n = 700000;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *with = NULL, *without = NULL;
    size_t with_size = 0, without_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_err err;
    zgec_frame_header fh;
    zgec_footer f;
    int ok = 0;
    int depth_blocks = 0;
    size_t i;

    if (!src) return 0;
    t_body(src, n);
    memset(&f, 0, sizeof(f));

    zgec_params_default(&p);
    p.block_log2 = 16;
    p.n_threads = 2;
    p.use_litref = 1;
    p.use_contexts = 1;

    e = zgec_encoder_create(&p);
    if (!e) { free(src); return 0; }
    err = zgec_encode_frame(e, src, n, &with, &with_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    p.use_litref = 0;
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encode_frame(e, src, n, &without, &without_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;

    if (!t_frame_parts(with, with_size, &fh, &f)) goto out;
    if ((fh.flags & ZGEC_FLAG_EXTENDED_LITREF) == 0) goto out;
    for (i = 0; i < f.block_count; i++) {
        if (f.blocks[i].lit_ref_depth > ZGEC_MAX_LITREF_DEPTH) goto out;
        if (f.blocks[i].lit_ref_depth > 0) depth_blocks++;
    }
    if (depth_blocks == 0) goto out;
    zgec_footer_free(&f);
    memset(&f, 0, sizeof(f));
    /* 6.3: a block is re-encoded with the region and the variant kept
     * only when it is strictly smaller, so a live chain (depth > 0
     * above) implies strictly fewer bytes. */
    if (with_size >= without_size) goto out;

    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decode_frame(d, with, with_size, &out, &out_size);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || out_size != n || memcmp(out, src, n) != 0) {
            zgec_free(out);
            goto out;
        }
        zgec_free(out);
    }
    {
        /* Random access into a block that follows a reference chain. */
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        const uint8_t *blk = NULL;
        size_t blk_size = 0;
        uint64_t bsize = (uint64_t)1 << fh.block_log2;
        uint64_t off = bsize + (bsize / 2); /* block 1 */
        err = zgec_decode_block(d, with, with_size, off, &blk, &blk_size);
        if (err != ZGEC_OK ||
            memcmp(blk, src + (size_t)((off / bsize) * bsize),
                   blk_size) != 0) {
            zgec_decoder_destroy(d);
            goto out;
        }
        zgec_decoder_destroy(d);
    }
    {
        /* V9: Core must refuse the frame *because* of the literal
         * references, not for any other reason. */
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_CORE, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decode_frame(d, with, with_size, &out, &out_size);
        zgec_decoder_destroy(d);
        zgec_free(out);
        if (err != ZGEC_ERR_LITREF_EXPORT) goto out;
    }
    {
        /* Literal export of one block (the LITREF source, 6.3). */
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *lit = NULL;
        size_t lit_size = 0;
        err = zgec_export_literals(d, with, with_size, 1, &lit, &lit_size);
        zgec_decoder_destroy(d);
        zgec_free(lit);
        if (err != ZGEC_OK || lit_size == 0) goto out;
    }
    ok = 1;
out:
    zgec_footer_free(&f);
    zgec_free(with);
    zgec_free(without);
    free(src);
    return ok;
}

/* Section 5.5 with both dictionary kinds: a trained epoch dictionary
 * that no block ends up referencing must not be counted or emitted, or
 * the frame would carry a zero-id dictionary entry and no decoder could
 * read it. */
static int zgec_test_dict_no_orphan(void)
{
    size_t block = 1u << 16;      /* 64 KiB blocks */
    size_t nblk = 12;             /* two epochs with the default 10 */
    size_t n = nblk * block;
    size_t dn = n;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *dict = (uint8_t *)malloc(dn);
    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_err err;
    zgec_frame_header fh;
    zgec_footer f;
    uint16_t did = 9;
    int ok = 0;
    size_t i;

    if (!src || !dict) { free(src); free(dict); return 0; }
    t_text_body(src, n, 11u);
    /* The external dictionary is the whole body, so every block takes it
     * (5.8) and the epoch candidates lose all of their users. */
    memcpy(dict, src, n);
    memset(&f, 0, sizeof(f));

    zgec_params_default(&p);
    p.block_log2 = 16;
    p.use_dicts = 1;
    p.use_contexts = 1;
    p.n_threads = 2;

    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encoder_set_external_dict(e, did, dict, dn);
    if (err == ZGEC_OK) err = zgec_encode_frame(e, src, n, &cmp, &cmp_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    if (!t_frame_parts(cmp, cmp_size, &fh, &f)) goto out;
    if (f.block_count != nblk) goto out;
    /* Exactly one dictionary, the external one: no orphan entry. */
    if (f.dict_count != 1) goto out;
    if (f.dicts[0].dict_id != did || f.dicts[0].kind != 1) goto out;
    for (i = 0; i < f.block_count; i++) {
        if (f.blocks[i].dict_id != did) goto out;
    }

    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decoder_add_external_dict(d, did, dict, dn);
        if (err == ZGEC_OK)
            err = zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || out_size != n || memcmp(out, src, n) != 0) {
            zgec_free(out);
            goto out;
        }
        zgec_free(out);
    }
    ok = 1;
out:
    zgec_footer_free(&f);
    zgec_free(cmp);
    free(src);
    free(dict);
    return ok;
}

/* Section 11.8: the worker pool must not change the encoded bytes, and
 * every worker count must decode what every other one produced. */
static int zgec_test_threads(void)
{
    /* 512 KiB blocks with the default 256 KiB segment hint, so blocks
     * hold several segments and the serial decode path (n_threads == 1)
     * really walks the segment directory. */
    size_t n = 3u * 524288u;   /* 512 KiB blocks, three of them */
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *one = NULL, *many = NULL;
    size_t one_size = 0, many_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_err err;
    int ok = 0;
    int t;
    if (!src) return 0;
    /* Every block holds a repetitive half and an incompressible half, so
     * the segmenter splits it and the block really has several segments. */
    for (t = 0; t < 3; t++) {
        size_t half = 262144u;
        t_text_body(src + (size_t)t * 524288u, half, 4242u + (uint32_t)t);
        t_noisy_body(src + (size_t)t * 524288u + half, half, 100,
                     4242u + (uint32_t)t);
    }

    zgec_params_default(&p);
    p.block_log2 = 19;
    p.use_contexts = 1;
    p.use_sublit = 1;
    p.use_conditioning = 1;

    p.n_threads = 1;
    e = zgec_encoder_create(&p);
    if (!e) { free(src); return 0; }
    err = zgec_encode_frame(e, src, n, &one, &one_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    /* The test only means something if some block has >= 2 segments. */
    if (t_max_segments(one, one_size) < 2) goto out;

    p.n_threads = 4;
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encode_frame(e, src, n, &many, &many_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    if (one_size != many_size || memcmp(one, many, one_size) != 0) goto out;

    for (t = 0; t < 2; t++) {
        int nt = t ? 4 : 1;
        zgec_limits lim;
        zgec_decoder *d;
        uint8_t *out = NULL;
        size_t out_size = 0;
        memset(&lim, 0, sizeof(lim));
        lim.n_threads = nt;
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
        if (!d) goto out;
        /* Serial decode must read what the pool wrote, and vice versa. */
        err = zgec_decode_frame(d, (t == 0) ? many : one,
                                (t == 0) ? many_size : one_size,
                                &out, &out_size);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || out_size != n || memcmp(out, src, n) != 0) {
            zgec_free(out);
            goto out;
        }
        zgec_free(out);
    }
    ok = 1;
out:
    zgec_free(one);
    zgec_free(many);
    free(src);
    return ok;
}

/* Section 6.3 with every feature requested at once: literal references
 * need plain, unconditioned predecessors, so the encoder must keep the
 * chain alive instead of emitting the optional sub-literal form or LL
 * conditioning, and the frame must still round-trip. */
static int zgec_test_litref_all_flags(void)
{
    size_t n = 700000;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_err err;
    zgec_frame_header fh;
    zgec_footer f;
    int ok = 0;
    int depth_blocks = 0;
    size_t i;

    if (!src) return 0;
    t_body(src, n);
    memset(&f, 0, sizeof(f));
    zgec_params_default(&p);
    p.block_log2 = 16;
    p.n_threads = 2;
    p.use_litref = 1;
    p.use_contexts = 1;
    p.use_sublit = 1;
    p.use_conditioning = 1;

    e = zgec_encoder_create(&p);
    if (!e) { free(src); return 0; }
    err = zgec_encode_frame(e, src, n, &cmp, &cmp_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;
    if (!t_frame_parts(cmp, cmp_size, &fh, &f)) goto out;
    if ((fh.flags & ZGEC_FLAG_EXTENDED_LITREF) == 0) goto out;
    for (i = 0; i < f.block_count; i++) {
        if (f.blocks[i].lit_ref_depth > 0) depth_blocks++;
    }
    if (depth_blocks == 0) goto out; /* the chain must stay alive */
    {
        zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        uint8_t *out = NULL;
        size_t out_size = 0;
        err = zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || out_size != n || memcmp(out, src, n) != 0) {
            zgec_free(out);
            goto out;
        }
        zgec_free(out);
    }
    ok = 1;
out:
    zgec_footer_free(&f);
    zgec_free(cmp);
    free(src);
    return ok;
}

/* Section 6.3 with a large dictionary: dict + region + block must stay
 * inside the P24 position limit, so the region is bounded and D shrinks.
 * A shrunken D must still name the *newest* D predecessors, the set the
 * decoder rebuilds, or the round-trip breaks. */static int zgec_test_litref_bound(void)
{
    size_t block = (size_t)1 << 21;      /* 2 MiB blocks */
    size_t n = 6u * block;               /* six blocks: deep enough that the
                                          * P24 16MiB cap binds regardless of
                                          * parse-level literal savings */
    static const size_t dict_mib[5] = { 11u, 12u, 13u, 14u, 15u };
    int round_tripped = 0;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    uint16_t did = 5;
    int ok = 0;
    int depth_blocks = 0;   /* blocks that reference a literal region */
    int shrunken = 0;       /* blocks whose depth is below what is available */
    int bad_depth = 0;      /* blocks referencing a non-exportable run */
    size_t cf;

    if (!src) return 0;
    /* A body that is roughly half literals keeps each block's literal
     * buffer large enough for the P24 prefix bound to bite. */
    t_noisy_body(src, n, 45, 999u);

    /* Whether the ratio gate chooses to reference a predecessor's literals
     * at all, and how many of them fit, depends on the parse (rep2 probing
     * included), so the test does not pin one dictionary size: it sweeps
     * sizes that all leave less room than three predecessors need (P24 caps
     * the virtual buffer at 16 MiB, so 11..15 MiB of dictionary plus a 2 MiB
     * block leaves under 5 MiB), and requires the chain to be used AND to be
     * shrunk below the available run somewhere in the sweep. What must hold
     * in EVERY configuration is the invariant: a block never references more
     * exportable predecessors than it actually has (6.3). */
    for (cf = 0; cf < sizeof(dict_mib) / sizeof(dict_mib[0]); cf++) {
        size_t dn = dict_mib[cf] << 20;
        uint8_t *dict = (uint8_t *)malloc(dn);
        zgec_params p;
        zgec_encoder *e;
        zgec_err err;
        zgec_frame_header fh;
        zgec_footer f;
        size_t i;
        int shrunk_here = 0;

        if (!dict) break;
        memset(&f, 0, sizeof(f));
        t_noisy_body(dict, dn, 45, 12345u);

        zgec_params_default(&p);
        p.block_log2 = 21;
        p.max_dict_log2 = 24;   /* 24.5.8: the frame must admit the dictionary */
        p.use_litref = 1;
        p.use_contexts = 1;
        p.n_threads = 4;

        e = zgec_encoder_create(&p);
        if (!e) { free(dict); break; }
        err = zgec_encoder_set_external_dict(e, did, dict, dn);
        if (err == ZGEC_OK)
            err = zgec_encode_frame(e, src, n, &cmp, &cmp_size);
        zgec_encoder_destroy(e);
        if (err != ZGEC_OK) {
            printf("  litref_bound: encode err=%d\n", err);
            free(dict);
            goto out;
        }
        if (!t_frame_parts(cmp, cmp_size, &fh, &f)) {
            printf("  litref_bound: frame parts failed\n");
            free(dict);
            goto out;
        }
        if (f.block_count != 6) {
            printf("  litref_bound: block_count=%u\n", f.block_count);
            zgec_footer_free(&f);
            free(dict);
            goto out;
        }
        for (i = 0; i < f.block_count; i++) {
            /* run = how many exportable predecessors the block has, which
             * is what bounds the encoder's chain (6.3). */
            size_t run = 0;
            size_t j;
            for (j = i; j > 0; j--) {
                if ((f.blocks[j - 1].rflags & ZGEC_RFLAG_LIT_EXPORTABLE) == 0)
                    break;
                run++;
            }
            if ((size_t)f.blocks[i].lit_ref_depth > run) bad_depth++;
            if (f.blocks[i].lit_ref_depth > 0) {
                depth_blocks++;
                /* Fewer references than available predecessors can only
                 * mean the P24 prefix bound shrank the region. */
                if ((size_t)f.blocks[i].lit_ref_depth < run) {
                    shrunken++;
                    shrunk_here = 1;
                }
            }
        }
        if (shrunk_here != 0) {
            /* Round-trip the configuration that exercised the shrink. A
             * shrunken depth that named the wrong predecessor set (oldest
             * instead of newest) surfaces here, not in the footer. */
            zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
            uint8_t *out = NULL;
            size_t out_size = 0;
            err = zgec_decoder_add_external_dict(d, did, dict, dn);
            if (err == ZGEC_OK)
                err = zgec_decode_frame(d, cmp, cmp_size, &out, &out_size);
            zgec_decoder_destroy(d);
            if (err != ZGEC_OK || out_size != n || memcmp(out, src, n) != 0) {
                printf("  litref_bound: round-trip failed err=%d size=%zu "
                       "want=%zu\n", (int)err, out_size, n);
                zgec_free(out);
                zgec_footer_free(&f);
                free(dict);
                goto out;
            }
            zgec_free(out);
            round_tripped = 1;
        }
        zgec_footer_free(&f);
        free(dict);
    }

    /* The chain must be used, the bound must have shrunk it somewhere in
     * the sweep, and no configuration may reference a non-exportable
     * predecessor. */
    if (bad_depth != 0 || depth_blocks == 0 || shrunken == 0 ||
        round_tripped == 0) {
        printf("  litref_bound: chain unused/bounded wrongly "
               "(depth_blocks=%d shrunken=%d bad_depth=%d)\n",
               depth_blocks, shrunken, bad_depth);
        goto out;
    }
    ok = 1;
out:
    zgec_free(cmp);
    free(src);
    return ok;
}

/* Section 10.6 block parallelism. The parallel path writes each block
 * straight into its slice of the frame output, so RLE, RAW and COMPRESSED
 * records all have to land in the right place, and the result must match
 * the serial scan byte for byte. */
static int zgec_test_block_parallel(void)
{
    const size_t bs = 262144u;   /* 256 KiB blocks: 4 of them */
    size_t n = 4u * bs;
    uint8_t *src = (uint8_t *)malloc(n);
    uint8_t *frame = NULL;
    uint8_t *serial = NULL;
    uint8_t *par = NULL;
    size_t frame_size = 0, serial_size = 0, par_size = 0;
    zgec_params p;
    zgec_encoder *e;
    zgec_frame_header fh;
    zgec_footer f;
    zgec_err err;
    uint32_t b;
    int n_rle = 0;
    int n_raw = 0;
    int t;
    int ok = 0;
    if (!src) return 0;

    memset(src, 0x5A, bs);                   /* all equal -> RLE */
    t_noisy_body(src + bs, bs, 100, 7u);     /* incompressible -> RAW */
    t_text_body(src + 2u * bs, bs, 11u);     /* COMPRESSED */
    t_text_body(src + 3u * bs, bs, 29u);     /* COMPRESSED */

    zgec_params_default(&p);
    p.block_log2 = 18;
    p.use_contexts = 1;
    p.n_threads = 1;
    e = zgec_encoder_create(&p);
    if (!e) goto out;
    err = zgec_encode_frame(e, src, n, &frame, &frame_size);
    zgec_encoder_destroy(e);
    if (err != ZGEC_OK) goto out;

    /* The test only means something if the frame really carries an RLE and
     * a RAW block, since those are the branches the parallel path redirects. */
    if (!t_frame_parts(frame, frame_size, &fh, &f)) goto out;
    for (b = 0; b < f.block_count; b++) {
        zgec_record_header rh;
        if (zgec_record_header_parse(&rh, frame + (size_t)f.blocks[b].offset,
                                     &fh) != ZGEC_OK) {
            zgec_footer_free(&f);
            goto out;
        }
        if (rh.record_type == ZGEC_REC_RLE) n_rle++;
        if (rh.record_type == ZGEC_REC_RAW) n_raw++;
    }
    zgec_footer_free(&f);
    if (n_rle < 1 || n_raw < 1) goto out;

    for (t = 0; t < 2; t++) {
        zgec_limits lim;
        zgec_decoder *d;
        uint8_t **dst = (t == 0) ? &serial : &par;
        size_t *szp = (t == 0) ? &serial_size : &par_size;
        memset(&lim, 0, sizeof(lim));
        lim.n_threads = (t == 0) ? 1 : 5;
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, &lim);
        if (!d) goto out;
        err = zgec_decode_frame(d, frame, frame_size, dst, szp);
        zgec_decoder_destroy(d);
        if (err != ZGEC_OK || *szp != n || memcmp(*dst, src, n) != 0) goto out;
    }
    if (serial_size != par_size || memcmp(serial, par, serial_size) != 0)
        goto out;
    ok = 1;
out:
    zgec_free(frame);
    zgec_free(serial);
    zgec_free(par);
    free(src);
    return ok;
}
/* Section 4.6 exhaustive random access: EVERY block of a multi-block
 * frame must decode identically by index and by original-data offset,
 * and must match both the sequential full-frame decode and the source.
 * Covers first/middle/last byte offsets of each block, by-index vs
 * by-offset agreement, out-of-range offset/index rejection, RAW/RLE/
 * COMPRESSED record kinds, checksums, and litref chains. */
static int zgec_test_random_access_full(void)
{
    uint8_t *src = NULL;
    uint8_t *frame = NULL;
    size_t frame_size = 0;
    uint8_t *seq = NULL;
    size_t seq_size = 0;
    zgec_params p;
    zgec_encoder *e = NULL;
    zgec_decoder *d = NULL;
    zgec_frame_header fh;
    zgec_footer f;
    zgec_err err;
    int ok = 0;
    uint64_t bsize;
    uint32_t b;
    size_t n = 700000;
    int mixed = 0;
    memset(&f, 0, sizeof(f));
    for (mixed = 0; mixed < 2; mixed++) {
#define RA_FAIL(msg) do { \
        printf("  random_access_full: case=%d blocks=%u step=%s err=%d\n", \
               mixed, f.block_count, msg, (int)err); \
        goto ra_out2; \
    } while (0)
        if (mixed == 0) {
            n = 700000;
            src = (uint8_t *)malloc(n);
            if (!src) goto ra_out;
            t_body(src, n);
            zgec_params_default(&p);
            p.block_log2 = 16;
            p.n_threads = 2;
            p.use_litref = 1;
            p.use_contexts = 1;
            p.block_checksums = 1;
        } else {
            const size_t bs = 65536u;
            n = 6u * bs;
            src = (uint8_t *)malloc(n);
            if (!src) goto ra_out;
            memset(src, 0x5Au, bs);
            {
                /* Truly incompressible: full xorshift bytes, no low-bit
                 * structure (the old (s>>7)^(s>>19)^k form has k-XOR
                 * structure that rep2 matching now compresses, flipping
                 * this block from RAW to COMPRESSED). */
                uint32_t s = 12345u;
                size_t k;
                for (k = 0; k < bs; k++) {
                    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
                    src[bs + k] = (uint8_t)(s >> 11);
                    s ^= s << 7;
                    src[bs + k] ^= (uint8_t)s;
                }
            }
            t_text_body(src + 2u * bs, n - 2u * bs, 77u);
            zgec_params_default(&p);
            p.block_log2 = 16;
            p.n_threads = 1;
            p.use_litref = 0;
            p.use_contexts = 0;
            p.block_checksums = 1;
        }
        e = zgec_encoder_create(&p);
        if (!e) goto ra_out;
        err = zgec_encode_frame(e, src, n, &frame, &frame_size);
        zgec_encoder_destroy(e);
        e = NULL;
        if (err != ZGEC_OK) RA_FAIL("encode");
        if (!t_frame_parts(frame, frame_size, &fh, &f)) RA_FAIL("frame_parts");
        if (f.block_count < 2) RA_FAIL("single-block");
        bsize = (uint64_t)1 << fh.block_log2;
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        if (!d) RA_FAIL("decoder-create");
        err = zgec_decode_frame(d, frame, frame_size, &seq, &seq_size);
        if (err != ZGEC_OK || seq_size != n || memcmp(seq, src, n) != 0)
            RA_FAIL("sequential");

        /* Every block: by index, plus start/mid/end offsets. */
        for (b = 0; b < f.block_count; b++) {
            const uint8_t *bi = NULL;
            size_t bisz = 0;
            uint64_t base = (uint64_t)b * bsize;
            uint64_t esz = (base + bsize <= (uint64_t)n)
                ? bsize : ((uint64_t)n - base);
            uint64_t probes[3];
            int pi;
            if (base >= (uint64_t)n) RA_FAIL("base-beyond");
            probes[0] = base;
            probes[1] = base + esz / 2u;
            probes[2] = base + esz - 1u;
            err = zgec_decode_block_index(d, frame, frame_size, b,
                                          &bi, &bisz);
            if (err != ZGEC_OK || (uint64_t)bisz != esz) RA_FAIL("by-index");
            if (memcmp(bi, src + (size_t)base, bisz) != 0)
                RA_FAIL("by-index-bytes");
            if (memcmp(bi, seq + (size_t)base, bisz) != 0)
                RA_FAIL("by-index-vs-seq");
            for (pi = 0; pi < 3; pi++) {
                const uint8_t *bo = NULL;
                size_t bosz = 0;
                uint64_t off = probes[pi];
                if (off >= (uint64_t)n) continue;
                err = zgec_decode_block(d, frame, frame_size, off,
                                        &bo, &bosz);
                if (err != ZGEC_OK) RA_FAIL("by-offset");
                if (bosz != bisz || bo != bi) RA_FAIL("idx-vs-off");
                if (memcmp(bo, src + (size_t)base, bosz) != 0)
                    RA_FAIL("by-offset-bytes");
                if (bo[(size_t)(off - base)] != src[(size_t)off])
                    RA_FAIL("byte-lane");
            }
        }
        /* Out-of-range offset/index must fail, never decode. */
        {
            const uint8_t *bx = NULL;
            size_t bxsz = 0;
            if (zgec_decode_block(d, frame, frame_size, (uint64_t)n,
                                  &bx, &bxsz) == ZGEC_OK) RA_FAIL("off-eq-n");
            if (zgec_decode_block_index(d, frame, frame_size,
                                        f.block_count, &bx,
                                        &bxsz) == ZGEC_OK) RA_FAIL("idx-eq-n");
            if (zgec_decode_block_index(d, frame, frame_size,
                                        f.block_count + 100u, &bx,
                                        &bxsz) == ZGEC_OK)
                RA_FAIL("idx-huge");
        }
        /* Truncated frame must fail, never crash. */
        if (frame_size > 64) {
            zgec_decoder *d2 = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
            const uint8_t *bx = NULL;
            size_t bxsz = 0;
            zgec_err e2;
            if (!d2) RA_FAIL("decoder2");
            e2 = zgec_decode_block_index(d2, frame, frame_size - 32u, 0,
                                         &bx, &bxsz);
            zgec_decoder_destroy(d2);
            if (e2 == ZGEC_OK) RA_FAIL("truncated");
        }
        /* Case B must really hold RLE + RAW + COMPRESSED kinds. */
        if (mixed == 1) {
            int n_rle = 0, n_raw = 0, n_cmp = 0;
            size_t i;
            for (i = 0; i < f.block_count; i++) {
                zgec_record_header rh;
                if (zgec_record_header_parse(&rh,
                        frame + (size_t)f.blocks[i].offset,
                        &fh) != ZGEC_OK) RA_FAIL("record-parse");
                if (rh.record_type == ZGEC_REC_RLE) n_rle++;
                else if (rh.record_type == ZGEC_REC_RAW) n_raw++;
                else if (rh.record_type == ZGEC_REC_COMPRESSED) n_cmp++;
            }
            if (n_rle < 1 || n_raw < 1 || n_cmp < 1) {
                printf("  random_access_full: kinds RLE=%d RAW=%d CMP=%d\n",
                       n_rle, n_raw, n_cmp);
                RA_FAIL("kinds");
            }
        }
        zgec_decoder_destroy(d);
        d = NULL;
        zgec_footer_free(&f);
        memset(&f, 0, sizeof(f));
        zgec_free(frame);
        frame = NULL;
        frame_size = 0;
        zgec_free(seq);
        seq = NULL;
        seq_size = 0;
        free(src);
        src = NULL;
        continue;
ra_out2:
        if (d) { zgec_decoder_destroy(d); d = NULL; }
        goto ra_out;
    }
#undef RA_FAIL
    /* Random offsets across the last-built input hit the right block.
     * Re-encode case A first: the loop above freed both frames, so
     * `frame/src/n/fh` no longer name a live buffer here. */
    {
        uint32_t s = 0xC0FFEEu;
        uint32_t k;
        n = 700000;
        free(src);
        src = (uint8_t *)malloc(n);
        if (!src) goto ra_out;
        t_body(src, n);
        zgec_params_default(&p);
        p.block_log2 = 16;
        p.n_threads = 1;
        p.use_litref = 0;
        p.use_contexts = 0;
        p.block_checksums = 0;
        e = zgec_encoder_create(&p);
        if (!e) goto ra_out;
        err = zgec_encode_frame(e, src, n, &frame, &frame_size);
        zgec_encoder_destroy(e);
        e = NULL;
        if (err != ZGEC_OK || !t_frame_parts(frame, frame_size, &fh, &f)) {
            printf("  random_access_full: rand-phase encode err=%d\n",
                   (int)err);
            goto ra_out;
        }
        d = zgec_decoder_create(ZGEC_LEVEL_EXTENDED, NULL);
        if (!d) goto ra_out;
        bsize = (uint64_t)1 << fh.block_log2;
        for (k = 0; k < 64u; k++) {
            uint64_t ro;
            uint32_t want;
            const uint8_t *br = NULL;
            size_t brsz = 0;
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            ro = (uint64_t)(s % (uint32_t)n);
            want = (uint32_t)(ro / bsize);
            err = zgec_decode_block(d, frame, frame_size, ro, &br, &brsz);
            if (err != ZGEC_OK) {
                printf("  random_access_full: rand-off %llu err=%d\n",
                       (unsigned long long)ro, (int)err);
                goto ra_out2b;
            }
            if (memcmp(br, src + (size_t)((uint64_t)want * bsize),
                       brsz) != 0) {
                printf("  random_access_full: rand-off %llu wrong block\n",
                       (unsigned long long)ro);
                goto ra_out2b;
            }
        }
        zgec_decoder_destroy(d);
        d = NULL;
    }
    ok = 1;
    goto ra_out;
ra_out2b:
    if (d) { zgec_decoder_destroy(d); d = NULL; }
ra_out:
    if (d) zgec_decoder_destroy(d);
    if (e) zgec_encoder_destroy(e);
    zgec_footer_free(&f);
    zgec_free(frame);
    zgec_free(seq);
    free(src);
    return ok;
}

int main(void)
{
    int passed = 0;
    int failed = 0;

    if (zgec_test_frame_round_trip()) passed++; else { printf("FAIL frame_round_trip\n"); failed++; }
    if (zgec_test_fse_round_trip()) passed++; else { printf("FAIL fse_round_trip\n"); failed++; }
    if (zgec_test_rans_round_trip()) passed++; else { printf("FAIL rans_round_trip\n"); failed++; }
    if (zgec_test_classify()) passed++; else { printf("FAIL classify\n"); failed++; }
    if (zgec_test_runstart()) passed++; else { printf("FAIL runstart\n"); failed++; }
    if (zgec_test_lane_starts()) passed++; else { printf("FAIL lane_starts\n"); failed++; }
    if (zgec_test_class_map_round_trip()) passed++; else { printf("FAIL class_map_round_trip\n"); failed++; }
    if (zgec_test_dict_cache()) passed++; else { printf("FAIL dict_cache\n"); failed++; }
    if (zgec_test_fse_seq_round_trip()) passed++; else { printf("FAIL fse_seq_round_trip\n"); failed++; }
    if (zgec_test_filter_transform()) passed++; else { printf("FAIL filter_transform\n"); failed++; }
    if (zgec_test_filter_frame()) passed++; else { printf("FAIL filter_frame\n"); failed++; }
    if (zgec_test_coder_paths()) passed++; else { printf("FAIL coder_paths\n"); failed++; }
    int rt = zgec_test_round_trip();
    if (rt) passed++; else { printf("FAIL round_trip\n"); failed++; }
    if (zgec_test_external_dict()) passed++; else { printf("FAIL external_dict\n"); failed++; }
    if (zgec_test_literal_refs()) passed++; else { printf("FAIL literal_refs\n"); failed++; }
    if (zgec_test_threads()) passed++; else { printf("FAIL threads\n"); failed++; }
    if (zgec_test_block_parallel()) passed++; else { printf("FAIL block_parallel\n"); failed++; }
    if (zgec_test_litref_all_flags()) passed++; else { printf("FAIL litref_all_flags\n"); failed++; }
    if (zgec_test_litref_bound()) passed++; else { printf("FAIL litref_bound\n"); failed++; }
    if (zgec_test_dict_no_orphan()) passed++; else { printf("FAIL dict_no_orphan\n"); failed++; }
    if (zgec_test_random_access_full()) passed++; else { printf("FAIL random_access_full\n"); failed++; }

    printf("PASS %d FAIL %d\n", passed, failed);
    return failed ? 1 : 0;
}
