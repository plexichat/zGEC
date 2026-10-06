#ifndef ZGEC_COMMON_H
#define ZGEC_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZGEC_MAGIC_U32 0x4345477Au /* "zGEC" little-endian */
#define ZGEC_FOOTER_MAGIC_U32 0x4645477Au /* "zGEF" */
#define ZGEC_TRAILER_MAGIC_U32 0x7A474543u /* "CEGz" */
#define ZGEC_VERSION_MAJOR 1
#define ZGEC_VERSION_MINOR 0

/* Frame flags (u16) */
#define ZGEC_FLAG_CONTENT_SIZE    0x0001u
#define ZGEC_FLAG_HAS_FOOTER      0x0002u
#define ZGEC_FLAG_BLOCK_CHECKSUMS 0x0004u
#define ZGEC_FLAG_EXTENDED_LITREF 0x0008u
#define ZGEC_FLAG_EXTERNAL_DICT   0x0010u
#define ZGEC_FLAG_ALL_KNOWN       0x001Fu

/* Record types */
#define ZGEC_REC_COMPRESSED 0u
#define ZGEC_REC_RAW        1u
#define ZGEC_REC_RLE        2u
#define ZGEC_REC_DICT       3u
#define ZGEC_REC_SKIPPABLE  0x80u /* 0x80..0xFF */

/* Record flags (rflags) */
#define ZGEC_RFLAG_HAS_CHECKSUM   0x01u
#define ZGEC_RFLAG_LIT_EXPORTABLE 0x02u
#define ZGEC_RFLAG_FILTERED       0x04u /* block pre-filter, section 7.5 */
#define ZGEC_RFLAG_ALL_KNOWN      0x07u

/* Block pre-filter (section 7.5) */
#define ZGEC_FILTER_NONE    0
#define ZGEC_FILTER_DELTA   1 /* byte-wise delta */
#define ZGEC_FILTER_SHUFFLE 2 /* stride-N byte shuffle */
#define ZGEC_FILTER_STRIDE_MIN 2
#define ZGEC_FILTER_STRIDE_MAX 64

/* Segment flags */
#define ZGEC_SEG_LIT_FORM       0x01u
#define ZGEC_SEG_LIT_CODER_MASK 0x06u
#define ZGEC_SEG_LIT_CODER_SHIFT 1
#define ZGEC_SEG_LIT_CODER_RAW  0x00u
#define ZGEC_SEG_LIT_CODER_RANS 0x02u /* (1 << shift) */
#define ZGEC_SEG_SEQ_CTX_OF     0x08u
#define ZGEC_SEG_SEQ_CTX_LL     0x10u
#define ZGEC_SEG_ALL_KNOWN      0x1Fu

/* Table modes (2 bits each) */
#define ZGEC_TBL_NEW    0u
#define ZGEC_TBL_REPEAT 1u
#define ZGEC_TBL_RLE    2u
#define ZGEC_TBL_MASK   3u

/* Context modes */
#define ZGEC_CTX_NONE   0
#define ZGEC_CTX_LSB6   1
#define ZGEC_CTX_MSB6   2
#define ZGEC_CTX_TEXT   3
#define ZGEC_CTX_SIGNED 4

/* Limits */
#define ZGEC_BLOCK_LOG2_MIN 16
#define ZGEC_BLOCK_LOG2_MAX 26
#define ZGEC_MAX_DICT_LOG2  26
#define ZGEC_SEG_HINT_LOG2_MIN 14
#define ZGEC_SEG_HINT_LOG2_MAX 22
#define ZGEC_MAX_SEGMENTS 4096u
#define ZGEC_MAX_SEQ_PER_SEG (1u << 22)
#define ZGEC_MAX_DICT_ID 65535u
#define ZGEC_MAX_EPOCH_BLOCKS 255u
#define ZGEC_MAX_LITREF_DEPTH 3u
#define ZGEC_MIN_AL 5
#define ZGEC_MAX_AL 11
#define ZGEC_LIT_AL 11
#define ZGEC_NSYM_SEQ 66
#define ZGEC_NSYM_LIT 256
#define ZGEC_NLANES 8
#define ZGEC_RANS_M 2048
#define ZGEC_RANS_STATE_MIN 65536u
#define ZGEC_OUTPUT_SLACK 64
#define ZGEC_LIT_SLACK 32
/* Encoder tier (section 11.2). */
typedef enum {
    ZGEC_TIER_FAST = 0,
    ZGEC_TIER_MAIN = 1,
    ZGEC_TIER_HIGH = 2
} zgec_tier;

