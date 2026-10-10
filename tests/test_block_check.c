#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "zgec.h"
#include "zgec_block.h"
#include "zgec_bitstream.h"
#include "zgec_fse.h"

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

/* make a valid FSE description with given al, nsym (all counts 1-ish normalized) */
static size_t mk_desc(uint8_t *buf, size_t cap, int nsym, int al)
{
    int16_t counts[256];
    int S = 1 << al;
    memset(counts, 0, sizeof(counts));
    /* distribute S evenly so sum == S */
    int q = S / nsym, r = S % nsym;
    for (int s = 0; s < nsym; s++) counts[s] = (int16_t)(q + (s < r ? 1 : 0));
    if (q == 0) { /* some would be 0; force -1 for first r */
        for (int s = 0; s < nsym; s++) counts[s] = (s < S) ? (int16_t)-1 : (int16_t)0;
    }
    return zgec_fse_write_counts(buf, cap, counts, nsym, al);
}

static void test_block_params(void)
{
    zgec_block_params bp, bp2;
    uint8_t buf[64];
    memset(&bp, 0, sizeof(bp));
    bp.plain.ctx_mode = 1; bp.plain.ctx_count = 4;
    for (int i = 0; i < 64; i++) bp.plain.class_map[i] = (uint8_t)(i % 4);
    bp.sub.ctx_mode = 0; bp.sub.ctx_count = 1;
    size_t n = zgec_block_params_emit(buf, sizeof(buf), &bp);
    CHECK(n == 26 + 2, "params emit size");
    size_t m = zgec_block_params_parse(&bp2, buf, n);
    CHECK(m == n, "params round-trip size");
    CHECK(bp2.plain.ctx_mode == 1 && bp2.plain.ctx_count == 4, "params plain fields");
    CHECK(memcmp(bp2.plain.class_map, bp.plain.class_map, 64) == 0, "params class_map");
    CHECK(bp2.sub.ctx_mode == 0 && bp2.sub.ctx_count == 1, "params sub fields");

    /* error cases */
    uint8_t bad[64];
    memcpy(bad, buf, n);
    bad[0] = 5; CHECK(zgec_block_params_parse(&bp2, bad, n) == 0, "ctx_mode 5 rejected");
    memcpy(bad, buf, n);
    bad[1] = 3; CHECK(zgec_block_params_parse(&bp2, bad, n) == 0, "ctx_count 3 rejected");
    memcpy(bad, buf, n);
    bad[0] = 0; bad[1] = 2; CHECK(zgec_block_params_parse(&bp2, bad, n) == 0, "mode0 k2 rejected");
    memcpy(bad, buf, n);
    bad[2] |= 0x07; /* class 0 entry = 7 >= k=4 */ CHECK(zgec_block_params_parse(&bp2, bad, n) == 0, "class_map>=k rejected");
    /* truncated: k>1 needs 26 */
    CHECK(zgec_block_params_parse(&bp2, buf, 10) == 0, "truncated params rejected");
    /* emit mirrors: bad mode/count rejected */
    zgec_block_params badbp = bp; badbp.plain.ctx_mode = 9;
    CHECK(zgec_block_params_emit(buf, sizeof(buf), &badbp) == 0, "emit bad mode rejected");
    badbp = bp; badbp.plain.ctx_count = 3;
    CHECK(zgec_block_params_emit(buf, sizeof(buf), &badbp) == 0, "emit bad count rejected");
    badbp = bp; badbp.plain.class_map[7] = 4;
    CHECK(zgec_block_params_emit(buf, sizeof(buf), &badbp) == 0, "emit bad class rejected");
    printf("params ok\n");
}

