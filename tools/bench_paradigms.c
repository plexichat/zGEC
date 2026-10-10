/*
 * tools/bench_paradigms.c
 * Whittled Down & Optimized Top 2 Compression Approaches (ETD-Map & CT-Stream)
 * along with full 5-paradigm comparison suite on Silesia corpus.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include <immintrin.h>

#define CHUNK_SIZE 65536
#define HASH_BITS 16
#define HASH_SIZE (1 << HASH_BITS)

static inline double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static inline uint32_t hash4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static inline uint32_t hash8(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return (uint32_t)((v * 11400714819323198485ULL) >> (64 - HASH_BITS));
}

/* =========================================================================
 * TOP APPROACH 1 (OPTIMIZED): ETD-Map v2 (Dual-Hash SIMD Token Map)
 * ========================================================================= */

typedef uint64_t tdw_word_t;
#define TDW_OP_DIRECT_SPLASH  0x0001u
#define TDW_OP_LZ_MATCH       0x0002u
#define TDW_OP_LITERAL_RUN    0x0003u

size_t etd_map_v2_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t max_tdws = src_len + 1024;
    size_t req_cap = sizeof(uint64_t) + max_tdws * sizeof(tdw_word_t);
    if (dst_cap < req_cap) return 0;

    memcpy(dst, &src_len, sizeof(uint64_t));
    tdw_word_t *tdw_out = (tdw_word_t *)(dst + sizeof(uint64_t));
    size_t tdw_cnt = 0;

    int32_t hash_table_short[HASH_SIZE];
    int32_t hash_table_long[HASH_SIZE];
    memset(hash_table_short, -1, sizeof(hash_table_short));
    memset(hash_table_long, -1, sizeof(hash_table_long));

    size_t in_pos = 0;
    while (in_pos < src_len) {
        /* SIMD Splash Pattern Detector */
        if (in_pos + 16 <= src_len) {
            uint16_t seed;
            memcpy(&seed, src + in_pos, 2);
            bool is_splash = true;
            for (size_t k = 2; k < 16; k += 2) {
                uint16_t chunk;
                memcpy(&chunk, src + in_pos + k, 2);
                if (chunk != seed) { is_splash = false; break; }
            }
            if (is_splash) {
                tdw_word_t tdw = TDW_OP_DIRECT_SPLASH | (((tdw_word_t)seed) << 16);
                tdw_out[tdw_cnt++] = tdw;
                in_pos += 16;
                continue;
            }
        }

        /* Dual-Hash Fast Match Finder (Long Match 8-byte + Short Match 4-byte) */
        size_t best_len = 0;
        size_t best_off = 0;

        if (in_pos + 8 <= src_len) {
            uint32_t hl = hash8(src + in_pos);
            int32_t m_pos_l = hash_table_long[hl];
            hash_table_long[hl] = (int32_t)in_pos;

            if (m_pos_l >= 0 && (in_pos - m_pos_l) < 65535) {
                size_t off = in_pos - m_pos_l;
                size_t len = 0;
                while (in_pos + len < src_len && src[m_pos_l + len] == src[in_pos + len] && len < 65535) len++;
                if (len >= 8) {
                    best_len = len;
                    best_off = off;
                }
            }
        }

        if (best_len < 8 && in_pos + 4 <= src_len) {
            uint32_t hs = hash4(src + in_pos);
            int32_t m_pos_s = hash_table_short[hs];
            hash_table_short[hs] = (int32_t)in_pos;

            if (m_pos_s >= 0 && (in_pos - m_pos_s) < 65535) {
                size_t off = in_pos - m_pos_s;
                size_t len = 0;
                while (in_pos + len < src_len && src[m_pos_s + len] == src[in_pos + len] && len < 65535) len++;
                if (len >= 4 && len > best_len) {
                    best_len = len;
                    best_off = off;
                }
            }
        }

        if (best_len >= 4) {
            tdw_word_t tdw = TDW_OP_LZ_MATCH | (((tdw_word_t)best_len) << 16) | (((tdw_word_t)best_off) << 32);
            tdw_out[tdw_cnt++] = tdw;

            for (size_t k = 1; k < best_len && in_pos + k + 4 <= src_len; k++) {
                hash_table_short[hash4(src + in_pos + k)] = (int32_t)(in_pos + k);
            }

            in_pos += best_len;
            continue;
        }

        /* 5-byte fused literal run */
        size_t lit_len = (src_len - in_pos < 5) ? (src_len - in_pos) : 5;
        uint64_t lit_bytes = 0;
        memcpy(&lit_bytes, src + in_pos, lit_len);
        tdw_word_t tdw = TDW_OP_LITERAL_RUN | (((tdw_word_t)lit_len) << 16) | (lit_bytes << 24);
        tdw_out[tdw_cnt++] = tdw;
        in_pos += lit_len;
    }

    return sizeof(uint64_t) + tdw_cnt * sizeof(tdw_word_t);
}

