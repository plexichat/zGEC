#define _POSIX_C_SOURCE 200809L
#include "zgec_block.h"
#include "zgec_fse.h"
#include "zgec_lit.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>

/* ================================================================
 * Block parameters (section 7.1)
 * ================================================================ */

static size_t zgec_ctx_desc_parse(zgec_ctx_desc *desc, const uint8_t *buf, size_t size)
{
    if (!desc || !buf || size < 2) return 0;
    uint8_t ctx_mode = buf[0];
    uint8_t ctx_count = buf[1];
    if (ctx_mode > 4) return 0;                      /* ZGEC_ERR_CTX_MODE */
    if (ctx_count != 1 && ctx_count != 2 &&
        ctx_count != 4 && ctx_count != 8) return 0;  /* ZGEC_ERR_CTX_COUNT */
    if (ctx_mode == 0 && ctx_count != 1) return 0;   /* ZGEC_ERR_CTX_COUNT */
    size_t needed = (ctx_count == 1) ? 2u : 26u;
    if (size < needed) return 0;

    desc->ctx_mode = ctx_mode;
    desc->ctx_count = ctx_count;
    if (ctx_count > 1) {
        /* Shared 3-bit codec (see zgec_lit.h); keeps the 24-byte layout
         * identical between block params and literal class maps. */
        if (zgec_class_map_decode(desc->class_map, buf + 2,
                                  (int)ctx_count) != ZGEC_OK)
            return 0;
    } else {
        memset(desc->class_map, 0, 64);
    }
    return needed;
}

static size_t zgec_ctx_desc_emit(uint8_t *buf, size_t cap, const zgec_ctx_desc *desc)
{
    if (!buf || !desc || cap < 2) return 0;
    if (desc->ctx_mode > 4) return 0;                  /* ZGEC_ERR_CTX_MODE */
    if (desc->ctx_count != 1 && desc->ctx_count != 2 &&
        desc->ctx_count != 4 && desc->ctx_count != 8) return 0; /* ZGEC_ERR_CTX_COUNT */
    if (desc->ctx_mode == 0 && desc->ctx_count != 1) return 0;  /* ZGEC_ERR_CTX_COUNT */
    size_t needed = (desc->ctx_count == 1) ? 2u : 26u;
    if (cap < needed) return 0;

    buf[0] = desc->ctx_mode;
    buf[1] = desc->ctx_count;
    if (desc->ctx_count > 1) {
        /* Validate before the unchecked trusted-input encode. */
        for (int i = 0; i < 64; i++) {
            if (desc->class_map[i] >= desc->ctx_count) return 0;
        }
        zgec_class_map_encode(buf + 2, desc->class_map);
    }
    return needed;
}

size_t zgec_block_params_parse(zgec_block_params *bp,
                               const uint8_t *buf, size_t size)
{
    if (!bp || !buf) return 0;
    size_t plain_len = zgec_ctx_desc_parse(&bp->plain, buf, size);
    if (plain_len == 0 || plain_len >= size) return 0;
    size_t sub_len = zgec_ctx_desc_parse(&bp->sub, buf + plain_len, size - plain_len);
    if (sub_len == 0) return 0;
    return plain_len + sub_len;
}

size_t zgec_block_params_emit(uint8_t *buf, size_t cap,
                              const zgec_block_params *bp)
{
    if (!buf || !bp) return 0;
    size_t plain_len = zgec_ctx_desc_emit(buf, cap, &bp->plain);
    if (plain_len == 0 || plain_len >= cap) return 0;
    size_t sub_len = zgec_ctx_desc_emit(buf + plain_len, cap - plain_len, &bp->sub);
    if (sub_len == 0) return 0;
    return plain_len + sub_len;
}

/* ================================================================
 * Segment directory (section 7.2)
 * ================================================================ */

zgec_err zgec_seg_dir_parse(zgec_seg_dir_entry *dir,
                            uint32_t segment_count,
                            const uint8_t *buf, size_t size)
{
    if (!dir || segment_count == 0 || segment_count > ZGEC_MAX_SEGMENTS)
        return ZGEC_ERR_SEGMENT_COUNT;
    if (!buf || size < (size_t)segment_count * 8u)
        return ZGEC_ERR_TRUNCATED;

    for (uint32_t i = 0; i < segment_count; i++) {
        const uint8_t *p = buf + (size_t)i * 8u;
        dir[i].comp_len = zgec_rd32(p);
        dir[i].raw_len  = zgec_rd32(p + 4);
    }
    return ZGEC_OK;
}

