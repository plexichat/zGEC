#define _POSIX_C_SOURCE 199309L
#include "zgec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static uint8_t *zgec_gen_data(size_t size)
{
    uint8_t *d = (uint8_t *)zgec_alloc(size, 64);
    if (!d) return NULL;
    for (size_t i = 0; i < size; i++) d[i] = (uint8_t)(i * 3 + 1);
    return d;
}

static double zgec_now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    size_t data_size = 10 * 1024 * 1024;
    uint8_t *data = zgec_gen_data(data_size);
    if (!data) { fprintf(stderr, "alloc failed\n"); return 1; }

    double t0 = zgec_now_sec();
    uint8_t *cmp = NULL;
    size_t cmp_size = 0;
    zgec_err err = zgec_compress(data, data_size, &cmp, &cmp_size);
    double t1 = zgec_now_sec();
    if (err != ZGEC_OK) { fprintf(stderr, "compress failed\n"); zgec_free(data); return 1; }

    double enc_mb = (double)data_size / (1024.0 * 1024.0);
    double enc_sec = t1 - t0;
    double enc_mbps = enc_mb / enc_sec;

    double t2 = zgec_now_sec();
    uint8_t *out = NULL;
    size_t out_size = 0;
    err = zgec_decompress(cmp, cmp_size, &out, &out_size);
    double t3 = zgec_now_sec();
    if (err != ZGEC_OK) { fprintf(stderr, "decompress failed\n"); zgec_free(cmp); zgec_free(data); return 1; }

    double dec_mb = (double)out_size / (1024.0 * 1024.0);
    double dec_sec = t3 - t2;
    double dec_mbps = dec_mb / dec_sec;

    printf("encode: %.1f MB/s (%.2f MB -> %zu bytes, %.2f%%)\n",
           enc_mbps, enc_mb, cmp_size, 100.0 * (double)cmp_size / (double)data_size);
    printf("decode: %.1f MB/s\n", dec_mbps);

    zgec_free(cmp);
    zgec_free(out);
    zgec_free(data);
    return 0;
}