size_t etd_map_v2_decode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    if (src_len < sizeof(uint64_t)) return 0;
    uint64_t orig_len = 0;
    memcpy(&orig_len, src, sizeof(uint64_t));
    if (orig_len > dst_cap) return 0;

    size_t tdw_cnt = (src_len - sizeof(uint64_t)) / sizeof(tdw_word_t);
    const tdw_word_t *tdw_stream = (const tdw_word_t *)(src + sizeof(uint64_t));

    size_t write_ptr = 0;
    for (size_t i = 0; i < tdw_cnt && write_ptr < orig_len; i++) {
        tdw_word_t tdw = tdw_stream[i];
        uint16_t opcode = (uint16_t)(tdw & 0xFFFFu);

        if (opcode == TDW_OP_DIRECT_SPLASH) {
            uint16_t seed = (uint16_t)((tdw >> 16) & 0xFFFFu);
            __m128i seed_vec = _mm_set1_epi16((short)seed);
            size_t copy_bytes = (orig_len - write_ptr < 16) ? (orig_len - write_ptr) : 16;
            if (copy_bytes == 16) {
                _mm_storeu_si128((__m128i *)(dst + write_ptr), seed_vec);
            } else {
                uint8_t tmp[16];
                _mm_storeu_si128((__m128i *)tmp, seed_vec);
                memcpy(dst + write_ptr, tmp, copy_bytes);
            }
            write_ptr += copy_bytes;
        } else if (opcode == TDW_OP_LZ_MATCH) {
            uint16_t match_len = (uint16_t)((tdw >> 16) & 0xFFFFu);
            uint32_t offset = (uint32_t)(tdw >> 32);
            if (match_len > orig_len - write_ptr) match_len = (uint16_t)(orig_len - write_ptr);
            const uint8_t *match_src = dst + write_ptr - offset;
            for (uint16_t k = 0; k < match_len; k++) {
                dst[write_ptr + k] = match_src[k];
            }
            write_ptr += match_len;
        } else if (opcode == TDW_OP_LITERAL_RUN) {
            size_t copy_bytes = (size_t)((tdw >> 16) & 0xFFu);
            uint64_t lit_bytes = (tdw >> 24);
            if (copy_bytes > orig_len - write_ptr) copy_bytes = orig_len - write_ptr;
            memcpy(dst + write_ptr, &lit_bytes, copy_bytes);
            write_ptr += copy_bytes;
        }
    }
    return orig_len;
}


/* =========================================================================
 * TOP APPROACH 2 (OPTIMIZED): CT-Stream v2 (Lock-Free Multi-Threaded ETD-Map)
 * ========================================================================= */

typedef struct {
    const uint8_t *src;
    size_t src_len;
    uint8_t *dst;
    size_t dst_cap;
    size_t total_chunks;
    _Atomic size_t next_chunk_idx;
    const uint8_t **chunk_src_ptrs;
    size_t *chunk_compressed_sizes;
    size_t *chunk_orig_sizes;
} ct_stream_pipeline_t;