void zgec_seg_dir_emit(uint8_t *buf,
                       const zgec_seg_dir_entry *dir,
                       uint32_t segment_count)
{
    if (!buf || !dir || segment_count == 0) return;
    for (uint32_t i = 0; i < segment_count; i++) {
        uint8_t *p = buf + (size_t)i * 8u;
        zgec_wr32(p,     dir[i].comp_len);
        zgec_wr32(p + 4, dir[i].raw_len);
    }
}

/* ================================================================
 * Segment header (section 7.3)
 * ================================================================ */

/* Probe one NEW FSE description without disturbing the caller.
 * max_nsym is 256 for literals, 66 for sequences.
 * Returns bytes consumed, or 0 on error. */
static size_t zgec_probe_new_desc(const uint8_t *buf, size_t avail,
                                  int max_nsym, int *al_out)
{
    int16_t counts[256];
    int nsym = 0;
    int al = 0;
    size_t n;
    if (max_nsym < 1 || max_nsym > 256) return 0;
    if (buf == NULL || avail == 0) return 0;
    n = zgec_fse_read_counts(counts, &nsym, &al, max_nsym, buf, avail);
    if (n == 0) return 0;
    if (nsym < 1 || nsym > max_nsym) return 0;
    if (al_out != NULL) *al_out = al;
    return n;
}

/* One NEW literal descriptor: AL must be exactly 11 (section 7.3). */
static size_t zgec_lit_new_bytes(const uint8_t *buf, size_t avail)
{
    int al = 0;
    size_t n = zgec_probe_new_desc(buf, avail, ZGEC_NSYM_LIT, &al);
    if (n == 0) return 0;
    if (al != ZGEC_LIT_AL) return 0;                 /* ZGEC_ERR_FSE_AL */
    return n;
}

/* One NEW sequence descriptor: AL 5..11 (section 7.3). */
static size_t zgec_seq_new_bytes(const uint8_t *buf, size_t avail, int *al_out)
{
    int al = 0;
    size_t n = zgec_probe_new_desc(buf, avail, ZGEC_NSYM_SEQ, &al);
    if (n == 0) return 0;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return 0; /* ZGEC_ERR_FSE_AL */
    if (al_out != NULL) *al_out = al;
    return n;
}

/* One RLE sequence descriptor byte: symbol code < 66 (V6). */
static size_t zgec_seq_rle_bytes(const uint8_t *buf, size_t avail)
{
    if (avail < 1) return 0;
    if (buf[0] >= (uint8_t)ZGEC_NSYM_SEQ) return 0;   /* ZGEC_ERR_FSE_SYMBOL */
    return 1;
}

/* NOTE: there is deliberately no literal-RLE helper. Spec 7.3 defines
 * literal tables as 0=new, 1=repeat only; RLE exists solely for the
 * 66-symbol sequence alphabets. decode.c rejects literal RLE too. */

/* Accumulate n into *acc, refusing to wrap. Every caller here already
 * guarantees n <= the bytes left in the buffer, so this cannot fire today;
 * it makes the size accumulators formally safe (review entry 10) instead of
 * depending on that argument holding for every future caller. */
static int zgec_size_add(size_t *acc, size_t n)
{
    if (n > SIZE_MAX - *acc) return 0;
    *acc += n;
    return 1;
}

/*
 * Count the total descriptor bytes for a segment header.
 * lit_coder: 0 = raw (no literal tables), 1 = rANS.
 * k: context count (1, 2, 4, or 8); when lit_coder != 0 and k == 1 there
 *    is one literal table; when k > 1 there are k+1 literal tables
 *    (contexts 0..k-1 plus the run-start table, each FSE AL == 11).
 * Sequence streams carry 1 description, or 3 when the corresponding
 * seq_ctx bit is set (all three share one AL in 5..11). RLE streams
 * carry one byte (< 66 for the 66-symbol sequence alphabets).
 * REPEAT streams carry nothing here (caller omits them).
 *
 * Returns the byte count, or (size_t)-1 on error (truncated, bad AL,
 * bad RLE symbol, or reserved mode). A legitimate empty descriptor run
 * returns 0, which is why the error sentinel is needed.
 */