static void test_seg_raw(void)
{
    /* raw literals, n_seq=0: no descriptors */
    zgec_seg_header sh, sh2;
    uint8_t buf[64], desc[16];
    size_t dsz = 0;
    memset(&sh, 0, sizeof(sh));
    sh.segment_flags = 0x00; sh.table_modes = 0x00;
    sh.n_seq = 0; sh.n_lit = 10;
    sh.lit_size = 10; sh.ll_size = 0; sh.ml_size = 0; sh.of_size = 0;
    size_t n = zgec_seg_header_emit(buf, sizeof(buf), &sh, NULL, 0);
    CHECK(n > 0, "raw hdr emit");
    size_t h = zgec_seg_header_parse_ex(&sh2, buf, n, desc, sizeof(desc), &dsz, 1);
    CHECK(h == n && dsz == 0, "raw hdr round-trip");
    CHECK(sh2.n_lit == 10 && sh2.lit_size == 10, "raw hdr fields");

    /* reserved bits / lit_coder / table mode validation */
    uint8_t b2[64]; memcpy(b2, buf, n);
    b2[0] |= 0x20; CHECK(zgec_seg_header_parse_ex(&sh2, b2, n, NULL, 0, NULL, 1) == 0, "reserved bit rejected");
    memcpy(b2, buf, n); b2[0] |= 0x04; CHECK(zgec_seg_header_parse_ex(&sh2, b2, n, NULL, 0, NULL, 1) == 0, "lit_coder 2 rejected");
    memcpy(b2, buf, n); b2[1] = 0xFF; CHECK(zgec_seg_header_parse_ex(&sh2, b2, n, NULL, 0, NULL, 1) == 0, "table mode 3 rejected");
    printf("seg raw ok\n");
}

static void test_seg_fse(void)
{
    /* rANS literals k=1 (1 table AL11) + LL/ML/OF NEW AL6, n_seq=2 */
    uint8_t dbuf[1024], hdr[1100], dout[1024];
    size_t dp = 0;
    size_t w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 256, 11); CHECK(w > 0, "lit desc"); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); CHECK(w > 0, "ll desc"); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); CHECK(w > 0, "ml desc"); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); CHECK(w > 0, "of desc"); dp += w;

    zgec_seg_header sh, sh2;
    memset(&sh, 0, sizeof(sh));
    sh.segment_flags = 0x02; /* lit_coder=1 */
    sh.table_modes = 0x00;   /* all NEW */
    sh.n_seq = 2; sh.n_lit = 16;
    sh.lit_size = 100; sh.ll_size = 10; sh.ml_size = 10; sh.of_size = 10;
    size_t n = zgec_seg_header_emit(hdr, sizeof(hdr), &sh, dbuf, dp);
    CHECK(n > 0, "fse hdr emit");
    size_t dsz = 0;
    size_t h = zgec_seg_header_parse_ex(&sh2, hdr, n, dout, sizeof(dout), &dsz, 1);
    CHECK(h == n && dsz == dp, "fse hdr round-trip");
    CHECK(memcmp(dout, dbuf, dp) == 0, "desc bytes identical");
    /* short caller buffer must fail, not truncate */
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, n, dout, dp - 1, NULL, 1) == 0, "short desc buf rejected");

    /* walk: 1 lit, 1 ll, ml, 1 of */
    zgec_tbl_desc lit[1], ll[1], ml, of[1];
    size_t c = zgec_seg_descriptors_walk(lit, 1, ll, 1, &ml, of, 1,
                                         dbuf, dp, 0, 1, 1, 0, 0);
    CHECK(c == dp, "walk consumed all");
    CHECK(lit[0].mode == 0 && ll[0].mode == 0 && ml.mode == 0 && of[0].mode == 0, "walk NEW modes");

    /* literal AL != 11 rejected */
    uint8_t badd[512]; size_t bp2 = 0;
    w = mk_desc(badd, sizeof(badd), 66, 6); CHECK(w > 0, "badlit desc"); bp2 += w;
    w = mk_desc(badd + bp2, sizeof(badd) - bp2, 66, 6); bp2 += w;
    w = mk_desc(badd + bp2, sizeof(badd) - bp2, 66, 6); bp2 += w;
    w = mk_desc(badd + bp2, sizeof(badd) - bp2, 66, 6); bp2 += w;
    zgec_seg_header shb = sh;
    size_t nb = zgec_seg_header_emit(hdr, sizeof(hdr), &shb, badd, bp2);
    CHECK(nb > 0, "badlit emit");
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, nb, NULL, 0, NULL, 1) == 0, "lit AL!=11 rejected");

    /* Section 7.3: literal tables are 0=new, 1=repeat only; RLE is defined
       solely for the 66-symbol sequence alphabets. A literal table marked
       RLE must be rejected (both by emit and by parse). */
    zgec_seg_header slr = sh;
    slr.table_modes = 0x02;                /* literal table mode = RLE */
    CHECK(zgec_seg_header_emit(hdr, sizeof(hdr), &slr, NULL, 0) == 0,
          "literal RLE emit rejected");

    /* Sequence RLE descriptor: lit=NEW, LL=RLE, ML/OF=REPEAT, with one LL
       RLE byte that must be a symbol < 66. */
    uint8_t litdesc[512];
    size_t lp = mk_desc(litdesc, sizeof(litdesc), 256, 11);
    CHECK(lp > 0, "rle lit desc");
    uint8_t rdesc[512];
    memcpy(rdesc, litdesc, lp);
    size_t rdesc_sz = lp + 1;
    zgec_seg_header shr = sh;
    shr.table_modes = (uint8_t)((ZGEC_TBL_NEW) | (ZGEC_TBL_RLE << 2) |
                                (ZGEC_TBL_REPEAT << 4) | (ZGEC_TBL_REPEAT << 6));

    rdesc[lp] = 66;                        /* LL RLE symbol 66 -> invalid */
    size_t nr = zgec_seg_header_emit(hdr, sizeof(hdr), &shr, rdesc, rdesc_sz);
    CHECK(nr > 0, "rle emit");
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, nr, NULL, 0, NULL, 1) == 0, "seq RLE 66 rejected");

    rdesc[lp] = 5;                         /* valid LL RLE symbol */
    size_t nr2 = zgec_seg_header_emit(hdr, sizeof(hdr), &shr, rdesc, rdesc_sz);
    CHECK(nr2 > 0, "rle emit2");
    size_t rdsz = 0;
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, nr2, dout, sizeof(dout), &rdsz, 1) > 0, "seq RLE 5 ok");
    CHECK(rdsz == rdesc_sz, "seq RLE desc size");

    /* walk: a literal byte that is not a valid literal descriptor (AL==11)
       is an error, and a valid lit + LL-RLE pair consumes exactly the run. */
    zgec_tbl_desc l2[1], ll2[1];
    uint8_t badlit[512];
    memcpy(badlit, rdesc, rdesc_sz);
    badlit[0] = 200;                       /* not a valid literal descriptor */
    CHECK(zgec_seg_descriptors_walk(l2, 1, ll2, 1, NULL, NULL, 0, badlit, rdesc_sz, 0, 1, 1, 0, 0) == 0,
          "walk bad literal descriptor rejected");
    CHECK(zgec_seg_descriptors_walk(l2, 1, ll2, 1, NULL, NULL, 0, rdesc, rdesc_sz, 0, 1, 1, 0, 0) == rdesc_sz,
          "walk lit + LL RLE ok");
    printf("seg fse ok\n");
}

