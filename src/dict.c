#include "zgec_dict.h"

#include "zgec_bitstream.h"
#include "zgec_block.h"
#include "zgec_fse.h"
#include "zgec_lit.h"
#include "zgec_rans.h"
#include "zgec_seq.h"
#include "zgec_xxhash.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Format limits per section 3.3: dictionary at most 2^26 bytes.
 * P24 profile limit (2^24) is warn-only: larger dictionaries within the
 * format limit remain decodable, so no error is raised for them. */
#define ZGEC_DICT_RAW_MAX ((uint64_t)1 << 26)
#define ZGEC_DICT_P24_MAX ((uint64_t)1 << 24)
/* Segment literal cap: n_lit never exceeds the segment raw output. */
#define ZGEC_DICT_NLIT_MAX ((uint64_t)1 << 26)

/* ---- simple LRU cache ---- */

typedef struct zgec_dict_node {
    zgec_dict dict;
    struct zgec_dict_node *next;
} zgec_dict_node;

struct zgec_dict_cache {
    zgec_dict_node *head;
    zgec_dict_node *tail;
    size_t capacity;
    size_t size;
};

zgec_dict_cache *zgec_dict_cache_create(size_t capacity)
{
    zgec_dict_cache *c = (zgec_dict_cache *)zgec_alloc(sizeof(*c), _Alignof(zgec_dict_cache));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->capacity = capacity > 0 ? capacity : 8;
    return c;
}

void zgec_dict_cache_destroy(zgec_dict_cache *c)
{
    if (!c) return;
    zgec_dict_node *n = c->head;
    while (n) {
        zgec_dict_node *next = n->next;
        zgec_dict_free(&n->dict);
        zgec_free(n);
        n = next;
    }
    zgec_free(c);
}

const zgec_dict *zgec_dict_cache_get(zgec_dict_cache *c, uint16_t dict_id)
{
    if (!c) return NULL;
    zgec_dict_node *prev = NULL;
    for (zgec_dict_node *n = c->head; n; prev = n, n = n->next) {
        if (n->dict.dict_id == dict_id) {
            if (n != c->tail) {
                if (prev) prev->next = n->next;
                else c->head = n->next;
                n->next = NULL;
                if (c->tail) c->tail->next = n;
                else c->head = n;
                c->tail = n;
            }
            return &n->dict;
        }
    }
    return NULL;
}