static size_t zgec_seg_desc_bytes_count(uint8_t table_modes,
                                        uint8_t lit_coder,
                                        uint32_t n_seq,
                                        int seq_ctx_of,
                                        int seq_ctx_ll,
                                        int k,
                                        const uint8_t *buf,
                                        size_t avail)
{
    size_t total = 0;

    /* Literal tables: 0 when lit_coder == 0. Only NEW carries bytes here;
       REPEAT carries nothing (caller omits it), and RLE is not defined for
       literals (section 7.3), so any other mode is an error. */
    if (lit_coder != 0) {
        uint8_t lit_mode = (uint8_t)((table_modes >> 0) & ZGEC_TBL_MASK);
        if (lit_mode != ZGEC_TBL_REPEAT) {
            int n_lit_tables;
            if (lit_mode != ZGEC_TBL_NEW) return (size_t)-1;
            n_lit_tables = (k <= 1) ? 1 : (k + 1);
            if (n_lit_tables < 1) return (size_t)-1;
            for (int i = 0; i < n_lit_tables; i++) {
                size_t a = (avail > total) ? (avail - total) : 0;
                size_t n = zgec_lit_new_bytes(buf + total, a);
                if (n == 0) return (size_t)-1;
                if (!zgec_size_add(&total, n)) return (size_t)-1;
            }
        }
    }

    if (n_seq == 0) return total;

    /* LL: 1 normally, 3 when seq_ctx_ll. The mode test is loop-invariant
       (review entry 11), so it is hoisted out of the loop: the RLE form
       always consumes one byte per table, the NEW form one description per
       table, and REPEAT consumes nothing. */
    {
        uint8_t ll_mode = (uint8_t)((table_modes >> 2) & ZGEC_TBL_MASK);
        int n_ll = seq_ctx_ll ? 3 : 1;
        if (ll_mode == ZGEC_TBL_RLE) {
            for (int i = 0; i < n_ll; i++) {
                size_t a = (avail > total) ? (avail - total) : 0;
                size_t n = zgec_seq_rle_bytes(buf + total, a);
                if (n == 0) return (size_t)-1;
                if (!zgec_size_add(&total, n)) return (size_t)-1;
            }
        } else if (ll_mode == ZGEC_TBL_NEW) {
            int first_al = -1;
            for (int i = 0; i < n_ll; i++) {
                size_t a = (avail > total) ? (avail - total) : 0;
                int al = 0;
                size_t n = zgec_seq_new_bytes(buf + total, a, &al);
                if (n == 0) return (size_t)-1;
                if (i == 0) first_al = al;
                else if (al != first_al) return (size_t)-1;
                if (!zgec_size_add(&total, n)) return (size_t)-1;
            }
        } else if (ll_mode != ZGEC_TBL_REPEAT) {
            return (size_t)-1;
        }
    }

    /* ML: always 1. */
    {
        uint8_t ml_mode = (uint8_t)((table_modes >> 4) & ZGEC_TBL_MASK);
        if (ml_mode != ZGEC_TBL_REPEAT) {
            size_t a = (avail > total) ? (avail - total) : 0;
            size_t n = 0;
            if (ml_mode == ZGEC_TBL_RLE) {
                n = zgec_seq_rle_bytes(buf + total, a);
            } else if (ml_mode == ZGEC_TBL_NEW) {
                n = zgec_seq_new_bytes(buf + total, a, NULL);
            } else {
                return (size_t)-1;
            }
            if (n == 0) return (size_t)-1;
            if (!zgec_size_add(&total, n)) return (size_t)-1;
        }
    }

    /* OF: 1 normally, 3 when seq_ctx_of. Same hoist as LL (entry 11). */
    {
        uint8_t of_mode = (uint8_t)((table_modes >> 6) & ZGEC_TBL_MASK);
        int n_of = seq_ctx_of ? 3 : 1;
        if (of_mode == ZGEC_TBL_RLE) {
            for (int i = 0; i < n_of; i++) {
                size_t a = (avail > total) ? (avail - total) : 0;
                size_t n = zgec_seq_rle_bytes(buf + total, a);
                if (n == 0) return (size_t)-1;
                if (!zgec_size_add(&total, n)) return (size_t)-1;
            }
        } else if (of_mode == ZGEC_TBL_NEW) {
            int first_al = -1;
            for (int i = 0; i < n_of; i++) {
                size_t a = (avail > total) ? (avail - total) : 0;
                int al = 0;
                size_t n = zgec_seq_new_bytes(buf + total, a, &al);
                if (n == 0) return (size_t)-1;
                if (i == 0) first_al = al;
                else if (al != first_al) return (size_t)-1;
                if (!zgec_size_add(&total, n)) return (size_t)-1;
            }
        } else if (of_mode != ZGEC_TBL_REPEAT) {
            return (size_t)-1;
        }
    }

    return total;
}

