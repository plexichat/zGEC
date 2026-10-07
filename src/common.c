#define _POSIX_C_SOURCE 200809L
#include "zgec_common.h"
#include "zgec_internal.h"

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
   Works for any alignment that is a power of two.

   Both size computations are guarded. The prefix
   (sizeof(void *) + align - 1) and the total (n + prefix) can each
   wrap size_t for attacker-influenced sizes, and a wrapped total
   would hand the caller a block smaller than the n bytes it believes
   it owns -- heap corruption on the next write. The UINTPTR_MAX test
   is defensive rather than presently reachable (user-space bases sit
   far below it), but it keeps the alignment arithmetic total. */
void *zgec_alloc(size_t n, size_t align)
{
    size_t prefix;
    size_t total;
    void *raw;
    uintptr_t base;
    uintptr_t aligned;

    if (n == 0) n = 1;
    if (align <= sizeof(void *)) align = sizeof(void *);
    /* align must be a power of two for the mask */
    if ((align & (align - 1)) != 0) return NULL;
    if (zgec_add_overflows(sizeof(void *), align - 1)) return NULL;
    prefix = sizeof(void *) + align - 1;
    if (zgec_add_overflows(n, prefix)) return NULL;
    total = n + prefix;
    raw = malloc(total);
    if (!raw) return NULL;
    base = (uintptr_t)raw;
    if (base > UINTPTR_MAX - prefix) {
        free(raw);
        return NULL;
    }
    aligned = (base + prefix) & ~(uintptr_t)(align - 1);
    ((void **)aligned)[-1] = raw;
    return (void *)aligned;
}

/* Paired with zgec_alloc, and valid only for NULL or a pointer that
   zgec_alloc returned. The original malloc pointer lives in the word
   immediately before p, so any other argument -- including a pointer
   from plain malloc -- reads out of bounds and then frees a garbage
   address. Such a pointer must not be passed to free() either:
   zgec_alloc over-allocates and returns an interior pointer, not the
   malloc result. Do not mix the two allocators. */
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
    /* The transform is defined out of place: delta reads src[i - 1]
       while writing dst[i], so dst == src -- or any partial overlap --
       makes later iterations read bytes this loop already rewrote. The
       header only documents the no-overlap contract; enforce it, so a
       caller that does not read the header gets an error instead of
       silently wrong output. The unsigned difference idiom is used
       rather than pointer comparisons so neither subtraction can
       overrun. */
    {
        uintptr_t d = (uintptr_t)dst;
        uintptr_t s = (uintptr_t)src;
        if (d - s < n || s - d < n) return ZGEC_ERR_INVAL;
    }
    if (mode == ZGEC_FILTER_DELTA) {
        size_t i;
        dst[0] = src[0];
        for (i = 1; i < n; i++) {
            dst[i] = (uint8_t)((unsigned)src[i] - (unsigned)src[i - 1]);
        }
    } else {
        /* Column c is every N-th byte from src + c. docs/spec.md:514 defines
           N = filter_param and R = ceil(n / N), iterating c = 0 to N - 1 and,
           for each c, r = 0 to R - 1 with r*N + c < n. full_rows is the floor
           of n / N, i.e. R minus one while c is inside the remainder, so a
           column holds full_rows + 1 bytes for c < rem and full_rows
           otherwise. That length is known before the loop, so the condition
           carries no multiply and the theoretical r * N overflow cannot
           arise. Lengths come from the shared zgec_filter_shuffle_count
           helper so apply and inverse agree.

           The loop steps an integer index rather than a pointer: advancing a
           pointer by N after the final store would form a value up to N - 1
           bytes past one-past-the-end, which is undefined by C11 6.5.6p8 even
           though the value is never dereferenced. */
        size_t N = (size_t)param;
        size_t full_rows = n / N;
        size_t rem = n % N;
        size_t c;
        size_t k = 0;
        for (c = 0; c < N; c++) {
            size_t count = zgec_filter_shuffle_count(full_rows, rem, c);
            size_t i = c;
            size_t r;
            for (r = 0; r < count; r++) {
                dst[k++] = src[i];
                i += N;
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
        /* Inverse of the apply shuffle: the same per-column lengths
           (docs/spec.md:514), with the destination walked by an index
           stepping N rather than by a pointer, for the reason given in
           zgec_filter_apply above. The temporary stays for now -- removing it
           needs a caller-supplied scratch buffer, i.e. a new signature
           for a function decode.c already calls. */
        size_t N = (size_t)param;
        size_t full_rows = n / N;
        size_t rem = n % N;
        size_t c;
        size_t k = 0;
        uint8_t *tmp = (uint8_t *)zgec_alloc(n, 64);
        if (!tmp) return ZGEC_ERR_NOMEM;
        memcpy(tmp, buf, n);
        for (c = 0; c < N; c++) {
            size_t count = zgec_filter_shuffle_count(full_rows, rem, c);
            size_t i = c;
            size_t r;
            for (r = 0; r < count; r++) {
                buf[i] = tmp[k++];
                i += N;
            }
        }
        zgec_free(tmp);
    }
    return ZGEC_OK;
}

