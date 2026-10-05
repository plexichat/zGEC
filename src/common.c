#define _POSIX_C_SOURCE 200809L
#include "zgec_common.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ZGEC_VER_STR2(x) #x
#define ZGEC_VER_STR(x) ZGEC_VER_STR2(x)

/* Version string of the library. */
const char *zgec_version(void)
{
    return ZGEC_VER_STR(ZGEC_VERSION_MAJOR) "."
           ZGEC_VER_STR(ZGEC_VERSION_MINOR);
}

const char *zgec_strerror(zgec_err e)
{
    switch (e) {
    case ZGEC_OK: return "success";
    case ZGEC_ERR_MAGIC: return "bad magic";
    case ZGEC_ERR_VERSION: return "unsupported version";
    case ZGEC_ERR_FLAGS: return "unknown flag bits set";
    case ZGEC_ERR_HEADER_CRC: return "frame header CRC mismatch";
    case ZGEC_ERR_FOOTER_CRC: return "footer CRC mismatch";
    case ZGEC_ERR_TRUNCATED: return "truncated input";
    case ZGEC_ERR_RECORD_TYPE: return "unknown record type";
    case ZGEC_ERR_RECORD_SIZE: return "record size mismatch";
    case ZGEC_ERR_BLOCK_SIZE: return "invalid block size";
    case ZGEC_ERR_SEGMENT_COUNT: return "invalid segment count";
    case ZGEC_ERR_SEGMENT_SIZE: return "invalid segment size";
    case ZGEC_ERR_STREAM_SIZE: return "invalid stream size";
    case ZGEC_ERR_VARINT: return "invalid varint";
    case ZGEC_ERR_BITSTREAM: return "bitstream error";
    case ZGEC_ERR_BITSTREAM_SENTINEL: return "bitstream sentinel missing";
    case ZGEC_ERR_BITSTREAM_OVERFLOW: return "bitstream read past end";
    case ZGEC_ERR_BITSTREAM_UNCONSUMED: return "bitstream not exactly consumed";
    case ZGEC_ERR_FSE_COUNTS: return "invalid FSE counts";
    case ZGEC_ERR_FSE_AL: return "invalid FSE accuracy log";
    case ZGEC_ERR_FSE_SUM: return "FSE counts do not sum to table size";
    case ZGEC_ERR_FSE_SYMBOL: return "invalid FSE symbol";
    case ZGEC_ERR_RANS_STATE: return "invalid rANS state";
    case ZGEC_ERR_RANS_CURSOR: return "rANS word cursor out of range";
    case ZGEC_ERR_CTX_MODE: return "invalid context mode";
    case ZGEC_ERR_CTX_COUNT: return "invalid context count";
    case ZGEC_ERR_CLASS_MAP: return "invalid class map";
    case ZGEC_ERR_LIT_CODER: return "invalid literal coder";
    case ZGEC_ERR_LIT_FORM: return "invalid literal form";
    case ZGEC_ERR_TABLE_MODE: return "invalid table mode";
    case ZGEC_ERR_TABLE_INHERIT: return "invalid table inheritance";
    case ZGEC_ERR_RESERVED: return "reserved bits set";
    case ZGEC_ERR_DICT_ID: return "invalid dictionary id";
    case ZGEC_ERR_DICT_HASH: return "dictionary hash mismatch";
    case ZGEC_ERR_DICT_SIZE: return "dictionary size mismatch";
    case ZGEC_ERR_DICT_NOT_FOUND: return "dictionary not found";
    case ZGEC_ERR_LITREF_DEPTH: return "invalid literal reference depth";
    case ZGEC_ERR_LITREF_EXPORT: return "block not literal-exportable";
    case ZGEC_ERR_LITREF_FORM: return "literal reference requires plain literals";
    case ZGEC_ERR_LITREF_CTX: return "literal reference forbids LL conditioning";
    case ZGEC_ERR_OFFSET: return "invalid offset";
    case ZGEC_ERR_MATCH_LENGTH: return "invalid match length";
    case ZGEC_ERR_OUTPUT_SIZE: return "output size exceeded";
    case ZGEC_ERR_RAW_LEN: return "segment raw_len mismatch";
    case ZGEC_ERR_LL_SUM: return "literal length sum exceeds n_lit";
    case ZGEC_ERR_CHECKSUM: return "checksum mismatch";
    case ZGEC_ERR_CONTENT_SIZE: return "content size mismatch";
    case ZGEC_ERR_NOMEM: return "out of memory";
    case ZGEC_ERR_INVAL: return "invalid argument";
    case ZGEC_ERR_INTERNAL: return "internal error";
    default: return "unknown error";
    }
}