void zgec_dict_cache_put(zgec_dict_cache *c, zgec_dict *d)
{
    if (!c || !d) return;
    if (c->capacity == 0) c->capacity = 8;
    /* Zero-size / NULL-data guard: a non-empty dict must own a buffer. */
    if (d->raw_size > 0 && d->data == NULL) return;
    zgec_dict_node *prev = NULL;
    zgec_dict_node *cur = c->head;
    while (cur) {
        if (cur->dict.dict_id == d->dict_id) {
            if (prev) prev->next = cur->next;
            else c->head = cur->next;
            if (cur == c->tail) c->tail = prev;
            zgec_dict_free(&cur->dict);
            zgec_free(cur);
            if (c->size > 0) c->size--;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    while (c->size >= c->capacity) {
        zgec_dict_node *old = c->head;
        if (!old) break;
        c->head = old->next;
        if (old == c->tail) c->tail = NULL;
        zgec_dict_free(&old->dict);
        zgec_free(old);
        c->size--;
    }
    zgec_dict_node *n = (zgec_dict_node *)zgec_alloc(sizeof(*n), _Alignof(zgec_dict_node));
    if (!n) { zgec_dict_free(d); return; }
    memset(n, 0, sizeof(*n));
    /* Copy the entry and detach the source's buffer ownership.
       The caller may pass a stack object (see tests/test.c usage),
       so the container itself must NOT be freed here; only the
       data buffer changes ownership. */
    n->dict = *d;
    d->data = NULL;
    d->raw_size = 0;
    d->dict_id = 0;
    d->content_hash = 0;
    d->external = 0;
    n->next = NULL;
    if (c->tail) c->tail->next = n;
    else c->head = n;
    c->tail = n;
    c->size++;
}

void zgec_dict_free(zgec_dict *d)
{
    if (!d) return;
    zgec_free(d->data);
    memset(d, 0, sizeof(*d));
}

/* ---- inner-record decoder ---- */

typedef struct {
    uint32_t *ll;
    uint32_t *ml;
    uint32_t *of;
    uint32_t *rep0_before;  /* rep0 in effect before each sequence */
    size_t n_seq;
    size_t n_lit;
    uint8_t *lit;
    uint32_t raw_len;
    uint32_t rep0;
    uint32_t rep1;
    uint32_t rep2;
    uint32_t rep0_tail;     /* rep0 after the last sequence (tail runs) */
    int lit_form;           /* 0 plain, 1 sub-literals */
} zgec_dict_seg;

static void zgec_dict_seg_release(zgec_dict_seg *s)
{
    if (!s) return;
    zgec_free(s->ll);
    zgec_free(s->ml);
    zgec_free(s->of);
    zgec_free(s->rep0_before);
    zgec_free(s->lit);
    memset(s, 0, sizeof(*s));
}

/* Store n literal bytes at vb[pos..pos+n): a plain copy for lit_form 0,
 * or the sub-literal reconstruction of section 9.6 for lit_form 1 (the
 * coded byte is a residual against VB[pos - rep0]). The single output
 * buffer is the dictionary's virtual buffer, so predictors may be read
 * from earlier output and from earlier bytes of the same run. */
static void zgec_dict_store_literals(uint8_t *vb, size_t pos,
                                     const uint8_t *src, size_t n,
                                     int lit_form, uint32_t rep0)
{
    uint8_t *dst = vb + pos;
    if (lit_form == 0) {
        for (size_t i = 0; i < n; i++) dst[i] = src[i];
        return;
    }
    for (size_t i = 0; i < n; i++) {
        size_t cur = pos + i;
        uint8_t pred = 0;
        if ((uint64_t)rep0 <= (uint64_t)cur) pred = vb[cur - (size_t)rep0];
        dst[i] = (uint8_t)(src[i] + pred);
    }
}

/* Forward byte copy: matches replicate the bytes just written (6.1), so
 * the source is the same buffer, not a snapshot. */
static void zgec_dict_copy_match(uint8_t *dst, size_t off, size_t len)
{
    const uint8_t *src = dst - off;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
}

/* Clone an FSE decode table (for REPEAT inheritance across segments). */
static zgec_fse_dec_table *zgec_dict_fse_clone(const zgec_fse_dec_table *s)
{
    size_t ds;
    size_t ts;
    zgec_fse_dec_table *t;
    if (!s) return NULL;
    if (s->al < ZGEC_MIN_AL || s->al > ZGEC_MAX_AL) return NULL;
    if (s->nsym != ZGEC_NSYM_SEQ) return NULL;
    ds = sizeof(zgec_fse_dec_table) + (size_t)s->nsym * sizeof(int);
    ts = ((size_t)1 << (unsigned)s->al) * sizeof(zgec_fse_dec_entry);
    t = (zgec_fse_dec_table *)zgec_alloc(ds + ts, _Alignof(zgec_fse_dec_table));
    if (!t) return NULL;
    memcpy(t, s, ds + ts);
    t->e = (zgec_fse_dec_entry *)((uint8_t *)t + ds);
    return t;
}

/* Tables carried across the segments of one inner COMPRESSED payload
 * for REPEAT inheritance (section 7.4; repeats never cross blocks). */
typedef struct {
    zgec_rans_dec_table *lit;   /* value tables: plain memcpy clone */
    int n_lit_tbl;
    int lit_form;
    int lit_k;
    int has_lit;
    zgec_fse_dec_table *ll[3];
    int ll_rle[3];
    int n_ll;
    int ll_al;
    zgec_fse_dec_table *ml;
    int ml_rle;
    int ml_al;
    zgec_fse_dec_table *of[3];
    int of_rle[3];
    int n_of;
    int of_al;
    int ctx_ll;
    int ctx_of;
    int has_seq;
} zgec_dict_prev;

static void zgec_dict_prev_init(zgec_dict_prev *p)
{
    size_t i;
    if (!p) return;
    memset(p, 0, sizeof(*p));
    for (i = 0; i < 3; i++) {
        p->ll_rle[i] = -1;
        p->of_rle[i] = -1;
    }
    p->ml_rle = -1;
}

static void zgec_dict_prev_free(zgec_dict_prev *p)
{
    size_t i;
    if (!p) return;
    for (i = 0; i < 3; i++) {
        if ((int)i < p->n_ll && p->ll[i]) zgec_fse_free_dec(p->ll[i]);
        if ((int)i < p->n_of && p->of[i]) zgec_fse_free_dec(p->of[i]);
    }
    if (p->ml) zgec_fse_free_dec(p->ml);
    zgec_free(p->lit);
    zgec_dict_prev_init(p);
}

static zgec_err zgec_dict_check_sentinel(const uint8_t *s, size_t n)
{
    if (s == NULL && n > 0) return ZGEC_ERR_TRUNCATED;
    if (n == 0) return ZGEC_ERR_STREAM_SIZE;
    if (s[n - 1] == 0) return ZGEC_ERR_BITSTREAM_SENTINEL;
    return ZGEC_OK;
}

static zgec_err zgec_dict_seq_one(uint32_t *out, size_t n,
                                  zgec_fse_dec_table *t, int rle,
                                  const uint8_t *stream, size_t ssize)
{
    zgec_br br;
    zgec_err e = zgec_dict_check_sentinel(stream, ssize);
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

/* Three-table conditioned stream decode (section 8.6): tables share one
 * AL; class = mlclass(ML) (OF path) or mlclass(prev ML), class 0 for
 * i == 0 (LL path). ML-first decode order. */
static zgec_err zgec_dict_seq_cond(uint32_t *out, size_t n,
                                   zgec_fse_dec_table *const t[3],
                                   const uint32_t *ml, int use_prev,
                                   const uint8_t *stream, size_t ssize)
{
    zgec_br br;
    uint32_t state = 0;
    unsigned S = 0;
    int al;
    size_t i;
    zgec_err e = zgec_dict_check_sentinel(stream, ssize);
    if (e != ZGEC_OK) return e;
    if (!t[0] || !t[1] || !t[2]) return ZGEC_ERR_TABLE_MODE;
    if (out == NULL || ml == NULL) return ZGEC_ERR_INVAL;
    al = t[0]->al;
    if (t[1]->al != al || t[2]->al != al) return ZGEC_ERR_FSE_AL;
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
        out[i] = zgec_seq_base[(unsigned)sym] + extra;
        if (i + 1 < n) {
            bits = zgec_br_read(&br, (unsigned)en->nb_bits);
            if (br.overflow) return ZGEC_ERR_BITSTREAM;
            next = en->baseline + (int32_t)bits;
            if (next < 0 || (unsigned)next >= S) return ZGEC_ERR_BITSTREAM;
            state = (unsigned)next;
        }
    }
    if (!zgec_br_done(&br)) return ZGEC_ERR_BITSTREAM_UNCONSUMED;
    return ZGEC_OK;
}

/* Build one NEW rANS literal table from an FSE description (AL must be
 * 11 over the 256-symbol alphabet). Advances *pp and *rem. */
static zgec_err zgec_dict_build_rans(zgec_rans_dec_table *out,
                                     const uint8_t **pp, size_t *rem)
{
    int16_t counts[ZGEC_NSYM_LIT];
    int nsym = 0;
    int al = 0;
    size_t n;
    if (!out || !pp || !rem || !*pp) return ZGEC_ERR_INVAL;
    if (*rem < 1) return ZGEC_ERR_TRUNCATED;
    if ((int)(*pp)[0] != ZGEC_LIT_AL) return ZGEC_ERR_FSE_AL;
    n = zgec_fse_read_counts(counts, &nsym, &al, ZGEC_NSYM_LIT, *pp, *rem);
    if (n == 0) return ZGEC_ERR_FSE_COUNTS;
    if (al != ZGEC_LIT_AL) return ZGEC_ERR_FSE_AL;
    if (zgec_rans_build_dec(out, counts) != ZGEC_OK) return ZGEC_ERR_FSE_SUM;
    *pp += n;
    *rem -= n;
    return ZGEC_OK;
}

/* Build one NEW sequence table (66 symbols, AL 5..11). */
static zgec_err zgec_dict_build_fse(zgec_fse_dec_table **out,
                                    const uint8_t **pp, size_t *rem)
{
    int16_t counts[ZGEC_NSYM_LIT];
    int nsym = 0;
    int al = 0;
    size_t n;
    if (!out || !pp || !rem || !*pp) return ZGEC_ERR_INVAL;
    if (*rem < 1) return ZGEC_ERR_TRUNCATED;
    if ((int)(*pp)[0] < ZGEC_MIN_AL || (int)(*pp)[0] > ZGEC_MAX_AL)
        return ZGEC_ERR_FSE_AL;
    n = zgec_fse_read_counts(counts, &nsym, &al, ZGEC_NSYM_SEQ, *pp, *rem);
    if (n == 0) return ZGEC_ERR_FSE_COUNTS;
    if (al < ZGEC_MIN_AL || al > ZGEC_MAX_AL) return ZGEC_ERR_FSE_AL;
    if (zgec_fse_build_dec(out, counts, ZGEC_NSYM_SEQ, al) != ZGEC_OK)
        return ZGEC_ERR_FSE_SUM;
    *pp += n;
    *rem -= n;
    return ZGEC_OK;
}

/* Same segment decoder as blocks (decode.c), but standalone so dict.c
   does not include decode.h. Ld = 0, Ll = 0 (no outer dict/litref):
   offsets are validated against the dict-output position only.
   Supports raw and rANS literal coders with contexts (section 9,
   using zgec_lit/zgec_rans), FSE/RLE sequence streams with optional
   conditioning (section 8.6), both lit_forms, and REPEAT inheritance
   via *prev (section 7.4). */
static zgec_err zgec_dict_decode_segment(zgec_dict_seg *seg,
                                         const uint8_t *seg_buf, size_t seg_size,
                                         const zgec_block_params *bp,
                                         zgec_dict_prev *prev, int is_first)
{
    const zgec_ctx_desc *cd = NULL;
    int k = 1;
    int ctx_mode = ZGEC_CTX_NONE;
    const uint8_t *class_map = NULL;
    int lit_form = 0;
    unsigned lit_coder = 0;
    int ctx_of = 0;
    int ctx_ll = 0;
    unsigned lit_mode = 0;
    unsigned ll_mode = 0;
    unsigned ml_mode = 0;
    unsigned of_mode = 0;
    int n_lit_tbl = 0;
    int n_ll = 1;
    int n_of = 1;
    zgec_seg_header sh;
    size_t hdr_size = 0;
    memset(seg, 0, sizeof(*seg));
    if (seg == NULL || seg_buf == NULL || bp == NULL || prev == NULL)
        return ZGEC_ERR_INVAL;
    if (seg_size < 2) return ZGEC_ERR_SEGMENT_SIZE;
    /* Peek lit_form to select the context descriptor before parsing. */
    lit_form = (int)(seg_buf[0] & 1u);
    cd = (lit_form == 0) ? &bp->plain : &bp->sub;
    k = (int)cd->ctx_count;
    ctx_mode = (int)cd->ctx_mode;
    if (k != 1 && k != 2 && k != 4 && k != 8) return ZGEC_ERR_CTX_COUNT;
    if (ctx_mode < ZGEC_CTX_NONE || ctx_mode > ZGEC_CTX_SIGNED)
        return ZGEC_ERR_CTX_MODE;
    if (ctx_mode == ZGEC_CTX_NONE && k != 1) return ZGEC_ERR_CTX_COUNT;
    if (k == 1) {
        class_map = NULL;
    } else {
        for (int ci = 0; ci < 64; ci++)
            if ((int)cd->class_map[ci] >= k) return ZGEC_ERR_CLASS_MAP;
        class_map = cd->class_map;
    }
    memset(&sh, 0, sizeof(sh));
    hdr_size = zgec_seg_header_parse_ex(&sh, seg_buf, seg_size,
                                        NULL, 0, NULL, k);
    if (hdr_size == 0) return ZGEC_ERR_SEGMENT_SIZE;
    if (sh.n_seq > ZGEC_MAX_SEQ_PER_SEG) return ZGEC_ERR_SEGMENT_COUNT;
    /* Format limit (section 3.3): accept up to 2^26 raw bytes.
     * The P24 profile limit (2^24) is warn-only for decoders. */
    if ((uint64_t)sh.n_lit > ZGEC_DICT_NLIT_MAX) return ZGEC_ERR_STREAM_SIZE;
    if ((uint64_t)sh.n_lit > ZGEC_DICT_P24_MAX) {
        /* P24 warn-only: decodable; kept silent (no stderr). */
    }
    if (hdr_size + (size_t)sh.lit_size + (size_t)sh.ll_size +
        (size_t)sh.ml_size + (size_t)sh.of_size != seg_size)
        return ZGEC_ERR_SEGMENT_SIZE;

    lit_coder = (unsigned)(((unsigned)sh.segment_flags >> 1) & 3u);
    if (lit_coder != 0 && lit_coder != 1) return ZGEC_ERR_LIT_CODER;
    ctx_of = (int)(((unsigned)sh.segment_flags >> 3) & 1u);
    ctx_ll = (int)(((unsigned)sh.segment_flags >> 4) & 1u);
    lit_mode = (unsigned)(((unsigned)sh.table_modes >> 0) & ZGEC_TBL_MASK);
    ll_mode = (unsigned)(((unsigned)sh.table_modes >> 2) & ZGEC_TBL_MASK);
    ml_mode = (unsigned)(((unsigned)sh.table_modes >> 4) & ZGEC_TBL_MASK);
    of_mode = (unsigned)(((unsigned)sh.table_modes >> 6) & ZGEC_TBL_MASK);
    if (lit_mode > ZGEC_TBL_RLE || ll_mode > ZGEC_TBL_RLE ||
        ml_mode > ZGEC_TBL_RLE || of_mode > ZGEC_TBL_RLE)
        return ZGEC_ERR_TABLE_MODE;
    if (lit_coder != 0) {
        if (lit_mode == ZGEC_TBL_RLE) return ZGEC_ERR_TABLE_MODE;
        n_lit_tbl = (k <= 1) ? 1 : (k + 1);
    }
    n_ll = ctx_ll ? 3 : 1;
    n_of = ctx_of ? 3 : 1;

    seg->n_seq = sh.n_seq;
    seg->n_lit = sh.n_lit;
    seg->lit_form = lit_form;
    seg->rep0 = 1;
    seg->rep1 = 4;
    seg->rep2 = 8;
    seg->rep0_tail = 1;
    seg->raw_len = 0;

    if (seg->n_seq > 0) {
        if (seg->n_seq > SIZE_MAX / sizeof(uint32_t)) return ZGEC_ERR_NOMEM;
        seg->ll = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->ml = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->of = (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        seg->rep0_before =
            (uint32_t *)zgec_alloc(seg->n_seq * sizeof(uint32_t), 64);
        if (!seg->ll || !seg->ml || !seg->of || !seg->rep0_before) {
            zgec_dict_seg_release(seg);
            return ZGEC_ERR_NOMEM;
        }
    }
    if (seg->n_lit > 0) {
        if ((uint64_t)seg->n_lit + ZGEC_LIT_SLACK > (uint64_t)SIZE_MAX / 2)
            return ZGEC_ERR_NOMEM;
        seg->lit = (uint8_t *)zgec_alloc(seg->n_lit + ZGEC_LIT_SLACK, 64);
        if (!seg->lit) { zgec_dict_seg_release(seg); return ZGEC_ERR_NOMEM; }
    }
    if (seg->n_seq == 0) {
        /* No sequence tables (section 7.3); literals are all tail.
         * Literal tables are still present when lit_coder == 1. */
        zgec_rans_dec_table *lt = NULL;
        zgec_err e0 = ZGEC_OK;
        seg->raw_len = (uint32_t)seg->n_lit;
        if (lit_coder == 0) {
            if ((size_t)sh.lit_size != seg->n_lit) {
                zgec_dict_seg_release(seg);
                return ZGEC_ERR_STREAM_SIZE;
            }
            if (seg->n_lit > 0)
                memcpy(seg->lit, seg_buf + hdr_size, seg->n_lit);
            /* Raw: no literal tables (section 7.3), and n_seq == 0
             * carries no sequence tables either, so REPEAT cannot
             * follow this segment. */
            zgec_dict_prev_free(prev);
            (void)is_first;
            return ZGEC_OK;
        }
        /* rANS tail-only segment: build tables, run-start, decode. */
        if (n_lit_tbl <= 0) { zgec_dict_seg_release(seg); return ZGEC_ERR_TABLE_MODE; }
        lt = (zgec_rans_dec_table *)zgec_alloc(
            (size_t)n_lit_tbl * sizeof(*lt), 64);
        if (!lt) { zgec_dict_seg_release(seg); return ZGEC_ERR_NOMEM; }
        {
            size_t fp = 2;
            uint32_t tmp = 0;
            size_t nn;
            const uint8_t *dp;
            size_t rem;
            nn = zgec_varint_decode(seg_buf + fp, seg_size - fp, &tmp);
            if (nn == 0) { zgec_free(lt); zgec_dict_seg_release(seg); return ZGEC_ERR_VARINT; }
            fp += nn;
            nn = zgec_varint_decode(seg_buf + fp, seg_size - fp, &tmp);
            if (nn == 0) { zgec_free(lt); zgec_dict_seg_release(seg); return ZGEC_ERR_VARINT; }
            fp += nn;
            dp = seg_buf + fp;
            rem = hdr_size > fp ? hdr_size - fp : 0;
            /* Descriptor region also holds the 4 stream-size varints
             * at its tail; walk only the literal tables here. */
            if (lit_mode == ZGEC_TBL_REPEAT) {
                if (is_first || !prev->has_lit || prev->lit_form != lit_form ||
                    prev->lit_k != k || prev->n_lit_tbl != n_lit_tbl) {
                    zgec_free(lt);
                    zgec_dict_seg_release(seg);
                    return ZGEC_ERR_TABLE_INHERIT;
                }
                memcpy(lt, prev->lit,
                       (size_t)n_lit_tbl * sizeof(*lt));
                rem = 0;
            } else {
                for (int ti = 0; ti < n_lit_tbl; ti++) {
                    size_t before = rem;
                    e0 = zgec_dict_build_rans(&lt[ti], &dp, &rem);
                    if (e0 != ZGEC_OK || rem == before) {
                        zgec_free(lt);
                        zgec_dict_seg_release(seg);
                        return e0 != ZGEC_OK ? e0 : ZGEC_ERR_FSE_COUNTS;
                    }
                }
                /* Pin the descriptor boundary: the remainder must be
                 * exactly the four stream-size varints (V8). */
                {
                    uint32_t vs[4];
                    size_t used = 0;
                    for (int vi = 0; vi < 4; vi++) {
                        size_t nn2 = zgec_varint_decode(
                            dp + used, rem - used, &vs[(unsigned)vi]);
                        if (nn2 == 0) {
                            zgec_free(lt);
                            zgec_dict_seg_release(seg);
                            return ZGEC_ERR_VARINT;
                        }
                        used += nn2;
                    }
                    if (used != rem || vs[0] != sh.lit_size ||
                        vs[1] != sh.ll_size || vs[2] != sh.ml_size ||
                        vs[3] != sh.of_size) {
                        zgec_free(lt);
                        zgec_dict_seg_release(seg);
                        return ZGEC_ERR_SEGMENT_SIZE;
                    }
                }
            }
        }
        if (seg->n_lit > 0) {
            uint8_t *rs = (uint8_t *)zgec_alloc(seg->n_lit, 1);
            const uint8_t *lit_stream = seg_buf + hdr_size;
            if (!rs) { zgec_free(lt); zgec_dict_seg_release(seg); return ZGEC_ERR_NOMEM; }
            e0 = zgec_lit_runstart(rs, seg->n_lit, NULL, 0);
            if (e0 != ZGEC_OK) { zgec_free(rs); zgec_free(lt); zgec_dict_seg_release(seg); return e0; }
            if (k <= 1)
                e0 = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                      (size_t)sh.lit_size, lt, 1,
                                      ctx_mode, NULL, NULL);
            else
                e0 = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                      (size_t)sh.lit_size, lt, k,
                                      ctx_mode, class_map, rs);
            zgec_free(rs);
            if (e0 != ZGEC_OK) { zgec_free(lt); zgec_dict_seg_release(seg); return e0; }
        }
        /* Carry literal tables for REPEAT. */
        zgec_dict_prev_free(prev);
        prev->lit = lt;
        prev->n_lit_tbl = n_lit_tbl;
        prev->lit_form = lit_form;
        prev->lit_k = k;
        prev->has_lit = 1;
        prev->has_seq = 0;
        prev->ctx_ll = ctx_ll;
        prev->ctx_of = ctx_of;
        return ZGEC_OK;
    }

    /* ---- table descriptors + streams (n_seq > 0) ---- */
    {
        size_t fp = 2;
        uint32_t tmp = 0;
        size_t nn;
        const uint8_t *dp;
        size_t rem;
        zgec_rans_dec_table *lt = NULL;
        zgec_fse_dec_table *llt[3] = { NULL, NULL, NULL };
        int ll_rle[3] = { -1, -1, -1 };
        int ll_al = 0;
        zgec_fse_dec_table *mlt = NULL;
        int ml_rle = -1;
        int ml_al = 0;
        zgec_fse_dec_table *oft[3] = { NULL, NULL, NULL };
        int of_rle[3] = { -1, -1, -1 };
        int of_al = 0;
        const uint8_t *lit_stream;
        const uint8_t *ll_stream;
        const uint8_t *ml_stream;
        const uint8_t *of_stream;
        uint8_t *rs = NULL;
        uint64_t ll_sum = 0;
        uint64_t ml_sum = 0;
        zgec_err derr = ZGEC_OK;
        int ti;
        nn = zgec_varint_decode(seg_buf + fp, seg_size - fp, &tmp);
        if (nn == 0) { zgec_dict_seg_release(seg); return ZGEC_ERR_VARINT; }
        fp += nn;
        nn = zgec_varint_decode(seg_buf + fp, seg_size - fp, &tmp);
        if (nn == 0) { zgec_dict_seg_release(seg); return ZGEC_ERR_VARINT; }
        fp += nn;
        if (fp > hdr_size) { zgec_dict_seg_release(seg); return ZGEC_ERR_SEGMENT_SIZE; }
        dp = seg_buf + fp;
        rem = hdr_size - fp;

        /* Literal tables (omitted when lit_coder == 0). */
        if (lit_coder != 0) {
            lt = (zgec_rans_dec_table *)zgec_alloc(
                (size_t)n_lit_tbl * sizeof(*lt), 64);
            if (!lt) { zgec_dict_seg_release(seg); return ZGEC_ERR_NOMEM; }
            if (lit_mode == ZGEC_TBL_REPEAT) {
                if (is_first || !prev->has_lit || prev->lit_form != lit_form ||
                    prev->lit_k != k || prev->n_lit_tbl != n_lit_tbl) {
                    zgec_free(lt);
                    zgec_dict_seg_release(seg);
                    return ZGEC_ERR_TABLE_INHERIT;
                }
                memcpy(lt, prev->lit, (size_t)n_lit_tbl * sizeof(*lt));
            } else {
                for (ti = 0; ti < n_lit_tbl; ti++) {
                    derr = zgec_dict_build_rans(&lt[ti], &dp, &rem);
                    if (derr != ZGEC_OK) goto lit_seq_fail;
                }
            }
        }

        /* LL tables (1, or 3 with the same AL when conditioned). */
        if (ll_mode == ZGEC_TBL_REPEAT) {
            if (is_first || !prev->has_seq || prev->ctx_ll != ctx_ll ||
                prev->ctx_of != ctx_of || prev->n_ll != n_ll)
                { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
            for (ti = 0; ti < n_ll; ti++) {
                if (prev->ll_rle[ti] >= 0) {
                    ll_rle[ti] = prev->ll_rle[ti];
                } else {
                    if (!prev->ll[ti]) { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
                    llt[ti] = zgec_dict_fse_clone(prev->ll[ti]);
                    if (!llt[ti]) { derr = ZGEC_ERR_NOMEM; goto lit_seq_fail; }
                    ll_rle[ti] = -1;
                }
            }
            ll_al = prev->ll_al;
        } else if (ll_mode == ZGEC_TBL_RLE) {
            int code;
            if (rem < 1) { derr = ZGEC_ERR_TRUNCATED; goto lit_seq_fail; }
            code = (int)*dp;
            dp += 1;
            rem -= 1;
            if (code >= ZGEC_NSYM_SEQ) { derr = ZGEC_ERR_FSE_SYMBOL; goto lit_seq_fail; }
            for (ti = 0; ti < n_ll; ti++) ll_rle[ti] = code;
        } else {
            for (ti = 0; ti < n_ll; ti++) {
                int alx = 0;
                derr = zgec_dict_build_fse(&llt[ti], &dp, &rem);
                if (derr != ZGEC_OK) goto lit_seq_fail;
                alx = llt[ti]->al;
                if (ti == 0) ll_al = alx;
                else if (alx != ll_al) { derr = ZGEC_ERR_FSE_AL; goto lit_seq_fail; }
                ll_rle[ti] = -1;
            }
        }

        /* ML table (always single). */
        if (ml_mode == ZGEC_TBL_REPEAT) {
            if (is_first || !prev->has_seq || prev->ctx_ll != ctx_ll ||
                prev->ctx_of != ctx_of)
                { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
            if (prev->ml_rle >= 0) {
                ml_rle = prev->ml_rle;
            } else {
                if (!prev->ml) { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
                mlt = zgec_dict_fse_clone(prev->ml);
                if (!mlt) { derr = ZGEC_ERR_NOMEM; goto lit_seq_fail; }
                ml_rle = -1;
            }
            ml_al = prev->ml_al;
        } else if (ml_mode == ZGEC_TBL_RLE) {
            int code;
            if (rem < 1) { derr = ZGEC_ERR_TRUNCATED; goto lit_seq_fail; }
            code = (int)*dp;
            dp += 1;
            rem -= 1;
            if (code >= ZGEC_NSYM_SEQ) { derr = ZGEC_ERR_FSE_SYMBOL; goto lit_seq_fail; }
            ml_rle = code;
        } else {
            derr = zgec_dict_build_fse(&mlt, &dp, &rem);
            if (derr != ZGEC_OK) goto lit_seq_fail;
            ml_al = mlt->al;
            ml_rle = -1;
        }

        /* OF tables (1, or 3 with the same AL when conditioned). */
        if (of_mode == ZGEC_TBL_REPEAT) {
            if (is_first || !prev->has_seq || prev->ctx_ll != ctx_ll ||
                prev->ctx_of != ctx_of || prev->n_of != n_of)
                { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
            for (ti = 0; ti < n_of; ti++) {
                if (prev->of_rle[ti] >= 0) {
                    of_rle[ti] = prev->of_rle[ti];
                } else {
                    if (!prev->of[ti]) { derr = ZGEC_ERR_TABLE_INHERIT; goto lit_seq_fail; }
                    oft[ti] = zgec_dict_fse_clone(prev->of[ti]);
                    if (!oft[ti]) { derr = ZGEC_ERR_NOMEM; goto lit_seq_fail; }
                    of_rle[ti] = -1;
                }
            }
            of_al = prev->of_al;
        } else if (of_mode == ZGEC_TBL_RLE) {
            int code;
            if (rem < 1) { derr = ZGEC_ERR_TRUNCATED; goto lit_seq_fail; }
            code = (int)*dp;
            dp += 1;
            rem -= 1;
            if (code >= ZGEC_NSYM_SEQ) { derr = ZGEC_ERR_FSE_SYMBOL; goto lit_seq_fail; }
            for (ti = 0; ti < n_of; ti++) of_rle[ti] = code;
        } else {
            for (ti = 0; ti < n_of; ti++) {
                int alx = 0;
                derr = zgec_dict_build_fse(&oft[ti], &dp, &rem);
                if (derr != ZGEC_OK) goto lit_seq_fail;
                alx = oft[ti]->al;
                if (ti == 0) of_al = alx;
                else if (alx != of_al) { derr = ZGEC_ERR_FSE_AL; goto lit_seq_fail; }
                of_rle[ti] = -1;
            }
        }

        /* The descriptor remainder must be exactly the four
         * stream-size varints (V8). */
        {
            uint32_t vs[4];
            size_t used = 0;
            for (int vi = 0; vi < 4; vi++) {
                size_t nn2 = zgec_varint_decode(dp + used, rem - used,
                                                &vs[(unsigned)vi]);
                if (nn2 == 0) { derr = ZGEC_ERR_VARINT; goto lit_seq_fail; }
                used += nn2;
            }
            if (used != rem || vs[0] != sh.lit_size || vs[1] != sh.ll_size ||
                vs[2] != sh.ml_size || vs[3] != sh.of_size) {
                derr = ZGEC_ERR_SEGMENT_SIZE;
                goto lit_seq_fail;
            }
        }

        lit_stream = seg_buf + hdr_size;
        ll_stream = lit_stream + (size_t)sh.lit_size;
        ml_stream = ll_stream + (size_t)sh.ll_size;
        of_stream = ml_stream + (size_t)sh.ml_size;

        /* ML first (needed for OF/LL conditioning classes). */
        derr = zgec_dict_seq_one(seg->ml, seg->n_seq, mlt, ml_rle,
                                 ml_stream, (size_t)sh.ml_size);
        if (derr != ZGEC_OK) goto lit_seq_fail;
        ml_sum = (uint64_t)seg->n_lit;
        for (size_t i = 0; i < seg->n_seq; i++) {
            uint64_t m = (uint64_t)seg->ml[i] + 3u;
            if (m < 3u || m > (uint64_t)0xFFFFFFFFu) {
                derr = ZGEC_ERR_MATCH_LENGTH;
                goto lit_seq_fail;
            }
            seg->ml[i] = (uint32_t)m;
            ml_sum += m;
            if (ml_sum > (uint64_t)0xFFFFFFFFu) {
                derr = ZGEC_ERR_OUTPUT_SIZE;
                goto lit_seq_fail;
            }
        }
        seg->raw_len = (uint32_t)ml_sum;

        /* OF (conditioned on mlclass(ML) when bit 3 set). */
        if (ctx_of) {
            if (n_of == 3 && oft[0] && oft[1] && oft[2] && of_rle[0] < 0) {
                zgec_fse_dec_table *t3[3] = { oft[0], oft[1], oft[2] };
                derr = zgec_dict_seq_cond(seg->of, seg->n_seq, t3, seg->ml, 0,
                                          of_stream, (size_t)sh.of_size);
            } else {
                derr = zgec_dict_seq_one(seg->of, seg->n_seq, oft[0], of_rle[0],
                                         of_stream, (size_t)sh.of_size);
            }
        } else {
            derr = zgec_dict_seq_one(seg->of, seg->n_seq, oft[0], of_rle[0],
                                     of_stream, (size_t)sh.of_size);
        }
        if (derr != ZGEC_OK) goto lit_seq_fail;
        for (size_t i = 0; i < seg->n_seq; i++) {
            uint64_t ob = (uint64_t)seg->of[i] + 1u;
            if (ob < 1u || ob > (uint64_t)0xFFFFFFFFu) {
                derr = ZGEC_ERR_OFFSET;
                goto lit_seq_fail;
            }
            seg->of[i] = (uint32_t)ob;
        }

        /* LL (conditioned on mlclass(prev ML), class 0 for i == 0). */
        if (ctx_ll) {
            if (n_ll == 3 && llt[0] && llt[1] && llt[2] && ll_rle[0] < 0) {
                zgec_fse_dec_table *t3[3] = { llt[0], llt[1], llt[2] };
                derr = zgec_dict_seq_cond(seg->ll, seg->n_seq, t3, seg->ml, 1,
                                          ll_stream, (size_t)sh.ll_size);
            } else {
                derr = zgec_dict_seq_one(seg->ll, seg->n_seq, llt[0], ll_rle[0],
                                         ll_stream, (size_t)sh.ll_size);
            }
        } else {
            derr = zgec_dict_seq_one(seg->ll, seg->n_seq, llt[0], ll_rle[0],
                                     ll_stream, (size_t)sh.ll_size);
        }
        if (derr != ZGEC_OK) goto lit_seq_fail;
        for (size_t i = 0; i < seg->n_seq; i++) {
            ll_sum += (uint64_t)seg->ll[i];
            if (ll_sum > (uint64_t)seg->n_lit) {
                derr = ZGEC_ERR_LL_SUM;
                goto lit_seq_fail;
            }
        }

        /* Run-start bitmap (section 9.3, incl. tail start). */
        if (seg->n_lit > 0) {
            rs = (uint8_t *)zgec_alloc(seg->n_lit, 1);
            if (!rs) { derr = ZGEC_ERR_NOMEM; goto lit_seq_fail; }
            derr = zgec_lit_runstart(rs, seg->n_lit, seg->ll, seg->n_seq);
            if (derr != ZGEC_OK) goto lit_seq_fail;
        }

        /* Literals: raw memcpy or rANS with contexts. */
        if (seg->n_lit > 0) {
            if (lit_coder == 0) {
                if ((size_t)sh.lit_size != seg->n_lit) {
                    derr = ZGEC_ERR_STREAM_SIZE;
                    goto lit_seq_fail;
                }
                memcpy(seg->lit, lit_stream, seg->n_lit);
            } else {
                if (k <= 1)
                    derr = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                            (size_t)sh.lit_size, lt, 1,
                                            ctx_mode, NULL, NULL);
                else
                    derr = zgec_rans_decode(seg->lit, seg->n_lit, lit_stream,
                                            (size_t)sh.lit_size, lt, k,
                                            ctx_mode, class_map, rs);
                if (derr != ZGEC_OK) goto lit_seq_fail;
            }
        }
        zgec_free(rs);
        rs = NULL;

        /* Repeat offsets from (1,4,8); record rep0_before + tail (8.2). */
        for (size_t i = 0; i < seg->n_seq; i++) {
            uint32_t offbase = seg->of[i];
            uint32_t d;
            seg->rep0_before[i] = seg->rep0;
            if (offbase == 1) {
                d = seg->rep0;
            } else if (offbase == 2) {
                d = seg->rep1;
                seg->rep1 = seg->rep0;
                seg->rep0 = d;
            } else if (offbase == 3) {
                d = seg->rep2;
                seg->rep2 = seg->rep1;
                seg->rep1 = seg->rep0;
                seg->rep0 = d;
            } else {
                if (offbase < 4) { derr = ZGEC_ERR_OFFSET; goto lit_seq_fail; }
                d = offbase - 3u;
                if (d < 1 || d > (uint32_t)0xFFFFFFFCu) {
                    derr = ZGEC_ERR_OFFSET;
                    goto lit_seq_fail;
                }
                seg->rep2 = seg->rep1;
                seg->rep1 = seg->rep0;
                seg->rep0 = d;
            }
            if (d < 1) { derr = ZGEC_ERR_OFFSET; goto lit_seq_fail; }
            seg->of[i] = d;
        }
        seg->rep0_tail = (seg->n_seq > 0) ? seg->rep0 : 1u;

        /* Carry tables for REPEAT (section 7.4). Raw segments carry
         * only sequence tables; rANS segments carry both. */
        zgec_dict_prev_free(prev);
        if (lit_coder != 0) {
            prev->lit = lt;
            lt = NULL;
            prev->n_lit_tbl = n_lit_tbl;
            prev->lit_form = lit_form;
            prev->lit_k = k;
            prev->has_lit = 1;
        }
        for (ti = 0; ti < n_ll; ti++) {
            if (ll_rle[ti] >= 0) {
                prev->ll_rle[ti] = ll_rle[ti];
                prev->ll[ti] = NULL;
            } else {
                prev->ll[ti] = llt[ti];
                llt[ti] = NULL;
                prev->ll_rle[ti] = -1;
            }
        }
        prev->n_ll = n_ll;
        prev->ll_al = ll_al;
        if (ml_rle >= 0) {
            prev->ml_rle = ml_rle;
            prev->ml = NULL;
        } else {
            prev->ml = mlt;
            mlt = NULL;
            prev->ml_rle = -1;
        }
        prev->ml_al = ml_al;
        for (ti = 0; ti < n_of; ti++) {
            if (of_rle[ti] >= 0) {
                prev->of_rle[ti] = of_rle[ti];
                prev->of[ti] = NULL;
            } else {
                prev->of[ti] = oft[ti];
                oft[ti] = NULL;
                prev->of_rle[ti] = -1;
            }
        }
        prev->n_of = n_of;
        prev->of_al = of_al;
        prev->ctx_ll = ctx_ll;
        prev->ctx_of = ctx_of;
        prev->has_seq = 1;
        return ZGEC_OK;

lit_seq_fail:
        zgec_free(rs);
        zgec_free(lt);
        for (ti = 0; ti < 3; ti++) {
            if (ti < n_ll && llt[ti]) zgec_fse_free_dec(llt[ti]);
            if (ti < n_of && oft[ti]) zgec_fse_free_dec(oft[ti]);
        }
        if (mlt) zgec_fse_free_dec(mlt);
        zgec_dict_seg_release(seg);
        return derr;
    }
}

static zgec_err zgec_dict_inner_compressed(zgec_dict **out,
                                          const uint8_t *comp, size_t comp_size,
                                          uint32_t segment_count, uint32_t raw_size)
{
    zgec_block_params bp;
    memset(&bp, 0, sizeof(bp));
    size_t params_len = zgec_block_params_parse(&bp, comp, comp_size);
    if (params_len == 0 || params_len > 52 || params_len > comp_size)
        return ZGEC_ERR_SEGMENT_SIZE;
    if (segment_count < 1 || segment_count > ZGEC_MAX_SEGMENTS)
        return ZGEC_ERR_SEGMENT_COUNT;
    if (comp_size < params_len + (size_t)segment_count * 8)
        return ZGEC_ERR_TRUNCATED;

    zgec_seg_dir_entry *dir = (zgec_seg_dir_entry *)zgec_alloc(
        (size_t)segment_count * sizeof(*dir), _Alignof(zgec_seg_dir_entry));
    if (!dir) return ZGEC_ERR_NOMEM;
    zgec_err err = zgec_seg_dir_parse(dir, segment_count, comp + params_len, comp_size - params_len);
    if (err != ZGEC_OK) { zgec_free(dir); return err; }

    zgec_dict *d = (zgec_dict *)zgec_alloc(sizeof(*d), _Alignof(zgec_dict));
    if (!d) { zgec_free(dir); return ZGEC_ERR_NOMEM; }
    memset(d, 0, sizeof(*d));
    if (raw_size == 0) {
        d->data = (uint8_t *)zgec_alloc(ZGEC_OUTPUT_SLACK, 64);
        if (!d->data) { zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_NOMEM; }
        d->raw_size = 0;
        zgec_free(dir);
        *out = d;
        return ZGEC_OK;
    }
    d->data = (uint8_t *)zgec_alloc((size_t)raw_size + ZGEC_OUTPUT_SLACK, 64);
    if (!d->data) { zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_NOMEM; }
    d->raw_size = raw_size;

    size_t seg_offset = params_len + (size_t)segment_count * 8;
    size_t out_pos = 0;
    zgec_dict_prev prev;
    zgec_dict_prev_init(&prev);
    for (uint32_t i = 0; i < segment_count; i++) {
        if (dir[i].comp_len == 0 || seg_offset + dir[i].comp_len > comp_size) {
            zgec_dict_prev_free(&prev);
            zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_SEGMENT_SIZE;
        }
        zgec_dict_seg seg;
        memset(&seg, 0, sizeof(seg));
        /* 7.4: REPEAT resolves against the previous segment of this
         * inner record; repeats never cross a record boundary. */
        err = zgec_dict_decode_segment(&seg, comp + seg_offset, dir[i].comp_len,
                                       &bp, &prev, (i == 0) ? 1 : 0);
        if (err != ZGEC_OK) { zgec_dict_prev_free(&prev); zgec_free(dir); zgec_dict_free(d); zgec_free(d); return err; }
        if (dir[i].raw_len != seg.raw_len) {
            zgec_dict_seg_release(&seg);
            zgec_dict_prev_free(&prev);
            zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_RAW_LEN;
        }
        if ((uint64_t)out_pos + seg.raw_len > raw_size) {
            zgec_dict_seg_release(&seg);
            zgec_dict_prev_free(&prev);
            zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_DICT_SIZE;
        }
        const uint8_t *lit_cur = seg.lit;
        for (size_t j = 0; j < seg.n_seq; j++) {
            uint32_t ll = seg.ll[j];
            uint32_t ml = seg.ml[j];
            uint32_t off = seg.of[j];
            if (ml < 3) { zgec_dict_seg_release(&seg); zgec_dict_prev_free(&prev); zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_MATCH_LENGTH; }
            if (off == 0 || off > out_pos) {
                if (!(out_pos == 0 && ll == 0)) {
                    zgec_dict_seg_release(&seg); zgec_dict_prev_free(&prev); zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_OFFSET;
                }
            }
            if (ll > 0) {
                if ((uint64_t)(lit_cur - seg.lit) + ll > seg.n_lit) {
                    zgec_dict_seg_release(&seg); zgec_dict_prev_free(&prev); zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_LL_SUM;
                }
                zgec_dict_store_literals(d->data, out_pos, lit_cur, ll,
                                         seg.lit_form, seg.rep0_before[j]);
                lit_cur += ll;
            }
            out_pos += ll;
            if (ml > 0) {
                if (off == 0 || off > out_pos) {
                    zgec_dict_seg_release(&seg); zgec_dict_prev_free(&prev); zgec_free(dir); zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_OFFSET;
                }
                zgec_dict_copy_match(d->data + out_pos, off, ml);
            }
            out_pos += ml;
        }
        if (seg.n_seq == 0 && seg.n_lit > 0) {
            zgec_dict_store_literals(d->data, out_pos, seg.lit, seg.n_lit,
                                     seg.lit_form, seg.rep0_tail);
            out_pos += seg.n_lit;
        } else if (lit_cur != NULL && seg.n_lit > 0) {
            size_t consumed = (size_t)(lit_cur - seg.lit);
            size_t tail = seg.n_lit - consumed;
            if (tail > 0) {
                zgec_dict_store_literals(d->data, out_pos, lit_cur, tail,
                                         seg.lit_form, seg.rep0_tail);
                out_pos += tail;
            }
        }
        zgec_dict_seg_release(&seg);
        seg_offset += dir[i].comp_len;
    }
    zgec_dict_prev_free(&prev);
    zgec_free(dir);
    if (out_pos != raw_size) { zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_DICT_SIZE; }
    *out = d;
    return ZGEC_OK;
}

zgec_err zgec_dict_decode(zgec_dict **out,
                           const uint8_t *payload, size_t payload_size,
                           uint32_t raw_size, uint64_t content_hash,
                           const zgec_frame_header *fh)
{
    (void)fh;
    if (!out || !payload || payload_size < 24) return ZGEC_ERR_INVAL;
    uint8_t type = payload[0];
    uint8_t rflags = payload[1];
    uint16_t inner_dict_id = zgec_rd16(payload + 2);
    uint32_t inner_raw = zgec_rd32(payload + 4);
    uint32_t inner_ps = zgec_rd32(payload + 8);
    uint32_t inner_sc = zgec_rd32(payload + 12);
    uint8_t inner_litref = payload[20];
    if (payload[21] != 0 || payload[22] != 0 || payload[23] != 0) return ZGEC_ERR_RESERVED;
    if ((rflags & (uint8_t)~ZGEC_RFLAG_ALL_KNOWN) != 0) return ZGEC_ERR_RESERVED;
    /* Spec 7.5/V11: a dictionary's inner record must not be FILTERED. */
    if ((rflags & ZGEC_RFLAG_FILTERED) != 0) return ZGEC_ERR_RESERVED;
    if (inner_dict_id != 0) return ZGEC_ERR_DICT_ID;
    if (inner_litref != 0) return ZGEC_ERR_LITREF_DEPTH;
    if (type != ZGEC_REC_RAW && type != ZGEC_REC_RLE && type != ZGEC_REC_COMPRESSED)
        return ZGEC_ERR_RECORD_TYPE;
    if (inner_raw != raw_size) return ZGEC_ERR_DICT_SIZE;
    if (payload_size < (size_t)24 + inner_ps) return ZGEC_ERR_TRUNCATED;
    if ((size_t)24 + inner_ps != payload_size) return ZGEC_ERR_RECORD_SIZE;
    if ((uint64_t)raw_size + ZGEC_OUTPUT_SLACK > (uint64_t)SIZE_MAX / 2)
        return ZGEC_ERR_NOMEM;

    zgec_dict *d = NULL;
    zgec_err err = ZGEC_OK;
    if (type == ZGEC_REC_RAW) {
        if (inner_sc != 0) return ZGEC_ERR_SEGMENT_COUNT;
        if ((size_t)inner_ps != inner_raw) return ZGEC_ERR_DICT_SIZE;
        d = (zgec_dict *)zgec_alloc(sizeof(*d), _Alignof(zgec_dict));
        if (!d) return ZGEC_ERR_NOMEM;
        memset(d, 0, sizeof(*d));
        d->data = (uint8_t *)zgec_alloc((size_t)raw_size + ZGEC_OUTPUT_SLACK, 64);
        if (!d->data) { zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_NOMEM; }
        if (raw_size > 0) memcpy(d->data, payload + 24, raw_size);
        d->raw_size = raw_size;
    } else if (type == ZGEC_REC_RLE) {
        if (inner_sc != 0) return ZGEC_ERR_SEGMENT_COUNT;
        if (inner_ps != 1 || payload_size != 25) return ZGEC_ERR_RECORD_SIZE;
        d = (zgec_dict *)zgec_alloc(sizeof(*d), _Alignof(zgec_dict));
        if (!d) return ZGEC_ERR_NOMEM;
        memset(d, 0, sizeof(*d));
        d->data = (uint8_t *)zgec_alloc((size_t)raw_size + ZGEC_OUTPUT_SLACK, 64);
        if (!d->data) { zgec_dict_free(d); zgec_free(d); return ZGEC_ERR_NOMEM; }
        if (raw_size > 0) memset(d->data, payload[24], raw_size);
        d->raw_size = raw_size;
    } else {
        err = zgec_dict_inner_compressed(&d, payload + 24, inner_ps, inner_sc, raw_size);
        if (err != ZGEC_OK) return err;
    }
    if (content_hash != 0 && zgec_xxh64(d->data, d->raw_size, 0) != content_hash) {
        zgec_dict_free(d);
        zgec_free(d);
        return ZGEC_ERR_DICT_HASH;
    }
    *out = d;
    return ZGEC_OK;
}

/* ---- dictionary training ---- */

typedef struct { uint64_t hash; uint32_t pos; uint32_t len; } zgec_chunk;

static uint64_t zgec_rolling_hash(uint64_t h, uint8_t byte)
{
    h = (h << 1) | (h >> 63);
    h ^= (uint64_t)byte * 0x9E3779B185EBCA87ull;
    return h;
}

static int zgec_chunk_high_entropy(const uint8_t *data, size_t len)
{
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    unsigned distinct = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (!seen[b]) {
            seen[b] = 1;
            distinct++;
            if (distinct > 200) return 1;
        }
    }
    return 0;
}

typedef struct { uint32_t idx; uint64_t score; uint32_t pos; uint32_t len; } zgec_sel_entry;

static int zgec_sel_cmp_asc(const void *a, const void *b)
{
    const zgec_sel_entry *x = (const zgec_sel_entry *)a;
    const zgec_sel_entry *y = (const zgec_sel_entry *)b;
    if (x->score < y->score) return -1;
    if (x->score > y->score) return 1;
    if (x->pos < y->pos) return -1;
    if (x->pos > y->pos) return 1;
    return 0;
}

zgec_err zgec_dict_train(uint8_t **out, size_t *out_size,
                          const uint8_t *data, size_t data_size,
                          size_t dict_size)
{
    if (!out || !out_size) return ZGEC_ERR_INVAL;
    if (!data) { *out = NULL; *out_size = 0; return ZGEC_OK; }
    if (data_size == 0 || dict_size == 0) { *out = NULL; *out_size = 0; return ZGEC_OK; }
    size_t mask = 0x1FF;
    size_t min_chunk = 64;
    size_t max_chunk = 4096;

    zgec_chunk *chunks = NULL;
    size_t n_chunks = 0;
    size_t chunks_cap = 1024;
    chunks = (zgec_chunk *)zgec_alloc(chunks_cap * sizeof(*chunks), _Alignof(zgec_chunk));
    if (!chunks) return ZGEC_ERR_NOMEM;

    uint64_t rolling = 0x9E3779B185EBCA87ull;
    size_t chunk_start = 0;
    for (size_t i = 0; i < data_size; i++) {
        rolling = zgec_rolling_hash(rolling, data[i]);
        if (((rolling & mask) == 0 || i - chunk_start >= max_chunk - 1) && i - chunk_start >= min_chunk) {
            if (n_chunks >= chunks_cap) {
                size_t nc = chunks_cap * 2;
                zgec_chunk *tmp = (zgec_chunk *)zgec_alloc(nc * sizeof(*tmp), _Alignof(zgec_chunk));
                if (!tmp) { zgec_free(chunks); return ZGEC_ERR_NOMEM; }
                memcpy(tmp, chunks, n_chunks * sizeof(*chunks));
                zgec_free(chunks);
                chunks = tmp;
                chunks_cap = nc;
            }
            chunks[n_chunks].hash = zgec_xxh64(data + chunk_start, i - chunk_start + 1, 0);
            chunks[n_chunks].pos = (uint32_t)chunk_start;
            chunks[n_chunks].len = (uint32_t)(i - chunk_start + 1);
            n_chunks++;
            chunk_start = i + 1;
            rolling = 0x9E3779B185EBCA87ull;
        }
    }
    if (chunk_start < data_size) {
        size_t len = data_size - chunk_start;
        if (len > max_chunk) len = max_chunk;
        if (n_chunks >= chunks_cap) {
            size_t nc = chunks_cap * 2;
            zgec_chunk *tmp = (zgec_chunk *)zgec_alloc(nc * sizeof(*tmp), _Alignof(zgec_chunk));
            if (!tmp) { zgec_free(chunks); return ZGEC_ERR_NOMEM; }
            memcpy(tmp, chunks, n_chunks * sizeof(*chunks));
            zgec_free(chunks);
            chunks = tmp;
            chunks_cap = nc;
        }
        chunks[n_chunks].hash = zgec_xxh64(data + chunk_start, len, 0);
        chunks[n_chunks].pos = (uint32_t)chunk_start;
        chunks[n_chunks].len = (uint32_t)len;
        n_chunks++;
    }

    size_t table_cap = 1;
    while (table_cap < n_chunks * 2) table_cap <<= 1;
    if (table_cap < 64) table_cap = 64;
    struct zgec_dict_table_entry { uint64_t hash; uint32_t count; };
    typedef struct zgec_dict_table_entry zgec_dict_table_entry;
    zgec_dict_table_entry *table = (zgec_dict_table_entry *)zgec_alloc(table_cap * sizeof(*table), _Alignof(zgec_dict_table_entry));
    if (!table) { zgec_free(chunks); return ZGEC_ERR_NOMEM; }
    memset(table, 0, table_cap * sizeof(*table));

    for (size_t i = 0; i < n_chunks; i++) {
        uint64_t h = chunks[i].hash;
        if (h == 0) h = 1;
        size_t idx = (size_t)(h & (uint64_t)(table_cap - 1));
        for (;;) {
            if (table[idx].hash == 0) { table[idx].hash = h; table[idx].count = 1; break; }
            if (table[idx].hash == h) { table[idx].count++; break; }
            idx = (idx + 1) & (table_cap - 1);
        }
    }

    /* 0 = available, 1 = selected, 2 = excluded (high-entropy or duplicate). */
    uint8_t *state = (uint8_t *)zgec_alloc(n_chunks ? n_chunks : 1, 1);
    if (!state) { zgec_free(table); zgec_free(chunks); return ZGEC_ERR_NOMEM; }
    memset(state, 0, n_chunks);
    for (size_t i = 0; i < n_chunks; i++) {
        if (zgec_chunk_high_entropy(data + chunks[i].pos, chunks[i].len)) state[i] = 2;
    }
    size_t total = 0;
    for (;;) {
        size_t best = (size_t)-1;
        uint64_t best_score = 0;
        for (size_t i = 0; i < n_chunks; i++) {
            if (state[i] != 0) continue;
            uint64_t h = chunks[i].hash;
            if (h == 0) h = 1;
            size_t idx = (size_t)(h & (uint64_t)(table_cap - 1));
            uint32_t occ = 0;
            for (;;) {
                if (table[idx].hash == h) { occ = table[idx].count; break; }
                idx = (idx + 1) & (table_cap - 1);
            }
            uint64_t score = (occ >= 2) ? (uint64_t)(occ - 1) * chunks[i].len : 0;
            if (score > best_score) { best_score = score; best = i; }
        }
        if (best == (size_t)-1 || best_score == 0) break;
        if (total + chunks[best].len > dict_size) break;
        state[best] = 1;
        total += chunks[best].len;
        for (size_t i = 0; i < n_chunks; i++) {
            if (state[i] == 0 && chunks[i].hash == chunks[best].hash) state[i] = 2;
        }
    }

    size_t n_sel = 0;
    for (size_t i = 0; i < n_chunks; i++) if (state[i] == 1) n_sel++;
    uint8_t *outbuf = (uint8_t *)zgec_alloc(total + ZGEC_OUTPUT_SLACK, 64);
    if (!outbuf) { zgec_free(state); zgec_free(table); zgec_free(chunks); return ZGEC_ERR_NOMEM; }
    if (n_sel > 0) {
        zgec_sel_entry *sel = (zgec_sel_entry *)zgec_alloc(n_sel * sizeof(*sel), _Alignof(zgec_sel_entry));
        if (!sel) { zgec_free(outbuf); zgec_free(state); zgec_free(table); zgec_free(chunks); return ZGEC_ERR_NOMEM; }
        size_t k = 0;
        for (size_t i = 0; i < n_chunks; i++) {
            if (state[i] != 1) continue;
            uint64_t h = chunks[i].hash;
            if (h == 0) h = 1;
            size_t idx = (size_t)(h & (uint64_t)(table_cap - 1));
            uint32_t occ = 0;
            for (;;) {
                if (table[idx].hash == h) { occ = table[idx].count; break; }
                idx = (idx + 1) & (table_cap - 1);
            }
            sel[k].idx = (uint32_t)i;
            sel[k].score = (occ >= 2) ? (uint64_t)(occ - 1) * chunks[i].len : 0;
            sel[k].pos = chunks[i].pos;
            sel[k].len = chunks[i].len;
            k++;
        }
        qsort(sel, n_sel, sizeof(*sel), zgec_sel_cmp_asc);
        /* Preserve adjacency: greedily chain chunks that were adjacent
           in source so longer matches survive. Walk ascending-score
           order; after emitting a chunk, immediately emit any unused
           successor that starts where it ends. */
        uint8_t *used = (uint8_t *)zgec_alloc(n_sel ? n_sel : 1, 1);
        if (!used) { zgec_free(sel); zgec_free(outbuf); zgec_free(state); zgec_free(table); zgec_free(chunks); return ZGEC_ERR_NOMEM; }
        memset(used, 0, n_sel);
        size_t off = 0;
        for (size_t i = 0; i < n_sel; i++) {
            if (used[i]) continue;
            size_t cur = i;
            used[cur] = 1;
            {
                uint32_t ci = sel[cur].idx;
                memcpy(outbuf + off, data + chunks[ci].pos, chunks[ci].len);
                off += chunks[ci].len;
            }
            for (;;) {
                uint64_t want = (uint64_t)sel[cur].pos + sel[cur].len;
                size_t nxt = (size_t)-1;
                for (size_t j = 0; j < n_sel; j++) {
                    if (used[j]) continue;
                    if ((uint64_t)sel[j].pos == want) { nxt = j; break; }
                }
                if (nxt == (size_t)-1) break;
                used[nxt] = 1;
                {
                    uint32_t ci = sel[nxt].idx;
                    memcpy(outbuf + off, data + chunks[ci].pos, chunks[ci].len);
                    off += chunks[ci].len;
                }
                cur = nxt;
            }
        }
        zgec_free(used);
        zgec_free(sel);
    }

    zgec_free(state);
    zgec_free(table);
    zgec_free(chunks);
    *out = outbuf;
    *out_size = total;
    return ZGEC_OK;
}

size_t zgec_dict_default_size(size_t epoch_raw_bytes, int max_dict_log2)
{
    size_t d = epoch_raw_bytes / 32;
    if (d < 65536) d = 65536;
    if (d > (size_t)1048576) d = (size_t)1048576;
    if (max_dict_log2 < 26) {
        size_t max = (max_dict_log2 >= 0) ? ((size_t)1 << (unsigned)max_dict_log2) : 0;
        if (d > max) d = max;
    }
    return d;
}
