#include "zgec_decode.h"

#include <stdlib.h>
#include <string.h>

#include "zgec.h"

#include "zgec_internal.h"

/* Threading shim lives in zgec_internal.h (§10.6). */

/*
 * Decoding procedure per spec sections 6, 7, 9, 10 and Annex D,
 * with validation V1..V10.
 *
 * Phase A (per segment, spec order): header + inheritance, LL,
 * run-start bitmap, literals (raw / rANS, residuals kept as Z),
 * ML then OF (ML-first for conditioning), repeat resolution with
 * rep0_before recording, validation.
 *
 * Phase B: scalar-conforming execute (Annex D) over the virtual
 * buffer VB = [DICT Ld][LITREF Ll][OUT].
 */

zgec_err zgec_decompress(const uint8_t *src, size_t src_size,
                          uint8_t **dst, size_t *dst_size)
{
    zgec_decoder *d = zgec_decoder_create(ZGEC_LEVEL_CORE, NULL);
    if (!d) return ZGEC_ERR_NOMEM;
    zgec_err err = zgec_decode_frame(d, src, src_size, dst, dst_size);
    zgec_decoder_destroy(d);
    return err;
}

/* ---- internal types ---- */

typedef struct {
    uint32_t *ll;
    uint32_t *ml;
    uint32_t *of;
    uint32_t *rep0_before; /* rep0 in effect before each sequence (sub) */
    uint32_t rep0_tail;    /* rep0 after the last sequence (tail sub) */
    size_t n_seq;
    size_t n_lit;
    uint8_t *lit;          /* n_lit + slack; residuals when lit_form==1 */
    uint8_t lit_form;
    int ll_cond;           /* bit4 set (for literal-export gating) */
    uint32_t raw_len;
} zgec_segment_arrays;

typedef struct {
    zgec_segment_arrays *segs;
    size_t n_seg;
    uint8_t *vb;           /* Ld + Ll + raw + 64 */
    uint8_t *out;          /* vb + Ld + Ll */
    size_t raw_size;
    size_t ld;
    size_t llref;
} zgec_block_arrays;

static void zgec_block_arrays_free(zgec_block_arrays *a)
{
    if (!a) return;
    if (a->segs) {
        for (size_t i = 0; i < a->n_seg; i++) {
            zgec_free(a->segs[i].ll);
            zgec_free(a->segs[i].ml);
            zgec_free(a->segs[i].of);
            zgec_free(a->segs[i].rep0_before);
            zgec_free(a->segs[i].lit);
        }
        zgec_free(a->segs);
    }
    zgec_free(a->vb);
    zgec_free(a);
}

/* Inherited / built tables for one segment (section 7.4). */
typedef struct {
    zgec_rans_dec_table *lit; /* n_lit_tbl entries (values, not pointers) */
    int n_lit_tbl;
    int lit_form;
    int lit_k;
    int has_lit;
    zgec_fse_dec_table *ll[3];
    int n_ll;
    int ll_rle[3];
    int ll_al;
    zgec_fse_dec_table *ml;
    int ml_rle;
    int ml_al;
    zgec_fse_dec_table *of[3];
    int n_of;
    int of_rle[3];
    int of_al;
    int ctx_of;
    int ctx_ll;
    int has_seq;
} seg_tables;

static void seg_tables_init(seg_tables *t)
{
    memset(t, 0, sizeof(*t));
    for (int i = 0; i < 3; i++) {
        t->ll_rle[i] = -1;
        t->of_rle[i] = -1;
    }
    t->ml_rle = -1;
}

static void seg_tables_free(seg_tables *t)
{
    int i;
    if (!t) return;
    /* Free by pointer, not by count: n_ll/n_of are set only after the
     * whole NEW loop succeeds, so a mid-loop failure would otherwise
     * leak the tables built so far. RLE slots hold NULL, init is zero. */
    for (i = 0; i < 3; i++) {
        if (t->ll[i]) zgec_fse_free_dec(t->ll[i]);
        if (t->of[i]) zgec_fse_free_dec(t->of[i]);
    }
    if (t->ml) zgec_fse_free_dec(t->ml);
    zgec_free(t->lit);
    seg_tables_init(t);
}

static zgec_fse_dec_table *fse_clone(const zgec_fse_dec_table *s)
{
    size_t ds;
    size_t ts;
    zgec_fse_dec_table *t;
    if (!s) return NULL;
    /* Clamp corrupt accuracy logs before the shift/alloc below: without
     * this a corrupt al overflows 1<<al or drives a huge allocation. */
    if (s->al < ZGEC_MIN_AL || s->al > ZGEC_MAX_AL) return NULL;
    if (s->nsym <= 0 || s->nsym > ZGEC_NSYM_LIT) return NULL;
    ds = sizeof(zgec_fse_dec_table) + (size_t)s->nsym * sizeof(int);
    ts = ((size_t)1 << (unsigned)s->al) * sizeof(zgec_fse_dec_entry);
    t = (zgec_fse_dec_table *)zgec_alloc(ds + ts, _Alignof(zgec_fse_dec_table));
    if (!t) return NULL;
    memcpy(t, s, ds + ts);
    t->e = (zgec_fse_dec_entry *)((uint8_t *)t + ds);
    return t;
}

/* ---- decoder state ---- */

#define ZGEC_DEC_MAX_EXT 16u

typedef struct {
    uint16_t id;
    uint8_t *data;
    size_t size;
    int used;
} dec_ext_dict;

struct zgec_decoder {
    zgec_level level;
    zgec_limits limits;
    zgec_dict_cache *dicts;
    zgec_mu cache_mu; /* guards dicts: shared cache stays safe (§10.6) */
    uint8_t *block_buf;
    size_t block_buf_cap;
    /* Sequential literal-reference history: last 3 blocks. */
    uint8_t *hist_lit[3];
    size_t hist_sz[3];
    int hist_ok[3];
    size_t hist_n;
    dec_ext_dict ext[ZGEC_DEC_MAX_EXT];
    int ext_borrowed; /* a block worker aliases its parent's ext[] bytes */
};

zgec_decoder *zgec_decoder_create(zgec_level level, const zgec_limits *limits)
{
    zgec_decoder *d = (zgec_decoder *)zgec_alloc(sizeof(*d), _Alignof(zgec_decoder));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->level = level;
    if (limits) d->limits = *limits;
    zgec_mu_init(&d->cache_mu);
    if (d->cache_mu.ok == 0) { zgec_free(d); return NULL; }
    d->dicts = zgec_dict_cache_create(16);
    if (!d->dicts) { zgec_mu_destroy(&d->cache_mu); zgec_free(d); return NULL; }
    return d;
}

void zgec_decoder_destroy(zgec_decoder *d)
{
    if (!d) return;
    zgec_dict_cache_destroy(d->dicts);
    zgec_mu_destroy(&d->cache_mu);
    zgec_free(d->block_buf);
    for (unsigned i = 0; i < 3u; i++) zgec_free(d->hist_lit[i]);
    if (!d->ext_borrowed) {
        for (unsigned i = 0; i < ZGEC_DEC_MAX_EXT; i++) zgec_free(d->ext[i].data);
    }
    zgec_free(d);
}

zgec_err zgec_decoder_set_threads(zgec_decoder *d, int n_threads)
{
    if (d == NULL || n_threads < 0) return ZGEC_ERR_INVAL;
    d->limits.n_threads = n_threads;
    return ZGEC_OK;
}

/* ---- memory limits (§13) ----
 * Per-worker bound = 2^block_log2 + 2^max_dict_log2 + litref
 * + slack, checked before decoding; actual sizes are checked
 * again at use. Block/dict-cap violations are INVAL,
 * total-limit violations are NOMEM. */

static uint64_t dec_block_cap(uint8_t block_log2)
{
    if (block_log2 < ZGEC_BLOCK_LOG2_MIN || block_log2 > ZGEC_BLOCK_LOG2_MAX)
        return 0u;
    return (uint64_t)1u << block_log2;
}

static uint64_t dec_dict_cap(uint8_t max_dict_log2)
{
    if (max_dict_log2 == 0u) return 0u;
    if (max_dict_log2 > ZGEC_MAX_DICT_LOG2) return 0u;
    return (uint64_t)1u << max_dict_log2;
}

/* Effective caps honouring zgec_limits (0 = frame default). */
static uint64_t dec_eff_max_block(const zgec_decoder *d, uint64_t frame_cap)
{
    uint64_t cfg = (d != NULL) ? (uint64_t)d->limits.max_block_size : 0u;
    return (cfg != 0u) ? cfg : frame_cap;
}

static uint64_t dec_eff_max_dict(const zgec_decoder *d, uint64_t frame_cap)
{
    uint64_t cfg = (d != NULL) ? (uint64_t)d->limits.max_dict_size : 0u;
    return (cfg != 0u) ? cfg : frame_cap;
}

/* Check one block's sizes up front: raw + dict + litref + slack. */
static zgec_err dec_check_worker_limits(const zgec_decoder *d,
                                         uint8_t block_log2,
                                         uint8_t max_dict_log2,
                                         uint64_t expect_raw,
                                         uint64_t dict_sz,
                                         uint64_t litref_len)
{
    uint64_t bcap = dec_block_cap(block_log2);
    uint64_t dcap = dec_dict_cap(max_dict_log2);
    uint64_t bound;
    uint64_t actual;
    uint64_t max_total;
    if (bcap == 0u && block_log2 != 0u) return ZGEC_ERR_INVAL;
    if (max_dict_log2 > ZGEC_MAX_DICT_LOG2) return ZGEC_ERR_INVAL;
    if (expect_raw > dec_eff_max_block(d, bcap)) return ZGEC_ERR_INVAL;
    if (dict_sz > dec_eff_max_dict(d, dcap)) return ZGEC_ERR_INVAL;
    /* Up-front bound with the frame caps (pre-decode, §13). */
    bound = bcap + dcap + litref_len + (uint64_t)ZGEC_OUTPUT_SLACK;
    actual = expect_raw + dict_sz + litref_len + (uint64_t)ZGEC_OUTPUT_SLACK;
    max_total = (d != NULL) ? (uint64_t)d->limits.max_total : 0u;
    if (max_total != 0u && (bound > max_total || actual > max_total))
        return ZGEC_ERR_NOMEM;
    return ZGEC_OK;
}

/* Plain raw-size gate for RAW/RLE records and DICT payloads. */
static zgec_err dec_check_raw_size(const zgec_decoder *d,
                                    uint8_t block_log2, uint64_t raw_size)
{
    uint64_t bcap = dec_block_cap(block_log2);
    if (bcap == 0u && block_log2 != 0u) return ZGEC_ERR_INVAL;
    if (raw_size > dec_eff_max_block(d, bcap)) return ZGEC_ERR_INVAL;
    return ZGEC_OK;
}

static zgec_err dec_check_dict_size(const zgec_decoder *d,
                                     uint8_t max_dict_log2, uint64_t raw_size)
{
    uint64_t dcap = dec_dict_cap(max_dict_log2);
    if (max_dict_log2 > ZGEC_MAX_DICT_LOG2) return ZGEC_ERR_INVAL;
    if (raw_size > dec_eff_max_dict(d, dcap)) return ZGEC_ERR_INVAL;
    return ZGEC_OK;
}

/* Resolve the worker count, capped at the item count and 64. */
static size_t dec_resolve_workers(const zgec_decoder *d, size_t n_items)
{
    long long nt;
    if (n_items <= 1u) return 1u;
    nt = (d != NULL) ? (long long)d->limits.n_threads : 1LL;
    if (nt == 0LL) nt = (long long)zgec_cpu_count();
    if (nt < 1LL) nt = 1LL;
    if (nt > (long long)ZGEC_MAX_WORKERS) nt = (long long)ZGEC_MAX_WORKERS;
    if ((uint64_t)nt > (uint64_t)n_items) nt = (long long)n_items;
    return (size_t)nt;
}

/* Mutex-guarded dictionary-cache access (shared cache, §10.6). */
static const zgec_dict *dec_cache_get(zgec_decoder *d, uint16_t dict_id)
{
    const zgec_dict *hit;
    if (d == NULL) return NULL;
    zgec_mu_lock(&d->cache_mu);
    hit = zgec_dict_cache_get(d->dicts, dict_id);
    zgec_mu_unlock(&d->cache_mu);
    return hit;
}

static void dec_cache_put(zgec_decoder *d, zgec_dict *nd)
{
    if (d == NULL) return;
    zgec_mu_lock(&d->cache_mu);
    zgec_dict_cache_put(d->dicts, nd);
    zgec_mu_unlock(&d->cache_mu);
}

/* ---- segment header parse (sections 7.3/7.4, V6/V7/V8) ----
 * Custom parse: resolves the descriptor layout using the block
 * parameters (k depends on lit_form), so REPEAT/NEW/RLE extents
 * are known before the stream-size varints are read. */

typedef struct {
    uint8_t seg_flags;
    uint8_t tbl_modes;
    uint32_t n_seq;
    uint32_t n_lit;
    uint32_t lit_size;
    uint32_t ll_size;
    uint32_t ml_size;
    uint32_t of_size;
    size_t header_size;
    size_t desc_off;
    size_t desc_len;
    int lit_coder;
    int lit_form;
    int ctx_of;
    int ctx_ll;
    int k;
    int ctx_mode;
    const uint8_t *class_map; /* NULL when k == 1 */
} seg_hdr_t;

/* Peek the accuracy log of one FSE description (1st byte). */
static zgec_err desc_peek_al(const uint8_t *p, size_t rem, int need_lit,
                              int *al_out)
{
    int al;
    if (rem < 1) return ZGEC_ERR_TRUNCATED;
    /* The accuracy log is packed in the low 4 bits as (AL - 5), with the
     * first count bits following in the high nibble (FSE count format,
     * fse.c). Reading the whole byte only coincidentally matched AL = 10
     * when the next bit was zero. */
    al = (int)(p[0] & 0x0Fu) + 5;
    if (need_lit) {
        if (al != ZGEC_LIT_AL) return ZGEC_ERR_FSE_AL;
    } else {
        if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    }
    *al_out = al;
    return ZGEC_OK;
}

/* Measure one NEW FSE description; returns its byte length. */
static zgec_err desc_measure_new(size_t *len_out, const uint8_t *p, size_t rem,
                                  int max_nsym, int need_lit, int *al_out)
{
    int16_t tmp[ZGEC_NSYM_LIT];
    int nsym = 0;
    int al = 0;
    size_t n;
    zgec_err e = desc_peek_al(p, rem, need_lit, &al);
    if (e != ZGEC_OK) return e;
    n = zgec_fse_read_counts(tmp, &nsym, &al, max_nsym, p, rem);
    if (n == 0) return ZGEC_ERR_FSE_COUNTS;
    if (need_lit) {
        if (al != ZGEC_LIT_AL) return ZGEC_ERR_FSE_AL;
    } else {
        if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    }
    *len_out = n;
    *al_out = al;
    return ZGEC_OK;
}