size_t zgec_seg_header_parse_ex(zgec_seg_header *sh,
                                const uint8_t *buf, size_t size,
                                uint8_t *descriptors,
                                size_t descriptors_cap,
                                size_t *descriptors_size,
                                int k)
{
    if (!sh || !buf || size < 2) return 0;
    /* k comes from the block parameters (section 7.1) and selects how many
       literal tables follow (1 for k == 1, k + 1 otherwise). Only 1, 2, 4
       and 8 are defined; anything else is a caller bug and is rejected here
       rather than silently sized as k == 1. */
    if (k != 1 && k != 2 && k != 4 && k != 8) return 0;

    size_t pos = 0;

    /* ---- segment_flags ---- */
    uint8_t segment_flags = buf[pos++];
    {
        uint8_t lit_form  = segment_flags & 0x01u;
        uint8_t lit_coder = (segment_flags >> 1) & 0x03u;
        uint8_t reserved  = (segment_flags >> 5) & 0x07u;

        if (reserved != 0)  return 0;               /* ZGEC_ERR_RESERVED */
        if (lit_form > 1)   return 0;               /* ZGEC_ERR_LIT_FORM */
        if (lit_coder != 0 && lit_coder != 1) return 0; /* ZGEC_ERR_LIT_CODER */
        (void)lit_coder;
    }

    /* ---- table_modes ---- */
    uint8_t table_modes = buf[pos++];
    for (int i = 0; i < 4; i++) {
        uint8_t mode = (table_modes >> (2 * i)) & ZGEC_TBL_MASK;
        if (mode > 2) return 0;                     /* ZGEC_ERR_TABLE_MODE */
    }
    /* Spec 7.3: literal tables are 0=new, 1=repeat; RLE is not defined for
       the 256-symbol literal alphabet (decode.c rejects it as well). */
    if (((table_modes >> 0) & ZGEC_TBL_MASK) == ZGEC_TBL_RLE) return 0;

    /* ---- n_seq varint ---- */
    uint32_t n_seq;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &n_seq);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- n_lit varint ---- */
    uint32_t n_lit;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &n_lit);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- Count descriptor bytes ---- */
    {
        uint8_t lit_coder  = (uint8_t)((segment_flags >> 1) & 0x03u);
        int    seq_ctx_of  = (segment_flags >> 3) & 1;
        int    seq_ctx_ll  = (segment_flags >> 4) & 1;
        size_t avail       = (size > pos) ? size - pos : 0;

        size_t desc_bytes = zgec_seg_desc_bytes_count(table_modes,
                                                      lit_coder,
                                                      n_seq,
                                                      seq_ctx_of,
                                                      seq_ctx_ll,
                                                      k,
                                                      buf + pos,
                                                      avail);
        if (desc_bytes == (size_t)-1) return 0;       /* bad descriptor */
        /* Written as a subtraction rather than pos + desc_bytes > size: pos
           is <= size here (each varint above consumed at most size - pos), so
           this cannot wrap, whereas the addition could with a large
           desc_bytes. Same predicate, entry 10's hardening. */
        if (desc_bytes > size - pos) return 0;        /* ZGEC_ERR_TRUNCATED */

        /* Copy raw descriptor bytes to caller's buffer. A short
           caller buffer is an error (no silent truncation) so that
           emit(parse(x)) round-trips exactly. NULL means count only. */
        if (desc_bytes > 0 && descriptors != NULL) {
            if (descriptors_cap < desc_bytes) return 0;
            memcpy(descriptors, buf + pos, desc_bytes);
        }
        if (descriptors_size != NULL) *descriptors_size = desc_bytes;

        pos += desc_bytes;
    }

    /* ---- lit_size varint ---- */
    uint32_t lit_size;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &lit_size);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- ll_size varint ---- */
    uint32_t ll_size;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &ll_size);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- ml_size varint ---- */
    uint32_t ml_size;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &ml_size);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- of_size varint ---- */
    uint32_t of_size;
    {
        size_t n = zgec_varint_decode(buf + pos, size - pos, &of_size);
        if (n == 0) return 0;                       /* ZGEC_ERR_VARINT */
        pos += n;
    }

    /* ---- Cross-field validation (review entry 12) ----
       The three format invariants the header alone can be held to, matching
       the rules decode.c applies to the same bytes (V8, spec:769) so the two
       parsers accept exactly the same headers. Spec 7.3 sets the three
       sequence stream sizes to 0 when there are no sequences; spec 9.1 makes
       a raw literal stream exactly the n_lit bytes of Z, so lit_size == n_lit
       whenever lit_coder is 0; and spec 9.5 makes n_lit == 0 the empty stream
       with lit_size == 0, which the first two do not cover for a coded (rANS)
       literal stream -- the reachable gap review finding 1 identified. A
       header that violates any of them describes streams that cannot exist;
       every consumer sizes buffers from these fields, so they are rejected
       here rather than downstream.

       On a 0 return both `descriptors` and `*descriptors_size` are
       unspecified: the sizes compared here are parsed after the descriptor
       run, so that copy has already happened (review finding 5). Spec 7.3
       fixes the field order, so the checks cannot be hoisted above it. */
    if (n_seq == 0 && (ll_size != 0 || ml_size != 0 || of_size != 0)) return 0;
    if (n_lit == 0 && lit_size != 0) return 0;                  /* V8 */
    {
        uint8_t lit_coder = (uint8_t)((segment_flags >> 1) & 0x03u);
        if (lit_coder == 0 && lit_size != n_lit) return 0;
    }

    /* ---- Fill output struct ---- */
    sh->segment_flags  = segment_flags;
    sh->table_modes    = table_modes;
    sh->n_seq          = n_seq;
    sh->n_lit          = n_lit;
    sh->lit_size       = lit_size;
    sh->ll_size        = ll_size;
    sh->ml_size        = ml_size;
    sh->of_size        = of_size;
    sh->streams_off    = pos;
    sh->header_size    = pos;

    return pos;
}