/* Error codes */
typedef enum {
    ZGEC_OK = 0,
    ZGEC_ERR_MAGIC,
    ZGEC_ERR_VERSION,
    ZGEC_ERR_FLAGS,
    ZGEC_ERR_HEADER_CRC,
    ZGEC_ERR_FOOTER_CRC,
    ZGEC_ERR_TRUNCATED,
    ZGEC_ERR_RECORD_TYPE,
    ZGEC_ERR_RECORD_SIZE,
    ZGEC_ERR_BLOCK_SIZE,
    ZGEC_ERR_SEGMENT_COUNT,
    ZGEC_ERR_SEGMENT_SIZE,
    ZGEC_ERR_STREAM_SIZE,
    ZGEC_ERR_VARINT,
    ZGEC_ERR_BITSTREAM,
    ZGEC_ERR_BITSTREAM_SENTINEL,
    ZGEC_ERR_BITSTREAM_OVERFLOW,
    ZGEC_ERR_BITSTREAM_UNCONSUMED,
    ZGEC_ERR_FSE_COUNTS,
    ZGEC_ERR_FSE_AL,
    ZGEC_ERR_FSE_SUM,
    ZGEC_ERR_FSE_SYMBOL,
    ZGEC_ERR_RANS_STATE,
    ZGEC_ERR_RANS_CURSOR,
    ZGEC_ERR_CTX_MODE,
    ZGEC_ERR_CTX_COUNT,
    ZGEC_ERR_CLASS_MAP,
    ZGEC_ERR_LIT_CODER,
    ZGEC_ERR_LIT_FORM,
    ZGEC_ERR_TABLE_MODE,
    ZGEC_ERR_TABLE_INHERIT,
    ZGEC_ERR_RESERVED,
    ZGEC_ERR_DICT_ID,
    ZGEC_ERR_DICT_HASH,
    ZGEC_ERR_DICT_SIZE,
    ZGEC_ERR_DICT_NOT_FOUND,
    ZGEC_ERR_LITREF_DEPTH,
    ZGEC_ERR_LITREF_EXPORT,
    ZGEC_ERR_LITREF_FORM,
    ZGEC_ERR_LITREF_CTX,
    ZGEC_ERR_OFFSET,
    ZGEC_ERR_MATCH_LENGTH,
    ZGEC_ERR_OUTPUT_SIZE,
    ZGEC_ERR_RAW_LEN,
    ZGEC_ERR_LL_SUM,
    ZGEC_ERR_CHECKSUM,
    ZGEC_ERR_CONTENT_SIZE,
    ZGEC_ERR_NOMEM,
    ZGEC_ERR_INVAL,
    ZGEC_ERR_INTERNAL
} zgec_err;

const char *zgec_strerror(zgec_err e);

/* Library version as "major.minor". */
const char *zgec_version(void);

/* ---- block pre-filter transforms (section 7.5) ---- */
/* Length-preserving. zgec_filter_validate checks a descriptor
 * (mode, param): mode 1 needs param 0, mode 2 needs param in 2..64;
 * modes 0 and 3..255 are reserved. zgec_filter_apply writes the filtered
 * form of src (n bytes) into dst (dst and src must not overlap; n == 0
 * is a no-op). zgec_filter_inverse reverses the transform in place over
 * n bytes. All return ZGEC_OK or an error. */