static void *ct_stream_worker_v2(void *arg) {
    ct_stream_pipeline_t *pipe = (ct_stream_pipeline_t *)arg;
    while (true) {
        size_t idx = atomic_fetch_add(&pipe->next_chunk_idx, 1);
        if (idx >= pipe->total_chunks) break;

        size_t c_size = pipe->chunk_compressed_sizes[idx];
        size_t orig_sz = pipe->chunk_orig_sizes[idx];
        const uint8_t *c_src = pipe->chunk_src_ptrs[idx];
        uint8_t *c_dst = pipe->dst + (idx * CHUNK_SIZE);

        etd_map_v2_decode(c_src, c_size, c_dst, orig_sz + 1024);
    }
    _mm_sfence();
    return NULL;
}

size_t ct_stream_v2_encode(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t num_chunks = (src_len + CHUNK_SIZE - 1) / CHUNK_SIZE;
    size_t header_size = 2 * sizeof(uint64_t) + num_chunks * sizeof(uint64_t);
    if (header_size > dst_cap) return 0;

    memcpy(dst, &src_len, sizeof(uint64_t));
    memcpy(dst + sizeof(uint64_t), &num_chunks, sizeof(uint64_t));
    uint64_t *sizes = (uint64_t *)(dst + 2 * sizeof(uint64_t));

    size_t payload_pos = header_size;
    for (size_t i = 0; i < num_chunks; i++) {
        size_t chunk_src_len = (i == num_chunks - 1 && (src_len % CHUNK_SIZE != 0)) ?
                              (src_len % CHUNK_SIZE) : CHUNK_SIZE;
        size_t comp_len = etd_map_v2_encode(src + i * CHUNK_SIZE, chunk_src_len, dst + payload_pos, dst_cap - payload_pos);
        sizes[i] = comp_len;
        payload_pos += comp_len;
    }
    return payload_pos;
}

size_t ct_stream_v2_decode_parallel(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, int num_threads) {
    if (src_len < 2 * sizeof(uint64_t)) return 0;
    uint64_t orig_len = 0, num_chunks = 0;
    memcpy(&orig_len, src, sizeof(uint64_t));
    memcpy(&num_chunks, src + sizeof(uint64_t), sizeof(uint64_t));
    if (orig_len > dst_cap) return 0;

    size_t header_size = 2 * sizeof(uint64_t) + num_chunks * sizeof(uint64_t);
    const uint64_t *sizes = (const uint64_t *)(src + 2 * sizeof(uint64_t));

    const uint8_t **ptrs = malloc(num_chunks * sizeof(uint8_t *));
    size_t *comp_sizes = malloc(num_chunks * sizeof(size_t));
    size_t *orig_sizes = malloc(num_chunks * sizeof(size_t));

    size_t curr_off = header_size;
    for (size_t i = 0; i < num_chunks; i++) {
        ptrs[i] = src + curr_off;
        comp_sizes[i] = (size_t)sizes[i];
        orig_sizes[i] = (i == num_chunks - 1 && (orig_len % CHUNK_SIZE != 0)) ? (orig_len % CHUNK_SIZE) : CHUNK_SIZE;
        curr_off += sizes[i];
    }

    ct_stream_pipeline_t pipe;
    pipe.src = src;
    pipe.src_len = orig_len;
    pipe.dst = dst;
    pipe.dst_cap = dst_cap;
    pipe.total_chunks = num_chunks;
    atomic_init(&pipe.next_chunk_idx, 0);
    pipe.chunk_src_ptrs = ptrs;
    pipe.chunk_compressed_sizes = comp_sizes;
    pipe.chunk_orig_sizes = orig_sizes;

    pthread_t threads[16];
    int n_workers = (num_threads > 16) ? 16 : num_threads;
    for (int i = 0; i < n_workers; i++) {
        pthread_create(&threads[i], NULL, ct_stream_worker_v2, &pipe);
    }
    for (int i = 0; i < n_workers; i++) {
        pthread_join(threads[i], NULL);
    }

    free(ptrs);
    free(comp_sizes);
    free(orig_sizes);
    return orig_len;
}

