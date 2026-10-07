#include "zgec_crc32c.h"

#include <string.h>

/* Castagnoli CRC-32C (polynomial 0x1EDC6F41, reflected,
   init and final XOR 0xFFFFFFFF), per zGEC section 2.1.

   Uses the x86 SSE4.2 crc32 instruction when available,
   otherwise a sliced table-based implementation. */

/* Portable thread-local storage for the software table and the
   capability cache below. MSVC's C11 mode historically lacks
   _Thread_local, so use __declspec(thread) there -- the same convention
   src/encode.c uses for its per-worker cluster scratch (ZGEC_ENC_TLS).

   Be clear about what is actually reachable: this file does not
   currently build with MSVC at all (see the probe note below), so the
   __declspec(thread) arm is chosen to be correct for the compiler
   rather than because any MSVC build exercises it today. gcc, clang and
   MinGW define __GNUC__ and take the _Thread_local arm. */
#if defined(_MSC_VER)
#define ZGEC_CRC_TLS __declspec(thread)
#else
#define ZGEC_CRC_TLS _Thread_local
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ZGEC_HAVE_SSE42 1
#include <immintrin.h>
/* SSE4.2 is not part of the x86-64 architectural baseline: valid
   x86-64 CPUs exist without it, so its presence has to be probed at
   run time rather than assumed from the architecture.

   The probed routine carries a target("sse4.2") attribute (see
   zgec_crc32c_hw below), so under gcc, clang or MinGW the file
   compiles whether or not the translation unit was given -msse4.2; the
   probe then decides at run time whether the instruction may actually
   be executed. When the whole translation unit is already compiled for
   SSE4.2 the binary requires the instruction everywhere, so the probe
   is unnecessary and returns 1.

   MSVC is NOT a supported compiler for this file, and the attribute
   does not make it one: on x64, cl.exe defines _M_X64 but not
   __SSE4_2__, so the branch below is taken and <cpuid.h> -- a
   gcc/clang header -- is included, which cl.exe does not ship. Making
   MSVC work needs an <intrin.h>/__cpuid arm for the probe, which is
   deliberately not written here because there is no MSVC build to
   verify it against and an untested compiler path is worse than an
   absent one. The supported set is gcc, clang and MinGW. */
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

/* Software CRC32C: sliced 8-table implementation.

   The table and its ready flag are thread-local. Building them lazily
   in shared statics is a formal C data race -- undefined behaviour
   even though every thread computes identical contents, and a
   visibility hazard on weakly ordered systems where one thread could
   observe the ready flag before the table writes. Per-thread state
   removes the sharing outright, which is stronger than guarding the
   shared writes, and costs one 8 KiB table per thread that takes the
   software path.

   That cost is not only memory: zgec_crc32c_init_table() builds 2048
   entries in a nested bit loop, and it now runs once per thread that
   takes the software path rather than once per process. The CLI starts
   up to n_threads workers per encode, so on a target without SSE4.2
   the build repeats per worker per encode instead of being amortised
   once. It is small next to the encode itself, but it is not free, and
   it was the price of removing a race that is undefined behaviour -- so
   the trade stands. Every thread builds the same values, so the
   checksums produced are unchanged. */
static ZGEC_CRC_TLS uint32_t zgec_crc32c_table[8][256];
static ZGEC_CRC_TLS int zgec_crc32c_table_ready = 0;

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
    /* Consume bytes one at a time until the pointer is 8-byte aligned.
       The wide loads below do not require alignment -- zgec_rd64 is
       defined byte-wise -- so this is not a correctness step and is
       kept only because it costs at most seven iterations and avoids
       unaligned-access penalties on targets that have them. */
    while (len && ((uintptr_t)p & 7)) {
        crc = zgec_crc32c_table[0][(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
        len--;
    }
    while (len >= 8) {
        /* zgec_rd64 is the shared little-endian reader (include/
           zgec_common.h:181): the same eight bytes, in the same order,
           as the hand-assembled load it replaces, and explicit about
           the byte order the CRC is defined over. */
        uint64_t v = zgec_rd64(p) ^ (uint64_t)crc;
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
/* The crc32 instruction is not permitted unless the compiler is
   targeting SSE4.2, so the routine is compiled with that target
   explicitly. Runtime CPUID decides whether it may be called; without
   this attribute a build that does not pass -msse4.2 fails to compile
   the intrinsic even though the call is guarded at run time. GCC and
   Clang accept the attribute; MSVC enables the intrinsics for x64
   unconditionally, so no attribute is needed there. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.2")))
#endif
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

/* Thread-local capability cache: writing this from two threads at once
   is the same data race as the table above. Per-thread it costs one
   CPUID per thread, and zgec_cpu_has_sse42() is itself pure. */
static ZGEC_CRC_TLS int zgec_crc32c_use_hw = -1;

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

/* Streaming convention, stated explicitly because it is easy to get
   wrong: update() accepts and returns the *externally finalised* CRC
   value, exactly as zgec_crc32c() does. The initial and final XORs are
   applied inside every call, so a caller must carry the value returned
   by update() straight into the next update() -- it must NOT keep an
   unfinalised internal state between calls, which is what most chained
   CRC APIs expect.

       uint32_t c = zgec_crc32c_init();             -- 0
       c = zgec_crc32c_update(c, a, na);
       c = zgec_crc32c_update(c, b, nb);            -- CRC(a || b)
       c = zgec_crc32c_final(c);                    -- no-op

   Under that convention update(c, a, na) followed by update(_, b, nb)
   is the CRC of a || b, which is why final() has nothing left to do. */
uint32_t zgec_crc32c_init(void) { return 0; }

uint32_t zgec_crc32c_update(uint32_t crc, const void *data, size_t len)
{
    return zgec_crc32c(data, len, crc);
}

uint32_t zgec_crc32c_final(uint32_t crc)
{
    /* No-op by construction: the one-shot zgec_crc32c() already applies
       the final XOR to the value it returns, and update() is that same
       call, so the running state the caller holds is already finalised. */
    return crc;
}