static void test_seg_x3(void)
{
    /* seq_ctx_of: 3 OF descriptions, same AL required */
    uint8_t dbuf[1024], hdr[1100];
    size_t dp = 0, w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w; /* ll */
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w; /* ml */
    size_t of0 = dp;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    (void)of0;
    zgec_seg_header sh; memset(&sh, 0, sizeof(sh));
    sh.segment_flags = 0x08; /* seq_ctx_of, lit raw */
    sh.table_modes = 0x00; sh.n_seq = 3; sh.n_lit = 5;
    sh.lit_size = 5; sh.ll_size = 4; sh.ml_size = 4; sh.of_size = 4;
    size_t n = zgec_seg_header_emit(hdr, sizeof(hdr), &sh, dbuf, dp);
    CHECK(n > 0, "x3 emit");
    zgec_seg_header sh2; size_t dsz = 0;
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, n, NULL, 0, &dsz, 1) == n && dsz == dp, "x3 round-trip");
    /* mismatched AL in x3 group rejected: rebuild 3rd OF with AL 7 */
    uint8_t d2[1024]; size_t q = 0;
    memcpy(d2, dbuf, dp); /* find OF group start: ll+ml lens unknown; instead rebuild fully */
    (void)q;
    uint8_t ebuf[1024]; size_t ep = 0;
    w = mk_desc(ebuf + ep, sizeof(ebuf) - ep, 66, 6); ep += w;
    w = mk_desc(ebuf + ep, sizeof(ebuf) - ep, 66, 6); ep += w;
    w = mk_desc(ebuf + ep, sizeof(ebuf) - ep, 66, 6); ep += w;
    w = mk_desc(ebuf + ep, sizeof(ebuf) - ep, 66, 6); ep += w;
    w = mk_desc(ebuf + ep, sizeof(ebuf) - ep, 66, 7); ep += w; /* different AL */
    size_t ne = zgec_seg_header_emit(hdr, sizeof(hdr), &sh, ebuf, ep);
    CHECK(ne > 0, "x3bad emit");
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, ne, NULL, 0, NULL, 1) == 0, "x3 mixed AL rejected");
    /* walk x3 same-AL enforcement */
    zgec_tbl_desc ll3[1], ml3, of3[3];
    CHECK(zgec_seg_descriptors_walk(NULL, 0, ll3, 1, &ml3, of3, 3, ebuf, ep, 0, 0, 1, 1, 0) == 0, "walk x3 mixed AL rejected");
    CHECK(zgec_seg_descriptors_walk(NULL, 0, ll3, 1, &ml3, of3, 3, dbuf, dp, 0, 0, 1, 1, 0) == dp, "walk x3 ok");
    printf("seg x3 ok\n");
}

