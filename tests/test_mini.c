#include "zgec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int zgec_test_fse_round_trip(void)
{
    printf("FSE round-trip... "); fflush(stdout);
    int16_t counts[66];
    memset(counts, 0, sizeof(counts));
    for (int s = 0; s < 66; s++) counts[s] = (int16_t)(s < 64 ? 31 : 32);
    int sum = 0;
    for (int s = 0; s < 66; s++) sum += counts[s];
    if (sum != 2048) { printf("FAIL sum=%d\n", sum); return 0; }

    zgec_fse_dec_table *dec = NULL;
    zgec_fse_enc_table *enc = NULL;
    if (zgec_fse_build_dec(&dec, counts, 66, 11) != ZGEC_OK) { printf("FAIL build_dec\n"); return 0; }
    if (zgec_fse_build_enc(&enc, dec) != ZGEC_OK) { zgec_fse_free_dec(dec); printf("FAIL build_enc\n"); return 0; }

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
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); printf("FAIL encode\n"); return 0;
    }
    size_t enc_size = zgec_bw_finish(&bw);

    zgec_br br;
    zgec_br_init(&br, buf, enc_size);
    uint32_t decoded[1024];
    if (zgec_fse_decode(dec, &br, decoded, NULL, 1024, zgec_seq_base, zgec_seq_nbits) != ZGEC_OK) {
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); printf("FAIL decode\n"); return 0;
    }
    if (!zgec_br_done(&br)) {
        zgec_fse_free_enc(enc); zgec_fse_free_dec(dec); printf("FAIL br_done\n"); return 0;
    }

    for (int i = 0; i < 1024; i++) {
        if (decoded[i] != values[i]) {
            zgec_fse_free_enc(enc); zgec_fse_free_dec(dec);
            printf("FAIL value mismatch at %d\n", i); return 0;
        }
    }

    zgec_fse_free_enc(enc);
    zgec_fse_free_dec(dec);
    printf("OK\n");
    return 1;
}

static int zgec_test_rans_round_trip(void)
{
    printf("rANS round-trip... "); fflush(stdout);
    int16_t counts[256];
    memset(counts, -1, sizeof(counts));
    for (int s = 0; s < 256; s++) counts[s] = (int16_t)(s < 128 ? 16 : 0);
    int sum = 0;
    for (int s = 0; s < 256; s++) sum += counts[s] < 0 ? 1 : counts[s];
    if (sum != 2048) { printf("FAIL sum=%d\n", sum); return 0; }

    zgec_rans_dec_table dec;
    zgec_rans_enc_table enc;
    if (zgec_rans_build_dec(&dec, counts) != ZGEC_OK) { printf("FAIL build_dec\n"); return 0; }
    if (zgec_rans_build_enc(&enc, counts) != ZGEC_OK) { printf("FAIL build_enc\n"); return 0; }

    uint8_t lit[256];
    for (int i = 0; i < 256; i++) lit[i] = (uint8_t)(i & 127);

    uint8_t stream[4096];
    size_t stream_size = zgec_rans_encode(lit, 256, stream, sizeof(stream), &enc, 1, ZGEC_CTX_NONE, NULL, NULL);
    if (stream_size == 0) { printf("FAIL encode\n"); return 0; }

    uint8_t decoded[256 + ZGEC_LIT_SLACK];
    if (zgec_rans_decode(decoded, 256, stream, stream_size, &dec, 1, ZGEC_CTX_NONE, NULL, NULL) != ZGEC_OK) { printf("FAIL decode\n"); return 0; }

    for (int i = 0; i < 256; i++) {
        if (decoded[i] != lit[i]) { printf("FAIL mismatch at %d\n", i); return 0; }
    }
    printf("OK\n");
    return 1;
}

static int zgec_test_frame_round_trip(void)
{
    printf("frame round-trip... "); fflush(stdout);
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
    if (zgec_frame_header_parse(&parsed, buf) != ZGEC_OK) { printf("FAIL parse\n"); return 0; }
    if (parsed.version_major != 1) { printf("FAIL version\n"); return 0; }
    if (parsed.block_log2 != 21) { printf("FAIL block_log2\n"); return 0; }
    if (parsed.content_size != 1024) { printf("FAIL content_size\n"); return 0; }
    printf("OK\n");
    return 1;
}

static int zgec_test_round_trip(void)
{
    printf("round-trip... "); fflush(stdout);
    uint8_t src[1024];
    for (int i = 0; i < 1024; i++) src[i] = (uint8_t)(i * 3 + 1);

    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    if (zgec_compress(src, sizeof(src), &cmp, &cmp_size) != ZGEC_OK) { printf("FAIL compress\n"); return 0; }

    uint8_t *out = NULL;
    size_t out_size = 0;
    if (zgec_decompress(cmp, cmp_size, &out, &out_size) != ZGEC_OK) {
        zgec_free(cmp); printf("FAIL decompress\n"); return 0;
    }

    int ok = (out_size == sizeof(src) && memcmp(src, out, sizeof(src)) == 0) ? 1 : 0;
    zgec_free(cmp);
    zgec_free(out);
    printf("%s\n", ok ? "OK" : "FAIL data mismatch");
    return ok;
}

int main(void)
{
    int passed = 0, failed = 0;

    if (zgec_test_frame_round_trip()) passed++; else failed++;
    if (zgec_test_fse_round_trip()) passed++; else failed++;
    if (zgec_test_rans_round_trip()) passed++; else failed++;
    if (zgec_test_round_trip()) passed++; else failed++;

    printf("PASS %d FAIL %d\n", passed, failed);
    return failed ? 1 : 0;
}