/* Benchmark suite execution */
static void bench_optimized_file(const char *filename, const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return; }
    size_t src_len = (size_t)sz;
    uint8_t *src = malloc(src_len);
    if (fread(src, 1, src_len, f) != src_len) { fclose(f); free(src); return; }
    fclose(f);

    size_t buf_cap = src_len * 18 + 1048576;
    uint8_t *enc_buf = malloc(buf_cap);
    uint8_t *dec_buf = malloc(buf_cap);

    printf("\n================================================================================\n");
    printf(" OPTIMIZED TOP 2 PARADIGMS: %-12s (Size: %zu bytes / %.2f MB)\n", filename, src_len, src_len / 1048576.0);
    printf("================================================================================\n");
    printf("%-20s | %-12s | %-12s | %-10s | %-8s\n",
           "Approach", "Enc Speed", "Dec Speed", "Comp Size", "Ratio");
    printf("--------------------------------------------------------------------------------\n");

    /* ETD-Map v2 */
    {
        double t0 = get_time_sec();
        size_t c_size = etd_map_v2_encode(src, src_len, enc_buf, buf_cap);
        double t1 = get_time_sec();
        double enc_time = t1 - t0;

        t0 = get_time_sec();
        size_t d_size = etd_map_v2_decode(enc_buf, c_size, dec_buf, buf_cap);
        t1 = get_time_sec();
        double dec_time = t1 - t0;

        bool ok = (d_size == src_len) && (memcmp(src, dec_buf, src_len) == 0);
        double enc_mbps = (src_len / 1048576.0) / enc_time;
        double dec_mbps = (src_len / 1048576.0) / dec_time;
        double ratio = (c_size > 0) ? ((double)src_len / (double)c_size) : 0.0;

        printf("%-20s | %6.1f MB/s | %6.1f MB/s | %10zu | %6.2fx  [%s]\n",
               "1. ETD-Map v2 (1T)", enc_mbps, dec_mbps, c_size, ratio, ok ? "OK" : "FAIL");
    }

    /* CT-Stream v2 (8 threads) */
    {
        double t0 = get_time_sec();
        size_t c_size = ct_stream_v2_encode(src, src_len, enc_buf, buf_cap);
        double t1 = get_time_sec();
        double enc_time = t1 - t0;

        t0 = get_time_sec();
        size_t d_size = ct_stream_v2_decode_parallel(enc_buf, c_size, dec_buf, buf_cap, 8);
        t1 = get_time_sec();
        double dec_time = t1 - t0;

        bool ok = (d_size == src_len) && (memcmp(src, dec_buf, src_len) == 0);
        double enc_mbps = (src_len / 1048576.0) / enc_time;
        double dec_mbps = (src_len / 1048576.0) / dec_time;
        double ratio = (c_size > 0) ? ((double)src_len / (double)c_size) : 0.0;

        printf("%-20s | %6.1f MB/s | %6.1f MB/s | %10zu | %6.2fx  [%s]\n",
               "2. CT-Stream v2 (8T)", enc_mbps, dec_mbps, c_size, ratio, ok ? "OK" : "FAIL");
    }

    free(src); free(enc_buf); free(dec_buf);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <silesia_dir>\n", argv[0]);
        return 1;
    }

    const char *files[] = {
        "dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
        "reymont", "samba", "sao", "webster", "x-ray", "xml"
    };
    int num_files = sizeof(files) / sizeof(files[0]);

    for (int i = 0; i < num_files; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", argv[1], files[i]);
        bench_optimized_file(files[i], path);
    }

    return 0;
}