/* Aligned allocation via over-allocation: the block
   is [ original pointer ][ padding ][ aligned data ].
   The original pointer is stored immediately before
   the aligned data so zgec_free can recover it.
   Works for any alignment that is a power of two. */
void *zgec_alloc(size_t n, size_t align)
{
    if (n == 0) n = 1;
    if (align <= sizeof(void *)) align = sizeof(void *);
    /* align must be a power of two for the mask */
    if ((align & (align - 1)) != 0) return NULL;
    size_t prefix = sizeof(void *) + align - 1;
    void *raw = malloc(n + prefix);
    if (!raw) return NULL;
    uintptr_t aligned = ((uintptr_t)raw + prefix) & ~(uintptr_t)(align - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

void zgec_free(void *p)
{
    if (!p) return;
    void *raw = ((void **)p)[-1];
    free(raw);
}

/* ---- block pre-filter transforms (section 7.5) ---- */

zgec_err zgec_filter_validate(unsigned mode, unsigned param)
{
    if (mode == ZGEC_FILTER_DELTA) {
        return (param == 0u) ? ZGEC_OK : ZGEC_ERR_RESERVED;
    }
    if (mode == ZGEC_FILTER_SHUFFLE) {
        if (param < (unsigned)ZGEC_FILTER_STRIDE_MIN ||
            param > (unsigned)ZGEC_FILTER_STRIDE_MAX) {
            return ZGEC_ERR_RESERVED;
        }
        return ZGEC_OK;
    }
    /* mode 0 and 3..255 are reserved (MUST NOT appear with FILTERED). */
    return ZGEC_ERR_RESERVED;
}

zgec_err zgec_filter_apply(uint8_t *dst, const uint8_t *src, size_t n,
                           unsigned mode, unsigned param)
{
    zgec_err v = zgec_filter_validate(mode, param);
    if (v != ZGEC_OK) return v;
    if (n == 0) return ZGEC_OK;
    if (dst == NULL || src == NULL) return ZGEC_ERR_INVAL;
    if (mode == ZGEC_FILTER_DELTA) {
        size_t i;
        dst[0] = src[0];
        for (i = 1; i < n; i++) {
            dst[i] = (uint8_t)((unsigned)src[i] - (unsigned)src[i - 1]);
        }
    } else {
        size_t N = (size_t)param;
        size_t c;
        size_t k = 0;
        for (c = 0; c < N; c++) {
            size_t r;
            for (r = 0; r * N + c < n; r++) {
                dst[k++] = src[r * N + c];
            }
        }
    }
    return ZGEC_OK;
}

zgec_err zgec_filter_inverse(uint8_t *buf, size_t n,
                             unsigned mode, unsigned param)
{
    zgec_err v = zgec_filter_validate(mode, param);
    if (v != ZGEC_OK) return v;
    if (n == 0) return ZGEC_OK;
    if (buf == NULL) return ZGEC_ERR_INVAL;
    if (mode == ZGEC_FILTER_DELTA) {
        size_t i;
        for (i = 1; i < n; i++) {
            buf[i] = (uint8_t)((unsigned)buf[i] + (unsigned)buf[i - 1]);
        }
    } else {
        size_t N = (size_t)param;
        size_t c;
        size_t k = 0;
        uint8_t *tmp = (uint8_t *)zgec_alloc(n, 64);
        if (!tmp) return ZGEC_ERR_NOMEM;
        memcpy(tmp, buf, n);
        for (c = 0; c < N; c++) {
            size_t r;
            for (r = 0; r * N + c < n; r++) {
                buf[r * N + c] = tmp[k++];
            }
        }
        zgec_free(tmp);
    }
    return ZGEC_OK;
}