size_t zgec_seg_header_parse(zgec_seg_header *sh,
                             const uint8_t *buf, size_t size,
                             uint8_t *descriptors,
                             size_t descriptors_cap,
                             size_t *descriptors_size)
{
    /* Single-context wrapper: assumes k == 1 (one literal table). Callers
       for blocks with k > 1 MUST use zgec_seg_header_parse_ex with the real
       k from the block parameters; sizing multi-context descriptors as k == 1
       would silently misparse the header, so this default must never be
       relied on when k is unknown. */
    return zgec_seg_header_parse_ex(sh, buf, size, descriptors, descriptors_cap, descriptors_size, 1);
}

/* Encode one size varint into buf at *pos, testing the capacity first.
 * zgec_varint_encode() has no capacity parameter and writes up to 5 bytes
 * unconditionally, so encoding straight into buf would store before the
 * bounds test could reject it (review finding 2). *pos <= cap must hold on
 * entry; every caller below maintains that. Returns 0 (no write) on error. */
static int zgec_seg_emit_varint(uint8_t *buf, size_t cap, size_t *pos,
                               uint32_t v)
{
    uint8_t scratch[5];
    size_t n = zgec_varint_encode(scratch, v);
    if (n > cap - *pos) return 0;
    memcpy(buf + *pos, scratch, n);
    *pos += n;
    return 1;
}