static void test_seg_k3(void)
{
    /* k=3 (use 4? k in {1,2,4,8}; use 4): k+1=5 literal tables AL11 */
    uint8_t dbuf[2048], hdr[2200];
    size_t dp = 0, w;
    for (int i = 0; i < 5; i++) { w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 256, 11); CHECK(w > 0, "k desc"); dp += w; }
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    w = mk_desc(dbuf + dp, sizeof(dbuf) - dp, 66, 6); dp += w;
    zgec_seg_header sh; memset(&sh, 0, sizeof(sh));
    sh.segment_flags = 0x02; sh.table_modes = 0x00;
    sh.n_seq = 1; sh.n_lit = 8;
    sh.lit_size = 50; sh.ll_size = 4; sh.ml_size = 4; sh.of_size = 4;
    size_t n = zgec_seg_header_emit(hdr, sizeof(hdr), &sh, dbuf, dp);
    CHECK(n > 0, "k emit");
    zgec_seg_header sh2; size_t dsz = 0;
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, n, NULL, 0, &dsz, 4) == n && dsz == dp, "k=4 k+1 round-trip");
    /* k=1 parse of same bytes must fail (expects 1 lit table, finds trailing garbage -> AL check/varint fail or size mismatch) */
    CHECK(zgec_seg_header_parse_ex(&sh2, hdr, n, NULL, 0, &dsz, 1) == 0, "k=1 on k=4 bytes rejected");
    /* walk k+1 */
    zgec_tbl_desc lit[5], ll[1], ml, of[1];
    CHECK(zgec_seg_descriptors_walk(lit, 5, ll, 1, &ml, of, 1, dbuf, dp, 0, 1, 4, 0, 0) == dp, "walk k+1 ok");
    printf("seg k ok\n");
}

static void test_bitstream(void)
{
    uint8_t buf[32];
    zgec_bw bw; zgec_bw_init(&bw, buf, sizeof(buf));
    zgec_bw_write(&bw, 0x15, 5);   /* 10101 */
    zgec_bw_write(&bw, 0x3, 2);
    zgec_bw_write(&bw, 0xAB, 8);
    size_t n = zgec_bw_finish(&bw);
    CHECK(n > 0 && n <= sizeof(buf), "bw finish");
    CHECK(buf[n - 1] != 0, "sentinel byte nonzero");
    /* last byte's top bit is sentinel: data below it */
    unsigned s = zgec_highbit32(buf[n - 1]);
    CHECK((buf[n - 1] & (uint8_t)(1u << s)) != 0, "sentinel set");

    zgec_br br; zgec_br_init(&br, buf, n);
    CHECK(!br.overflow, "br init ok");
    /* consumption order is reverse of write order for this LSB-first
       writer: last-written value's MSB comes first. Re-read in reverse:
       read 0xAB(8), 0x3(2), 0x15(5) bit-reversed? Instead verify exact round-trip
       of single values: rewrite and read back MSB-first. */
    (void)s;
    /* empty stream: bare sentinel */
    zgec_bw_init(&bw, buf, sizeof(buf));
    size_t n0 = zgec_bw_finish(&bw);
    CHECK(n0 == 1 && buf[0] == 0x01, "bare sentinel");
    zgec_br_init(&br, buf, n0);
    CHECK(zgec_br_done(&br), "bare stream done");
    /* zero-length stream rejected */
    zgec_br_init(&br, buf, 0);
    CHECK(br.overflow, "empty init overflow");
    /* zero last byte rejected */
    buf[0] = 0; zgec_br_init(&br, buf, 1);
    CHECK(br.overflow, "zero sentinel overflow");
    /* over-read sets overflow, done==0 */
    zgec_bw_init(&bw, buf, sizeof(buf));
    zgec_bw_write(&bw, 0x1, 1);
    n = zgec_bw_finish(&bw);
    zgec_br_init(&br, buf, n);
    (void)zgec_br_read(&br, 1);
    CHECK(zgec_br_done(&br), "exact done");
    (void)zgec_br_read(&br, 1);
    CHECK(br.overflow && !zgec_br_done(&br), "overread overflow");
    printf("bitstream ok\n");
}