zgec_err zgec_filter_validate(unsigned mode, unsigned param);
zgec_err zgec_filter_apply(uint8_t *dst, const uint8_t *src, size_t n,
                           unsigned mode, unsigned param);
zgec_err zgec_filter_inverse(uint8_t *buf, size_t n,
                             unsigned mode, unsigned param);

/* ---- little-endian helpers ---- */
static inline uint16_t zgec_rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t zgec_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t zgec_rd64(const uint8_t *p) {
    return (uint64_t)zgec_rd32(p) | ((uint64_t)zgec_rd32(p + 4) << 32);
}
static inline void zgec_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void zgec_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void zgec_wr64(uint8_t *p, uint64_t v) {
    zgec_wr32(p, (uint32_t)v); zgec_wr32(p + 4, (uint32_t)(v >> 32));
}

/* ---- bit helpers ---- */
static inline unsigned zgec_highbit32(uint32_t v) {
    /* floor(log2(v)) for v > 0; undefined for v == 0 */
    unsigned n = 0;
    if (v >= 1u << 16) { v >>= 16; n += 16; }
    if (v >= 1u << 8)  { v >>= 8;  n += 8; }
    if (v >= 1u << 4)  { v >>= 4;  n += 4; }
    if (v >= 1u << 2)  { v >>= 2;  n += 2; }
    if (v >= 1u << 1)  { n += 1; }
    return n;
}
static inline unsigned zgec_highbit64(uint64_t v) {
    unsigned n = 0;
    if (v >= 1ull << 32) { v >>= 32; n += 32; }
    return n + zgec_highbit32((uint32_t)v);
}

/* ---- fast approximate log2 (encoder heuristics only) ----
 * Integer part from the leading-zero count plus a linear fraction from
 * the next 8 bits. The linear fraction overestimates log2(1+f)
 * slightly but consistently, so differences of two estimates (all the
 * encoder's cost comparisons) stay faithful. No libm call. */
static inline double zgec_fast_log2_u32(uint32_t x) {
    unsigned e, shift;
    if (x < 2u) return 0.0;
    e = 31u - (unsigned)__builtin_clz(x);
    shift = (e >= 8u) ? (e - 8u) : 0u;
    return (double)e + (double)((x >> shift) & 0xFFu) * (1.0 / 256.0);
}
static inline double zgec_fast_log2_u64(uint64_t x) {
    unsigned e, shift;
    if (x < 2u) return 0.0;
    e = 63u - (unsigned)__builtin_clzll(x);
    shift = (e >= 8u) ? (e - 8u) : 0u;
    return (double)e + (double)((x >> shift) & 0xFFu) * (1.0 / 256.0);
}

/* ---- varint (LEB128, max 5 bytes for 32-bit values) ---- */
/* Returns bytes written, or 0 on error (value too large for 5 bytes is impossible for u32). */
static inline size_t zgec_varint_encode(uint8_t *p, uint32_t v) {
    size_t n = 0;
    while (v >= 0x80u) { p[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}
/* Returns bytes read, or 0 on error (truncated or > 5 bytes). */
static inline size_t zgec_varint_decode(const uint8_t *p, size_t avail, uint32_t *out) {
    uint32_t v = 0;
    size_t n = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (n >= avail) return 0;
        uint8_t b = p[n++];
        if (shift == 28) {
            /* 5th byte: only low 4 bits valid for a 32-bit value */
            if (b & 0xF0u) return 0;
            v |= (uint32_t)(b & 0x0Fu) << shift;
            *out = v;
            return n;
        }
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (!(b & 0x80u)) { *out = v; return n; }
    }
    return 0;
}

/* ---- checked size arithmetic ---- */
static inline int zgec_add_overflows(size_t a, size_t b) {
    return a > SIZE_MAX - b;
}

/* ---- memory helpers (aligned) ---- */
void *zgec_alloc(size_t n, size_t align);
void  zgec_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* ZGEC_COMMON_H */