size_t zgec_seg_header_emit(uint8_t *buf, size_t cap,
                            const zgec_seg_header *sh,
                            const uint8_t *descriptors,
                            size_t descriptors_size)
{
    if (!buf || !sh || cap < 2) return 0;

    /* Mirror the parser's cross-field rules so emit and parse stay inverses
       (review finding 3): a header this function accepts must be readable by
       zgec_seg_header_parse_ex. These are the same three the parser rejects
       below, so no conformant stream is refused and an encoder regression
       fails loudly here instead of writing a self-inconsistent header. */
    if (sh->n_seq == 0 &&
        (sh->ll_size != 0 || sh->ml_size != 0 || sh->of_size != 0)) return 0;
    if (sh->n_lit == 0 && sh->lit_size != 0) return 0;          /* V8 */
    {
        uint8_t lit_coder = (uint8_t)((sh->segment_flags >> 1) & 0x03u);
        if (lit_coder == 0 && sh->lit_size != sh->n_lit) return 0;
    }

    /* Validate fixed fields. */
    {
        uint8_t lit_form  = sh->segment_flags & 0x01u;
        uint8_t lit_coder = (sh->segment_flags >> 1) & 0x03u;
        uint8_t reserved  = (sh->segment_flags >> 5) & 0x07u;

        if (reserved != 0)   return 0;               /* ZGEC_ERR_RESERVED */
        if (lit_form > 1)    return 0;               /* ZGEC_ERR_LIT_FORM */
        if (lit_coder != 0 && lit_coder != 1) return 0; /* ZGEC_ERR_LIT_CODER */
    }

    for (int i = 0; i < 4; i++) {
        uint8_t mode = (sh->table_modes >> (2 * i)) & ZGEC_TBL_MASK;
        if (mode > 2) return 0;                      /* ZGEC_ERR_TABLE_MODE */
    }
    /* No RLE for literal tables (section 7.3). */
    if (((sh->table_modes >> 0) & ZGEC_TBL_MASK) == ZGEC_TBL_RLE) return 0;

    size_t pos = 0;

    buf[pos++] = sh->segment_flags;
    buf[pos++] = sh->table_modes;

    /* n_seq varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->n_seq)) return 0;

    /* n_lit varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->n_lit)) return 0;

    /* Descriptors verbatim. Subtracting rather than adding: pos <= cap here,
       but pos + descriptors_size can wrap for a huge descriptors_size and let
       the memcpy below run past the buffer (entry 10's family). */
    if (descriptors_size > cap - pos) return 0;
    if (descriptors && descriptors_size > 0)
        memcpy(buf + pos, descriptors, descriptors_size);
    pos += descriptors_size;

    /* lit_size varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->lit_size)) return 0;

    /* ll_size varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->ll_size)) return 0;

    /* ml_size varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->ml_size)) return 0;

    /* of_size varint */
    if (!zgec_seg_emit_varint(buf, cap, &pos, sh->of_size)) return 0;

    return pos;
}

/* ================================================================
 * Table descriptor walk (section 7.3 / 8.6 / 9.3)
 *
 * Contract: the caller has already resolved table_modes. REPEAT
 * tables carry no bytes and are skipped by passing 0/NULL for that
 * stream (n_*_tables == 0 or the pointer == NULL). Every entry the
 * caller asks to consume IS present in buf: either NEW (an FSE
 * description) or, for sequence streams only, RLE (one byte). The walk
 * classifies each entry by probing for a well-formed FSE description
 * with the stream's alphabet/AL rules (literals: 256 symbols, AL == 11;
 * sequences: 66 symbols, AL 5..11); a sequence entry that is not a valid
 * description must be a single RLE byte (< 66), while a literal entry
 * that fails the probe is an error (literals have no RLE mode, section
 * 7.3). A probe that parses as FSE but with a wrong AL is an error,
 * not RLE.
 *
 * The first-segment repeat rule (7.4) cannot be checked here: this
 * function sees only one segment's descriptor bytes. Callers enforce
 * it by passing n_*_tables > 0 / non-NULL only for modes the segment
 * is allowed to use (first segment: no REPEAT), or by comparing
 * table_modes against the previous segment before calling.
 *
 * Conditioned streams (seq_ctx_of/ll) carry 3 descriptions that MUST
 * share one AL; when n_ll/n_of == 3 the walk enforces equal AL across
 * the NEW entries of the group.
 * ================================================================ */