#include "zgec_rans.h"

static void test_rans_context_bounds(void)
{
    /* Test zgec_rans_decode with invalid class_map bounds and malformed streams */
    uint8_t Z[64] = {0};
    uint8_t stream[64] = {0};
    zgec_rans_dec_table tables[9];
    uint8_t class_map[64];
    uint8_t runstart[64] = {0};

    /* Build a valid dummy dec table */
    int16_t counts[256];
    for (int i = 0; i < 256; i++) counts[i] = (i < 8) ? 256 : 0;
    for (int t = 0; t < 9; t++) {
        zgec_err e = zgec_rans_build_dec(&tables[t], counts);
        CHECK(e == ZGEC_OK, "build dec table");
    }

    /* Set up stream header state (state >= ZGEC_RANS_STATE_MIN = 65536) */
    for (int lane = 0; lane < 8; lane++) {
        zgec_wr32(stream + 4 * lane, 65536);
    }

    /* Invalid class_map entry >= k (k=2, entry=2) */
    memset(class_map, 0, sizeof(class_map));
    class_map[0] = 2; /* invalid for k=2 */

    zgec_err err = zgec_rans_decode(Z, 16, stream, sizeof(stream),
                                    tables, 2, ZGEC_CTX_LSB6, class_map, runstart);
    CHECK(err == ZGEC_ERR_CLASS_MAP, "invalid class_map entry rejected");

    /* Valid class_map but truncated stream (stream_size < 32) */
    class_map[0] = 0;
    err = zgec_rans_decode(Z, 16, stream, 16,
                            tables, 2, ZGEC_CTX_LSB6, class_map, runstart);
    CHECK(err == ZGEC_ERR_TRUNCATED, "short stream rejected");

    printf("rans context bounds ok\n");
}

static void test_corrupted_bitstreams(void)
{
    uint8_t dummy[1024];
    memset(dummy, 0xFF, sizeof(dummy));
    uint8_t *out = NULL;
    size_t out_size = 0;

    zgec_err err = zgec_decompress(dummy, 10, &out, &out_size);
    CHECK(err != ZGEC_OK, "truncated header rejected");
    zgec_free(out); out = NULL;

    err = zgec_decompress(dummy, sizeof(dummy), &out, &out_size);
    CHECK(err != ZGEC_OK, "bad magic rejected");
    zgec_free(out); out = NULL;

    for (size_t len = 1; len <= 128; len += 7) {
        err = zgec_decompress(dummy, len, &out, &out_size);
        CHECK(err != ZGEC_OK, "random noise length rejected");
        zgec_free(out); out = NULL;
    }
    printf("corrupted bitstreams ok\n");
}

int main(void)
{
    test_block_params();
    test_seg_raw();
    test_seg_fse();
    test_seg_x3();
    test_seg_k3();
    test_bitstream();
    test_rans_context_bounds();
    test_corrupted_bitstreams();
    if (fails == 0) printf("ALL BLOCK/BITSTREAM CHECKS PASSED\n");
    else printf("FAILURES %d\n", fails);
    return fails ? 1 : 0;
}