static zgec_err seg_hdr_parse(seg_hdr_t *h, const uint8_t *buf, size_t size,
                               const zgec_block_params *bp)
{
    const zgec_ctx_desc *cd;
    size_t pos = 0;
    size_t n = 0;
    size_t avail = 0;
    size_t dpos = 0;
    int lit_mode;
    int ll_mode;
    int ml_mode;
    int of_mode;
    int n_lit_tables;
    int n_ll;
    int n_of;
    int i;
    int al0 = 0;
    int alx = 0;
    size_t dl = 0;
    zgec_err e;

    if (!h || !buf || !bp) return ZGEC_ERR_INVAL;
    memset(h, 0, sizeof(*h));
    if (size < 2) return ZGEC_ERR_TRUNCATED;
    h->seg_flags = buf[0];
    h->tbl_modes = buf[1];
    if (((unsigned)h->seg_flags >> 5u) & 7u) return ZGEC_ERR_RESERVED; /* V7 */
    h->lit_form = (int)(h->seg_flags & 1u);
    h->lit_coder = (int)(((unsigned)h->seg_flags >> 1u) & 3u);
    if (h->lit_coder != 0 && h->lit_coder != 1) return ZGEC_ERR_LIT_CODER; /* V7 */
    if (h->lit_form != 0 && h->lit_form != 1) return ZGEC_ERR_LIT_FORM; /* V7 */
    h->ctx_of = (int)(((unsigned)h->seg_flags >> 3u) & 1u);
    h->ctx_ll = (int)(((unsigned)h->seg_flags >> 4u) & 1u);
    for (i = 0; i < 4; i++) {
        unsigned m = ((unsigned)h->tbl_modes >> (unsigned)(2 * i)) & 3u;
        if (m > 2u) return ZGEC_ERR_TABLE_MODE; /* V7 */
    }
    pos = 2;
    n = zgec_varint_decode(buf + pos, size - pos, &h->n_seq);
    if (n == 0) return ZGEC_ERR_VARINT; /* V8 */
    pos += n;
    n = zgec_varint_decode(buf + pos, size - pos, &h->n_lit);
    if (n == 0) return ZGEC_ERR_VARINT; /* V8 */
    pos += n;
    if (h->n_seq > ZGEC_MAX_SEQ_PER_SEG) return ZGEC_ERR_SEGMENT_COUNT;

    cd = (h->lit_form == 0) ? &bp->plain : &bp->sub;
    h->k = (int)cd->ctx_count;
    h->ctx_mode = (int)cd->ctx_mode;
    h->class_map = (h->k > 1) ? cd->class_map : NULL;
    /* Defensive V7: k defined, mode defined, map entries below k. */
    if (h->k != 1 && h->k != 2 && h->k != 4 && h->k != 8)
        return ZGEC_ERR_CTX_COUNT;
    if (h->ctx_mode < ZGEC_CTX_NONE || h->ctx_mode > ZGEC_CTX_SIGNED)
        return ZGEC_ERR_CTX_MODE;
    if (h->k == 1) {
        h->class_map = NULL;
    } else {
        int ci;
        if (h->class_map == NULL) return ZGEC_ERR_INTERNAL;
        for (ci = 0; ci < 64; ci++) {
            if (h->class_map[ci] >= (uint8_t)h->k) return ZGEC_ERR_CLASS_MAP;
        }
    }

    lit_mode = (int)(((unsigned)h->tbl_modes >> 0u) & 3u);
    ll_mode = (int)(((unsigned)h->tbl_modes >> 2u) & 3u);
    ml_mode = (int)(((unsigned)h->tbl_modes >> 4u) & 3u);
    of_mode = (int)(((unsigned)h->tbl_modes >> 6u) & 3u);

    /* Walk descriptors to find their total length (V6/V8). */
    avail = (size > pos) ? size - pos : 0;
    dpos = 0;
    if (h->lit_coder != 0) {
        n_lit_tables = (h->k <= 1) ? 1 : (h->k + 1);
        if (lit_mode == ZGEC_TBL_REPEAT) {
            /* nothing */
        } else if (lit_mode == ZGEC_TBL_RLE) {
            return ZGEC_ERR_TABLE_MODE; /* no RLE literal tables */
        } else {
            for (i = 0; i < n_lit_tables; i++) {
                e = desc_measure_new(&dl, buf + pos + dpos, avail - dpos,
                                     ZGEC_NSYM_LIT, 1, &al0);
                if (e != ZGEC_OK) return e;
                dpos += dl;
                if (dpos > avail) return ZGEC_ERR_TRUNCATED;
            }
        }
    }
    if (h->n_seq > 0) {
        n_ll = h->ctx_ll ? 3 : 1;
        n_of = h->ctx_of ? 3 : 1;
        if (ll_mode == ZGEC_TBL_RLE) {
            if (dpos + 1 > avail) return ZGEC_ERR_TRUNCATED;
            if (buf[pos + dpos] >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
            dpos += 1;
        } else if (ll_mode == ZGEC_TBL_NEW) {
            al0 = 0;
            for (i = 0; i < n_ll; i++) {
                e = desc_measure_new(&dl, buf + pos + dpos, avail - dpos,
                                     ZGEC_NSYM_SEQ, 0, &alx);
                if (e != ZGEC_OK) return e;
                if (i == 0) al0 = alx;
                else if (alx != al0) return ZGEC_ERR_FSE_AL; /* same AL */
                dpos += dl;
                if (dpos > avail) return ZGEC_ERR_TRUNCATED;
            }
        }
        if (ml_mode == ZGEC_TBL_RLE) {
            if (dpos + 1 > avail) return ZGEC_ERR_TRUNCATED;
            if (buf[pos + dpos] >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
            dpos += 1;
        } else if (ml_mode == ZGEC_TBL_NEW) {
            e = desc_measure_new(&dl, buf + pos + dpos, avail - dpos,
                                 ZGEC_NSYM_SEQ, 0, &al0);
            if (e != ZGEC_OK) return e;
            dpos += dl;
            if (dpos > avail) return ZGEC_ERR_TRUNCATED;
        }
        if (of_mode == ZGEC_TBL_RLE) {
            if (dpos + 1 > avail) return ZGEC_ERR_TRUNCATED;
            if (buf[pos + dpos] >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
            dpos += 1;
        } else if (of_mode == ZGEC_TBL_NEW) {
            al0 = 0;
            for (i = 0; i < n_of; i++) {
                e = desc_measure_new(&dl, buf + pos + dpos, avail - dpos,
                                     ZGEC_NSYM_SEQ, 0, &alx);
                if (e != ZGEC_OK) return e;
                if (i == 0) al0 = alx;
                else if (alx != al0) return ZGEC_ERR_FSE_AL; /* same AL */
                dpos += dl;
                if (dpos > avail) return ZGEC_ERR_TRUNCATED;
            }
        }
    }
    h->desc_off = pos;
    h->desc_len = dpos;
    pos += dpos;

    n = zgec_varint_decode(buf + pos, size - pos, &h->lit_size);
    if (n == 0) return ZGEC_ERR_VARINT; /* V8 */
    pos += n;
    n = zgec_varint_decode(buf + pos, size - pos, &h->ll_size);
    if (n == 0) return ZGEC_ERR_VARINT;
    pos += n;
    n = zgec_varint_decode(buf + pos, size - pos, &h->ml_size);
    if (n == 0) return ZGEC_ERR_VARINT;
    pos += n;
    n = zgec_varint_decode(buf + pos, size - pos, &h->of_size);
    if (n == 0) return ZGEC_ERR_VARINT;
    pos += n;
    h->header_size = pos;

    /* Early V8: no stream may claim more than the segment itself holds.
     * Exact tiling is re-checked by the caller; this fails fast on
     * absurd sizes without changing the tiling error below. */
    if ((uint64_t)h->lit_size > (uint64_t)size ||
        (uint64_t)h->ll_size > (uint64_t)size ||
        (uint64_t)h->ml_size > (uint64_t)size ||
        (uint64_t)h->of_size > (uint64_t)size)
        return ZGEC_ERR_STREAM_SIZE;

    if (h->n_seq == 0) {
        if (h->ll_size != 0 || h->ml_size != 0 || h->of_size != 0)
            return ZGEC_ERR_STREAM_SIZE; /* V8 */
    }
    if (h->n_lit == 0 && h->lit_size != 0) return ZGEC_ERR_STREAM_SIZE; /* V8 */
    if (h->lit_coder == 0 && h->lit_size != h->n_lit) return ZGEC_ERR_STREAM_SIZE;
    return ZGEC_OK;
}

/* Build one NEW FSE table from the descriptor at *pp (advances it). */
static zgec_err build_new_fse(zgec_fse_dec_table **out, const uint8_t **pp,
                               size_t *rem, int max_nsym, int need_lit)
{
    int16_t counts[ZGEC_NSYM_LIT];
    int nsym = 0;
    int al = 0;
    size_t n;
    zgec_err e;
    /* Literal tables always go through build_new_rans; the old need_lit
     * path built a rANS table only to discard it, so reject it outright. */
    if (need_lit) return ZGEC_ERR_TABLE_MODE;
    e = desc_peek_al(*pp, *rem, 0, &al);
    if (e != ZGEC_OK) return e;
    n = zgec_fse_read_counts(counts, &nsym, &al, max_nsym, *pp, *rem);
    if (n == 0) return ZGEC_ERR_FSE_COUNTS; /* V6 */
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL; /* V6 */
    e = zgec_fse_build_dec(out, counts, ZGEC_NSYM_SEQ, al); /* V6 sum check */
    if (e != ZGEC_OK) return e;
    *pp += n;
    *rem -= n;
    return ZGEC_OK;
}

/* Build one NEW rANS literal table. */
static zgec_err build_new_rans(zgec_rans_dec_table *out, const uint8_t **pp,
                                size_t *rem)
{
    int16_t counts[ZGEC_NSYM_LIT];
    int nsym = 0;
    int al = 0;
    size_t n;
    zgec_err e = desc_peek_al(*pp, *rem, 1, &al);
    if (e != ZGEC_OK) return e;
    n = zgec_fse_read_counts(counts, &nsym, &al, ZGEC_NSYM_LIT, *pp, *rem);
    if (n == 0) return ZGEC_ERR_FSE_COUNTS; /* V6 */
    if (al != ZGEC_LIT_AL) return ZGEC_ERR_FSE_AL; /* V6 */
    e = zgec_rans_build_dec(out, counts);
    if (e != ZGEC_OK) return e;
    *pp += n;
    *rem -= n;
    return ZGEC_OK;
}

/* Build the table set for a segment; REPEAT clones the previous one. */
static zgec_err seg_tables_build(seg_tables *cur, const seg_hdr_t *h,
                                  const uint8_t *seg_buf, size_t seg_size,
                                  const seg_tables *prev, int is_first)
{
    const uint8_t *p;
    size_t rem;
    int lit_mode;
    int ll_mode;
    int ml_mode;
    int of_mode;
    int n_lit_tables;
    int n_ll;
    int n_of;
    int i;
    zgec_err e;

    if (cur == NULL || h == NULL || seg_buf == NULL) return ZGEC_ERR_INVAL;
    if (h->desc_len > seg_size) return ZGEC_ERR_TRUNCATED;
    if (h->desc_off > seg_size - h->desc_len) return ZGEC_ERR_TRUNCATED;
    if (!is_first && prev == NULL) return ZGEC_ERR_TABLE_INHERIT;
    seg_tables_init(cur);
    p = seg_buf + h->desc_off;
    rem = h->desc_len;
    lit_mode = (int)(((unsigned)h->tbl_modes >> 0u) & 3u);
    ll_mode = (int)(((unsigned)h->tbl_modes >> 2u) & 3u);
    ml_mode = (int)(((unsigned)h->tbl_modes >> 4u) & 3u);
    of_mode = (int)(((unsigned)h->tbl_modes >> 6u) & 3u);

    /* Literal tables (omitted when lit_coder == 0). */
    if (h->lit_coder != 0) {
        n_lit_tables = (h->k <= 1) ? 1 : (h->k + 1);
        if (lit_mode == ZGEC_TBL_REPEAT) {
            if (is_first || !prev->has_lit) return ZGEC_ERR_TABLE_INHERIT;
            if (prev->lit_form != h->lit_form || prev->lit_k != h->k)
                return ZGEC_ERR_TABLE_INHERIT;
            if (prev->n_lit_tbl != n_lit_tables) return ZGEC_ERR_TABLE_INHERIT;
            cur->lit = (zgec_rans_dec_table *)zgec_alloc(
                (size_t)n_lit_tables * sizeof(zgec_rans_dec_table), 64);
            if (!cur->lit) return ZGEC_ERR_NOMEM;
            memcpy(cur->lit, prev->lit,
                   (size_t)n_lit_tables * sizeof(zgec_rans_dec_table));
            cur->n_lit_tbl = n_lit_tables;
        } else if (lit_mode == ZGEC_TBL_RLE) {
            return ZGEC_ERR_TABLE_MODE;
        } else {
            cur->lit = (zgec_rans_dec_table *)zgec_alloc(
                (size_t)n_lit_tables * sizeof(zgec_rans_dec_table), 64);
            if (!cur->lit) return ZGEC_ERR_NOMEM;
            cur->n_lit_tbl = n_lit_tables;
            for (i = 0; i < n_lit_tables; i++) {
                e = build_new_rans(&cur->lit[i], &p, &rem);
                if (e != ZGEC_OK) return e;
            }
        }
        cur->lit_form = h->lit_form;
        cur->lit_k = h->k;
        cur->has_lit = 1;
    }

    cur->ctx_of = h->ctx_of;
    cur->ctx_ll = h->ctx_ll;

    if (h->n_seq == 0) return ZGEC_OK;

    /* LL tables. */
    n_ll = h->ctx_ll ? 3 : 1;
    if (ll_mode == ZGEC_TBL_REPEAT) {
        if (is_first || !prev->has_seq) return ZGEC_ERR_TABLE_INHERIT;
        if (prev->ctx_ll != h->ctx_ll)
            return ZGEC_ERR_TABLE_INHERIT;
        if (prev->n_ll != n_ll) return ZGEC_ERR_TABLE_INHERIT;
        for (i = 0; i < n_ll; i++) {
            if (prev->ll_rle[i] >= 0) {
                cur->ll_rle[i] = prev->ll_rle[i];
                cur->ll[i] = NULL;
            } else {
                cur->ll[i] = fse_clone(prev->ll[i]);
                if (!cur->ll[i]) return ZGEC_ERR_NOMEM;
                cur->ll_rle[i] = -1;
            }
        }
        cur->n_ll = n_ll;
        cur->ll_al = prev->ll_al;
    } else if (ll_mode == ZGEC_TBL_RLE) {
        int code;
        if (rem < 1) return ZGEC_ERR_TRUNCATED;
        code = (int)*p;
        p += 1;
        rem -= 1;
        if (code >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
        for (i = 0; i < n_ll; i++) {
            cur->ll_rle[i] = code;
            cur->ll[i] = NULL;
        }
        cur->n_ll = n_ll;
        cur->ll_al = 0;
    } else {
        int al0 = 0;
        for (i = 0; i < n_ll; i++) {
            int alx = 0;
            e = build_new_fse(&cur->ll[i], &p, &rem, ZGEC_NSYM_SEQ, 0);
            if (e != ZGEC_OK) return e;
            alx = cur->ll[i]->al;
            if (i == 0) al0 = alx;
            else if (alx != al0) return ZGEC_ERR_FSE_AL; /* V6 same AL */
            cur->ll_rle[i] = -1;
        }
        cur->n_ll = n_ll;
        cur->ll_al = al0;
    }

    /* ML table (always single). */
    if (ml_mode == ZGEC_TBL_REPEAT) {
        if (is_first || !prev->has_seq) return ZGEC_ERR_TABLE_INHERIT;
        if (prev->ml_rle >= 0) {
            cur->ml_rle = prev->ml_rle;
            cur->ml = NULL;
        } else {
            cur->ml = fse_clone(prev->ml);
            if (!cur->ml) return ZGEC_ERR_NOMEM;
            cur->ml_rle = -1;
        }
        cur->ml_al = prev->ml_al;
    } else if (ml_mode == ZGEC_TBL_RLE) {
        int code;
        if (rem < 1) return ZGEC_ERR_TRUNCATED;
        code = (int)*p;
        p += 1;
        rem -= 1;
        if (code >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
        cur->ml_rle = code;
        cur->ml = NULL;
        cur->ml_al = 0;
    } else {
        cur->ml = NULL;
        e = build_new_fse(&cur->ml, &p, &rem, ZGEC_NSYM_SEQ, 0);
        if (e != ZGEC_OK) return e;
        cur->ml_rle = -1;
        cur->ml_al = cur->ml->al;
    }

    /* OF tables. */
    n_of = h->ctx_of ? 3 : 1;
    if (of_mode == ZGEC_TBL_REPEAT) {
        if (is_first || !prev->has_seq) return ZGEC_ERR_TABLE_INHERIT;
        if (prev->ctx_of != h->ctx_of)
            return ZGEC_ERR_TABLE_INHERIT;
        if (prev->n_of != n_of) return ZGEC_ERR_TABLE_INHERIT;
        for (i = 0; i < n_of; i++) {
            if (prev->of_rle[i] >= 0) {
                cur->of_rle[i] = prev->of_rle[i];
                cur->of[i] = NULL;
            } else {
                cur->of[i] = fse_clone(prev->of[i]);
                if (!cur->of[i]) return ZGEC_ERR_NOMEM;
                cur->of_rle[i] = -1;
            }
        }
        cur->n_of = n_of;
        cur->of_al = prev->of_al;
    } else if (of_mode == ZGEC_TBL_RLE) {
        int code;
        if (rem < 1) return ZGEC_ERR_TRUNCATED;
        code = (int)*p;
        p += 1;
        rem -= 1;
        if (code >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL; /* V6 */
        for (i = 0; i < n_of; i++) {
            cur->of_rle[i] = code;
            cur->of[i] = NULL;
        }
        cur->n_of = n_of;
        cur->of_al = 0;
    } else {
        int al0 = 0;
        for (i = 0; i < n_of; i++) {
            int alx = 0;
            e = build_new_fse(&cur->of[i], &p, &rem, ZGEC_NSYM_SEQ, 0);
            if (e != ZGEC_OK) return e;
            alx = cur->of[i]->al;
            if (i == 0) al0 = alx;
            else if (alx != al0) return ZGEC_ERR_FSE_AL; /* V6 same AL */
            cur->of_rle[i] = -1;
        }
        cur->n_of = n_of;
        cur->of_al = al0;
    }
    cur->has_seq = 1;
    if (rem != 0) return ZGEC_ERR_TRUNCATED; /* V8: exact descriptor use */
    return ZGEC_OK;
}

/* ---- sequence stream helpers (sections 8.4/8.5, V5) ---- */

static zgec_err check_stream_sentinel(const uint8_t *s, size_t n)
{
    /* Shared V5 sentinel check (see zgec_internal.h). */
    return zgec_check_stream_sentinel(s, n);
}

/* Single-table (or RLE) stream decode with V5 checks. */
static zgec_err seq_decode_one(uint32_t *out, size_t n,
                                zgec_fse_dec_table *t, int rle,
                                const uint8_t *stream, size_t ssize)
{
    zgec_br br;
    zgec_err e = check_stream_sentinel(stream, ssize);
    if (e != ZGEC_OK) return e;
    zgec_br_init(&br, stream, ssize);
    if (br.overflow) return ZGEC_ERR_BITSTREAM_SENTINEL;
    if (rle >= 0)
        return zgec_seq_stream_decode(out, n, NULL, rle, &br,
                                      zgec_seq_base, zgec_seq_nbits);
    if (!t) return ZGEC_ERR_TABLE_MODE;
    return zgec_seq_stream_decode(out, n, t, -1, &br,
                                  zgec_seq_base, zgec_seq_nbits);
}

/* Three-table conditioned stream decode (section 8.6).
 * Tables share one AL; class = mlclass(ML) (bit3) or
 * mlclass(prev ML), class 0 for i == 0 (bit4). ML-first. */
static zgec_err seq_decode_cond(uint32_t *out, size_t n,
                                 zgec_fse_dec_table *const t[3],
                                 const uint32_t *ml, int use_prev,
                                 const uint8_t *stream, size_t ssize)
{
    zgec_br br;
    uint32_t state = 0;
    unsigned S = 0;
    int al;
    size_t i;
    zgec_err e = check_stream_sentinel(stream, ssize);
    if (e != ZGEC_OK) return e;
    if (!t[0] || !t[1] || !t[2]) return ZGEC_ERR_TABLE_MODE;
    al = t[0]->al;
    if (t[1]->al != al || t[2]->al != al) return ZGEC_ERR_FSE_AL; /* V6 */
    zgec_br_init(&br, stream, ssize);
    if (br.overflow) return ZGEC_ERR_BITSTREAM_SENTINEL;
    S = (unsigned)1 << (unsigned)al;
    state = zgec_br_read(&br, (unsigned)al);
    if (br.overflow) return ZGEC_ERR_BITSTREAM;
    if (state >= S) return ZGEC_ERR_BITSTREAM;
    for (i = 0; i < n; i++) {
        unsigned cls;
        const zgec_fse_dec_entry *en;
        int sym;
        uint32_t extra = 0;
        uint32_t bits = 0;
        int32_t next = 0;
        if (use_prev)
            cls = (i == 0) ? 0u : zgec_mlclass(ml[i - 1]);
        else
            cls = zgec_mlclass(ml[i]);
        if (cls > 2u) return ZGEC_ERR_INTERNAL;
        en = &t[cls]->e[state];
        sym = (int)en->symbol;
        if (sym < 0 || sym >= ZGEC_NSYM_SEQ) return ZGEC_ERR_FSE_SYMBOL;
        if (zgec_seq_nbits[sym] > 0) {
            extra = zgec_br_read(&br, (unsigned)zgec_seq_nbits[sym]);
            if (br.overflow) return ZGEC_ERR_BITSTREAM;
        }
        out[i] = zgec_seq_base[sym] + extra;
        if (i + 1 < n) {
            bits = zgec_br_read(&br, (unsigned)en->nb_bits);
            if (br.overflow) return ZGEC_ERR_BITSTREAM;
            next = en->baseline + (int32_t)bits;
            if (next < 0 || (unsigned)next >= S) return ZGEC_ERR_BITSTREAM;
            state = (unsigned)next;
        }
    }
    if (!zgec_br_done(&br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED; /* V5 */
    return ZGEC_OK;
}

/* ---- Phase A: decode one segment (section 10.2) ---- */

/* Centralised n_lit + slack guard: n_lit is a u32 (<= 0xFFFFFFFF) and
 * vb_total is capped at 0xFFFFFFFF, which keeps n_lit + ZGEC_LIT_SLACK
 * in-bounds on 64-bit; on 32-bit SIZE_MAX == 0xFFFFFFFF so the addition
 * itself could wrap. Every n_lit + SLACK allocation goes through this. */
static int lit_buf_fits(size_t n_lit)
{
    return n_lit <= SIZE_MAX - (size_t)ZGEC_LIT_SLACK;
}

/* Streams + validation for one segment. The header is already parsed
 * (*hp) and the tables already built (*cur, serial pre-pass); only
 * *seg is written while *hp and *cur stay read-only, so concurrent calls
 * on different segments are safe. Keeps the §10.2 order: header
 * (done by the caller) + inherit (done by the caller), LL,
 * runstart, literals, ML-then-OF, reps+rep0_before, validate. */
static zgec_err decode_segment_streams(zgec_segment_arrays *seg,
                                const uint8_t *seg_buf, size_t seg_size,
                                const seg_hdr_t *hp,
                                seg_tables *cur, size_t Ld, size_t Ll,
                                size_t seg_out_start, uint32_t dir_raw_len)
{
    seg_hdr_t h;
    zgec_err e;
    uint64_t streams_total;
    const uint8_t *lit_stream;
    const uint8_t *ll_stream;
    const uint8_t *ml_stream;
    const uint8_t *of_stream;
    uint64_t sum_ll = 0;
    uint64_t sum_ml = 0;
    uint64_t raw_total = 0;
    uint64_t tail = 0;
    size_t i = 0;
    uint8_t *runstart = NULL;
    int need_rs = 0;
    uint32_t rep0 = 1;
    uint32_t rep1 = 4;
    uint32_t rep2 = 8;

    if (seg == NULL || seg_buf == NULL || hp == NULL || cur == NULL)
        return ZGEC_ERR_INVAL;
    h = *hp; /* shallow copy; class_map stays borrowed, read-only */
    if (h.k > 1 && h.class_map == NULL) return ZGEC_ERR_INTERNAL;
    need_rs = (h.k > 1 && h.lit_coder != 0);
    memset(seg, 0, sizeof(*seg));

    /* V8: streams must tile the segment exactly. */
    streams_total = (uint64_t)h.header_size + (uint64_t)h.lit_size +
                    (uint64_t)h.ll_size + (uint64_t)h.ml_size +
                    (uint64_t)h.of_size;
    if (streams_total != (uint64_t)seg_size) return ZGEC_ERR_SEGMENT_SIZE;
    if ((uint64_t)h.n_lit > (uint64_t)dir_raw_len) return ZGEC_ERR_RAW_LEN;

    seg->n_seq = (size_t)h.n_seq;
    seg->n_lit = (size_t)h.n_lit;
    seg->lit_form = (uint8_t)h.lit_form;
    seg->ll_cond = h.ctx_ll;

    if (seg->n_seq > 0) {
        seg->ll = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->ml = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->of = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->rep0_before =
            (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        if (!seg->ll || !seg->ml || !seg->of || !seg->rep0_before) {
            zgec_free(seg->ll);
            zgec_free(seg->ml);
            zgec_free(seg->of);
            zgec_free(seg->rep0_before);
            seg->ll = seg->ml = seg->of = seg->rep0_before = NULL;
            return ZGEC_ERR_NOMEM;
        }
    }
    if (seg->n_lit > 0) {
        if (!lit_buf_fits(seg->n_lit)) {
            zgec_free(seg->ll);
            zgec_free(seg->ml);
            zgec_free(seg->of);
            zgec_free(seg->rep0_before);
            seg->ll = seg->ml = seg->of = seg->rep0_before = NULL;
            return ZGEC_ERR_NOMEM;
        }
        seg->lit = (uint8_t *)zgec_alloc(seg->n_lit + ZGEC_LIT_SLACK, 64);
        if (!seg->lit) {
            zgec_free(seg->ll);
            zgec_free(seg->ml);
            zgec_free(seg->of);
            zgec_free(seg->rep0_before);
            seg->ll = seg->ml = seg->of = seg->rep0_before = NULL;
            return ZGEC_ERR_NOMEM;
        }
        memset(seg->lit + seg->n_lit, 0, ZGEC_LIT_SLACK);
    }

    /* Guarded stream slicing: every addition is proven in-bounds
     * (header + streams == seg_size was checked above), so no
     * pointer arithmetic runs past the segment. */
    if (h.header_size > seg_size) return ZGEC_ERR_TRUNCATED;
    if ((size_t)h.lit_size > seg_size - h.header_size)
        return ZGEC_ERR_TRUNCATED;
    if ((size_t)h.ll_size > seg_size - h.header_size - (size_t)h.lit_size)
        return ZGEC_ERR_TRUNCATED;
    if ((size_t)h.ml_size > seg_size - h.header_size - (size_t)h.lit_size -
            (size_t)h.ll_size)
        return ZGEC_ERR_TRUNCATED;
    if ((size_t)h.of_size != seg_size - h.header_size - (size_t)h.lit_size -
            (size_t)h.ll_size - (size_t)h.ml_size)
        return ZGEC_ERR_SEGMENT_SIZE;
    lit_stream = seg_buf + h.header_size;
    ll_stream = lit_stream + (size_t)h.lit_size;
    ml_stream = ll_stream + (size_t)h.ll_size;
    of_stream = ml_stream + (size_t)h.ml_size;

    if (h.ctx_ll || h.ctx_of) {
        /* Conditioned: ML first, then OF/LL with mlclass (8.6/10.2). */
        if (seg->n_seq > 0) {
            e = seq_decode_one(seg->ml, seg->n_seq, cur->ml, cur->ml_rle,
                               ml_stream, (size_t)h.ml_size);
            if (e != ZGEC_OK) return e;
            for (i = 0; i < seg->n_seq; i++) {
                uint64_t m = (uint64_t)seg->ml[i] + 3u;
                if (m < 3u || m > 0xFFFFFFFFu) return ZGEC_ERR_MATCH_LENGTH; /* V3 */
                seg->ml[i] = (uint32_t)m;
            }
            if (h.ctx_of) {
                if (cur->n_of == 3 && cur->of[0] && cur->of[1] && cur->of[2] &&
                    cur->of_rle[0] < 0) {
                    zgec_fse_dec_table *const t[3] = {
                        cur->of[0], cur->of[1], cur->of[2]
                    };
                    e = seq_decode_cond(seg->of, seg->n_seq, t, seg->ml, 0,
                                        of_stream, (size_t)h.of_size);
                } else {
                    e = seq_decode_one(seg->of, seg->n_seq, cur->of[0],
                                       cur->of_rle[0], of_stream,
                                       (size_t)h.of_size);
                }
            } else {
                e = seq_decode_one(seg->of, seg->n_seq,
                                   (cur->n_of > 0) ? cur->of[0] : NULL,
                                   (cur->n_of > 0) ? cur->of_rle[0] : -1,
                                   of_stream, (size_t)h.of_size);
            }
            if (e != ZGEC_OK) return e;
            for (i = 0; i < seg->n_seq; i++) {
                uint64_t ob = (uint64_t)seg->of[i] + 1u;
                if (ob < 1u || ob > 0xFFFFFFFFu) return ZGEC_ERR_OFFSET;
                seg->of[i] = (uint32_t)ob;
            }
            if (h.ctx_ll) {
                if (cur->n_ll == 3 && cur->ll[0] && cur->ll[1] && cur->ll[2] &&
                    cur->ll_rle[0] < 0) {
                    zgec_fse_dec_table *const t[3] = {
                        cur->ll[0], cur->ll[1], cur->ll[2]
                    };
                    e = seq_decode_cond(seg->ll, seg->n_seq, t, seg->ml, 1,
                                        ll_stream, (size_t)h.ll_size);
                } else {
                    e = seq_decode_one(seg->ll, seg->n_seq, cur->ll[0],
                                       cur->ll_rle[0], ll_stream,
                                       (size_t)h.ll_size);
                }
            } else {
                e = seq_decode_one(seg->ll, seg->n_seq, cur->ll[0],
                                   cur->ll_rle[0], ll_stream,
                                   (size_t)h.ll_size);
            }
            if (e != ZGEC_OK) return e;
        }
    } else {
        /* Unconditioned spec order: LL, literals, ML, OF. */
        if (seg->n_seq > 0) {
            e = seq_decode_one(seg->ll, seg->n_seq, cur->ll[0],
                               cur->ll_rle[0], ll_stream, (size_t)h.ll_size);
            if (e != ZGEC_OK) return e;
        }
    }

    /* Prefix sums of LL + run-start bitmap (9.3), incl. tail bit.
     * runstart is non-NULL exactly when the rANS context path needs it
     * (k > 1 with a coded literal stream); for k <= 1 or raw literals
     * rans_decode takes NULL and the bitmap would only burn memory
     * traffic, so the allocation (and its writes) are skipped there.
     * Every index remains range-checked before use. */
    if (need_rs && seg->n_lit > 0) {
        runstart = (uint8_t *)zgec_alloc(seg->n_lit, 1);
        if (!runstart) return ZGEC_ERR_NOMEM;
        memset(runstart, 0, seg->n_lit);
    }
    if (seg->n_seq > 0 && seg->ll == NULL) {
        zgec_free(runstart);
        return ZGEC_ERR_INTERNAL;
    }
    for (i = 0; i < seg->n_seq; i++) {
        sum_ll += (uint64_t)seg->ll[i];
        if (sum_ll > (uint64_t)seg->n_lit) { /* V1 */
            zgec_free(runstart);
            return ZGEC_ERR_LL_SUM;
        }
        if (seg->ll[i] > 0) {
            size_t start = (size_t)sum_ll - (size_t)seg->ll[i];
            if (start >= seg->n_lit) {
                zgec_free(runstart);
                return ZGEC_ERR_LL_SUM;
            }
            if (need_rs) {
                if (runstart == NULL) {
                    zgec_free(runstart);
                    return ZGEC_ERR_INTERNAL;
                }
                runstart[start] = 1;
            }
        }
    }
    if (sum_ll > (uint64_t)seg->n_lit) {
        zgec_free(runstart);
        return ZGEC_ERR_LL_SUM; /* V1 */
    }
    tail = (uint64_t)seg->n_lit - sum_ll;
    if (tail > 0) {
        if ((uint64_t)sum_ll >= (uint64_t)seg->n_lit) {
            zgec_free(runstart);
            return ZGEC_ERR_LL_SUM;
        }
        if (need_rs) {
            if (runstart == NULL) {
                zgec_free(runstart);
                return ZGEC_ERR_LL_SUM;
            }
            runstart[(size_t)sum_ll] = 1; /* tail run start (9.3) */
        }
    }

    /* Literal stream (section 9): raw memcpy or rANS; sub stays as Z. */
    if (seg->n_lit > 0) {
        if (seg->lit == NULL) {
            zgec_free(runstart);
            return ZGEC_ERR_INTERNAL;
        }
        if (h.lit_coder == 0) {
            memcpy(seg->lit, lit_stream, seg->n_lit);
        } else {
            const zgec_rans_dec_table *tabs = cur->lit;
            const uint8_t *cmap = h.class_map;
            if (tabs == NULL) {
                zgec_free(runstart);
                return ZGEC_ERR_TABLE_MODE;
            }
            if (h.k <= 1) {
                if (cur->n_lit_tbl < 1) {
                    zgec_free(runstart);
                    return ZGEC_ERR_TABLE_MODE;
                }
                e = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                     (size_t)h.lit_size, tabs, 1,
                                     h.ctx_mode, NULL, NULL);
            } else {
                if (cmap == NULL || runstart == NULL) {
                    zgec_free(runstart);
                    return ZGEC_ERR_INTERNAL;
                }
                if (cur->n_lit_tbl != h.k + 1) {
                    zgec_free(runstart);
                    return ZGEC_ERR_TABLE_MODE;
                }
                e = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                     (size_t)h.lit_size, tabs, h.k,
                                     h.ctx_mode, cmap, runstart);
            }
            if (e != ZGEC_OK) {
                zgec_free(runstart);
                return e; /* V5 inside */
            }
        }
    }
    zgec_free(runstart);
    runstart = NULL;

    /* ML then OF for the unconditioned path (ML-first). */
    if (!h.ctx_ll && !h.ctx_of && seg->n_seq > 0) {
        e = seq_decode_one(seg->ml, seg->n_seq, cur->ml, cur->ml_rle,
                           ml_stream, (size_t)h.ml_size);
        if (e != ZGEC_OK) return e;
        for (i = 0; i < seg->n_seq; i++) {
            uint64_t m = (uint64_t)seg->ml[i] + 3u;
            if (m < 3u || m > 0xFFFFFFFFu) return ZGEC_ERR_MATCH_LENGTH; /* V3 */
            seg->ml[i] = (uint32_t)m;
        }
        e = seq_decode_one(seg->of, seg->n_seq,
                           (cur->n_of > 0) ? cur->of[0] : NULL,
                           (cur->n_of > 0) ? cur->of_rle[0] : -1,
                           of_stream, (size_t)h.of_size);
        if (e != ZGEC_OK) return e;
        for (i = 0; i < seg->n_seq; i++) {
            uint64_t ob = (uint64_t)seg->of[i] + 1u;
            if (ob < 1u || ob > 0xFFFFFFFFu) return ZGEC_ERR_OFFSET;
            seg->of[i] = (uint32_t)ob;
        }
    }

    /* Resolve repeat offsets from (1,4,8); record rep0_before (8.2). */
    for (i = 0; i < seg->n_seq; i++) {
        uint32_t ob = seg->of[i];
        uint32_t dd;
        seg->rep0_before[i] = rep0;
        if (ob == 1) {
            dd = rep0;
        } else if (ob == 2) {
            dd = rep1;
            rep1 = rep0;
            rep0 = dd;
        } else if (ob == 3) {
            dd = rep2;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = dd;
        } else {
            if (ob < 4) return ZGEC_ERR_OFFSET;
            dd = ob - 3u;
            if (dd < 1 || dd > 0xFFFFFFFCu) return ZGEC_ERR_OFFSET;
            rep2 = rep1;
            rep1 = rep0;
            rep0 = dd;
        }
        if (dd < 1) return ZGEC_ERR_OFFSET; /* V3 */
        seg->of[i] = dd;
    }
    seg->rep0_tail = (seg->n_seq > 0) ? rep0 : 1u;

    /* V1: raw_len == n_lit + sum ML (u64, then 32-bit fit). */
    for (i = 0; i < seg->n_seq; i++) sum_ml += (uint64_t)seg->ml[i];
    raw_total = (uint64_t)seg->n_lit + sum_ml;
    if (raw_total > 0xFFFFFFFFu) return ZGEC_ERR_RAW_LEN;
    if ((uint32_t)raw_total != dir_raw_len) return ZGEC_ERR_RAW_LEN;
    seg->raw_len = (uint32_t)raw_total;

    /* V2/V3: offsets within Ld + Ll + p, ML >= 3, off >= 1.
     * Checked in ~1024-sequence batches (vectorisable pass). */
    {
        uint64_t pos = (uint64_t)seg_out_start;
        size_t done = 0;
        while (done < seg->n_seq) {
            size_t chunk = seg->n_seq - done;
            size_t k2;
            if (chunk > 1024) chunk = 1024;
            for (k2 = 0; k2 < chunk; k2++) {
                size_t j = done + k2;
                uint64_t mpos;
                if (seg->ml[j] < 3) return ZGEC_ERR_MATCH_LENGTH; /* V3 */
                if (seg->of[j] < 1) return ZGEC_ERR_OFFSET; /* V3 */
                mpos = pos + (uint64_t)seg->ll[j]; /* first match byte */
                if ((uint64_t)seg->of[j] > (uint64_t)Ld + (uint64_t)Ll + mpos)
                    return ZGEC_ERR_OFFSET; /* V2 */
                pos += (uint64_t)seg->ll[j] + (uint64_t)seg->ml[j];
            }
            done += chunk;
        }
        if (pos + tail != (uint64_t)seg_out_start + (uint64_t)seg->raw_len)
            return ZGEC_ERR_RAW_LEN;
    }
    return ZGEC_OK;
}

/* Serial Phase-A for one segment: parse header, build tables
 * (resolving inheritance), then decode streams. The n_threads==1
 * path uses exactly this, as before. */
static zgec_err decode_segment(zgec_segment_arrays *seg,
                                const uint8_t *seg_buf, size_t seg_size,
                                const zgec_block_params *bp,
                                seg_tables *cur, const seg_tables *prev,
                                int is_first, size_t Ld, size_t Ll,
                                size_t seg_out_start, uint32_t dir_raw_len)
{
    seg_hdr_t h;
    zgec_err e;
    uint64_t streams_total;
    if (seg == NULL || seg_buf == NULL || bp == NULL || cur == NULL)
        return ZGEC_ERR_INVAL;
    if (!is_first && prev == NULL) return ZGEC_ERR_TABLE_INHERIT;
    e = seg_hdr_parse(&h, seg_buf, seg_size, bp);
    if (e != ZGEC_OK) return e;
    streams_total = (uint64_t)h.header_size + (uint64_t)h.lit_size +
                    (uint64_t)h.ll_size + (uint64_t)h.ml_size +
                    (uint64_t)h.of_size;
    if (streams_total != (uint64_t)seg_size) return ZGEC_ERR_SEGMENT_SIZE;
    if ((uint64_t)h.n_lit > (uint64_t)dir_raw_len) return ZGEC_ERR_RAW_LEN;
    e = seg_tables_build(cur, &h, seg_buf, seg_size, prev, is_first);
    if (e != ZGEC_OK) return e;
    return decode_segment_streams(seg, seg_buf, seg_size, &h, cur,
                                  Ld, Ll, seg_out_start, dir_raw_len);
}

/* ---- Phase B: execute (Annex D, scalar-conforming) ---- */

/* Copy n literals byte-exactly, without calling the C library.
 *
 * n averages about 1.4 on text, so an out-of-line memcpy spent far more
 * time on its call sequence and length dispatch than on the copy. Every
 * copy stays inside [d, d+n) and reads only bytes [s, s+n), so it is
 * exact and cannot overrun either buffer.
 *
 * For n >= 8 an aligned 8-byte stride is used and the final 8 bytes are
 * re-copied (overlapping the last full chunk) rather than handled as a
 * remainder; for n < 8 a 4/2/1 cascade copies the head and the tail of
 * the run, which together cover it. */
static void exec_copy_literals(uint8_t *d, const uint8_t *s, size_t n)
{
    size_t i = 0;
    if (n >= 8) {
        while (i + 8 <= n) {
            memcpy(d + i, s + i, 8);
            i += 8;
        }
        memcpy(d + n - 8, s + n - 8, 8);
        return;
    }
    if (n >= 4) {
        memcpy(d, s, 4);
        memcpy(d + n - 4, s + n - 4, 4);
        return;
    }
    if (n >= 2) {
        memcpy(d, s, 2);
        memcpy(d + n - 2, s + n - 2, 2);
        return;
    }
    if (n == 1) d[0] = s[0];
}

static zgec_err exec_literals_plain(uint8_t *dst, const uint8_t *lit, size_t n,
                                  size_t pos, size_t raw_size)
{
    size_t i;
    /* Defensive: Phase A validated pos + n <= raw_size and lit != NULL
     * whenever n > 0; re-check here instead of trusting it blindly. */
    if (n == 0) return ZGEC_OK;
    if (dst == NULL || lit == NULL) return ZGEC_ERR_RAW_LEN;
    if (n > raw_size || pos > raw_size - n) return ZGEC_ERR_RAW_LEN;
    if (raw_size > 64 && pos + n > raw_size - 64) {
        for (i = 0; i < n; i++) dst[pos + i] = lit[i]; /* safe scalar tail */
    } else {
        exec_copy_literals(dst + pos, lit, n);
    }
    return ZGEC_OK;
}

static void exec_literals_sub(uint8_t *vb, uint8_t *dst, const uint8_t *res,
                               size_t n, uint32_t rep0, size_t pos,
                               size_t LdLl)
{
    size_t t;
    if (n == 0 || vb == NULL || dst == NULL || res == NULL) return;
    for (t = 0; t < n; t++) {
        size_t vbpos = LdLl + pos + t;
        uint8_t pred = 0;
        if ((uint64_t)rep0 <= (uint64_t)vbpos) {
            size_t back = (size_t)((uint64_t)vbpos - (uint64_t)rep0);
            pred = vb[back];
        }
        dst[pos + t] = (uint8_t)(res[t] + pred);
    }
}

/* Copy one match.
 *
 * Spec 6.1/Annex D: a match copies byte by byte in increasing order, so
 * an overlapping match (len > off) replicates the bytes just written.
 * When off >= 8 that ordering is exactly what 8-byte chunks produce: each
 * chunk at offset i reads [i-off, i-off+8) and writes [i, i+8), and
 * off >= 8 puts every source byte strictly before every destination byte
 * of the same chunk, so the source is always final. Chunking therefore
 * replaces the out-of-line memmove call with a handful of 8-byte moves
 * while preserving the byte-at-a-time result. For off < 8 the copy must
 * stay byte at a time, because the source can run into the bytes this
 * match is itself producing. src is the VB position of the match; it is
 * never below the virtual buffer because V2 checked off <= Ld + Ll + pos. */
static zgec_err exec_match(uint8_t *dst, size_t off, size_t len, size_t pos,
                           size_t raw_size, size_t LdLl)
{
    size_t i = 0;
    uint8_t *d;
    const uint8_t *src;
    if (len == 0) return ZGEC_OK;
    if (off < 1) return ZGEC_ERR_OFFSET;
    /* Defensive re-validation (Phase A proved all of this): without these
     * the pointer arithmetic below is UB before any byte is touched. */
    if (len > raw_size || pos > raw_size - len) return ZGEC_ERR_RAW_LEN;
    if ((uint64_t)off > (uint64_t)LdLl + (uint64_t)pos)
        return ZGEC_ERR_OFFSET;
    d = dst + pos; /* pos + len <= raw_size: stays inside OUT */
    src = d - off; /* off <= LdLl + pos: at or after the VB start */
    if (raw_size > 64 && pos + len > raw_size - 64) {
        for (i = 0; i < len; i++) d[i] = src[i]; /* safe scalar tail */
        return ZGEC_OK;
    }
    if (off == 1) {
        memset(d, src[0], len);
        return ZGEC_OK;
    }
    if (off >= 16) {
        if (len >= 16) {
            while (i + 16 <= len) {
                memcpy(d + i, src + i, 16);
                i += 16;
            }
            memcpy(d + len - 16, src + len - 16, 16);
            return ZGEC_OK;
        }
        if (len >= 8) {
            memcpy(d, src, 8);
            memcpy(d + len - 8, src + len - 8, 8);
            return ZGEC_OK;
        }
        if (len >= 4) {
            memcpy(d, src, 4);
            memcpy(d + len - 4, src + len - 4, 4);
            return ZGEC_OK;
        }
    } else if (off >= 8) {
        if (len >= 8) {
            while (i + 8 <= len) {
                memcpy(d + i, src + i, 8);
                i += 8;
            }
            memcpy(d + len - 8, src + len - 8, 8);
            return ZGEC_OK;
        }
        if (len >= 4) {
            memcpy(d, src, 4);
            memcpy(d + len - 4, src + len - 4, 4);
            return ZGEC_OK;
        }
    }
    for (i = 0; i < len; i++) d[i] = src[i];
    return ZGEC_OK;
}

static zgec_err exec_block(zgec_block_arrays *ba)
{
    /* u64 accumulators: on 32-bit a size_t sum_ll could wrap past the
     * >n_lit check, and out_pos arithmetic must not wrap either. */
    uint64_t out_pos = 0;
    size_t LdLl;
    size_t s;
    if (ba == NULL || (ba->n_seg > 0 && ba->segs == NULL)) return ZGEC_ERR_INVAL;
    if (ba->n_seg > 0 && (ba->out == NULL || ba->vb == NULL))
        return ZGEC_ERR_INTERNAL;
    LdLl = ba->ld + ba->llref;
    for (s = 0; s < ba->n_seg; s++) {
        zgec_segment_arrays *seg = &ba->segs[s];
        /* Explicit cursor: NULL exactly when n_lit == 0; every
         * advance is range-checked (no NULL + offset, no overrun). */
        const uint8_t *lit_cur = seg->lit;
        size_t lit_rem = seg->n_lit;
        uint64_t sum_ll = 0;
        uint64_t tail = 0;
        size_t j;
        if (seg->n_lit > 0 && lit_cur == NULL) return ZGEC_ERR_INTERNAL;
        if (seg->n_seq > 0 &&
            (seg->ll == NULL || seg->ml == NULL || seg->of == NULL ||
              seg->rep0_before == NULL))
            return ZGEC_ERR_INTERNAL;
        for (j = 0; j < seg->n_seq; j++) {
            uint32_t ll = seg->ll[j];
            uint32_t ml = seg->ml[j];
            uint32_t off = seg->of[j];
            zgec_err me;
            if (off < 1) return ZGEC_ERR_OFFSET;
            sum_ll += (uint64_t)ll;
            if (sum_ll > (uint64_t)seg->n_lit) return ZGEC_ERR_LL_SUM;
            if (out_pos + (uint64_t)ll + (uint64_t)ml >
                (uint64_t)ba->raw_size)
                return ZGEC_ERR_RAW_LEN;
            if (ll > 0) {
                if (lit_cur == NULL || (size_t)ll > lit_rem)
                    return ZGEC_ERR_LL_SUM;
                if (seg->lit_form == 0) {
                    me = exec_literals_plain(ba->out, lit_cur, (size_t)ll,
                                        (size_t)out_pos, ba->raw_size);
                    if (me != ZGEC_OK) return me;
                } else
                    exec_literals_sub(ba->vb, ba->out, lit_cur, (size_t)ll,
                                      seg->rep0_before[j], (size_t)out_pos,
                                      LdLl);
                lit_cur += (size_t)ll; /* ll <= lit_rem: stays in-bounds */
                lit_rem -= (size_t)ll;
            }
            out_pos += (uint64_t)ll;
            me = exec_match(ba->out, (size_t)off, (size_t)ml,
                            (size_t)out_pos, ba->raw_size, LdLl);
            if (me != ZGEC_OK) return me;
            out_pos += (uint64_t)ml;
        }
        /* Tail literals always copied (n_lit - sumLL). */
        if (sum_ll > (uint64_t)seg->n_lit) return ZGEC_ERR_LL_SUM;
        tail = (uint64_t)seg->n_lit - sum_ll;
        if (tail > 0) {
            zgec_err te;
            if (out_pos + tail > (uint64_t)ba->raw_size)
                return ZGEC_ERR_RAW_LEN;
            if (lit_cur == NULL || (size_t)tail > lit_rem)
                return ZGEC_ERR_LL_SUM;
            if (seg->lit_form == 0) {
                te = exec_literals_plain(ba->out, lit_cur, (size_t)tail,
                                    (size_t)out_pos, ba->raw_size);
                if (te != ZGEC_OK) return te;
            } else
                exec_literals_sub(ba->vb, ba->out, lit_cur, (size_t)tail,
                                  seg->rep0_tail, (size_t)out_pos, LdLl);
            out_pos += tail;
        }
    }
    if (out_pos != (uint64_t)ba->raw_size) return ZGEC_ERR_RAW_LEN;
    return ZGEC_OK;
}

/* ---- parallel Phase A (§10.6) ----
 * Segment headers are parsed serially (cheap) and the table
 * inheritance chain is pre-built serially; Phase A streams of all
 * segments then run concurrently on worker threads, and Phase B
 * stays serial in segment order. Tables are read-only while
 * workers run; each worker writes only its own segment arrays.
 * n_threads==1 keeps the original serial loop, unchanged. */

typedef struct {
    zgec_segment_arrays *seg;
    const uint8_t *seg_buf;
    size_t seg_size;
    const seg_hdr_t *hdr;
    seg_tables *tabs;
    size_t Ld;
    size_t Ll;
    size_t seg_out_start;
    uint32_t dir_raw_len;
    zgec_err err;
} zgec_phaseA_job;

typedef struct {
    zgec_phaseA_job *jobs;
    size_t begin;
    size_t end;
} zgec_phaseA_range;

static void zgec_phaseA_run_range(zgec_phaseA_range *r)
{
    size_t k;
    if (r == NULL) return;
    for (k = r->begin; k < r->end; k++) {
        zgec_phaseA_job *jb = &r->jobs[k];
        jb->err = decode_segment_streams(jb->seg, jb->seg_buf, jb->seg_size,
                                         jb->hdr, jb->tabs, jb->Ld, jb->Ll,
                                         jb->seg_out_start, jb->dir_raw_len);
        if (jb->err != ZGEC_OK) break; /* error scan stays index-ordered */
    }
}

#if defined(_WIN32)
static DWORD WINAPI zgec_phaseA_proc_win(LPVOID arg)
{
    zgec_phaseA_run_range((zgec_phaseA_range *)arg);
    return (DWORD)0;
}
#else
static void *zgec_phaseA_proc_posix(void *arg)
{
    zgec_phaseA_run_range((zgec_phaseA_range *)arg);
    return NULL;
}
#endif

/* Spawn w ranges; a range whose thread cannot start runs inline. */
static void zgec_phaseA_spawn(zgec_phaseA_range *ranges, size_t w)
{
#if defined(_WIN32)
    HANDLE *hs = NULL;
    size_t t;
    if (ranges == NULL || w == 0u) return;
    hs = (HANDLE *)zgec_alloc(w * sizeof(*hs), _Alignof(HANDLE));
    if (hs == NULL) {
        for (t = 0; t < w; t++) zgec_phaseA_run_range(&ranges[t]);
        return;
    }
    for (t = 0; t < w; t++) hs[t] = NULL;
    for (t = 0; t < w; t++) {
        hs[t] = CreateThread(NULL, 0, zgec_phaseA_proc_win,
                             (LPVOID)&ranges[t], 0, NULL);
        if (hs[t] == NULL) zgec_phaseA_run_range(&ranges[t]); /* fallback */
    }
    /* Compact out NULL handles before waiting: a NULL entry makes
     * WaitForMultipleObjects fail immediately while the other threads
     * are still writing, which would let Phase B start early. */
    {
        DWORD nv = 0;
        for (t = 0; t < w; t++) {
            if (hs[t] != NULL) hs[nv++] = hs[t];
        }
        if (nv > 0u) (void)WaitForMultipleObjects(nv, hs, TRUE, INFINITE);
        for (t = 0; t < (size_t)nv; t++) (void)CloseHandle(hs[t]);
    }
    zgec_free(hs);
#else
    pthread_t *ths = NULL;
    unsigned char *started = NULL;
    size_t t;
    if (ranges == NULL || w == 0u) return;
    ths = (pthread_t *)zgec_alloc(w * sizeof(*ths), _Alignof(pthread_t));
    started = (unsigned char *)zgec_alloc(w * sizeof(*started), 1);
    if (ths == NULL || started == NULL) {
        for (t = 0; t < w; t++) zgec_phaseA_run_range(&ranges[t]);
        zgec_free(ths);
        zgec_free(started);
        return;
    }
    memset(started, 0, w * sizeof(*started));
    for (t = 0; t < w; t++) {
        if (pthread_create(&ths[t], NULL, zgec_phaseA_proc_posix,
                           (void *)&ranges[t]) == 0) {
            started[t] = 1;
        } else {
            zgec_phaseA_run_range(&ranges[t]); /* fallback */
        }
    }
    for (t = 0; t < w; t++) {
        if (started[t] != 0) (void)pthread_join(ths[t], NULL);
    }
    zgec_free(ths);
    zgec_free(started);
#endif
}

/* Parallel Phase A for a whole block. Serial: parse headers, build
 * the table chain, prefix segment outputs. Concurrent: streams.
 * Returns the first segment error in index order. */
static zgec_err decode_segments_parallel(zgec_block_arrays *ba,
                                          const uint8_t *payload, size_t psz,
                                          const zgec_seg_dir_entry *dir,
                                          uint32_t sc,
                                          const zgec_block_params *bp,
                                          size_t Ld, size_t litref_len,
                                          size_t base_offset, size_t n_workers,
                                          int *export_ok)
{
    seg_hdr_t *hdrs = NULL;
    seg_tables *tabs = NULL;
    zgec_phaseA_job *jobs = NULL;
    zgec_phaseA_range *ranges = NULL;
    size_t n = (size_t)sc;
    size_t seg_offset = 0;
    size_t seg_out = 0;
    size_t chunk = 0;
    size_t t;
    size_t k;
    zgec_err first = ZGEC_OK;

    if (ba == NULL || payload == NULL || dir == NULL || bp == NULL ||
        export_ok == NULL)
        return ZGEC_ERR_INVAL;
    *export_ok = 0;
    if (n == 0u || n_workers <= 1u) return ZGEC_ERR_INVAL;
    if (n_workers > n) n_workers = n;

    hdrs = (seg_hdr_t *)zgec_alloc(n * sizeof(*hdrs), _Alignof(seg_hdr_t));
    tabs = (seg_tables *)zgec_alloc(n * sizeof(*tabs), _Alignof(seg_tables));
    jobs = (zgec_phaseA_job *)zgec_alloc(n * sizeof(*jobs),
                                        _Alignof(zgec_phaseA_job));
    ranges = (zgec_phaseA_range *)zgec_alloc(n_workers * sizeof(*ranges),
                                             _Alignof(zgec_phaseA_range));
    if (!hdrs || !tabs || !jobs || !ranges) {
        zgec_free(hdrs);
        zgec_free(tabs);
        zgec_free(jobs);
        zgec_free(ranges);
        return ZGEC_ERR_NOMEM;
    }
    memset(jobs, 0, n * sizeof(*jobs));
    for (k = 0; k < n; k++) seg_tables_init(&tabs[k]);

    /* Serial header pass (cheap). */
    seg_offset = base_offset;
    for (k = 0; k < n; k++) {
        uint32_t comp_len = dir[k].comp_len;
        zgec_err pe;
        if (comp_len == 0u || (size_t)comp_len > psz - seg_offset) {
            first = ZGEC_ERR_TRUNCATED;
            goto pdone;
        }
        pe = seg_hdr_parse(&hdrs[k], payload + seg_offset,
                           (size_t)comp_len, bp);
        if (pe != ZGEC_OK) {
            first = pe;
            goto pdone;
        }
        seg_offset += (size_t)comp_len;
    }

    /* Serial table-inheritance pre-pass (§7.4). */
    seg_offset = base_offset;
    for (k = 0; k < n; k++) {
        uint32_t comp_len = dir[k].comp_len;
        zgec_err be = seg_tables_build(&tabs[k], &hdrs[k],
                                       payload + seg_offset,
                                       (size_t)comp_len,
                                       (k == 0u) ? NULL : &tabs[k - 1u],
                                       (k == 0u) ? 1 : 0);
        if (be != ZGEC_OK) {
            first = be;
            goto pdone;
        }
        seg_offset += (size_t)comp_len;
    }

    /* Prefix segment output starts; fill jobs. */
    seg_offset = base_offset;
    seg_out = 0;
    for (k = 0; k < n; k++) {
        uint32_t comp_len = dir[k].comp_len;
        memset(&jobs[k], 0, sizeof(jobs[k]));
        jobs[k].seg = &ba->segs[k];
        jobs[k].seg_buf = payload + seg_offset;
        jobs[k].seg_size = (size_t)comp_len;
        jobs[k].hdr = &hdrs[k];
        jobs[k].tabs = &tabs[k];
        jobs[k].Ld = Ld;
        jobs[k].Ll = litref_len;
        jobs[k].seg_out_start = seg_out;
        jobs[k].dir_raw_len = dir[k].raw_len;
        jobs[k].err = ZGEC_OK;
        seg_offset += (size_t)comp_len;
        seg_out += (size_t)dir[k].raw_len;
    }

    /* Contiguous ranges; capped workers, so no empty range. */
    chunk = (n + n_workers - 1u) / n_workers;
    if (chunk == 0u) chunk = 1u;
    for (t = 0; t < n_workers; t++) {
        size_t begin = t * chunk;
        size_t end = begin + chunk;
        if (begin > n) begin = n;
        if (end > n) end = n;
        ranges[t].jobs = jobs;
        ranges[t].begin = begin;
        ranges[t].end = end;
    }
    zgec_phaseA_spawn(ranges, n_workers);

    for (k = 0; k < n; k++) {
        if (jobs[k].err != ZGEC_OK) {
            first = jobs[k].err;
            break;
        }
    }
    if (first == ZGEC_OK) {
        int ok = 1;
        for (k = 0; k < n; k++) {
            if (ba->segs[k].lit_form != 0 || ba->segs[k].ll_cond) ok = 0;
        }
        *export_ok = ok;
    }

pdone:
    for (k = 0; k < n; k++) seg_tables_free(&tabs[k]);
    zgec_free(tabs);
    zgec_free(hdrs);
    zgec_free(jobs);
    zgec_free(ranges);
    return first;
}

/* ---- COMPRESSED block decode (Phase A + B, V1..V10) ---- */

static zgec_err decode_compressed(zgec_block_arrays **out,
                                   zgec_decoder *d,
                                   const zgec_frame_header *fh,
                                   const uint8_t *payload, size_t psz,
                                   uint32_t sc, const zgec_dict *dict,
                                   const uint8_t *litref, size_t litref_len,
                                   uint32_t expect_raw, uint32_t checksum,
                                   int has_cksum, int *export_ok,
                                   int filter_mode, int filter_param)
{
    zgec_block_params bp;
    size_t params_len;
    zgec_seg_dir_entry *dir = NULL;
    zgec_block_arrays *ba = NULL;
    seg_tables prev;
    seg_tables cur;
    size_t seg_offset = 0;
    size_t seg_out = 0;
    uint64_t comp_sum = 0;
    uint64_t raw_sum = 0;
    uint64_t vb_total;
    size_t Ld = 0;
    uint32_t i;
    zgec_err e;
    int ok = 1;
    size_t n_workers;

    if (export_ok) *export_ok = 0;
    if (out == NULL || payload == NULL || fh == NULL) return ZGEC_ERR_INVAL;
    if (litref_len > 0 && litref == NULL) return ZGEC_ERR_INVAL;
    memset(&bp, 0, sizeof(bp));
    params_len = zgec_block_params_parse(&bp, payload, psz);
    if (params_len == 0 || params_len > 52 || params_len > psz)
        return ZGEC_ERR_CTX_MODE; /* V7 */
    if (sc < 1 || sc > ZGEC_MAX_SEGMENTS) return ZGEC_ERR_SEGMENT_COUNT;
    if (psz < params_len + (size_t)sc * 8) return ZGEC_ERR_TRUNCATED; /* V8 */
    dir = (zgec_seg_dir_entry *)zgec_alloc((size_t)sc * sizeof(*dir),
                                           _Alignof(zgec_seg_dir_entry));
    if (!dir) return ZGEC_ERR_NOMEM;
    e = zgec_seg_dir_parse(dir, sc, payload + params_len, psz - params_len);
    if (e != ZGEC_OK) {
        zgec_free(dir);
        return e;
    }
    for (i = 0; i < sc; i++) {
        if (dir[i].comp_len == 0) {
            zgec_free(dir);
            return ZGEC_ERR_SEGMENT_SIZE; /* V8 */
        }
        comp_sum += (uint64_t)dir[i].comp_len;
        raw_sum += (uint64_t)dir[i].raw_len;
    }
    /* V8 tiling: params + dir + segments == payload. */
    if (params_len + (size_t)sc * 8 + comp_sum != (uint64_t)psz) {
        zgec_free(dir);
        return ZGEC_ERR_SEGMENT_SIZE;
    }
    /* V4: segment raw lengths sum to the block raw size. */
    if (raw_sum != (uint64_t)expect_raw) {
        zgec_free(dir);
        return ZGEC_ERR_RAW_LEN;
    }

    Ld = (dict != NULL) ? (size_t)dict->raw_size : 0;
    /* §13: pre-compute the per-worker bound and refuse above the
     * configured limits before decoding anything. A VB above the
     * P24 profile (16 MiB) is still accepted silently:
     * profile-exceed is encoder-side only. */
    e = dec_check_worker_limits(d, fh->block_log2, fh->max_dict_log2,
                                (uint64_t)expect_raw, (uint64_t)Ld,
                                (uint64_t)litref_len);
    if (e != ZGEC_OK) {
        zgec_free(dir);
        return e;
    }
    vb_total = (uint64_t)Ld + (uint64_t)litref_len + (uint64_t)expect_raw;
    if (vb_total > 0xFFFFFFFFu) { /* format limit 2^32-1 (V9) */
        zgec_free(dir);
        return ZGEC_ERR_OUTPUT_SIZE;
    }

    ba = (zgec_block_arrays *)zgec_alloc(sizeof(*ba), _Alignof(zgec_block_arrays));
    if (!ba) {
        zgec_free(dir);
        return ZGEC_ERR_NOMEM;
    }
    memset(ba, 0, sizeof(*ba));
    ba->n_seg = (size_t)sc;
    ba->raw_size = (size_t)expect_raw;
    ba->ld = Ld;
    ba->llref = litref_len;
    ba->segs = (zgec_segment_arrays *)zgec_alloc(
        (size_t)sc * sizeof(zgec_segment_arrays), _Alignof(zgec_segment_arrays));
    if (!ba->segs) {
        zgec_free(dir);
        zgec_block_arrays_free(ba);
        return ZGEC_ERR_NOMEM;
    }
    memset(ba->segs, 0, (size_t)sc * sizeof(zgec_segment_arrays));
    if (vb_total > (uint64_t)SIZE_MAX - (uint64_t)ZGEC_OUTPUT_SLACK) {
        zgec_free(dir);
        zgec_block_arrays_free(ba);
        return ZGEC_ERR_NOMEM;
    }
    ba->vb = (uint8_t *)zgec_alloc(
        (size_t)vb_total + ZGEC_OUTPUT_SLACK, 64);
    if (!ba->vb) {
        zgec_free(dir);
        zgec_block_arrays_free(ba);
        return ZGEC_ERR_NOMEM;
    }
    memset(ba->vb + (size_t)vb_total, 0, ZGEC_OUTPUT_SLACK);
    if (Ld > 0) {
        if (dict == NULL || dict->data == NULL) {
            zgec_free(dir);
            zgec_block_arrays_free(ba);
            return ZGEC_ERR_INTERNAL;
        }
        memcpy(ba->vb, dict->data, Ld);
    }
    if (litref_len > 0) {
        if (litref == NULL) {
            zgec_free(dir);
            zgec_block_arrays_free(ba);
            return ZGEC_ERR_INVAL;
        }
        memcpy(ba->vb + Ld, litref, litref_len);
    }
    ba->out = ba->vb + Ld + litref_len; /* Ld+Ll+raw <= vb_total: in-bounds */

    n_workers = dec_resolve_workers(d, (size_t)sc);
    if (n_workers > 1u) {
        /* §10.6: the segment headers and the inheritance chain are
         * resolved serially (cheap), then Phase A of every segment
         * runs on worker threads; Phase B stays serial below. */
        e = decode_segments_parallel(ba, payload, psz, dir, sc, &bp, Ld,
                                     litref_len,
                                     params_len + (size_t)sc * 8u,
                                     n_workers, &ok);
        if (e != ZGEC_OK) goto fail;
    } else {
        seg_tables_init(&prev);
        seg_tables_init(&cur);
        seg_offset = params_len + (size_t)sc * 8;
        for (i = 0; i < sc; i++) {
            if (seg_offset > psz ||
                (size_t)dir[i].comp_len > psz - seg_offset) {
                e = ZGEC_ERR_TRUNCATED;
                goto fail;
            }
            e = decode_segment(&ba->segs[i], payload + seg_offset,
                               (size_t)dir[i].comp_len, &bp, &cur, &prev,
                               (i == 0) ? 1 : 0, Ld, litref_len, seg_out,
                               dir[i].raw_len);
            if (e != ZGEC_OK) goto fail;
            if (ba->segs[i].lit_form != 0 || ba->segs[i].ll_cond) ok = 0;
            seg_tables_free(&prev);
            prev = cur;
            seg_tables_init(&cur);
            memset(&cur, 0, sizeof(cur));
            cur.ll_rle[0] = cur.ll_rle[1] = cur.ll_rle[2] = -1;
            cur.of_rle[0] = cur.of_rle[1] = cur.of_rle[2] = -1;
            cur.ml_rle = -1;
            /* Advance past this segment: the segment directory holds
             * the sizes, and the serial path must walk the payload. */
            seg_offset += (size_t)dir[i].comp_len;
            seg_out += (size_t)ba->segs[i].raw_len;
        }
        seg_tables_free(&prev);
        seg_tables_free(&cur);
    }
    zgec_free(dir);
    dir = NULL;

    e = exec_block(ba);
    if (e != ZGEC_OK) {
        zgec_block_arrays_free(ba);
        return e;
    }
    if (filter_mode != 0) {
        /* 10.4: undo the pre-filter (7.5) in place over OUT after phase
         * B, before the checksum. The pass stays inside OUT by
         * construction (V11). */
        e = zgec_filter_inverse(ba->out, ba->raw_size,
                                (unsigned)filter_mode,
                                (unsigned)filter_param);
        if (e != ZGEC_OK) {
            zgec_block_arrays_free(ba);
            return e;
        }
    }
    if (has_cksum) { /* V10 */
        uint32_t crc = zgec_crc32c(ba->out, ba->raw_size, 0u);
        if (crc != checksum) {
            zgec_block_arrays_free(ba);
            return ZGEC_ERR_CHECKSUM;
        }
    }
    if (export_ok) *export_ok = ok;
    *out = ba;
    return ZGEC_OK;

fail:
    seg_tables_free(&prev);
    seg_tables_free(&cur);
    zgec_free(dir);
    zgec_block_arrays_free(ba);
    return e;
}

/* ---- literal export (section 6.3): LL + literals only ---- */

static zgec_err export_block_literals(const uint8_t *payload, size_t psz,
                                      uint32_t sc, uint8_t **o, size_t *osz)
{
    zgec_block_params bp;
    size_t params_len;
    zgec_seg_dir_entry *dir = NULL;
    seg_tables prev;
    seg_tables cur;
    uint8_t *lit = NULL;
    size_t lit_cap = 0;
    size_t lit_len = 0;
    size_t seg_offset = 0;
    uint32_t i;
    zgec_err e;

    memset(&bp, 0, sizeof(bp));
    params_len = zgec_block_params_parse(&bp, payload, psz);
    if (params_len == 0 || params_len > 52 || params_len > psz)
        return ZGEC_ERR_CTX_MODE;
    if (sc < 1 || sc > ZGEC_MAX_SEGMENTS) return ZGEC_ERR_SEGMENT_COUNT;
    if (psz < params_len + (size_t)sc * 8) return ZGEC_ERR_TRUNCATED;
    dir = (zgec_seg_dir_entry *)zgec_alloc((size_t)sc * sizeof(*dir),
                                           _Alignof(zgec_seg_dir_entry));
    if (!dir) return ZGEC_ERR_NOMEM;
    e = zgec_seg_dir_parse(dir, sc, payload + params_len, psz - params_len);
    if (e != ZGEC_OK) {
        zgec_free(dir);
        return e;
    }
    seg_tables_init(&prev);
    seg_tables_init(&cur);
    seg_offset = params_len + (size_t)sc * 8;
    for (i = 0; i < sc; i++) {
        seg_hdr_t h;
        const uint8_t *seg_buf;
        size_t seg_size;
        const uint8_t *lit_stream;
        const uint8_t *ll_stream;
        uint32_t *ll = NULL;
        uint8_t *runstart = NULL;
        uint64_t sum_ll = 0;
        size_t j = 0;
        if (dir[i].comp_len == 0 || seg_offset > psz ||
            (size_t)dir[i].comp_len > psz - seg_offset) {
            e = ZGEC_ERR_TRUNCATED;
            goto efail;
        }
        seg_buf = payload + seg_offset;
        seg_size = (size_t)dir[i].comp_len;
        e = seg_hdr_parse(&h, seg_buf, seg_size, &bp);
        if (e != ZGEC_OK) goto efail;
        if (h.lit_form != 0) { /* 6.3: plain only */
            e = ZGEC_ERR_LITREF_FORM;
            goto efail;
        }
        if (h.ctx_ll) { /* 6.3: no LL conditioning */
            e = ZGEC_ERR_LITREF_CTX;
            goto efail;
        }
        {
            uint64_t tot = (uint64_t)h.header_size + (uint64_t)h.lit_size +
                           (uint64_t)h.ll_size + (uint64_t)h.ml_size +
                           (uint64_t)h.of_size;
            if (tot != (uint64_t)seg_size) {
                e = ZGEC_ERR_SEGMENT_SIZE;
                goto efail;
            }
        }
        e = seg_tables_build(&cur, &h, seg_buf, seg_size, &prev,
                             (i == 0) ? 1 : 0);
        if (e != ZGEC_OK) goto efail;
        lit_stream = seg_buf + h.header_size;
        ll_stream = lit_stream + (size_t)h.lit_size;
        if (h.n_lit > 0 && h.lit_coder != 0 && h.k > 1) {
            runstart = (uint8_t *)zgec_alloc((size_t)h.n_lit, 1);
            if (!runstart) {
                e = ZGEC_ERR_NOMEM;
                goto efail;
            }
            memset(runstart, 0, (size_t)h.n_lit);
        }
        /* The LL array is allocated, tested and read inside one block, so
         * that the read is dominated by its own NULL test. Split across
         * two n_seq tests, the loop read is a dereference of a pointer the
         * compiler only knows is conditionally allocated, which gcc 13
         * reports as a potential null dereference. runstart is allocated
         * before the LL array because this loop writes it, so every
         * failure path below releases it too. */
        if (h.n_seq > 0) {
            ll = (uint32_t *)zgec_alloc((size_t)h.n_seq * sizeof(uint32_t), 64);
            if (!ll) {
                zgec_free(runstart);
                e = ZGEC_ERR_NOMEM;
                goto efail;
            }
            e = seq_decode_one(ll, (size_t)h.n_seq, cur.ll[0],
                               cur.ll_rle[0], ll_stream, (size_t)h.ll_size);
            if (e != ZGEC_OK) {
                zgec_free(ll);
                zgec_free(runstart);
                goto efail;
            }
            for (j = 0; j < (size_t)h.n_seq; j++) {
                sum_ll += (uint64_t)ll[j];
                if (sum_ll > (uint64_t)h.n_lit) {
                    zgec_free(ll);
                    zgec_free(runstart);
                    e = ZGEC_ERR_LL_SUM;
                    goto efail;
                }
                if (ll[j] > 0 && runstart)
                    runstart[(size_t)sum_ll - (size_t)ll[j]] = 1;
            }
        }
        if (sum_ll < (uint64_t)h.n_lit && h.n_lit > 0 && runstart)
            runstart[(size_t)sum_ll] = 1;
        if (lit_len + (size_t)h.n_lit < lit_len) {
            zgec_free(ll);
            zgec_free(runstart);
            e = ZGEC_ERR_OUTPUT_SIZE;
            goto efail;
        }
        if (lit_len + (size_t)h.n_lit > lit_cap) {
            size_t need = lit_len + (size_t)h.n_lit;
            size_t nc = lit_cap ? lit_cap : 1024;
            uint8_t *nb;
            if (need > SIZE_MAX - 1024) {
                zgec_free(ll);
                zgec_free(runstart);
                e = ZGEC_ERR_OUTPUT_SIZE;
                goto efail;
            }
            while (nc < need) {
                if (nc > SIZE_MAX / 2) { nc = need; break; }
                nc *= 2;
            }
            nb = (uint8_t *)zgec_alloc(nc ? nc : 1, 64);
            if (!nb) {
                zgec_free(ll);
                zgec_free(runstart);
                e = ZGEC_ERR_NOMEM;
                goto efail;
            }
            if (lit_len > 0) memcpy(nb, lit, lit_len);
            zgec_free(lit);
            lit = nb;
            lit_cap = nc;
        }
        if (h.n_lit > 0) {
            if (h.lit_coder == 0) {
                memcpy(lit + lit_len, lit_stream, (size_t)h.n_lit);
            } else {
                if (h.k <= 1) {
                    e = zgec_rans_decode(lit + lit_len, (size_t)h.n_lit,
                                         lit_stream, (size_t)h.lit_size,
                                         cur.lit, 1, h.ctx_mode, NULL, NULL);
                } else {
                    e = zgec_rans_decode(lit + lit_len, (size_t)h.n_lit,
                                         lit_stream, (size_t)h.lit_size,
                                         cur.lit, h.k, h.ctx_mode,
                                         h.class_map, runstart);
                }
                if (e != ZGEC_OK) {
                    zgec_free(ll);
                    zgec_free(runstart);
                    goto efail;
                }
            }
            lit_len += (size_t)h.n_lit;
        }
        zgec_free(ll);
        zgec_free(runstart);
        seg_tables_free(&prev);
        prev = cur;
        seg_tables_init(&cur);
        memset(&cur, 0, sizeof(cur));
        cur.ll_rle[0] = cur.ll_rle[1] = cur.ll_rle[2] = -1;
        cur.of_rle[0] = cur.of_rle[1] = cur.of_rle[2] = -1;
        cur.ml_rle = -1;
        seg_offset += (size_t)dir[i].comp_len;
    }
    seg_tables_free(&prev);
    seg_tables_free(&cur);
    zgec_free(dir);
    if (lit == NULL) {
        lit = (uint8_t *)zgec_alloc(1, 64);
        if (!lit) return ZGEC_ERR_NOMEM;
        lit_cap = 1;
    }
    *o = lit;
    *osz = lit_len;
    return ZGEC_OK;

efail:
    seg_tables_free(&prev);
    seg_tables_free(&cur);
    zgec_free(dir);
    zgec_free(lit);
    return e;
}

/* ---- dictionary fetch (sections 4.6, 5.8, 5.9, V9) ---- */

static const zgec_footer_dict_entry *find_dict_entry(const zgec_footer *f,
                                                     uint16_t id)
{
    uint32_t i;
    for (i = 0; i < f->dict_count; i++) {
        if (f->dicts[i].dict_id == id) return &f->dicts[i];
    }
    return NULL;
}

static zgec_err fetch_dict(zgec_decoder *d, const zgec_frame_header *fh,
                            const zgec_footer *f, int have_footer,
                            uint16_t dict_id, const uint8_t *frame,
                            size_t frame_size, const zgec_dict **out)
{
    const zgec_dict *hit;
    const zgec_footer_dict_entry *en = NULL;
    unsigned ei;
    zgec_dict *nd = NULL;
    zgec_err e;

    *out = NULL;
    if (dict_id == 0) return ZGEC_OK;
    hit = dec_cache_get(d, dict_id);
    if (hit) {
        if (have_footer) {
            en = find_dict_entry(f, dict_id);
            if (en) {
                uint64_t hh;
                if ((uint64_t)hit->raw_size != (uint64_t)en->raw_size)
                    return ZGEC_ERR_DICT_SIZE;
                hh = zgec_xxh64(hit->data, hit->raw_size, 0);
                if (hh != en->content_hash) return ZGEC_ERR_DICT_HASH;
            }
        }
        *out = hit;
        return ZGEC_OK;
    }
    /* Registered external dictionaries. */
    for (ei = 0; ei < ZGEC_DEC_MAX_EXT; ei++) {
        if (d->ext[ei].used && d->ext[ei].id == dict_id) {
            if (have_footer) {
                uint64_t hh;
                en = find_dict_entry(f, dict_id);
                if (!en) return ZGEC_ERR_DICT_NOT_FOUND; /* V9 */
                if ((size_t)en->raw_size != d->ext[ei].size)
                    return ZGEC_ERR_DICT_SIZE; /* V9 */
                hh = zgec_xxh64(d->ext[ei].data, d->ext[ei].size, 0);
                if (hh != en->content_hash) return ZGEC_ERR_DICT_HASH; /* V9 */
            }
            /* Without a footer any size > 4G would truncate into the u32
             * raw_size below, under-allocating vb and defeating the V2
             * off <= Ld + Ll + pos check. Reject it (V9). */
            if (d->ext[ei].size > 0xFFFFFFFFu) return ZGEC_ERR_DICT_SIZE;
            if (d->ext[ei].size > SIZE_MAX - (size_t)ZGEC_OUTPUT_SLACK) return ZGEC_ERR_NOMEM;
            nd = (zgec_dict *)zgec_alloc(sizeof(*nd), _Alignof(zgec_dict));
            if (!nd) return ZGEC_ERR_NOMEM;
            memset(nd, 0, sizeof(*nd));
            nd->data = (uint8_t *)zgec_alloc(
                d->ext[ei].size + ZGEC_OUTPUT_SLACK, 64);
            if (!nd->data) {
                zgec_free(nd);
                return ZGEC_ERR_NOMEM;
            }
            memcpy(nd->data, d->ext[ei].data, d->ext[ei].size);
            nd->raw_size = (uint32_t)d->ext[ei].size;
            nd->dict_id = dict_id;
            nd->external = 1;
            nd->content_hash = zgec_xxh64(nd->data, d->ext[ei].size, 0);
            dec_cache_put(d, nd);
            zgec_free(nd); /* put detached the buffer; the shell is ours */
            hit = dec_cache_get(d, dict_id);
            if (!hit) return ZGEC_ERR_NOMEM;
            *out = hit;
            return ZGEC_OK;
        }
    }
    if (!have_footer) return ZGEC_ERR_DICT_NOT_FOUND; /* V9 */
    en = find_dict_entry(f, dict_id);
    if (!en) return ZGEC_ERR_DICT_NOT_FOUND; /* V9 */
    if (en->kind == 1) return ZGEC_ERR_DICT_NOT_FOUND; /* external, missing */
    if (dec_check_dict_size(d, fh->max_dict_log2, en->raw_size) != ZGEC_OK)
        return ZGEC_ERR_DICT_SIZE;
    if (en->offset + 24 > (uint64_t)frame_size) return ZGEC_ERR_TRUNCATED;
    {
        const uint8_t *rh = frame + (size_t)en->offset;
        zgec_record_header drh;
        const uint8_t *inner;
        size_t inner_avail;
        e = zgec_record_header_parse(&drh, rh, fh);
        if (e != ZGEC_OK) return e;
        if (drh.record_type != ZGEC_REC_DICT) return ZGEC_ERR_RECORD_TYPE;
        if (drh.dict_id != dict_id) return ZGEC_ERR_DICT_ID;
        if ((uint64_t)en->offset + 24u + (uint64_t)drh.payload_size >
            (uint64_t)frame_size)
            return ZGEC_ERR_TRUNCATED;
        if (drh.raw_size != en->raw_size) return ZGEC_ERR_DICT_SIZE;
        inner = rh + 24;
        inner_avail = (size_t)drh.payload_size;
        if (inner_avail >= 24) {
            zgec_record_header irh;
            if (zgec_record_header_parse(&irh, inner, fh) == ZGEC_OK) {
                if (irh.dict_id != 0) return ZGEC_ERR_DICT_ID;
                if (irh.lit_ref_depth != 0) return ZGEC_ERR_LITREF_DEPTH;
            }
        }
        e = zgec_dict_decode(&nd, inner, inner_avail, en->raw_size,
                             en->content_hash, fh); /* V9 hash verify */
        if (e != ZGEC_OK) return e;
        nd->dict_id = dict_id;
        dec_cache_put(d, nd);
        zgec_free(nd); /* put detached the buffer; the shell is ours */
        hit = dec_cache_get(d, dict_id);
        if (!hit) return ZGEC_ERR_NOMEM;
        *out = hit;
        return ZGEC_OK;
    }
}

/* ---- sequential frame decode (section 4.7) ---- */

/* Append one block's output.
 *
 * The capacity grows geometrically. Growing by exactly the bytes needed
 * (as this did) re-copies the whole accumulated output once per block, so
 * a frame of K blocks costs O(K) copies of the output -- quadratic, and
 * the dominant cost of a single-threaded decode of many small blocks.
 * Doubling bounds the total copied bytes to under twice the output. */
static zgec_err frame_output_append(uint8_t **out, size_t *len, size_t *cap,
                                     const uint8_t *data, size_t n,
                                     uint64_t max_total)
{
    if (n == 0) return ZGEC_OK;
    if (*len + n < *len) return ZGEC_ERR_OUTPUT_SIZE;
    /* Incremental total-limit enforcement (§13): many small blocks each
     * passing the per-block check could otherwise exceed max_total
     * unbounded before the final content-size check runs. */
    if (max_total != 0u && (uint64_t)*len + (uint64_t)n > max_total)
        return ZGEC_ERR_NOMEM;
    if (*len + n > *cap) {
        size_t need = *len + n;
        size_t nc = (*cap != 0) ? *cap : (size_t)1u << 20;
        if (need > (SIZE_MAX >> 1)) return ZGEC_ERR_OUTPUT_SIZE;
        while (nc < need) nc <<= 1;
        {
            uint8_t *nb = (uint8_t *)zgec_alloc(nc, 64);
            if (!nb) return ZGEC_ERR_NOMEM;
            if (*len > 0) memcpy(nb, *out, *len);
            zgec_free(*out);
            *out = nb;
            *cap = nc;
        }
    }
    memcpy(*out + *len, data, n);
    *len += n;
    return ZGEC_OK;
}

/* Adopt an already-allocated literal buffer into the history without
 * copying it again (the COMPRESSED path builds the buffer anyway). */
static void hist_push_owned(zgec_decoder *d, uint8_t *lit, size_t n, int ok)
{
    unsigned slot = (unsigned)(d->hist_n % 3u);
    zgec_free(d->hist_lit[slot]);
    if (lit != NULL && n > 0) {
        d->hist_lit[slot] = lit;
        d->hist_sz[slot] = n;
        d->hist_ok[slot] = ok;
    } else {
        zgec_free(lit);
        d->hist_lit[slot] = NULL;
        d->hist_sz[slot] = 0;
        d->hist_ok[slot] = ok;
    }
    d->hist_n++;
}

static void hist_push(zgec_decoder *d, const uint8_t *lit, size_t n, int ok)
{
    unsigned slot = (unsigned)(d->hist_n % 3u);
    zgec_free(d->hist_lit[slot]);
    d->hist_lit[slot] = NULL;
    d->hist_sz[slot] = 0;
    d->hist_ok[slot] = 0;
    if (n > 0 && lit) {
        d->hist_lit[slot] = (uint8_t *)zgec_alloc(n, 64);
        if (d->hist_lit[slot]) {
            memcpy(d->hist_lit[slot], lit, n);
            d->hist_sz[slot] = n;
            d->hist_ok[slot] = ok;
        }
    } else {
        d->hist_sz[slot] = 0;
        d->hist_ok[slot] = ok;
    }
    d->hist_n++;
}

/* Defined below (random access, 4.6); the block-parallel path reuses it so
 * a block decodes identically whether it is read alone or as part of a
 * frame. */
static zgec_err decode_block_record(zgec_decoder *d, const uint8_t *src,
                                     size_t src_size,
                                     const zgec_frame_header *fh,
                                     const zgec_footer *f,
                                     const zgec_footer_block_entry *be,
                                     uint8_t *ovr, size_t ovr_cap,
                                     const uint8_t **rdst, size_t *rsz);

/* ---- block-parallel frame decode (10.6, G4) ----
 *
 * Blocks are independent (4.6), so whole blocks decode concurrently, one
 * per worker, each writing straight into its slice of the frame output.
 * A worker owns its decoder and therefore its dictionary cache, so an
 * eviction can never free a dictionary another worker is still reading,
 * and it re-derives its literal-reference region from the predecessor
 * records (6.3) exactly as a single random-access read does. The source
 * frame, the parsed footer and the external dictionaries are read-only.
 *
 * Frames carrying DICT records are left to the sequential scan, whose
 * validation of embedded dictionaries (4.4) is exhaustive.
 */

typedef struct {
    zgec_decoder *w;
    const uint8_t *src;
    size_t src_size;
    const zgec_frame_header *fh;
    const zgec_footer *f;
    const size_t *off;
    const size_t *sz;
    uint8_t *out;
    uint32_t nblk;
    zgec_mu *mu;        /* guards next/err/err_idx */
    uint32_t *next;
    zgec_err *err;
    uint32_t *err_idx;
} zgec_dec_block_job;

static void zgec_dec_blocks_run(zgec_dec_block_job *jb)
{
    if (jb == NULL) return;
    for (;;) {
        uint32_t i;
        const uint8_t *p = NULL;
        size_t got = 0;
        zgec_err e;
        zgec_mu_lock(jb->mu);
        i = (*jb->next)++;
        zgec_mu_unlock(jb->mu);
        if (i >= jb->nblk) return;
        e = decode_block_record(jb->w, jb->src, jb->src_size, jb->fh, jb->f,
                                &jb->f->blocks[i],
                                jb->out + jb->off[i], jb->sz[i], &p, &got);
        if (e == ZGEC_OK && (got != jb->sz[i] || p != jb->out + jb->off[i])) {
            e = ZGEC_ERR_INTERNAL;
        }
        if (e != ZGEC_OK) {
            /* Record the lowest-numbered failure so the reported error
             * does not depend on which worker reached it first. The other
             * workers keep going: blocks are independent, and finishing
             * the scan is what makes the choice deterministic. */
            zgec_mu_lock(jb->mu);
            if (*jb->err == ZGEC_OK || i < *jb->err_idx) {
                *jb->err = e;
                *jb->err_idx = i;
            }
            zgec_mu_unlock(jb->mu);
        }
    }
}

#if defined(_WIN32)
static DWORD WINAPI zgec_dec_blocks_proc_win(LPVOID arg)
{
    zgec_dec_blocks_run((zgec_dec_block_job *)arg);
    return (DWORD)0;
}
#else
static void *zgec_dec_blocks_proc_posix(void *arg)
{
    zgec_dec_blocks_run((zgec_dec_block_job *)arg);
    return NULL;
}
#endif

/* Spawn w workers; a job whose thread cannot start runs inline. */
static void zgec_dec_blocks_spawn(zgec_dec_block_job *jobs, size_t w)
{
#if defined(_WIN32)
    HANDLE *hs;
    size_t t;
    if (jobs == NULL || w == 0u) return;
    hs = (HANDLE *)zgec_alloc(w * sizeof(*hs), _Alignof(HANDLE));
    if (hs == NULL) {
        for (t = 0; t < w; t++) zgec_dec_blocks_run(&jobs[t]);
        return;
    }
    for (t = 0; t < w; t++) {
        hs[t] = CreateThread(NULL, 0, zgec_dec_blocks_proc_win,
                             (LPVOID)&jobs[t], 0, NULL);
        if (hs[t] == NULL) zgec_dec_blocks_run(&jobs[t]); /* fallback */
    }
    /* Compact out NULL handles: waiting on a NULL entry fails at once
     * while live workers are still writing into the frame output. */
    {
        DWORD nv = 0;
        for (t = 0; t < w; t++) {
            if (hs[t] != NULL) hs[nv++] = hs[t];
        }
        if (nv > 0u) (void)WaitForMultipleObjects(nv, hs, TRUE, INFINITE);
        for (t = 0; t < (size_t)nv; t++) (void)CloseHandle(hs[t]);
    }
    zgec_free(hs);
#else
    pthread_t *ths;
    unsigned char *started;
    size_t t;
    if (jobs == NULL || w == 0u) return;
    ths = (pthread_t *)zgec_alloc(w * sizeof(*ths), _Alignof(pthread_t));
    started = (unsigned char *)zgec_alloc(w * sizeof(*started), 1);
    if (ths == NULL || started == NULL) {
        for (t = 0; t < w; t++) zgec_dec_blocks_run(&jobs[t]);
        zgec_free(ths);
        zgec_free(started);
        return;
    }
    memset(started, 0, w * sizeof(*started));
    for (t = 0; t < w; t++) {
        if (pthread_create(&ths[t], NULL, zgec_dec_blocks_proc_posix,
                           (void *)&jobs[t]) == 0) {
            started[t] = 1;
        } else {
            zgec_dec_blocks_run(&jobs[t]); /* fallback */
        }
    }
    for (t = 0; t < w; t++) {
        if (started[t] != 0) (void)pthread_join(ths[t], NULL);
    }
    zgec_free(ths);
    zgec_free(started);
#endif
}

/* A block worker: the parent's level and limits, serial inside the block
 * (the block parallelism is what uses the cores), borrowing the parent's
 * external dictionaries so the bytes are not copied once per worker. */
static zgec_decoder *dec_block_worker_create(const zgec_decoder *parent,
                                             int inner)
{
    zgec_limits lim = parent->limits;
    zgec_decoder *w;
    lim.n_threads = (inner > 0) ? inner : 1;
    w = zgec_decoder_create(parent->level, &lim);
    if (!w) return NULL;
    memcpy(w->ext, parent->ext, sizeof(w->ext));
    w->ext_borrowed = 1;
    return w;
}

/* Take the block-parallel path when it applies. *took is set to 1 once
 * this path owns the frame (whether it succeeds or fails), and left 0
 * when the caller must fall back to the sequential scan. */
static zgec_err zgec_dec_frame_blocks(zgec_decoder *d, const uint8_t *src,
                                      size_t src_size,
                                      const zgec_frame_header *fh,
                                      const zgec_footer *f, size_t scan_end,
                                      size_t n_workers, int *took,
                                      uint8_t **dst, size_t *dst_size)
{
    uint32_t nblk = f->block_count;
    size_t *off = NULL;
    size_t *sz = NULL;
    uint8_t *out = NULL;
    zgec_decoder **ws = NULL;
    zgec_dec_block_job *jobs = NULL;
    zgec_mu mu;
    uint32_t next = 0;
    uint32_t err_idx = nblk;
    zgec_err werr = ZGEC_OK;
    zgec_err err = ZGEC_OK;
    uint64_t total = 0;
    uint32_t done = 0;
    size_t off_rec;
    size_t t;
    size_t inner;
    int mu_ok = 0;

    *took = 0;
    if ((uint64_t)nblk * (uint64_t)sizeof(*off) > (uint64_t)SIZE_MAX) { err = ZGEC_ERR_NOMEM; goto done; }
    off = (size_t *)zgec_alloc((size_t)nblk * sizeof(*off), _Alignof(size_t));
    sz = (size_t *)zgec_alloc((size_t)nblk * sizeof(*sz), _Alignof(size_t));
    if (off == NULL || sz == NULL) {
        err = ZGEC_ERR_NOMEM;
        goto done;
    }

    /* Pass 1: walk the records exactly as the sequential scan does, but
     * only to validate the tiling and size every block; no payload is
     * decoded here. */
    off_rec = 32u;
    while (off_rec + 24u <= scan_end) {
        zgec_record_header rh;
        size_t rec_size;
        err = zgec_record_header_parse(&rh, src + off_rec, fh);
        if (err != ZGEC_OK) goto done;
        if ((uint64_t)off_rec + 24u + (uint64_t)rh.payload_size >
            (uint64_t)scan_end) {
            err = ZGEC_ERR_TRUNCATED; /* V8 */
            goto done;
        }
        {
            uint64_t rs64 = 24u + (uint64_t)rh.payload_size;
            if (rs64 > (uint64_t)SIZE_MAX) { err = ZGEC_ERR_TRUNCATED; goto done; }
            rec_size = (size_t)rs64;
        }
        if ((rh.rflags & ZGEC_RFLAG_FILTERED) != 0 &&
            rh.record_type != ZGEC_REC_COMPRESSED) {
            err = ZGEC_ERR_RESERVED; /* V11 */
            goto done;
        }
        if (rh.record_type >= ZGEC_REC_SKIPPABLE) {
            /* Skippable: ignored via payload_size. */
        } else if (rh.record_type == ZGEC_REC_DICT) {
            /* Embedded dictionary: the sequential scan validates it
             * exhaustively (4.4), so hand the whole frame over to it. */
            goto done;
        } else if (rh.record_type == ZGEC_REC_RAW ||
                   rh.record_type == ZGEC_REC_RLE) {
            if (rh.record_type == ZGEC_REC_RAW &&
                rh.payload_size != rh.raw_size) {
                err = ZGEC_ERR_RECORD_SIZE;
                goto done;
            }
            if (rh.record_type == ZGEC_REC_RLE && rh.payload_size != 1u) {
                err = ZGEC_ERR_RECORD_SIZE;
                goto done;
            }
            if (dec_check_raw_size(d, fh->block_log2, rh.raw_size) !=
                ZGEC_OK) {
                err = ZGEC_ERR_BLOCK_SIZE;
                goto done;
            }
            if ((fh->flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
                (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
                err = ZGEC_ERR_CHECKSUM; /* V10 */
                goto done;
            }
        } else if (rh.record_type == ZGEC_REC_COMPRESSED) {
            if (rh.lit_ref_depth > 0 && d->level == ZGEC_LEVEL_CORE) {
                err = ZGEC_ERR_LITREF_EXPORT; /* V9 */
                goto done;
            }
            /* Mirror the sequential path (decode_compressed): bound the
             * segment count and the raw size up front so an oversize
             * raw_size cannot pass Pass 1 and inflate total into a
             * pre-decode OOM before workers reject it. */
            if (rh.segment_count < 1u ||
                rh.segment_count > ZGEC_MAX_SEGMENTS) {
                err = ZGEC_ERR_SEGMENT_COUNT;
                goto done;
            }
            if (dec_check_raw_size(d, fh->block_log2, rh.raw_size) !=
                ZGEC_OK) {
                err = ZGEC_ERR_BLOCK_SIZE;
                goto done;
            }
            if ((fh->flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
                (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
                err = ZGEC_ERR_CHECKSUM; /* V10 */
                goto done;
            }
        } else {
            err = ZGEC_ERR_RECORD_TYPE; /* 0x04..0x7F reserved */
            goto done;
        }
        if (rh.record_type < ZGEC_REC_SKIPPABLE &&
            rh.record_type != ZGEC_REC_DICT) {
            if (done >= nblk) {
                err = ZGEC_ERR_CONTENT_SIZE;
                goto done;
            }
            /* The footer index must agree with the record order, since
             * each worker locates its record through that index. */
            if (f->blocks[done].offset != (uint64_t)off_rec) {
                err = ZGEC_ERR_TRUNCATED;
                goto done;
            }
            sz[done] = (size_t)rh.raw_size;
            off[done] = (size_t)total;
            total += (uint64_t)rh.raw_size;
            if (total > (uint64_t)SIZE_MAX) {
                err = ZGEC_ERR_OUTPUT_SIZE;
                goto done;
            }
            done++;
        }
        off_rec += rec_size;
    }
    if (off_rec != scan_end) {
        err = ZGEC_ERR_TRUNCATED; /* V8 */
        goto done;
    }
    if (done != nblk) {
        err = ZGEC_ERR_CONTENT_SIZE;
        goto done;
    }
    if ((fh->flags & ZGEC_FLAG_CONTENT_SIZE) != 0 &&
        total != fh->content_size) {
        err = ZGEC_ERR_CONTENT_SIZE;
        goto done;
    }
    if (fh->block_count != 0 && nblk != fh->block_count) {
        err = ZGEC_ERR_BLOCK_SIZE;
        goto done;
    }
    if (f->content_size != total) {
        err = ZGEC_ERR_CONTENT_SIZE;
        goto done;
    }
    /* Frame-level total limit (§13): per-block checks alone cannot bound
     * a frame of many small blocks. */
    if (d != NULL && d->limits.max_total != 0u &&
        total > (uint64_t)d->limits.max_total) {
        err = ZGEC_ERR_NOMEM;
        goto done;
    }

    out = (uint8_t *)zgec_alloc(total ? (size_t)total : 1u, 64);
    if (out == NULL) {
        err = ZGEC_ERR_NOMEM;
        goto done;
    }

    /* Pass 2: one worker per block, each serial inside its block. */
    if (n_workers > (size_t)nblk) n_workers = (size_t)nblk;
    {
        size_t want = (d->limits.n_threads > 0) ? (size_t)d->limits.n_threads
                                                : (size_t)zgec_cpu_count();
        inner = (want > n_workers) ? (want / n_workers) : 1u;
        if (inner < 1u) inner = 1u;
    }
    ws = (zgec_decoder **)zgec_alloc(n_workers * sizeof(*ws),
                                     _Alignof(zgec_decoder *));
    if (ws) memset(ws, 0, n_workers * sizeof(*ws));
    jobs = (zgec_dec_block_job *)zgec_alloc(n_workers * sizeof(*jobs),
                                            _Alignof(zgec_dec_block_job));
    if (jobs) memset(jobs, 0, n_workers * sizeof(*jobs));
    if (ws == NULL || jobs == NULL) {
        err = ZGEC_ERR_NOMEM;
        goto done;
    }
    for (t = 0; t < n_workers; t++) {
        ws[t] = dec_block_worker_create(d, (int)inner);
        if (ws[t] == NULL) {
            err = ZGEC_ERR_NOMEM;
            goto done;
        }
    }
    zgec_mu_init(&mu);
    if (mu.ok == 0) {
        err = ZGEC_ERR_NOMEM;
        goto done;
    }
    mu_ok = 1;
    for (t = 0; t < n_workers; t++) {
        jobs[t].w = ws[t];
        jobs[t].src = src;
        jobs[t].src_size = src_size;
        jobs[t].fh = fh;
        jobs[t].f = f;
        jobs[t].off = off;
        jobs[t].sz = sz;
        jobs[t].out = out;
        jobs[t].nblk = nblk;
        jobs[t].mu = &mu;
        jobs[t].next = &next;
        jobs[t].err = &werr;
        jobs[t].err_idx = &err_idx;
    }
    *took = 1;
    zgec_dec_blocks_spawn(jobs, n_workers);
    if (werr != ZGEC_OK) {
        err = werr;
        goto done;
    }
    *dst = out;
    *dst_size = (size_t)total;
    out = NULL;

done:
    if (mu_ok != 0) zgec_mu_destroy(&mu);
    if (ws != NULL) {
        for (t = 0; t < n_workers && ws != NULL; t++) {
            if (ws[t] != NULL) zgec_decoder_destroy(ws[t]);
        }
    }
    zgec_free(ws);
    zgec_free(jobs);
    zgec_free(off);
    zgec_free(sz);
    zgec_free(out);
    return err;
}

zgec_err zgec_decode_frame(zgec_decoder *d,
                            const uint8_t *src, size_t src_size,
                            uint8_t **dst, size_t *dst_size)
{
    zgec_frame_header fh;
    zgec_err err;
    size_t scan_end;
    size_t offset = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t n_blocks = 0;
    int have_footer = 0;
    zgec_footer footer;
    zgec_trailer trailer;

    if (!d || !src || !dst || !dst_size) return ZGEC_ERR_INVAL;
    if (src_size < 32) return ZGEC_ERR_TRUNCATED;
    memset(&footer, 0, sizeof(footer));
    memset(&trailer, 0, sizeof(trailer));
    err = zgec_frame_header_parse(&fh, src);
    if (err != ZGEC_OK) return err; /* V7 */
    if (d->level == ZGEC_LEVEL_CORE &&
        (fh.flags & ZGEC_FLAG_EXTENDED_LITREF) != 0)
        return ZGEC_ERR_LITREF_EXPORT; /* V9: Core gating */

    /* Footer when present (CRC verified), else scanning fallback. */
    scan_end = src_size;
    if ((fh.flags & ZGEC_FLAG_HAS_FOOTER) != 0) {
        if (src_size < 16) return ZGEC_ERR_TRUNCATED;
        if (zgec_rd32(src + src_size - 4) != ZGEC_TRAILER_MAGIC_U32)
            return ZGEC_ERR_MAGIC;
        err = zgec_trailer_parse(&trailer, src + src_size - 16);
        if (err != ZGEC_OK) return err;
        if (trailer.footer_offset + (uint64_t)trailer.footer_size + 16u !=
            (uint64_t)src_size)
            return ZGEC_ERR_TRUNCATED;
        if (trailer.footer_offset > (uint64_t)src_size ||
            (size_t)trailer.footer_offset < 32)
            return ZGEC_ERR_TRUNCATED;
        err = zgec_footer_parse(&footer, src + (size_t)trailer.footer_offset,
                                trailer.footer_size);
        if (err != ZGEC_OK) return err; /* footer CRC (V10 scope) */
        have_footer = 1;
        scan_end = (size_t)trailer.footer_offset;
    } else if (src_size >= 16 &&
               zgec_rd32(src + src_size - 4) == ZGEC_TRAILER_MAGIC_U32) {
        /* Trailer present without the flag: still honour it. */
        if (zgec_trailer_parse(&trailer, src + src_size - 16) == ZGEC_OK &&
            trailer.footer_offset + (uint64_t)trailer.footer_size + 16u ==
                (uint64_t)src_size &&
            trailer.footer_offset >= 32 &&
            trailer.footer_offset < (uint64_t)src_size) {
            if (zgec_footer_parse(&footer,
                                  src + (size_t)trailer.footer_offset,
                                  trailer.footer_size) == ZGEC_OK) {
                have_footer = 1;
                scan_end = (size_t)trailer.footer_offset;
            }
        }
    }

    /* Block-parallel decode (10.6). Falls back to the scan below when the
     * frame carries embedded dictionaries or has only one block. */
    if (have_footer && footer.block_count > 0u) {
        size_t nw = dec_resolve_workers(d, (size_t)footer.block_count);
        if (nw > 1u) {
            int took = 0;
            err = zgec_dec_frame_blocks(d, src, src_size, &fh, &footer,
                                        scan_end, nw, &took, dst, dst_size);
            if (took != 0) {
                zgec_footer_free(&footer);
                return err;
            }
        }
    }

    /* Reserve the output once when the footer states its size. Growing it
     * block by block would re-copy everything appended so far on each
     * growth step. The claim is clamped to what the frame's own block count
     * and block size can possibly produce (every block record contributes
     * at most 2^block_log2 bytes), so a bogus content_size cannot make the
     * decoder reserve arbitrary memory; a claim that is too small or too
     * large is still caught by the content-size checks at the end, and if
     * the reservation fails the buffer simply grows as before. */
    if (have_footer && footer.content_size > 0u) {
        uint64_t bcap = dec_block_cap(fh.block_log2);
        if (bcap != 0u) {
            uint64_t bound;
            uint64_t want = footer.content_size;
            uint64_t max_total = (uint64_t)d->limits.max_total;
            /* bcap * block_count in u64 with an overflow guard. */
            if (footer.block_count != 0u &&
                bcap > UINT64_MAX / (uint64_t)footer.block_count)
                bound = UINT64_MAX;
            else
                bound = bcap * (uint64_t)footer.block_count;
            if (want > bound) want = bound;
            if (max_total != 0u && want > max_total) want = max_total;
            if (want > 0u && want <= (uint64_t)SIZE_MAX) {
                out = (uint8_t *)zgec_alloc((size_t)want, 64);
                if (out != NULL) out_cap = (size_t)want;
            }
        }
    }

    offset = 32;
    while (offset + 24 <= scan_end) {
        zgec_record_header rh;
        size_t rec_size;
        const uint8_t *payload;
        err = zgec_record_header_parse(&rh, src + offset, &fh);
        if (err != ZGEC_OK) {
            zgec_free(out);
            if (have_footer) zgec_footer_free(&footer);
            return err;
        }
        if ((uint64_t)offset + 24u + (uint64_t)rh.payload_size > scan_end) {
            zgec_free(out);
            if (have_footer) zgec_footer_free(&footer);
            return ZGEC_ERR_TRUNCATED; /* V8 */
        }
        {
            uint64_t rs64 = 24u + (uint64_t)rh.payload_size;
            if (rs64 > (uint64_t)SIZE_MAX) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_TRUNCATED;
            }
            rec_size = (size_t)rs64;
        }
        payload = src + offset + 24;

        if ((rh.rflags & ZGEC_RFLAG_FILTERED) != 0 &&
            rh.record_type != ZGEC_REC_COMPRESSED) {
            zgec_free(out);
            if (have_footer) zgec_footer_free(&footer);
            return ZGEC_ERR_RESERVED; /* V11 */
        }

        if (rh.record_type >= ZGEC_REC_SKIPPABLE) {
            /* Skippable 0x80..0xFF: ignore via payload_size. */
        } else if (rh.record_type == ZGEC_REC_RAW) {
            if (rh.payload_size != rh.raw_size) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_RECORD_SIZE;
            }
            if (dec_check_raw_size(d, fh.block_log2, rh.raw_size) !=
                ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_BLOCK_SIZE;
            }
            if ((fh.flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
                (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_CHECKSUM; /* V10 */
            }
            if ((rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0) {
                uint32_t crc = zgec_crc32c(payload, rh.raw_size, 0u);
                if (crc != rh.checksum) { /* V10 */
                    zgec_free(out);
                    if (have_footer) zgec_footer_free(&footer);
                    return ZGEC_ERR_CHECKSUM;
                }
            }
            err = frame_output_append(&out, &out_len, &out_cap, payload,
                                      rh.raw_size,
                                      (uint64_t)d->limits.max_total);
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            hist_push(d, payload, rh.raw_size,
                      (rh.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0);
            n_blocks++;
        } else if (rh.record_type == ZGEC_REC_RLE) {
            uint8_t *tmp = NULL;
            if (rh.payload_size != 1) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_RECORD_SIZE;
            }
            if (dec_check_raw_size(d, fh.block_log2, rh.raw_size) !=
                ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_BLOCK_SIZE;
            }
            if ((fh.flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
                (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_CHECKSUM; /* V10 */
            }
            tmp = (uint8_t *)zgec_alloc(
                rh.raw_size ? (size_t)rh.raw_size : 1, 64);
            if (!tmp) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_NOMEM;
            }
            memset(tmp, payload[0], (size_t)rh.raw_size);
            if ((rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0) {
                uint32_t crc = zgec_crc32c(tmp, rh.raw_size, 0u);
                if (crc != rh.checksum) { /* V10 */
                    zgec_free(tmp);
                    zgec_free(out);
                    if (have_footer) zgec_footer_free(&footer);
                    return ZGEC_ERR_CHECKSUM;
                }
            }
            err = frame_output_append(&out, &out_len, &out_cap, tmp,
                                      rh.raw_size,
                                      (uint64_t)d->limits.max_total);
            hist_push(d, tmp, rh.raw_size,
                      (rh.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0);
            zgec_free(tmp);
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            n_blocks++;
        } else if (rh.record_type == ZGEC_REC_COMPRESSED) {
            zgec_block_arrays *ba = NULL;
            const zgec_dict *dict = NULL;
            uint8_t *lits = NULL;
            size_t lits_len = 0;
            int exp_ok = 0;
            uint8_t D = rh.lit_ref_depth;
            if (D > 0 && d->level == ZGEC_LEVEL_CORE) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_LITREF_EXPORT; /* V9 gating */
            }
            if (D > 0) {
                size_t need = 0;
                uint8_t k8;
                if (d->hist_n < (size_t)D) {
                    zgec_free(out);
                    if (have_footer) zgec_footer_free(&footer);
                    return ZGEC_ERR_LITREF_DEPTH; /* V9 */
                }
                for (k8 = 0; k8 < D; k8++) {
                    unsigned slot =
                        (unsigned)((d->hist_n - (size_t)D + (size_t)k8) % 3u);
                    if (!d->hist_ok[slot]) {
                        zgec_free(out);
                        if (have_footer) zgec_footer_free(&footer);
                        return ZGEC_ERR_LITREF_EXPORT; /* V9 */
                    }
                    need += d->hist_sz[slot];
                }
                if (need > 0) {
                    size_t pp = 0;
                    lits = (uint8_t *)zgec_alloc(need, 64);
                    if (!lits) {
                        zgec_free(out);
                        if (have_footer) zgec_footer_free(&footer);
                        return ZGEC_ERR_NOMEM;
                    }
                    for (k8 = 0; k8 < D; k8++) {
                        unsigned slot =
                            (unsigned)((d->hist_n - (size_t)D + (size_t)k8) %
                                       3u);
                        if (d->hist_sz[slot] > 0)
                            memcpy(lits + pp, d->hist_lit[slot],
                                   d->hist_sz[slot]);
                        pp += d->hist_sz[slot];
                    }
                    lits_len = need;
                }
            }
            if (rh.dict_id != 0) {
                /* 4.6 step 3 / V9: cache, then registered external
                 * dictionaries, then the embedded record. */
                err = fetch_dict(d, &fh, &footer, have_footer, rh.dict_id,
                                 src, src_size, &dict);
                if (err != ZGEC_OK) {
                    zgec_free(lits);
                    zgec_free(out);
                    if (have_footer) zgec_footer_free(&footer);
                    return err; /* V9 */
                }
            }
            if ((fh.flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
                (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
                zgec_free(lits);
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_CHECKSUM; /* V10 */
            }
            {
                /* Spec 4.3/7.5: a FILTERED COMPRESSED payload starts with
                 * the 2-byte filter descriptor; V11 checks it. */
                const uint8_t *cpayload = payload;
                size_t cpsz = (size_t)rh.payload_size;
                int fmode = 0;
                int fparam = 0;
                if ((rh.rflags & ZGEC_RFLAG_FILTERED) != 0) {
                    if (D != 0 ||
                        (rh.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0 ||
                        cpsz < 2u) {
                        zgec_free(lits);
                        zgec_free(out);
                        if (have_footer) zgec_footer_free(&footer);
                        return ZGEC_ERR_RESERVED; /* V11 */
                    }
                    fmode = (int)payload[0];
                    fparam = (int)payload[1];
                    if (zgec_filter_validate((unsigned)fmode,
                                             (unsigned)fparam) != ZGEC_OK) {
                        zgec_free(lits);
                        zgec_free(out);
                        if (have_footer) zgec_footer_free(&footer);
                        return ZGEC_ERR_RESERVED; /* V11 */
                    }
                    cpayload = payload + 2;
                    cpsz -= 2u;
                }
                err = decode_compressed(
                    &ba, d, &fh, cpayload, cpsz,
                    rh.segment_count, dict, lits, lits_len, rh.raw_size,
                    rh.checksum,
                    (rh.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0,
                    &exp_ok, fmode, fparam);
            }
            zgec_free(lits);
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            err = frame_output_append(&out, &out_len, &out_cap, ba->out,
                                      ba->raw_size,
                                      (uint64_t)d->limits.max_total);
            if (err == ZGEC_OK) {
                /* Save literal buffer for later litref predecessors.
                 * The assembled buffer is adopted by the history
                 * (no second copy). */
                size_t tl = 0;
                size_t s2;
                int hexp = ((rh.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0) &&
                           (exp_ok != 0);
                for (s2 = 0; s2 < ba->n_seg; s2++)
                    tl += ba->segs[s2].n_lit;
                if (tl > 0) {
                    uint8_t *lb =
                        (uint8_t *)zgec_alloc(tl, 64);
                    if (lb) {
                        size_t qq = 0;
                        for (s2 = 0; s2 < ba->n_seg; s2++) {
                            if (ba->segs[s2].n_lit > 0)
                                memcpy(lb + qq, ba->segs[s2].lit,
                                       ba->segs[s2].n_lit);
                            qq += ba->segs[s2].n_lit;
                        }
                        hist_push_owned(d, lb, tl, hexp);
                    } else {
                        hist_push(d, NULL, 0, 0);
                    }
                } else {
                    hist_push(d, NULL, 0, hexp);
                }
            }
            zgec_block_arrays_free(ba);
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            n_blocks++;
        } else if (rh.record_type == ZGEC_REC_DICT) {
            zgec_record_header irh;
            zgec_dict *nd = NULL;
            if (rh.payload_size < 24) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_TRUNCATED;
            }
            err = zgec_record_header_parse(&irh, payload, &fh);
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            if (irh.record_type != ZGEC_REC_RAW &&
                irh.record_type != ZGEC_REC_RLE &&
                irh.record_type != ZGEC_REC_COMPRESSED) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_RECORD_TYPE;
            }
            if (irh.dict_id != 0) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_DICT_ID;
            }
            if (irh.lit_ref_depth != 0) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_LITREF_DEPTH;
            }
            if (irh.raw_size != rh.raw_size) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return ZGEC_ERR_DICT_SIZE;
            }
            {
                /* 4.4: verify an embedded dictionary against the
                 * footer content_hash on the sequential path too. */
                uint64_t want_hash = 0;
                if (have_footer) {
                    const zgec_footer_dict_entry *de =
                        find_dict_entry(&footer, rh.dict_id);
                    if (de != NULL) want_hash = de->content_hash;
                }
                err = zgec_dict_decode(&nd, payload, (size_t)rh.payload_size,
                                       rh.raw_size, want_hash, &fh);
            }
            if (err != ZGEC_OK) {
                zgec_free(out);
                if (have_footer) zgec_footer_free(&footer);
                return err;
            }
            nd->dict_id = rh.dict_id;
            dec_cache_put(d, nd);
            zgec_free(nd); /* put detached the buffer; the shell is ours */
        } else {
            /* 0x04..0x7F reserved: reject. */
            zgec_free(out);
            if (have_footer) zgec_footer_free(&footer);
            return ZGEC_ERR_RECORD_TYPE;
        }
        offset += rec_size;
    }
    if (offset != scan_end) {
        zgec_free(out);
        if (have_footer) zgec_footer_free(&footer);
        return ZGEC_ERR_TRUNCATED; /* V8 */
    }
    if ((fh.flags & ZGEC_FLAG_CONTENT_SIZE) != 0 &&
        (uint64_t)out_len != fh.content_size) {
        zgec_free(out);
        if (have_footer) zgec_footer_free(&footer);
        return ZGEC_ERR_CONTENT_SIZE;
    }
    if (fh.block_count != 0 && (uint64_t)n_blocks != fh.block_count) {
        zgec_free(out);
        if (have_footer) zgec_footer_free(&footer);
        return ZGEC_ERR_BLOCK_SIZE;
    }
    if (have_footer) {
        if (footer.block_count != (uint32_t)n_blocks) {
            zgec_free(out);
            zgec_footer_free(&footer);
            return ZGEC_ERR_CONTENT_SIZE;
        }
        if (footer.content_size != (uint64_t)out_len) {
            zgec_free(out);
            zgec_footer_free(&footer);
            return ZGEC_ERR_CONTENT_SIZE;
        }
        zgec_footer_free(&footer);
    }
    if (out == NULL) {
        out = (uint8_t *)zgec_alloc(1, 64);
        if (!out) return ZGEC_ERR_NOMEM;
    }
    *dst = out;
    *dst_size = out_len;
    return ZGEC_OK;
}

/* ---- random access (section 4.6) ---- */

/* Block output goes to the caller's buffer when one is supplied and large
 * enough (the block-parallel frame path, where each worker writes straight
 * into its slice of the frame output), otherwise to the decoder's own
 * scratch buffer as for a single random-access read. */
static uint8_t *dec_block_dst(zgec_decoder *d, uint8_t *ovr, size_t cap,
                              size_t need)
{
    if (ovr != NULL && cap >= need) return ovr;
    if (d->block_buf_cap < need) {
        uint8_t *nb = (uint8_t *)zgec_alloc(need ? need : 1, 64);
        if (!nb) return NULL;
        zgec_free(d->block_buf);
        d->block_buf = nb;
        d->block_buf_cap = need;
    }
    return d->block_buf;
}

static zgec_err decode_block_record(zgec_decoder *d, const uint8_t *src,
                                     size_t src_size,
                                     const zgec_frame_header *fh,
                                     const zgec_footer *f,
                                     const zgec_footer_block_entry *be,
                                     uint8_t *ovr, size_t ovr_cap,
                                     const uint8_t **rdst, size_t *rsz)
{
    const uint8_t *rh;
    zgec_record_header rec;
    const uint8_t *payload;
    zgec_err e;
    const zgec_dict *dict = NULL;
    uint8_t *litref = NULL;
    size_t litref_len = 0;
    zgec_block_arrays *ba = NULL;
    int dummy_ok = 0;
    int filter_mode = 0;
    int filter_param = 0;

    if (be->offset + 24u > (uint64_t)src_size) return ZGEC_ERR_TRUNCATED;
    rh = src + (size_t)be->offset;
    e = zgec_record_header_parse(&rec, rh, fh);
    if (e != ZGEC_OK) return e;
    if (be->offset + 24u + (uint64_t)rec.payload_size >
        (uint64_t)src_size)
        return ZGEC_ERR_TRUNCATED; /* V8 */
    if ((uint64_t)rec.payload_size + 24u != (uint64_t)be->record_size)
        return ZGEC_ERR_TRUNCATED;
    payload = rh + 24;

    /* V11: a FILTERED record is COMPRESSED only. RAW/RLE/DICT carry no
     * filter descriptor, so the bit set there is rejected. */
    if ((rec.rflags & ZGEC_RFLAG_FILTERED) != 0 &&
        rec.record_type != ZGEC_REC_COMPRESSED) {
        return ZGEC_ERR_RESERVED;
    }

    /* V10: when the frame announces block checksums, every block record
     * MUST carry one. Checked here so the random-access and block-parallel
     * paths enforce it exactly as the sequential scan does. */
    if ((fh->flags & ZGEC_FLAG_BLOCK_CHECKSUMS) != 0 &&
        (rec.rflags & ZGEC_RFLAG_HAS_CHECKSUM) == 0) {
        return ZGEC_ERR_CHECKSUM;
    }

    if (rec.record_type == ZGEC_REC_RAW) {
        uint8_t *bp;
        if (rec.payload_size != rec.raw_size) return ZGEC_ERR_RECORD_SIZE;
        if (dec_check_raw_size(d, fh->block_log2, rec.raw_size) != ZGEC_OK)
            return ZGEC_ERR_BLOCK_SIZE;
        bp = dec_block_dst(d, ovr, ovr_cap, (size_t)rec.raw_size);
        if (bp == NULL) return ZGEC_ERR_NOMEM;
        if (rec.raw_size > 0) memcpy(bp, payload, rec.raw_size);
        if ((rec.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0) {
            uint32_t crc = zgec_crc32c(bp, rec.raw_size, 0u);
            if (crc != rec.checksum) return ZGEC_ERR_CHECKSUM; /* V10 */
        }
        *rdst = bp;
        *rsz = (size_t)rec.raw_size;
        return ZGEC_OK;
    }
    if (rec.record_type == ZGEC_REC_RLE) {
        uint8_t *bp;
        if (rec.payload_size != 1) return ZGEC_ERR_RECORD_SIZE;
        if (dec_check_raw_size(d, fh->block_log2, rec.raw_size) != ZGEC_OK)
            return ZGEC_ERR_BLOCK_SIZE;
        bp = dec_block_dst(d, ovr, ovr_cap, (size_t)rec.raw_size);
        if (bp == NULL) return ZGEC_ERR_NOMEM;
        memset(bp, payload[0], (size_t)rec.raw_size);
        if ((rec.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0) {
            uint32_t crc = zgec_crc32c(bp, rec.raw_size, 0u);
            if (crc != rec.checksum) return ZGEC_ERR_CHECKSUM; /* V10 */
        }
        *rdst = bp;
        *rsz = (size_t)rec.raw_size;
        return ZGEC_OK;
    }
    if (rec.record_type != ZGEC_REC_COMPRESSED) return ZGEC_ERR_RECORD_TYPE;

    /* Dictionary (4.6 step 3). */
    e = fetch_dict(d, fh, f, 1, rec.dict_id, src, src_size, &dict);
    if (e != ZGEC_OK) return e; /* V9 */

    /* Literal references: export blocks b-D..b-1 (4.6 step 4, 6.3).
     * Each predecessor is exported once into a side buffer; the sizes
     * are pre-summed so the result needs a single allocation and one
     * copy pass (no per-predecessor realloc + re-copy). */
    if (rec.lit_ref_depth > 0) {
        uint32_t b;
        uint8_t k8;
        uint8_t depth = rec.lit_ref_depth;
        uint8_t *parts[ZGEC_MAX_LITREF_DEPTH];
        size_t part_sz[ZGEC_MAX_LITREF_DEPTH];
        size_t total = 0;
        size_t wpos = 0;
        if (d->level == ZGEC_LEVEL_CORE) return ZGEC_ERR_LITREF_EXPORT; /* V9 */
        if (depth > ZGEC_MAX_LITREF_DEPTH) return ZGEC_ERR_LITREF_DEPTH;
        /* Find this block's index: caller passes it via be lookup. */
        b = 0;
        for (b = 0; b < f->block_count; b++) {
            if (f->blocks[b].offset == be->offset) break;
        }
        if (b >= f->block_count) return ZGEC_ERR_INTERNAL;
        if ((uint32_t)depth > b) return ZGEC_ERR_LITREF_DEPTH;
        for (k8 = 0; k8 < depth; k8++) {
            parts[k8] = NULL;
            part_sz[k8] = 0;
        }
        for (k8 = 0; k8 < depth; k8++) {
            uint32_t pb = b - (uint32_t)depth + (uint32_t)k8;
            const zgec_footer_block_entry *pe = &f->blocks[pb];
            const uint8_t *prh;
            zgec_record_header pr;
            const uint8_t *ppay;
            uint8_t *elb = NULL;
            size_t elsz = 0;
            uint8_t j8;
            if (pe->offset + 24u > (uint64_t)src_size) {
                e = ZGEC_ERR_TRUNCATED;
                goto litref_fail;
            }
            prh = src + (size_t)pe->offset;
            e = zgec_record_header_parse(&pr, prh, fh);
            if (e != ZGEC_OK) goto litref_fail;
            if (pe->offset + 24u + (uint64_t)pr.payload_size >
                (uint64_t)src_size) {
                e = ZGEC_ERR_TRUNCATED;
                goto litref_fail;
            }
            if ((pr.rflags & ZGEC_RFLAG_FILTERED) != 0) {
                /* V11: a FILTERED block is not a literal-reference
                 * predecessor. */
                e = ZGEC_ERR_LITREF_EXPORT;
                goto litref_fail;
            }
            if ((pr.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) == 0) {
                e = ZGEC_ERR_LITREF_EXPORT; /* V9 */
                goto litref_fail;
            }
            ppay = prh + 24;
            if (pr.record_type == ZGEC_REC_RAW) {
                if (pr.payload_size != pr.raw_size) {
                    e = ZGEC_ERR_RECORD_SIZE;
                    goto litref_fail;
                }
                elsz = (size_t)pr.raw_size;
                if (elsz > 0) {
                    elb = (uint8_t *)zgec_alloc(elsz, 64);
                    if (!elb) {
                        e = ZGEC_ERR_NOMEM;
                        goto litref_fail;
                    }
                    memcpy(elb, ppay, elsz);
                }
            } else if (pr.record_type == ZGEC_REC_RLE) {
                if (pr.payload_size != 1u) {
                    e = ZGEC_ERR_RECORD_SIZE;
                    goto litref_fail;
                }
                elsz = (size_t)pr.raw_size;
                if (elsz > 0) {
                    elb = (uint8_t *)zgec_alloc(elsz, 64);
                    if (!elb) {
                        e = ZGEC_ERR_NOMEM;
                        goto litref_fail;
                    }
                    memset(elb, ppay[0], elsz);
                }
            } else if (pr.record_type == ZGEC_REC_COMPRESSED) {
                e = export_block_literals(ppay, (size_t)pr.payload_size,
                                          pr.segment_count, &elb, &elsz);
                if (e != ZGEC_OK) goto litref_fail;
                if (elsz == 0) {
                    zgec_free(elb);
                    elb = NULL;
                }
            } else {
                e = ZGEC_ERR_RECORD_TYPE;
                goto litref_fail;
            }
            if (total + elsz < total) {
                zgec_free(elb);
                e = ZGEC_ERR_OUTPUT_SIZE;
                goto litref_fail;
            }
            parts[k8] = elb;
            part_sz[k8] = elsz;
            total += elsz;
            continue;
litref_fail:
            zgec_free(elb);
            for (j8 = 0; j8 < k8; j8++) zgec_free(parts[j8]);
            return e;
        }
        if (total > 0) {
            litref = (uint8_t *)zgec_alloc(total, 64);
            if (!litref) {
                for (k8 = 0; k8 < depth; k8++) zgec_free(parts[k8]);
                return ZGEC_ERR_NOMEM;
            }
            for (k8 = 0; k8 < depth; k8++) {
                if (part_sz[k8] > 0) {
                    memcpy(litref + wpos, parts[k8], part_sz[k8]);
                    wpos += part_sz[k8];
                }
                zgec_free(parts[k8]);
            }
        }
        litref_len = total;
    }

    {
        /* Spec 4.3: when FILTERED is set the payload starts with the
         * 2-byte filter descriptor (7.5); V11 checks it and the block
         * flags before the inverse pass. */
        const uint8_t *cpayload = payload;
        size_t cpsize = (size_t)rec.payload_size;
        if ((rec.rflags & ZGEC_RFLAG_FILTERED) != 0) {
            if (rec.lit_ref_depth != 0) {
                zgec_free(litref);
                return ZGEC_ERR_LITREF_DEPTH; /* V11 */
            }
            if ((rec.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0) {
                zgec_free(litref);
                return ZGEC_ERR_RESERVED; /* V11 */
            }
            if (cpsize < 2u) {
                zgec_free(litref);
                return ZGEC_ERR_TRUNCATED; /* V11 */
            }
            filter_mode = (int)payload[0];
            filter_param = (int)payload[1];
            if (zgec_filter_validate((unsigned)filter_mode,
                                     (unsigned)filter_param) != ZGEC_OK) {
                zgec_free(litref);
                return ZGEC_ERR_RESERVED; /* V11 */
            }
            cpayload = payload + 2;
            cpsize -= 2u;
        }
        e = decode_compressed(&ba, d, fh, cpayload, cpsize,
                              rec.segment_count, dict, litref, litref_len,
                              rec.raw_size, rec.checksum,
                              (rec.rflags & ZGEC_RFLAG_HAS_CHECKSUM) != 0,
                              &dummy_ok, filter_mode, filter_param);
    }
    zgec_free(litref);
    if (e != ZGEC_OK) return e;
    {
        uint8_t *bp = dec_block_dst(d, ovr, ovr_cap, ba->raw_size);
        if (bp == NULL) {
            zgec_block_arrays_free(ba);
            return ZGEC_ERR_NOMEM;
        }
        if (ba->raw_size > 0) memcpy(bp, ba->out, ba->raw_size);
        *rsz = ba->raw_size;
        zgec_block_arrays_free(ba);
        *rdst = bp;
    }
    return ZGEC_OK;
}

zgec_err zgec_decode_block(zgec_decoder *d,
                            const uint8_t *src, size_t src_size,
                            uint64_t offset,
                            const uint8_t **dst, size_t *dst_size)
{
    zgec_frame_header fh;
    zgec_trailer tr;
    zgec_footer f;
    zgec_err e;
    uint64_t bsize;
    uint32_t idx;
    if (!d || !src || !dst || !dst_size || src_size < 32) return ZGEC_ERR_INVAL;
    e = zgec_frame_header_parse(&fh, src);
    if (e != ZGEC_OK) return e;
    if (zgec_rd32(src + src_size - 4) != ZGEC_TRAILER_MAGIC_U32)
        return ZGEC_ERR_MAGIC;
    e = zgec_trailer_parse(&tr, src + src_size - 16);
    if (e != ZGEC_OK) return e;
    if (tr.footer_offset + (uint64_t)tr.footer_size + 16u !=
        (uint64_t)src_size)
        return ZGEC_ERR_TRUNCATED;
    memset(&f, 0, sizeof(f));
    e = zgec_footer_parse(&f, src + (size_t)tr.footer_offset,
                          tr.footer_size);
    if (e != ZGEC_OK) return e;
    if (offset >= f.content_size) {
        zgec_footer_free(&f);
        return ZGEC_ERR_BLOCK_SIZE;
    }
    bsize = (uint64_t)1 << fh.block_log2;
    idx = (uint32_t)(offset / bsize);
    zgec_footer_free(&f);
    return zgec_decode_block_index(d, src, src_size, idx, dst, dst_size);
}

zgec_err zgec_decode_block_index(zgec_decoder *d,
                                  const uint8_t *src, size_t src_size,
                                  uint32_t block_index,
                                  const uint8_t **dst, size_t *dst_size)
{
    zgec_frame_header fh;
    zgec_trailer tr;
    zgec_footer f;
    zgec_err e;
    if (!d || !src || !dst || !dst_size || src_size < 48) return ZGEC_ERR_INVAL;
    e = zgec_frame_header_parse(&fh, src);
    if (e != ZGEC_OK) return e;
    if (d->level == ZGEC_LEVEL_CORE &&
        (fh.flags & ZGEC_FLAG_EXTENDED_LITREF) != 0)
        return ZGEC_ERR_LITREF_EXPORT; /* V9 */
    if (zgec_rd32(src + src_size - 4) != ZGEC_TRAILER_MAGIC_U32)
        return ZGEC_ERR_MAGIC;
    e = zgec_trailer_parse(&tr, src + src_size - 16);
    if (e != ZGEC_OK) return e;
    if (tr.footer_offset + (uint64_t)tr.footer_size + 16u !=
        (uint64_t)src_size)
        return ZGEC_ERR_TRUNCATED;
    memset(&f, 0, sizeof(f));
    e = zgec_footer_parse(&f, src + (size_t)tr.footer_offset,
                          tr.footer_size);
    if (e != ZGEC_OK) return e;
    if (block_index >= f.block_count) {
        zgec_footer_free(&f);
        return ZGEC_ERR_BLOCK_SIZE;
    }
    e = decode_block_record(d, src, src_size, &fh, &f,
                            &f.blocks[block_index], NULL, 0, dst, dst_size);
    zgec_footer_free(&f);
    return e;
}

zgec_err zgec_export_literals(zgec_decoder *d,
                               const uint8_t *src, size_t src_size,
                               uint32_t block_index,
                               uint8_t **lit_buf, size_t *lit_size)
{
    zgec_frame_header fh;
    zgec_trailer tr;
    zgec_footer f;
    zgec_err e;
    const zgec_footer_block_entry *be;
    const uint8_t *rh;
    zgec_record_header rec;
    const uint8_t *payload;
    if (!d || !src || !lit_buf || !lit_size || src_size < 48)
        return ZGEC_ERR_INVAL;
    (void)d;
    e = zgec_frame_header_parse(&fh, src);
    if (e != ZGEC_OK) return e;
    if (zgec_rd32(src + src_size - 4) != ZGEC_TRAILER_MAGIC_U32)
        return ZGEC_ERR_MAGIC;
    e = zgec_trailer_parse(&tr, src + src_size - 16);
    if (e != ZGEC_OK) return e;
    if (tr.footer_offset + (uint64_t)tr.footer_size + 16u !=
        (uint64_t)src_size)
        return ZGEC_ERR_TRUNCATED;
    memset(&f, 0, sizeof(f));
    e = zgec_footer_parse(&f, src + (size_t)tr.footer_offset,
                          tr.footer_size);
    if (e != ZGEC_OK) return e;
    if (block_index >= f.block_count) {
        zgec_footer_free(&f);
        return ZGEC_ERR_BLOCK_SIZE;
    }
    be = &f.blocks[block_index];
    if (be->offset + 24u > (uint64_t)src_size) {
        zgec_footer_free(&f);
        return ZGEC_ERR_TRUNCATED;
    }
    rh = src + (size_t)be->offset;
    e = zgec_record_header_parse(&rec, rh, &fh);
    if (e != ZGEC_OK) {
        zgec_footer_free(&f);
        return e;
    }
    if (be->offset + 24u + (uint64_t)rec.payload_size > (uint64_t)src_size) {
        zgec_footer_free(&f);
        return ZGEC_ERR_TRUNCATED;
    }
    payload = rh + 24;
    if ((rec.rflags & ZGEC_RFLAG_FILTERED) != 0 &&
        rec.record_type != ZGEC_REC_COMPRESSED) {
        zgec_footer_free(&f);
        return ZGEC_ERR_RESERVED; /* V11 */
    }
    if (rec.record_type == ZGEC_REC_RAW) {
        uint8_t *lb = (uint8_t *)zgec_alloc(
            rec.raw_size ? (size_t)rec.raw_size : 1, 64);
        if (!lb) {
            zgec_footer_free(&f);
            return ZGEC_ERR_NOMEM;
        }
        if (rec.raw_size > 0) memcpy(lb, payload, rec.raw_size);
        *lit_buf = lb;
        *lit_size = (size_t)rec.raw_size;
        zgec_footer_free(&f);
        return ZGEC_OK;
    }
    if (rec.record_type == ZGEC_REC_RLE) {
        uint8_t *lb;
        if (rec.payload_size != 1) {
            zgec_footer_free(&f);
            return ZGEC_ERR_RECORD_SIZE;
        }
        lb = (uint8_t *)zgec_alloc(rec.raw_size ? (size_t)rec.raw_size : 1,
                                   64);
        if (!lb) {
            zgec_footer_free(&f);
            return ZGEC_ERR_NOMEM;
        }
        memset(lb, payload[0], (size_t)rec.raw_size);
        *lit_buf = lb;
        *lit_size = (size_t)rec.raw_size;
        zgec_footer_free(&f);
        return ZGEC_OK;
    }
    if (rec.record_type != ZGEC_REC_COMPRESSED) {
        zgec_footer_free(&f);
        return ZGEC_ERR_RECORD_TYPE;
    }
    if ((rec.rflags & ZGEC_RFLAG_FILTERED) != 0) {
        /* V11: a FILTERED block is never literal-exportable. */
        zgec_footer_free(&f);
        return ZGEC_ERR_LITREF_EXPORT;
    }
    if ((rec.rflags & ZGEC_RFLAG_LIT_EXPORTABLE) == 0) {
        zgec_footer_free(&f);
        return ZGEC_ERR_LITREF_EXPORT; /* V9 */
    }
    e = export_block_literals(payload, (size_t)rec.payload_size,
                              rec.segment_count, lit_buf, lit_size);
    zgec_footer_free(&f);
    return e;
}

zgec_err zgec_decoder_add_external_dict(zgec_decoder *d,
                                          uint16_t dict_id,
                                          const uint8_t *data,
                                          size_t size)
{
    unsigned i;
    uint8_t *cp = NULL;
    if (!d || !data || size == 0 || dict_id == 0) return ZGEC_ERR_INVAL;
    cp = (uint8_t *)zgec_alloc(size, 64);
    if (!cp) return ZGEC_ERR_NOMEM;
    memcpy(cp, data, size);
    for (i = 0; i < ZGEC_DEC_MAX_EXT; i++) {
        if (d->ext[i].used && d->ext[i].id == dict_id) {
            zgec_free(d->ext[i].data);
            d->ext[i].data = cp;
            d->ext[i].size = size;
            return ZGEC_OK;
        }
    }
    for (i = 0; i < ZGEC_DEC_MAX_EXT; i++) {
        if (!d->ext[i].used) {
            d->ext[i].used = 1;
            d->ext[i].id = dict_id;
            d->ext[i].data = cp;
            d->ext[i].size = size;
            return ZGEC_OK;
        }
    }
    zgec_free(cp);
    return ZGEC_ERR_NOMEM;
}