/* Classify and consume one present descriptor at buf[pos].
 * Fills *out, returns bytes consumed, or 0 on error.
 * When the entry is NEW and al_out != NULL, *al_out gets its AL. */
static size_t zgec_walk_one(zgec_tbl_desc *out,
                            const uint8_t *buf, size_t size, size_t pos,
                            int is_lit, int *al_out)
{
    int max_nsym = is_lit ? ZGEC_NSYM_LIT : ZGEC_NSYM_SEQ;
    int al = 0;
    size_t n;

    if (pos >= size) return 0;
    n = zgec_probe_new_desc(buf + pos, size - pos, max_nsym, &al);
    if (n > 0) {
        int ok = is_lit ? (al == ZGEC_LIT_AL)
                        : (al >= ZGEC_MIN_AL && al <= ZGEC_MAX_AL);
        if (!ok) return 0;                            /* bad AL, not RLE */
        out->mode       = (int)ZGEC_TBL_NEW;
        out->ptr        = buf + pos;
        out->size       = n;
        out->rle_symbol = 0;
        if (al_out != NULL) *al_out = al;
        return n;
    }
    /* Not a well-formed FSE description. Sequence streams fall back to one
       RLE byte (< 66); literals have no RLE mode (section 7.3), so a failed
       FSE probe is an error for them. */
    if (is_lit) return 0;
    if (buf[pos] >= (uint8_t)ZGEC_NSYM_SEQ) return 0;
    out->mode       = (int)ZGEC_TBL_RLE;
    out->ptr        = NULL;
    out->size       = 0;
    out->rle_symbol = (int)buf[pos];
    if (al_out != NULL) *al_out = -1;
    return 1;
}

size_t zgec_seg_descriptors_walk(zgec_tbl_desc *lit_desc, int n_lit_tables,
                                  zgec_tbl_desc *ll_desc,  int n_ll_tables,
                                  zgec_tbl_desc *ml_desc,
                                  zgec_tbl_desc *of_desc,  int n_of_tables,
                                  const uint8_t *buf, size_t size,
                                  int lit_form, int lit_coder, int k,
                                  int seq_ctx_of, int seq_ctx_ll)
{
    (void)lit_form;
    (void)k;

    size_t pos = 0;

    /* Literal tables are omitted when lit_coder == 0; REPEAT
       literal tables are skipped by passing 0/NULL. Each present
       entry is NEW (AL == 11); literals have no RLE mode. */
    if (lit_coder != 0 && lit_desc != NULL && n_lit_tables > 0) {
        for (int i = 0; i < n_lit_tables; i++) {
            size_t n = zgec_walk_one(&lit_desc[i], buf, size, pos, 1, NULL);
            if (n == 0) return 0;
            if (!zgec_size_add(&pos, n)) return 0;
        }
    }

    /* Sequence tables are omitted when n_seq == 0: the caller passes
       NULL/0 for LL/ML/OF in that case, and likewise for any REPEAT
       stream. Conditioned x3 groups must share one AL. */
    if (ll_desc != NULL && n_ll_tables > 0) {
        int first_al = -2;
        for (int i = 0; i < n_ll_tables; i++) {
            int al = -2;
            size_t n = zgec_walk_one(&ll_desc[i], buf, size, pos, 0, &al);
            if (n == 0) return 0;
            if (!zgec_size_add(&pos, n)) return 0;
            if (n_ll_tables == 3 && seq_ctx_ll && al >= 0) {
                if (i == 0) first_al = al;
                else if (al != first_al) return 0;
            }
        }
    }
    if (ml_desc != NULL) {
        size_t n = zgec_walk_one(ml_desc, buf, size, pos, 0, NULL);
        if (n == 0) return 0;
        if (!zgec_size_add(&pos, n)) return 0;
    }
    if (of_desc != NULL && n_of_tables > 0) {
        int first_al = -2;
        for (int i = 0; i < n_of_tables; i++) {
            int al = -2;
            size_t n = zgec_walk_one(&of_desc[i], buf, size, pos, 0, &al);
            if (n == 0) return 0;
            if (!zgec_size_add(&pos, n)) return 0;
            if (n_of_tables == 3 && seq_ctx_of && al >= 0) {
                if (i == 0) first_al = al;
                else if (al != first_al) return 0;
            }
        }
    }

    return pos;
}
