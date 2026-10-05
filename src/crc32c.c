#include "zgec_crc32c.h"

#include <string.h>

/* Castagnoli CRC-32C (polynomial 0x1EDC6F41, reflected,
   init and final XOR 0xFFFFFFFF), per zGEC section 2.1.

   Uses the x86 SSE4.2 crc32 instruction when available,
   otherwise a sliced table-based implementation. */

#if defined(__x86_64__) || defined(_M_X64)
#define ZGEC_HAVE_SSE42 1
#include <immintrin.h>
/* runtime check is done once; the compiler targets
   x86-64 which is assumed to have SSE4.2 (all AMD64
   CPUs since 2011 do). For maximum portability the
   software path is used unless __SSE4_2__ is defined
   or the CPU reports it. */
#if !defined(__SSE4_2__)
#include <cpuid.h>
static int zgec_cpu_has_sse42(void)
{
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return 0;
    return (ecx & (1u << 20)) != 0;   /* bit 20 = SSE4.2 */
}
#else
static int zgec_cpu_has_sse42(void) { return 1; }
#endif
#else
#define ZGEC_HAVE_SSE42 0
static int zgec_cpu_has_sse42(void) { return 0; }
#endif

/* Software CRC32C: sliced 8-table implementation. */
static uint32_t zgec_crc32c_table[8][256];
static int zgec_crc32c_table_ready = 0;

static void zgec_crc32c_init_table(void)
{
    uint32_t poly = 0x82F63B78u;   /* reflected Castagnoli */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (poly & (0u - (crc & 1u)));
        zgec_crc32c_table[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = zgec_crc32c_table[0][i];
        for (int k = 1; k < 8; k++) {
            crc = (crc >> 8) ^ zgec_crc32c_table[0][crc & 0xFFu];
            zgec_crc32c_table[k][i] = crc;
        }
    }
    zgec_crc32c_table_ready = 1;
}

static uint32_t zgec_crc32c_sw(uint32_t crc, const uint8_t *p, size_t len)
{
    if (!zgec_crc32c_table_ready) zgec_crc32c_init_table();
    crc = ~crc;
    /* align to 8 bytes */
    while (len && ((uintptr_t)p & 7)) {
        crc = zgec_crc32c_table[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
        len--;
    }
    while (len >= 8) {
        uint64_t v = (uint64_t)crc ^ ((uint64_t)p[0] | ((uint64_t)p[1] << 8) |
                     ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
                     ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
                     ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56));
        crc = (uint32_t)(zgec_crc32c_table[7][v & 0xFFu] ^
                         zgec_crc32c_table[6][(v >> 8) & 0xFFu] ^
                         zgec_crc32c_table[5][(v >> 16) & 0xFFu] ^
                         zgec_crc32c_table[4][(v >> 24) & 0xFFu] ^
                         zgec_crc32c_table[3][(v >> 32) & 0xFFu] ^
                         zgec_crc32c_table[2][(v >> 40) & 0xFFu] ^
                         zgec_crc32c_table[1][(v >> 48) & 0xFFu] ^
                         zgec_crc32c_table[0][(v >> 56) & 0xFFu]);
        p += 8;
        len -= 8;
    }
    while (len--) {
        crc = zgec_crc32c_table[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

#if ZGEC_HAVE_SSE42
static uint32_t zgec_crc32c_hw(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        crc = (uint32_t)_mm_crc32_u64(crc, v);
        p += 8;
        len -= 8;
    }
    while (len >= 4) {
        uint32_t v;
        memcpy(&v, p, 4);
        crc = _mm_crc32_u32(crc, v);
        p += 4;
        len -= 4;
    }
    while (len--) {
        crc = _mm_crc32_u8(crc, *p++);
    }
    return ~crc;
}
#endif

static int zgec_crc32c_use_hw = -1;

uint32_t zgec_crc32c(const void *data, size_t len, uint32_t crc)
{
    if (zgec_crc32c_use_hw < 0)
        zgec_crc32c_use_hw = zgec_cpu_has_sse42() ? 1 : 0;
#if ZGEC_HAVE_SSE42
    if (zgec_crc32c_use_hw)
        return zgec_crc32c_hw(crc, (const uint8_t *)data, len);
#endif
    return zgec_crc32c_sw(crc, (const uint8_t *)data, len);
}

uint32_t zgec_crc32c_init(void) { return 0; }

uint32_t zgec_crc32c_update(uint32_t crc, const void *data, size_t len)
{
    return zgec_crc32c(data, len, crc);
}

uint32_t zgec_crc32c_final(uint32_t crc)
{
    /* the one-shot already applies the final XOR;
       for the streaming form the caller passes the
       running state and the final XOR is applied by
       zgec_crc32c. This function is a no-op for the
       one-shot convention used internally. */
    return crc;
}
