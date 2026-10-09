#include "zgec_encode.h"
#include "zgec_block.h"
#include "zgec_bitstream.h"
#include "zgec_crc32c.h"
#include "zgec_dict.h"
#include "zgec_fse.h"
#include "zgec_lit.h"
#include "zgec_parse.h"
#include "zgec_rans.h"
#include "zgec_seq.h"
#include "zgec_xxhash.h"

#include "zgec_internal.h"

#include "zgec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Encoder pipeline per spec sections 11.1, 11.5-11.8 and 5.3-5.5.
 *
 * Pipeline (11.1):
 *   1. Dictionary pass: per epoch, train a candidate from the
 *      previous epoch's raw data (5.7) and keep it only if the
 *      sampled saving passes the ratio gate (5.5).
 *   2. Block pass over blocks on a worker pool that takes one block
 *      at a time (blocks are independent once their dictionaries are
 *      known, 11.8): parse, segment (11.5), context maps (11.6),
 *      per-segment coder selection (11.7), emit. The two
 *      order-dependent steps -- the literal-reference chain of 6.3
 *      and the RAW fallback -- stay in a serial pass in file order.
 *   3. Assemble in order (DICT before first user), footer, trailer.
 *
 * Coder emission (section 11.7): the decoder accepts raw and rANS
 * literals, sub-literals and sequence conditioning, so the encoder
 * emits all of them when the 11.6/11.7 decision machinery selects
 * them. The three ZGEC_ENC_EMIT_* switches below are the single place
 * that gates emission; the selection logic is untouched by them.
 * use_litref keeps the run of exportable predecessors and passes the
 * resulting LITREF region to a block only when a measurement shows a
 * smaller payload (6.3); use_dicts, use_sublit, use_contexts,
 * use_conditioning and use_filter gate the 5.5/11.6/7.5 machinery the
 * same way.
 */
#define ZGEC_ENC_EMIT_RANS 1
#define ZGEC_ENC_EMIT_SUBLIT 1
#define ZGEC_ENC_EMIT_SEQ_COND 1

/* Per-segment table/header overhead estimate in bits (directory
 * entry + header + typical sequence-table descriptors). Used only
 * for merge/selection comparisons (informative). */
#define ZGEC_ENC_SEG_OVERHEAD_BITS (600.0 * 8.0)

/* Decode-cycle estimates per symbol (11.7, platform constants). */
#define ZGEC_ENC_CYC_RAW_PER_SYM 1.0
#define ZGEC_ENC_CYC_RANS_PER_SYM 3.0
#define ZGEC_ENC_CYC_SUBLIT_FAST_PER_SYM 1.0
#define ZGEC_ENC_CYC_SUBLIT_SLOW_PER_SYM 10.0
#define ZGEC_ENC_CYC_SEQ_PER_SEQ 6.0

/* Approximate bytes of one rANS literal-table description (AL=11,
 * 256 symbols) when an exact serialisation is not at hand. */
#define ZGEC_ENC_RANS_TABLE_BYTES 300u

/* ---- threading (section 11.8) ----
 * Shared shim lives in zgec_internal.h (same shape as the decoder used).
 * Workers claim work items from one shared counter, so a pool takes one
 * block at a time (11.8) and the work is balanced without static ranges. */

/* One shared work list: fn(ctx, i) for i in 0..n-1, claimed one index
 * at a time. Writers of item i are the only writers of i, so the
 * result does not depend on the claim order. */
typedef struct {
    zgec_mu mu;
    size_t     next;
    size_t     n;
    void     (*fn)(void *, size_t);
    void      *ctx;
} zgec_enc_work;

static void zgec_enc_work_run(zgec_enc_work *w)
{
    for (;;) {
        size_t i;
        zgec_mu_lock(&w->mu);
        i = w->next;
        if (i < w->n) w->next = i + 1;
        zgec_mu_unlock(&w->mu);
        if (i >= w->n) return;
        w->fn(w->ctx, i);
    }
}

#if defined(_WIN32)
static DWORD WINAPI zgec_enc_work_proc_win(LPVOID arg)
{
    zgec_enc_work_run((zgec_enc_work *)arg);
    return (DWORD)0;
}
#else
static void *zgec_enc_work_proc_posix(void *arg)
{
    zgec_enc_work_run((zgec_enc_work *)arg);
    return NULL;
}
#endif

/* Run fn over n items on at most n_threads workers. One thread (or one
 * item) runs inline in the caller, which also takes a share of the
 * work, so a failed thread spawn only costs parallelism. */
static void zgec_enc_parallel_for(int n_threads, size_t n,
                                  void (*fn)(void *, size_t), void *ctx)
{
    zgec_enc_work w;
    size_t workers;
    size_t t;
    size_t i;
    if (fn == NULL || n == 0) return;
    if (n_threads < 1) n_threads = 1;
    workers = (size_t)n_threads;
    if (workers > n) workers = n;
    if (workers > (size_t)ZGEC_MAX_WORKERS)
        workers = (size_t)ZGEC_MAX_WORKERS;
    if (workers <= 1) {
        for (i = 0; i < n; i++) fn(ctx, i);
        return;
    }
    memset(&w, 0, sizeof(w));
    zgec_mu_init(&w.mu);
    w.next = 0;
    w.n = n;
    w.fn = fn;
    w.ctx = ctx;
    if (w.mu.ok == 0) {
        /* No lock means no safe claim order: stay serial rather than
         * race on the shared counter. */
        for (i = 0; i < n; i++) fn(ctx, i);
        return;
    }
#if defined(_WIN32)
    {
        HANDLE *hs = (HANDLE *)zgec_alloc((workers - 1) * sizeof(*hs),
                                          _Alignof(HANDLE));
        if (hs != NULL) {
            for (t = 0; t + 1 < workers; t++) {
                hs[t] = CreateThread(NULL, 0, zgec_enc_work_proc_win,
                                     (LPVOID)&w, 0, NULL);
            }
        }
        zgec_enc_work_run(&w); /* the caller works too */
        if (hs != NULL) {
            /* Every worker must have stopped before the work list -- a
             * caller stack frame -- goes out of scope. */
            for (t = 0; t + 1 < workers; t++) {
                if (hs[t] != NULL) (void)WaitForSingleObject(hs[t], INFINITE);
            }
            for (t = 0; t + 1 < workers; t++) {
                if (hs[t] != NULL) (void)CloseHandle(hs[t]);
            }
            zgec_free(hs);
        }
    }
#else
    {
        pthread_t *ths = (pthread_t *)zgec_alloc((workers - 1) * sizeof(*ths),
                                                 _Alignof(pthread_t));
        unsigned char *started = (unsigned char *)zgec_alloc(workers - 1, 1);
        if (started != NULL) memset(started, 0, workers - 1);
        if (ths != NULL && started != NULL) {
            for (t = 0; t + 1 < workers; t++) {
                if (pthread_create(&ths[t], NULL, zgec_enc_work_proc_posix,
                                   (void *)&w) == 0) {
                    started[t] = 1;
                }
            }
        }
        zgec_enc_work_run(&w); /* the caller works too */
        if (ths != NULL && started != NULL) {
            for (t = 0; t + 1 < workers; t++) {
                if (started[t] != 0) (void)pthread_join(ths[t], NULL);
            }
        }
        zgec_free(ths);
        zgec_free(started);
    }
#endif
    zgec_mu_destroy(&w.mu);
}

zgec_err zgec_compress(const uint8_t *src, size_t src_size,
                        uint8_t **dst, size_t *dst_size)
{
    zgec_params p;
    zgec_params_default(&p);
    zgec_encoder *e = zgec_encoder_create(&p);
    if (!e) return ZGEC_ERR_NOMEM;
    zgec_err err = zgec_encode_frame(e, src, src_size, dst, dst_size);
    zgec_encoder_destroy(e);
    return err;
}

/* ---- defaults ---- */

void zgec_params_default(zgec_params *p)
{
    memset(p, 0, sizeof(*p));
    p->block_log2 = 21;
    p->epoch_blocks = 10;
    p->max_dict_log2 = 20;
    p->seg_hint_log2 = 18;
    p->tier = ZGEC_TIER_MAIN;
    p->n_threads = 0; /* one worker per core (11.8) */
    p->use_dicts = 0;
    p->use_litref = 0;
    p->use_sublit = 0;
    p->use_contexts = 0;
    p->use_conditioning = 0;
    p->block_checksums = 0;
    p->lambda = 0.0;
}

/* ---- encoder state ---- */

/* External dictionaries registered through the API (section 5.8). Their
 * bytes never enter the frame, so the encoder keeps its own copy. */
#define ZGEC_ENC_MAX_EXT 4

typedef struct {
    int      used;
    uint16_t id;
    uint8_t *data;
    size_t   size;
} zgec_enc_ext_dict;

struct zgec_encoder {
    zgec_params params;
    zgec_enc_ext_dict ext[ZGEC_ENC_MAX_EXT];
};

zgec_encoder *zgec_encoder_create(const zgec_params *p)
{
    zgec_encoder *e = (zgec_encoder *)zgec_alloc(sizeof(*e), _Alignof(zgec_encoder));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    if (p) e->params = *p;
    return e;
}

void zgec_encoder_destroy(zgec_encoder *e)
{
    int i;
    if (!e) return;
    for (i = 0; i < ZGEC_ENC_MAX_EXT; i++) zgec_free(e->ext[i].data);
    zgec_free(e);
}

zgec_err zgec_encoder_set_external_dict(zgec_encoder *e,
                                        uint16_t dict_id,
                                        const uint8_t *data, size_t size)
{
    int slot = -1;
    int i;
    uint8_t *cp;
    if (!e || !data || size == 0 || dict_id == 0) return ZGEC_ERR_INVAL;
    if (size > ((size_t)1 << (unsigned)ZGEC_MAX_DICT_LOG2))
        return ZGEC_ERR_DICT_SIZE;
    for (i = 0; i < ZGEC_ENC_MAX_EXT; i++) {
        if (e->ext[i].used && e->ext[i].id == dict_id) { slot = i; break; }
    }
    if (slot < 0) {
        for (i = 0; i < ZGEC_ENC_MAX_EXT; i++) {
            if (!e->ext[i].used) { slot = i; break; }
        }
    }
    if (slot < 0) return ZGEC_ERR_NOMEM;
    cp = (uint8_t *)zgec_alloc(size, 64);
    if (!cp) return ZGEC_ERR_NOMEM;
    memcpy(cp, data, size);
    zgec_free(e->ext[slot].data);
    e->ext[slot].used = 1;
    e->ext[slot].id = dict_id;
    e->ext[slot].data = cp;
    e->ext[slot].size = size;
    return ZGEC_OK;
}

void zgec_encoder_clear_external_dicts(zgec_encoder *e)
{
    int i;
    if (!e) return;
    for (i = 0; i < ZGEC_ENC_MAX_EXT; i++) {
        zgec_free(e->ext[i].data);
        e->ext[i].data = NULL;
        e->ext[i].used = 0;
        e->ext[i].id = 0;
        e->ext[i].size = 0;
    }
}

/* ---- small utilities ---- */

/* Grow *buf to hold need bytes; realloc-style (zgec_alloc has no realloc). */
static zgec_err zgec_payload_grow(uint8_t **buf, size_t *cap, size_t used, size_t need)
{
    uint8_t *nb;
    size_t ncap;
    if (!buf || !cap) return ZGEC_ERR_INVAL;
    if (need <= *cap) return ZGEC_OK;
    if (used > *cap) return ZGEC_ERR_INTERNAL;
    if (used > need) return ZGEC_ERR_INTERNAL;
    ncap = *cap ? *cap : 256;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2) { ncap = need; break; }
        ncap *= 2;
    }
    nb = (uint8_t *)zgec_alloc(ncap, 64);
    if (!nb) return ZGEC_ERR_NOMEM;
    if (*buf && used > 0) memcpy(nb, *buf, used);
    zgec_free(*buf);
    *buf = nb;
    *cap = ncap;
    return ZGEC_OK;
}

/* Histogram normalisation lives in seq.c (zgec_normalize_counts,
 * see zgec_internal.h); the former byte-identical copy here was removed. */

/* Shannon entropy estimate in bits of a histogram. */
static double zgec_enc_entropy_bits_u32(const uint32_t *hist, int nsym)
{
    uint64_t total = 0;
    for (int s = 0; s < nsym; s++) total += (uint64_t)hist[s];
    if (total == 0) return 0.0;
    {
        double tlog = zgec_fast_log2_u64(total);
        double bits = 0.0;
        for (int s = 0; s < nsym; s++) {
            if (hist[s] > 0) {
                bits += (double)(uint64_t)hist[s] *
                        (tlog - zgec_fast_log2_u64((uint64_t)hist[s]));
            }
        }
        return bits;
    }
}

/* Exact serialised size in bytes of one sequence-table description
 * (AL=10, 66 symbols) for header-cost accounting. */
static size_t zgec_enc_seq_desc_size(const uint32_t *hist)
{
    int16_t counts[ZGEC_NSYM_SEQ];
    uint8_t tmp[1024];
    (void)zgec_normalize_counts(counts, hist, ZGEC_NSYM_SEQ, 10);
    return zgec_fse_write_counts(tmp, sizeof(tmp), counts, ZGEC_NSYM_SEQ, 10);
}

/* Exact serialised size in bytes of one literal-table description
 * (AL=11, 256 symbols). */
static size_t zgec_enc_lit_desc_size(const uint32_t *hist)
{
    int16_t counts[ZGEC_NSYM_LIT];
    uint8_t tmp[2048];
    zgec_err err = zgec_rans_normalise(counts, hist);
    if (err != ZGEC_OK) return ZGEC_ENC_RANS_TABLE_BYTES;
    {
        size_t n = zgec_fse_write_counts(tmp, sizeof(tmp), counts,
                                         ZGEC_NSYM_LIT, ZGEC_LIT_AL);
        return (n > 0) ? n : (size_t)ZGEC_ENC_RANS_TABLE_BYTES;
    }
}

static int zgec_enc_all_same_byte(const uint8_t *src, size_t n)
{
    uint64_t w;
    size_t i;
    size_t head;
    if (n == 0) return 0;
    if (n < 8) {
        for (i = 1; i < n; i++) {
            if (src[i] != src[0]) return 0;
        }
        return 1;
    }
    w = 0;
    memset(&w, src[0], sizeof(w));
    head = n & ~(size_t)7;
    for (i = 0; i < head; i += 8) {
        uint64_t v = 0;
        memcpy(&v, src + i, sizeof(v));
        if (v != w) return 0;
    }
    for (i = head; i < n; i++) {
        if (src[i] != src[0]) return 0;
    }
    return 1;
}

/* Run-start bitmap per 9.3 via the shared zgec_lit_runstart helper.
 * The encoder generates valid (ll, n_lit) pairs, so the validated call
 * always succeeds; on the impossible error path the bitmap is cleared
 * to keep the downstream histograms well-defined. */
static void zgec_enc_runstart(uint8_t *runstart, size_t n_lit,
                              const uint32_t *ll, size_t n_seq)
{
    if (n_lit == 0) return;
    if (runstart == NULL) return;
    if (zgec_lit_runstart(runstart, n_lit, ll, n_seq) != ZGEC_OK) {
        memset(runstart, 0, n_lit);
    }
}

/* Section 8.2: repeat offsets reset to (1, 4, 8) at the start of every
 * segment and never carry over, but the parse assigns each sequence's
 * offbase with one move-to-front chain running across the whole block
 * (segment boundaries are not known until after segmentation). Rewrite
 * the offbase of every sequence from its explicit distance, restarting
 * the chain at each segment, so the decoder's per-segment reset
 * resolves exactly the offsets the parse chose. dist[] is caller-owned
 * explicit distances (D3), resolved once from the whole-block chain by
 * the caller, so re-segmentation can rewrite for new bounds. */
static void zgec_enc_rewrite_offbase_from_dist(zgec_parse *parse,
                                               const size_t *bounds,
                                               size_t n_segments,
                                               const uint32_t *dist,
                                               int *changed)
{
    size_t s;
    size_t i;
    if (changed) {
        *changed = 0;
    }
    if (!parse || !bounds || !dist) return;
    for (s = 0; s < n_segments; s++) {
        zgec_reps seg;
        zgec_reps_init(&seg);
        for (i = bounds[s]; i < bounds[s + 1] && i < parse->n_seq; i++) {
            uint32_t ob = zgec_reps_encode(&seg, dist[i]);
            (void)zgec_reps_resolve(&seg, ob);
            if (parse->seq[i].offbase != ob) {
                if (changed) {
                    *changed = 1;
                }
                parse->seq[i].offbase = ob;
            }
        }
    }
}

/* rep0 in effect before each sequence of a run of n sequences. */
static void zgec_enc_rep0_before(uint32_t *rep0_out,
                                 const zgec_sequence *seq, size_t n)
{
    zgec_reps r;
    size_t i;
    zgec_reps_init(&r);
    for (i = 0; i < n; i++) {
        rep0_out[i] = r.rep[0];
        (void)zgec_reps_resolve(&r, seq[i].offbase);
    }
}

/* rep0 in effect after the last sequence of a run (used as the tail
 * literals' predictor, section 9.6). */
static uint32_t zgec_enc_rep0_after(const zgec_sequence *seq, size_t n)
{
    zgec_reps r;
    size_t i;
    zgec_reps_init(&r);
    for (i = 0; i < n; i++) (void)zgec_reps_resolve(&r, seq[i].offbase);
    return r.rep[0];
}

/* Build the sub-literal residual buffer Z (section 9.6) exactly as the
 * decoder reconstructs it: each literal is predicted from the byte rep0
 * back in the virtual buffer, the match bytes are skipped, and tail
 * literals use rep0 after the last sequence. ml_coded[s] holds the coded
 * ML-3 value (section 8.1), so the real match length is ml_coded[s] + 3.
 * vb_start is the virtual-buffer position of the slice's first output
 * byte. */
static void zgec_enc_build_resid(uint8_t *z, const uint8_t *lit,
                                 size_t n_lit, const uint32_t *ll,
                                 const uint32_t *ml_coded, size_t n_seq,
                                 const uint32_t *rep0_before,
                                 uint32_t rep0_tail, const uint8_t *vb,
                                 size_t vb_start)
{
    size_t p = vb_start;
    size_t li = 0;
    size_t s;
    for (s = 0; s < n_seq; s++) {
        uint32_t r0 = rep0_before ? rep0_before[s] : 1u;
        uint32_t llen = ll ? ll[s] : 0u;
        uint32_t mlen = ml_coded ? (ml_coded[s] + 3u) : 0u;
        uint32_t u;
        for (u = 0; u < llen && li < n_lit; u++) {
            uint8_t pred = 0;
            if (r0 != 0u && (size_t)r0 <= p) pred = vb[p - (size_t)r0];
            z[li] = (uint8_t)((unsigned)lit[li] - (unsigned)pred);
            li++;
            p++;
        }
        p += (size_t)mlen;
    }
    for (; li < n_lit; li++) {
        uint8_t pred = 0;
        if (rep0_tail != 0u && (size_t)rep0_tail <= p)
            pred = vb[p - (size_t)rep0_tail];
        z[li] = (uint8_t)((unsigned)lit[li] - (unsigned)pred);
        p++;
    }
}

/* ---- segmentation (11.5) ---- */

typedef struct {
    uint32_t lit[ZGEC_NSYM_LIT];
    uint32_t ll[ZGEC_NSYM_SEQ];
    uint32_t ml[ZGEC_NSYM_SEQ];
    uint32_t of[ZGEC_NSYM_SEQ];
    uint64_t extra_bits;
    size_t n_seq;
} zgec_granule;

static double zgec_granule_cost_bits(const zgec_granule *g)
{
    double bits;
    if (g->n_seq == 0) return 0.0;
    bits = zgec_enc_entropy_bits_u32(g->lit, ZGEC_NSYM_LIT);
    bits += zgec_enc_entropy_bits_u32(g->ll, ZGEC_NSYM_SEQ);
    bits += zgec_enc_entropy_bits_u32(g->ml, ZGEC_NSYM_SEQ);
    bits += zgec_enc_entropy_bits_u32(g->of, ZGEC_NSYM_SEQ);
    bits += (double)g->extra_bits;
    bits += ZGEC_ENC_SEG_OVERHEAD_BITS;
    return bits;
}

static void zgec_granule_merge(zgec_granule *dst, const zgec_granule *src)
{
    int s;
    for (s = 0; s < ZGEC_NSYM_LIT; s++) dst->lit[(size_t)s] += src->lit[(size_t)s];
    for (s = 0; s < ZGEC_NSYM_SEQ; s++) {
        dst->ll[(size_t)s] += src->ll[(size_t)s];
        dst->ml[(size_t)s] += src->ml[(size_t)s];
        dst->of[(size_t)s] += src->of[(size_t)s];
    }
    dst->extra_bits += src->extra_bits;
    dst->n_seq += src->n_seq;
}

/* Split the parse into ~16K-sequence granules with additive histograms,
 * then greedily merge adjacent granules while one table set (plus one
 * header) is no larger than two. Boundaries always fall between
 * sequences, so a segment owns its literals entirely. */
static zgec_err zgec_segment_greedy(const zgec_parse *parse,
                                    size_t **bounds_out, size_t *n_seg_out)
{
    const size_t granule_seqs = 16384;
    size_t n_gran;
    zgec_granule *grans = NULL;
    size_t *gstart = NULL;
    uint32_t *lit_of_seq = NULL;
    size_t i;
    size_t gi;
    size_t lit_pos;
    int changed;

    if (!parse || !bounds_out || !n_seg_out) return ZGEC_ERR_INVAL;
    if (parse->n_seq == 0) {
        size_t *b = (size_t *)zgec_alloc(2 * sizeof(size_t), _Alignof(size_t));
        if (!b) return ZGEC_ERR_NOMEM;
        b[0] = 0;
        b[1] = 0;
        *bounds_out = b;
        *n_seg_out = 1;
        return ZGEC_OK;
    }

    n_gran = (parse->n_seq + granule_seqs - 1) / granule_seqs;
    if (n_gran == 0) n_gran = 1;
    if (n_gran > ZGEC_MAX_SEGMENTS) n_gran = ZGEC_MAX_SEGMENTS;

    grans = (zgec_granule *)zgec_alloc(n_gran * sizeof(*grans),
                                      _Alignof(zgec_granule));
    gstart = (size_t *)zgec_alloc((n_gran + 1) * sizeof(*gstart),
                                 _Alignof(size_t));
    if (parse->n_seq > UINT32_MAX - 1) return ZGEC_ERR_INVAL;
    lit_of_seq = (uint32_t *)zgec_alloc((parse->n_seq + 1) * sizeof(*lit_of_seq),
                                     _Alignof(uint32_t));
    if (!grans || !gstart || !lit_of_seq) {
        zgec_free(grans);
        zgec_free(gstart);
        zgec_free(lit_of_seq);
        return ZGEC_ERR_NOMEM;
    }
    memset(grans, 0, n_gran * sizeof(*grans));

    /* Sequence -> literal-start map from LL prefix sums. */
    lit_pos = 0;
    for (i = 0; i < parse->n_seq; i++) {
        if (lit_pos > UINT32_MAX) return ZGEC_ERR_INTERNAL;
        lit_of_seq[i] = (uint32_t)lit_pos;
        lit_pos += (size_t)parse->seq[i].ll;
    }
    if (lit_pos > UINT32_MAX) { zgec_free(grans); zgec_free(gstart); zgec_free(lit_of_seq); return ZGEC_ERR_INTERNAL; }
    lit_of_seq[parse->n_seq] = (uint32_t)lit_pos;

    {
        size_t per = (parse->n_seq + n_gran - 1) / n_gran;
        gstart[0] = 0;
        for (gi = 1; gi < n_gran; gi++) {
            size_t p = gi * per;
            gstart[gi] = (p > parse->n_seq) ? parse->n_seq : p;
        }
        gstart[n_gran] = parse->n_seq;
    }

    for (gi = 0; gi < n_gran; gi++) {
        zgec_granule *g = &grans[gi];
        for (i = gstart[gi]; i < gstart[gi + 1]; i++) {
            const zgec_sequence *q = &parse->seq[i];
            uint8_t nb = 0;
            uint8_t c;
            size_t t;
            uint32_t mlv = (q->ml >= 3) ? (q->ml - 3u) : 0u;
            uint32_t ofv = (q->offbase >= 1) ? (q->offbase - 1u) : 0u;
            c = zgec_seq_code_of(q->ll, &nb);
            g->ll[c]++;
            g->extra_bits += (uint64_t)nb;
            c = zgec_seq_code_of(mlv, &nb);
            g->ml[c]++;
            g->extra_bits += (uint64_t)nb;
            c = zgec_seq_code_of(ofv, &nb);
            g->of[c]++;
            g->extra_bits += (uint64_t)nb;
            g->n_seq++;
            for (t = lit_of_seq[i]; t < lit_of_seq[i + 1]; t++) {
                if (t < parse->n_lit) g->lit[parse->lit[t]]++;
            }
        }
    }
    /* Tail literals belong to the last granule. */
    for (i = lit_pos; i < parse->n_lit; i++) grans[n_gran - 1].lit[parse->lit[i]]++;

    /* Greedy adjacent merge passes. P5: per-granule costs are cached
     * once per pass (the old code re-evaluated both inputs' entropy on
     * every candidate pair, i.e. 3 full 4x(256+198)-symbol scans per
     * pair), and candidates merge into one reused scratch instead of a
     * ~1.9 KiB stack copy per pair. */
    changed = 1;
    {
        double *gcost = (double *)zgec_alloc(
            (n_gran ? n_gran : 1) * sizeof(*gcost), _Alignof(double));
        zgec_granule comb;
        if (!gcost) {
            zgec_free(grans);
            zgec_free(gstart);
            zgec_free(lit_of_seq);
            return ZGEC_ERR_NOMEM;
        }
        memset(&comb, 0, sizeof(comb));
        while (changed && n_gran > 1) {
            size_t w = 0;
            changed = 0;
            for (gi = 0; gi < n_gran; gi++) {
                gcost[gi] = zgec_granule_cost_bits(&grans[gi]);
            }
            for (gi = 0; gi < n_gran; gi++) {
                if (gi + 1 < n_gran) {
                    double cc;
                    comb = grans[gi];
                    zgec_granule_merge(&comb, &grans[gi + 1]);
                    cc = zgec_granule_cost_bits(&comb);
                    if (cc <= gcost[gi] + gcost[gi + 1]) {
                        grans[w] = comb;
                        gcost[w] = cc;
                        gstart[w + 1] = gstart[gi + 2];
                        gi++; /* consumed the neighbour */
                        changed = 1;
                        w++;
                        continue;
                    }
                }
                if (w != gi) {
                    grans[w] = grans[gi];
                    gcost[w] = gcost[gi];
                }
                gstart[w + 1] = gstart[gi + 1];
                w++;
            }
            n_gran = w;
        }
        zgec_free(gcost);
    }

    {
        size_t *b = (size_t *)zgec_alloc((n_gran + 1) * sizeof(*b),
                                        _Alignof(size_t));
        if (!b) {
            zgec_free(grans);
            zgec_free(gstart);
            zgec_free(lit_of_seq);
            return ZGEC_ERR_NOMEM;
        }
        for (gi = 0; gi <= n_gran; gi++) b[gi] = gstart[gi];
        *bounds_out = b;
        *n_seg_out = n_gran;
    }
    zgec_free(grans);
    zgec_free(gstart);
    zgec_free(lit_of_seq);
    return ZGEC_OK;
}

/* ---- context map construction (11.6) ---- */

#define ZGEC_ENC_NCLASS 64
#define ZGEC_ENC_MAXCTX 8

/* Entropy of one 256-entry class histogram. */
static double zgec_class_entropy(const uint32_t *h)
{
    return zgec_enc_entropy_bits_u32(h, ZGEC_NSYM_LIT);
}

/* Portable thread-local storage (S3): MSVC's C11 mode historically lacks
 * _Thread_local, so use __declspec(thread) there. */
#if defined(_MSC_VER)
#define ZGEC_ENC_TLS __declspec(thread)
#else
#define ZGEC_ENC_TLS _Thread_local
#endif

/* Snapshot the current clustering as a class -> context map in which
 * every entry is strictly below `k` (the number of alive clusters).
 * Called at the moment the merge loop reaches k clusters, so the map
 * matches exactly the table set whose body bits were just recorded. */
static void zgec_cluster_snapshot(uint8_t members[ZGEC_ENC_NCLASS][ZGEC_ENC_NCLASS],
                                  const uint8_t *nmem, const int *alive,
                                  uint8_t map_out[ZGEC_ENC_NCLASS])
{
    uint8_t next = 0;
    int a;
    for (a = 0; a < ZGEC_ENC_NCLASS; a++) {
        uint8_t i;
        if (!alive[a]) continue;
        for (i = 0; i < nmem[a]; i++) {
            map_out[members[a][i]] = next;
        }
        next++;
    }
}

/* Greedy clustering of 64 classes (section 11.6): start from
 * singletons, repeatedly merge the pair whose merge increases coded
 * size least, and record the exact body bits and the exact class ->
 * context map at 8, 4, 2 and 1 clusters. The maps are the ones the
 * caller emits: an earlier implementation kept only the final (single)
 * cluster and folded its ids modulo k, which mapped every class to
 * context 0 and defeated context coding entirely. */
static void zgec_cluster_greedy(const uint32_t *cls_hist /*[64][256]*/,
                                double *bits_k8, double *bits_k4,
                                double *bits_k2, double *bits_k1,
                                uint8_t assign_out[4][ZGEC_ENC_NCLASS])
{
    /* Cluster histograms, at most 64 clusters of merged classes. The
     * scratch is large (64 KiB), so it is thread-local rather than a
     * stack local: the block pass runs one block per worker (11.8). */
    static ZGEC_ENC_TLS uint32_t
        work[ZGEC_ENC_NCLASS][ZGEC_NSYM_LIT];
    /* Cached merge deltas dlt[a][b] for a < b. A merge changes only the
     * deltas that involve the surviving cluster, so one row is
     * refreshed per merge instead of every pair. The greedy choice and
     * its tie-breaking scan order are unchanged, so the result is
     * identical to recomputing all pairs each time. */
    static ZGEC_ENC_TLS double dlt[ZGEC_ENC_NCLASS][ZGEC_ENC_NCLASS];
    uint32_t merged[ZGEC_NSYM_LIT];
    uint8_t members[ZGEC_ENC_NCLASS][ZGEC_ENC_NCLASS];
    uint8_t nmem[ZGEC_ENC_NCLASS];
    int alive[ZGEC_ENC_NCLASS];
    double cent[ZGEC_ENC_NCLASS];   /* cached entropy of each live cluster */
    int ncl = ZGEC_ENC_NCLASS;
    int c;
    int s;
    double total = 0.0;

    for (c = 0; c < ZGEC_ENC_NCLASS; c++) {
        for (s = 0; s < ZGEC_NSYM_LIT; s++) {
            work[(size_t)c][(size_t)s] = cls_hist[(size_t)c * 256u + (uint32_t)s];
        }
        members[(size_t)c][0] = (uint8_t)c;
        nmem[(size_t)c] = 1;
        alive[(size_t)c] = 1;
        cent[(size_t)c] = zgec_class_entropy(work[(size_t)c]);
        total += cent[(size_t)c];
    }
    memset(assign_out, 0, 4u * ZGEC_ENC_NCLASS);
    *bits_k8 = -1.0;
    *bits_k4 = -1.0;
    *bits_k2 = -1.0;
    *bits_k1 = -1.0;

    /* Seed the delta cache once (all pairs of singletons). */
    for (c = 0; c < ZGEC_ENC_NCLASS; c++) {
        int bb;
        for (bb = c + 1; bb < ZGEC_ENC_NCLASS; bb++) {
            for (s = 0; s < ZGEC_NSYM_LIT; s++) {
                merged[(size_t)s] =
                    work[(size_t)c][(size_t)s] + work[(size_t)bb][(size_t)s];
            }
            dlt[(size_t)c][(size_t)bb] =
                zgec_class_entropy(merged) - cent[(size_t)c] - cent[(size_t)bb];
        }
    }

    while (ncl > 1) {
        int best_a = -1;
        int best_b = -1;
        double best_delta = 0.0;
        int first = 1;
        int a;

        if (ncl == 8) { *bits_k8 = total; zgec_cluster_snapshot(members, nmem, alive, assign_out[3]); }
        if (ncl == 4) { *bits_k4 = total; zgec_cluster_snapshot(members, nmem, alive, assign_out[2]); }
        if (ncl == 2) { *bits_k2 = total; zgec_cluster_snapshot(members, nmem, alive, assign_out[1]); }

        for (a = 0; a < ZGEC_ENC_NCLASS; a++) {
            int bb;
            if (!alive[a]) continue;
            for (bb = a + 1; bb < ZGEC_ENC_NCLASS; bb++) {
                double delta;
                if (!alive[bb]) continue;
                delta = dlt[(size_t)a][(size_t)bb];
                if (first || delta < best_delta) {
                    best_delta = delta;
                    best_a = a;
                    best_b = bb;
                    first = 0;
                }
            }
        }
        if (best_a < 0) break;
        /* Merge best_b into best_a. */
        for (s = 0; s < ZGEC_NSYM_LIT; s++) {
            work[(size_t)best_a][(size_t)s] += work[(size_t)best_b][(size_t)s];
        }
        {
            uint8_t nm = nmem[(size_t)best_a];
            uint8_t i;
            for (i = 0; i < nmem[(size_t)best_b]; i++) {
                members[(size_t)best_a][(size_t)nm + (size_t)i] =
                    members[(size_t)best_b][(size_t)i];
            }
            nmem[(size_t)best_a] = (uint8_t)((unsigned)nm + (unsigned)nmem[(size_t)best_b]);
        }
        cent[(size_t)best_a] = zgec_class_entropy(work[(size_t)best_a]);
        alive[best_b] = 0;
        total += best_delta;
        ncl--;
        /* Refresh only the deltas that involve the survivor. */
        for (a = 0; a < ZGEC_ENC_NCLASS; a++) {
            int lo;
            int hi;
            if (a == best_a || !alive[a]) continue;
            lo = (a < best_a) ? a : best_a;
            hi = (a < best_a) ? best_a : a;
            for (s = 0; s < ZGEC_NSYM_LIT; s++) {
                merged[(size_t)s] =
                    work[(size_t)lo][(size_t)s] + work[(size_t)hi][(size_t)s];
            }
            dlt[(size_t)lo][(size_t)hi] =
                zgec_class_entropy(merged) - cent[(size_t)lo] - cent[(size_t)hi];
        }
    }
    *bits_k1 = total;
    zgec_cluster_snapshot(members, nmem, alive, assign_out[0]);
}

/* Choose the context mode and count for the block (plain literals).
 * Builds the 64-class next-byte histogram (run starts skipped) for each
 * of LSB6/MSB6/TEXT/SIGNED in one pass over the literals, then clusters
 * each mode's classes greedily and jointly picks the (mode, k in
 * {1,2,4,8}) minimising body bits plus the block-parameter header
 * (2 bytes for k=1, 26 bytes otherwise). Every mode is clustered: how
 * far merging lowers the body depends on the mode, so ranking the modes
 * by their 64-class entropy is not a valid short cut. k=1 canonicalises
 * to mode 0. The map is emitted once per block; segments only pick
 * tables.
 * Cost note (P8): one 4*64*256-u32 histogram build plus 4 greedy
 * clusterings (each O(64^2*256) seed + 63 merges with one refreshed
 * row each). Once per block; acceptable next to parse/emit. */
static zgec_err zgec_select_contexts(const uint8_t *lit, size_t n_lit,
                                     const uint32_t *ll, size_t n_seq,
                                     uint8_t *mode_out, uint8_t *k_out,
                                     uint8_t cmap_out[ZGEC_ENC_NCLASS])
{
    static const int modes[4] = { ZGEC_CTX_LSB6, ZGEC_CTX_MSB6,
                                    ZGEC_CTX_TEXT, ZGEC_CTX_SIGNED };
    static const int ks[4] = { 1, 2, 4, 8 };
    const size_t hist_bytes = (size_t)64 * 256u * sizeof(uint32_t);
    uint8_t *runstart = NULL;
    uint32_t *hist4 = NULL;
    int best_mode = 0;
    int best_k = 1;
    uint8_t best_map[ZGEC_ENC_NCLASS];
    int mi;

    if (!lit || !mode_out || !k_out || !cmap_out) return ZGEC_ERR_INVAL;
    memset(best_map, 0, sizeof(best_map));
    if (n_lit == 0) {
        *mode_out = 0;
        *k_out = 1;
        memset(cmap_out, 0, ZGEC_ENC_NCLASS);
        return ZGEC_OK;
    }
    runstart = (uint8_t *)zgec_alloc(n_lit, 1);
    hist4 = (uint32_t *)zgec_alloc(4u * hist_bytes, 64);
    if (!runstart || !hist4) {
        zgec_free(runstart);
        zgec_free(hist4);
        return ZGEC_ERR_NOMEM;
    }
    zgec_enc_runstart(runstart, n_lit, ll, n_seq);

    /* One pass builds all four class histograms: LSB6 and MSB6 are
     * bit operations, TEXT/SIGNED are the Annex C tables. Every class
     * is < 64 by construction, so no class check is needed. */
    {
        uint32_t *h0 = hist4;
        uint32_t *h1 = hist4 + (size_t)64 * 256u;
        uint32_t *h2 = hist4 + (size_t)2 * (size_t)64 * 256u;
        uint32_t *h3 = hist4 + (size_t)3 * (size_t)64 * 256u;
        size_t j;
        memset(hist4, 0, 4u * hist_bytes);
        for (j = 1; j < n_lit; j++) {
            unsigned c0, c1, c2, c3;
            uint32_t nxt;
            uint8_t prev;
            if (runstart[j]) continue; /* run starts use their own table */
            prev = lit[j - 1];
            nxt = (uint32_t)lit[j];
            c0 = (unsigned)(prev & 63u);
            c1 = (unsigned)(prev >> 2u);
            c2 = (unsigned)zgec_classify(ZGEC_CTX_TEXT, prev);
            c3 = (unsigned)zgec_classify(ZGEC_CTX_SIGNED, prev);
            h0[(size_t)c0 * 256u + nxt]++;
            h1[(size_t)c1 * 256u + nxt]++;
            h2[(size_t)c2 * 256u + nxt]++;
            h3[(size_t)c3 * 256u + nxt]++;
        }
    }

    {
        double best_total = 0.0;
        int first = 1;
        for (mi = 0; mi < 4; mi++) {
            double b8 = 0.0;
            double b4 = 0.0;
            double b2 = 0.0;
            double b1 = 0.0;
            uint8_t assign[4][ZGEC_ENC_NCLASS];
            int ki;
            zgec_cluster_greedy(hist4 + (size_t)mi * (size_t)64 * 256u,
                                &b8, &b4, &b2, &b1, assign);
            for (ki = 0; ki < 4; ki++) {
                double bd = (ki == 0) ? b1 : ((ki == 1) ? b2
                                                       : ((ki == 2) ? b4 : b8));
                double hdr = (ks[(size_t)ki] > 1)
                                 ? (24.0 * 8.0 +
                                    (double)(unsigned)ks[(size_t)ki] *
                                        (double)ZGEC_ENC_RANS_TABLE_BYTES * 8.0)
                                 : ((double)ZGEC_ENC_RANS_TABLE_BYTES * 8.0);
                double tot = bd + hdr;
                if (first || tot < best_total) {
                    first = 0;
                    best_total = tot;
                    best_mode = modes[(size_t)mi];
                    best_k = ks[(size_t)ki];
                    if (ks[(size_t)ki] == 1) {
                        memset(best_map, 0, sizeof(best_map));
                    } else {
                        /* The snapshot taken at exactly this k: every
                         * entry is < k, so it is a valid class_map. */
                        memcpy(best_map, assign[(size_t)ki], ZGEC_ENC_NCLASS);
                    }
                }
            }
        }
    }
    zgec_free(runstart);
    zgec_free(hist4);
    if (best_k == 1) {
        *mode_out = 0;
        *k_out = 1;
        memset(cmap_out, 0, ZGEC_ENC_NCLASS);
    } else {
        *mode_out = (uint8_t)best_mode;
        *k_out = (uint8_t)best_k;
        memcpy(cmap_out, best_map, ZGEC_ENC_NCLASS);
    }
    return ZGEC_OK;
}

/* ---- per-segment coder selection (11.7) ---- */

typedef struct {
    int lit_form;   /* 0 plain, 1 sub-literal */
    int lit_coder;  /* 0 raw, 1 rANS */
    int ctx_k;      /* 1 or block k, meaningful when lit_coder=1 */
    int seq_cond_of;
    int seq_cond_ll;
    double score;   /* bits + lambda * cycles */
} zgec_coder_choice;

/* Estimate one literal-coding candidate for a byte buffer Z. */
static double zgec_score_lit_cand(const uint32_t *hist, size_t n_syms,
                                  int n_tables, double lambda,
                                  double cyc_per_sym)
{
    double body;
    double hdr;
    if (n_tables <= 1) {
        body = zgec_enc_entropy_bits_u32(hist, ZGEC_NSYM_LIT);
        hdr = (double)zgec_enc_lit_desc_size(hist) * 8.0 + 32.0 * 8.0;
    } else {
        body = zgec_enc_entropy_bits_u32(hist, ZGEC_NSYM_LIT);
        hdr = (double)(unsigned)n_tables *
                  (double)ZGEC_ENC_RANS_TABLE_BYTES * 8.0 +
              32.0 * 8.0;
    }
    return body + hdr + lambda * cyc_per_sym * (double)(uint64_t)n_syms;
}

/* Coded bits of a sequence-code histogram under the normalised counts
 * the encoder will actually serialise (AL = 10), plus the serialised
 * descriptor size, from a SINGLE normalisation (P7): the old pair of
 * zgec_hist_coded_bits + zgec_enc_seq_desc_size normalised and walked
 * the same histogram twice. The result is identical (one deterministic
 * normalise either way); -log2(c/1024) uses the shared fast log2 since
 * this is estimate-only. Quantisation loss of a small class table is
 * charged, not just the ideal entropy. */
static double zgec_seq_coded_bits_desc(const uint32_t *hist,
                                       size_t *desc_bytes_out)
{
    int16_t counts[ZGEC_NSYM_SEQ];
    uint64_t total = 0;
    double bits = 0.0;
    uint8_t tmp[1024];
    size_t nw = 0;
    int s;
    for (s = 0; s < ZGEC_NSYM_SEQ; s++) total += (uint64_t)hist[s];
    if (total == 0) {
        if (desc_bytes_out) *desc_bytes_out = 0;
        return 0.0;
    }
    (void)zgec_normalize_counts(counts, hist, ZGEC_NSYM_SEQ, 10);
    for (s = 0; s < ZGEC_NSYM_SEQ; s++) {
        int c;
        if (hist[s] == 0) continue;
        c = (counts[s] < 0) ? 1 : (int)counts[s];
        bits += (double)(uint64_t)hist[s] *
                (10.0 - zgec_fast_log2_u64((uint64_t)(uint32_t)c));
    }
    nw = zgec_fse_write_counts(tmp, sizeof(tmp), counts, ZGEC_NSYM_SEQ, 10);
    if (desc_bytes_out) *desc_bytes_out = nw;
    return bits;
}

/* Net bit change of conditioning one sequence stream on the match-length
 * class (section 8.6): the plain stream's coded body plus its one table
 * description against the three class streams' coded bodies plus their
 * three descriptions. Positive means conditioning is worth emitting. */
static double zgec_cond_net_gain(const uint32_t *cls_hist /*[3][66]*/,
                                 const uint32_t *plain_hist)
{
    size_t plain_desc = 0;
    double plain = zgec_seq_coded_bits_desc(plain_hist, &plain_desc) +
                   (double)plain_desc * 8.0;
    double cond = 0.0;
    int c;
    for (c = 0; c < 3; c++) {
        const uint32_t *h = cls_hist + (size_t)c * ZGEC_NSYM_SEQ;
        size_t dd = 0;
        cond += zgec_seq_coded_bits_desc(h, &dd) + (double)dd * 8.0;
    }
    return plain - cond;
}

/* Partitioned rANS body estimate for a byte buffer under an explicit
 * (mode, class map, k). Mirrors emission (zgec_rans_histograms): lane
 * starts and run starts use the run-start table, every other byte the
 * context table its class maps to (D5). cls_hist is 64x256 scratch. */
static double zgec_ctx_body_bits(const uint8_t *z, size_t n_z,
                                 const uint8_t *runstart,
                                 int ctx_mode, const uint8_t *cmap, int k,
                                 uint32_t *cls_hist)
{
    double body = 0.0;
    uint32_t rs_hist[ZGEC_NSYM_LIT];
    size_t start[ZGEC_NLANES];
    size_t j;
    int g;
    memset(cls_hist, 0, (size_t)64 * 256u * sizeof(uint32_t));
    memset(rs_hist, 0, sizeof(rs_hist));
    zgec_lit_lane_starts(start, n_z);
    {
        /* Lane starts are at most 8 positions: mark them once instead of
         * scanning all 8 per byte. */
        uint8_t lane_mark = 0;
        size_t lane_pos[ZGEC_NLANES];
        size_t n_lane = 0;
        unsigned lane;
        for (lane = 0; lane < (unsigned)ZGEC_NLANES; lane++) {
            if (start[lane] < n_z) lane_pos[n_lane++] = start[lane];
        }
        for (j = 0; j < n_z; j++) {
            unsigned cls;
            int is_lane_start = 0;
            size_t li;
            if (runstart && runstart[j]) {
                rs_hist[z[j]]++;
                continue;
            }
            for (li = 0; li < n_lane; li++) {
                if (lane_pos[li] == j) { is_lane_start = 1; break; }
            }
            (void)lane_mark;
        if (is_lane_start) {
            rs_hist[z[j]]++;
            continue;
        }
        if (j == 0) {
            rs_hist[z[j]]++;
            continue;
        }
        cls = zgec_classify(ctx_mode, z[j - 1]);
        if (cls >= 64u) cls = 0u;
        {
            unsigned grp = (unsigned)cmap[cls];
            if (grp >= (unsigned)k) grp = 0u;
            cls_hist[(size_t)grp * 256u + (uint32_t)z[j]]++;
        }
    }
    }
    for (g = 0; g < k; g++) {
        body += zgec_enc_entropy_bits_u32(cls_hist + (size_t)g * 256u,
                                          ZGEC_NSYM_LIT);
    }
    body += zgec_enc_entropy_bits_u32(rs_hist, ZGEC_NSYM_LIT);
    return body;
}

/* Compare raw / rANS-1 / rANS-ctx each with and without sub-literals,
 * scoring bits + lambda * decode_cycles (sub-literal rep0<32 is the
 * slow outlier). Respects use_sublit/use_conditioning and the lambda
 * dial; each tier maps to a lambda at the call site. Sequence
 * conditioning is measured, not assumed: each of the OF and LL streams
 * is conditioned only when its own entropy drop covers the two extra
 * table descriptions (section 8.6).
 *
 * D1: conditioning is measured even when n_lit_slice == 0 (it only
 * touches h_ll/h_of); only the literal candidates need literals.
 * D5: the context candidate partitions through the block's real
 * (mode, class map), exactly as emission folds it.
 * D6: residual (sub-literal) candidates use the block's sub
 * (mode, map, k) the same way when sub contexts are trained.
 * allow_ll_cond == 0 (use_litref predecessor duty, 6.3) suppresses
 * only LL conditioning; OF conditioning keeps the block exportable.
 * resid_in (P2) is a prebuilt residual the caller shares with
 * emission; scratch_cls/scratch_cond (M2) are block-level scratch
 * the caller may provide to avoid per-segment 64 KiB churn. */
static zgec_err zgec_select_coder(const zgec_params *params,
                                  const uint8_t *seg_lit, size_t n_lit_slice,
                                  const uint32_t *ll_arr,
                                  const uint32_t *ml_arr,
                                  const uint32_t *of_arr, size_t n,
                                  const uint32_t *rep0_before,
                                  uint32_t rep0_tail,
                                  const uint8_t *vb, size_t vb_seg_start,
                                  const uint8_t *block_cmap, int block_k,
                                  int block_ctx_mode,
                                  const uint8_t *sub_cmap, int sub_k,
                                  int sub_mode,
                                  const uint32_t *h_ll, const uint32_t *h_ml,
                                  const uint32_t *h_of, uint64_t extra_bits,
                                  const uint8_t *resid_in,
                                  uint32_t *scratch_cls,
                                  uint32_t *scratch_cond,
                                  int allow_ll_cond,
                                  zgec_coder_choice *out)
{
    zgec_coder_choice best;
    uint32_t *hist = NULL;
    uint32_t *cls_hist = NULL;
    int cls_owned = 0;
    uint8_t *runstart = NULL;
    uint8_t *resid = NULL;
    int resid_owned = 0;
    double seq_bits;
    int slow_sublit = 0;
    size_t i;

    if (!params || !out) return ZGEC_ERR_INVAL;
    (void)h_ml;
    best.lit_form = 0;
    best.lit_coder = 0;
    best.ctx_k = 1;
    best.seq_cond_of = 0;
    best.seq_cond_ll = 0;
    best.score = (double)(uint64_t)n_lit_slice * 8.0 +
                 params->lambda * ZGEC_ENC_CYC_RAW_PER_SYM *
                     (double)(uint64_t)n_lit_slice;

    seq_bits = zgec_enc_entropy_bits_u32(h_ll, ZGEC_NSYM_SEQ) +
               zgec_enc_entropy_bits_u32(h_ml, ZGEC_NSYM_SEQ) +
               zgec_enc_entropy_bits_u32(h_of, ZGEC_NSYM_SEQ) +
               (double)extra_bits;
    (void)seq_bits;

    if (n_lit_slice > 0) {
        hist = (uint32_t *)zgec_alloc(256 * sizeof(*hist), 64);
        if (!hist) return ZGEC_ERR_NOMEM;
        memset(hist, 0, 256 * sizeof(*hist));
        for (i = 0; i < n_lit_slice; i++) hist[seg_lit[i]]++;

        /* Candidate: rANS order-0 on plain literals. The number of
         * literal tables is fixed per block by the block parameters
         * (1 for k == 1, k + 1 otherwise), so order-0 and context
         * coding cannot both be offered: the block picks one and
         * every rANS segment follows it. */
        if (block_k <= 1) {
            double s = zgec_score_lit_cand(hist, n_lit_slice, 1,
                                           params->lambda,
                                           ZGEC_ENC_CYC_RANS_PER_SYM);
            if (s < best.score) {
                best.score = s;
                best.lit_form = 0;
                best.lit_coder = 1;
                best.ctx_k = 1;
            }
        }

        /* Candidate: rANS with block contexts on plain literals,
         * partitioned through the block's real (mode, class map). */
        if (params->use_contexts && block_k > 1 && block_cmap) {
            runstart = (uint8_t *)zgec_alloc(n_lit_slice ? n_lit_slice : 1,
                                             1);
            cls_hist = scratch_cls;
            if (!runstart) {
                zgec_free(hist);
                return ZGEC_ERR_NOMEM;
            }
            if (!cls_hist) {
                cls_hist = (uint32_t *)zgec_alloc(
                    (size_t)64 * 256u * sizeof(uint32_t), 64);
                if (!cls_hist) {
                    zgec_free(hist);
                    zgec_free(runstart);
                    return ZGEC_ERR_NOMEM;
                }
                cls_owned = 1;
            }
            {
                /* Rebuild run starts for this slice from its LL array. */
                size_t pos = 0;
                size_t t;
                memset(runstart, 0, n_lit_slice);
                for (t = 0; t < n; t++) {
                    if (ll_arr[t] > 0 && pos < n_lit_slice) runstart[pos] = 1;
                    pos += (size_t)ll_arr[t];
                }
                if (pos < n_lit_slice) runstart[pos] = 1;
            }
            {
                double body = zgec_ctx_body_bits(seg_lit, n_lit_slice,
                                                 runstart, block_ctx_mode,
                                                 block_cmap, block_k,
                                                 cls_hist);
                double hdr = (double)(unsigned)(block_k + 1) *
                                 (double)ZGEC_ENC_RANS_TABLE_BYTES * 8.0 +
                             32.0 * 8.0;
                double s = body + hdr +
                           params->lambda * ZGEC_ENC_CYC_RANS_PER_SYM *
                               (double)(uint64_t)n_lit_slice;
                if (s < best.score) {
                    best.score = s;
                    best.lit_form = 0;
                    best.lit_coder = 1;
                    best.ctx_k = block_k;
                }
            }
        }
    }

    /* Candidates with sub-literals (residuals against rep0). The
     * residual buffer is shared with emission when the caller passes
     * it (P2); otherwise it is built (and freed) here. */
    if (n_lit_slice > 0 && params->use_sublit && n > 0 && vb) {
        size_t t;
        if (resid_in) {
            resid = (uint8_t *)resid_in;
        } else {
            resid = (uint8_t *)zgec_alloc(n_lit_slice ? n_lit_slice : 1,
                                          64);
            if (!resid) {
                zgec_free(hist);
                zgec_free(runstart);
                if (cls_owned) zgec_free(cls_hist);
                return ZGEC_ERR_NOMEM;
            }
            resid_owned = 1;
            /* Residuals are built exactly as emission and decoding do,
             * so the estimate matches the bytes that would actually
             * be coded. */
            zgec_enc_build_resid(resid, seg_lit, n_lit_slice, ll_arr, ml_arr,
                                 n, rep0_before, rep0_tail, vb,
                                 vb_seg_start);
        }
        for (t = 0; t < n; t++) {
            if (rep0_before[t] < 32u) slow_sublit = 1;
        }
        {
            uint32_t rhist[ZGEC_NSYM_LIT];
            size_t r;
            double cyc =
                slow_sublit ? ZGEC_ENC_CYC_SUBLIT_SLOW_PER_SYM
                            : (ZGEC_ENC_CYC_RANS_PER_SYM +
                               ZGEC_ENC_CYC_SUBLIT_FAST_PER_SYM);
            memset(rhist, 0, sizeof(rhist));
            for (r = 0; r < n_lit_slice; r++) rhist[resid[r]]++;
            /* NOTE (D9): raw residuals are never scored: bytewise they
             * are exactly n_lit_slice bytes like raw plain, with no
             * better model, so raw plain always ties-or-beats them. */
            /* rANS on residuals, order-0 or with the sub contexts. */
            if (params->use_contexts && sub_k > 1 && sub_cmap) {
                double body;
                double hdr;
                double s;
                if (!runstart) {
                    runstart = (uint8_t *)zgec_alloc(n_lit_slice, 1);
                    if (!runstart) {
                        zgec_free(hist);
                        if (cls_owned) zgec_free(cls_hist);
                        if (resid_owned) zgec_free(resid);
                        return ZGEC_ERR_NOMEM;
                    }
                }
                {
                    size_t pos = 0;
                    size_t t2;
                    memset(runstart, 0, n_lit_slice);
                    for (t2 = 0; t2 < n; t2++) {
                        if (ll_arr[t2] > 0 && pos < n_lit_slice)
                            runstart[pos] = 1;
                        pos += (size_t)ll_arr[t2];
                    }
                    if (pos < n_lit_slice) runstart[pos] = 1;
                }
                if (!cls_hist) {
                    cls_hist = (uint32_t *)zgec_alloc(
                        (size_t)64 * 256u * sizeof(uint32_t), 64);
                    if (!cls_hist) {
                        zgec_free(hist);
                        zgec_free(runstart);
                        if (resid_owned) zgec_free(resid);
                        return ZGEC_ERR_NOMEM;
                    }
                    cls_owned = 1;
                }
                body = zgec_ctx_body_bits(resid, n_lit_slice, runstart,
                                          sub_mode, sub_cmap, sub_k,
                                          cls_hist);
                hdr = (double)(unsigned)(sub_k + 1) *
                          (double)ZGEC_ENC_RANS_TABLE_BYTES * 8.0 +
                      32.0 * 8.0;
                s = body + hdr + params->lambda * cyc *
                                          (double)(uint64_t)n_lit_slice;
                if (s < best.score) {
                    best.score = s;
                    best.lit_form = 1;
                    best.lit_coder = 1;
                    best.ctx_k = sub_k;
                }
            }
            {
                double s = zgec_score_lit_cand(rhist, n_lit_slice, 1,
                                              params->lambda, cyc);
                if (s < best.score) {
                    best.score = s;
                    best.lit_form = 1;
                    best.lit_coder = 1;
                    best.ctx_k = 1;
                }
            }
        }
    }

    /* Sequence conditioning (8.6): the OF stream is coded with three
     * tables selected by the match-length class of the current sequence
     * and the LL stream by the class of the previous one (ML is decoded
     * first, so both classes are known). Each stream is conditioned only
     * when the measured entropy drop covers the two extra table
     * descriptions that carry. D1: this runs even for literal-free
     * segments. D6: under use_litref only OF conditioning is allowed
     * (OF-only keeps the block exportable per zgec_payload_exportable;
     * LL conditioning would break the 6.3 chain). */
    if (params->use_conditioning && n > 0 && ll_arr && ml_arr && of_arr &&
        h_ll && h_of) {
#if ZGEC_ENC_EMIT_SEQ_COND
        uint32_t *cond_hist = scratch_cond;
        int cond_owned = 0;
        if (!cond_hist) {
            cond_hist = (uint32_t *)zgec_alloc(
                3u * ZGEC_NSYM_SEQ * sizeof(uint32_t), 64);
            if (cond_hist) cond_owned = 1;
        }
        if (cond_hist) {
            size_t k;
            /* OF(i) on mlclass(ML(i)). */
            memset(cond_hist, 0, 3u * ZGEC_NSYM_SEQ * sizeof(uint32_t));
            for (k = 0; k < n; k++) {
                uint8_t nb = 0;
                uint8_t cd = zgec_seq_code_of(of_arr[k], &nb);
                unsigned cl = zgec_mlclass(ml_arr[k] + 3u);
                cond_hist[(size_t)cl * ZGEC_NSYM_SEQ + (size_t)cd]++;
            }
            if (zgec_cond_net_gain(cond_hist, h_of) > 64.0) {
                best.seq_cond_of = 1;
            }
            /* LL(i) on mlclass(ML(i-1)), class 0 for i == 0. */
            if (allow_ll_cond) {
                memset(cond_hist, 0,
                       3u * ZGEC_NSYM_SEQ * sizeof(uint32_t));
                for (k = 0; k < n; k++) {
                    uint8_t nb = 0;
                    uint8_t cd = zgec_seq_code_of(ll_arr[k], &nb);
                    unsigned cl = (k == 0) ? 0u
                                           : zgec_mlclass(ml_arr[k - 1] + 3u);
                    cond_hist[(size_t)cl * ZGEC_NSYM_SEQ + (size_t)cd]++;
                }
                if (zgec_cond_net_gain(cond_hist, h_ll) > 64.0) {
                    best.seq_cond_ll = 1;
                }
            }
            if (cond_owned) zgec_free(cond_hist);
        }
#else
        (void)ll_arr;
        (void)of_arr;
#endif
    }

    zgec_free(hist);
    zgec_free(runstart);
    if (cls_owned) zgec_free(cls_hist);
    if (resid_owned) zgec_free(resid);

    /* Final emission gates (all enabled; kept so a build can turn the
     * extended coders back off without touching the selection logic). */
#if !ZGEC_ENC_EMIT_RANS
    best.lit_coder = 0;
    best.ctx_k = 1;
#endif
#if !ZGEC_ENC_EMIT_SUBLIT
    best.lit_form = 0;
#endif
#if !ZGEC_ENC_EMIT_SEQ_COND
    best.seq_cond_of = 0;
    best.seq_cond_ll = 0;
#endif
    *out = best;
    return ZGEC_OK;
}

/* ---- literal stream emission (section 9) ---- */

/* Encode the literal stream of one segment slice.
 *
 * lit_src/lit_n:  the n_lit plain literal bytes of the slice.
 * lit_form:  0 plain, 1 sub-literal residuals (section 9.6).
 * lit_coder: 0 raw, 1 rANS.
 * k/ctx_mode/cmap: literal context description (k == 1 disables contexts).
 * ll/ml/n_seq:   sequence lengths (NULL/0 when the segment has none).
 * rep0_before[i]/rep0_tail: explicit repeat offsets used as sub-literal
 *                predictors (section 9.6).
 * vb/vb_base/seg_out_start: virtual buffer for sub-literal prediction.
 *
 * On success desc_out/desc_size_out hold the literal table descriptors
 * (freshly allocated; NULL/0 when raw) and stream_out/stream_size_out the
 * coded stream bytes (raw literals, or the rANS stream). */
static zgec_err zgec_emit_lit_stream(uint8_t **desc, size_t *desc_size,
                                     uint8_t **stream, size_t *stream_size,
                                     const uint8_t *lit_src, size_t lit_n,
                                     int lit_form, int lit_coder,
                                     int k, int ctx_mode, const uint8_t *cmap,
                                     const uint32_t *ll, const uint32_t *ml,
                                     size_t n_seq,
                                     const uint32_t *rep0_before,
                                     uint32_t rep0_tail,
                                     const uint8_t *vb, size_t vb_base,
                                     size_t seg_out_start,
                                     const uint8_t *resid_in)
{
    uint8_t *z = NULL;
    const uint8_t *zin = lit_src;
    uint8_t *runstart = NULL;
    uint8_t *desc_buf = NULL;
    size_t desc_len = 0;
    size_t desc_cap = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    uint32_t *hist = NULL;
    zgec_rans_enc_table *enc_tabs = NULL;
    int n_tables;
    zgec_err err = ZGEC_OK;
    int t;

    if (!desc || !desc_size || !stream || !stream_size) return ZGEC_ERR_INVAL;
    *desc = NULL;
    *desc_size = 0;
    *stream = NULL;
    *stream_size = 0;

    if (lit_n == 0) {
        *stream = (uint8_t *)zgec_alloc(1, 1);
        if (!*stream) return ZGEC_ERR_NOMEM;
        *stream_size = 0;
        return ZGEC_OK;
    }

    if (lit_coder == 0) {
        out = (uint8_t *)zgec_alloc(lit_n, 64);
        if (!out) return ZGEC_ERR_NOMEM;
        memcpy(out, lit_src, lit_n);
        *stream = out;
        *stream_size = lit_n;
        return ZGEC_OK;
    }

    n_tables = (k <= 1) ? 1 : (k + 1);

    /* Coder input Z: plain literals, or sub-literal residuals built
     * exactly as the decoder reconstructs them (section 9.6). P2: the
     * caller may pass the residual buffer it already built for
     * scoring, so it is not built twice per segment. */
    if (lit_form == 1) {
        if (resid_in) {
            zin = resid_in;
        } else {
            if (vb == NULL) return ZGEC_ERR_INVAL;
            z = (uint8_t *)zgec_alloc(lit_n, 64);
            if (!z) return ZGEC_ERR_NOMEM;
            zgec_enc_build_resid(z, lit_src, lit_n, ll, ml, n_seq,
                                 rep0_before, rep0_tail, vb,
                                 vb_base + seg_out_start);
            zin = z;
        }
    }

    if (k > 1) {
        runstart = (uint8_t *)zgec_alloc(lit_n, 1);
        if (!runstart) { err = ZGEC_ERR_NOMEM; goto done; }
        zgec_enc_runstart(runstart, lit_n, ll, n_seq);
    }

    desc_cap = (size_t)n_tables * 4096u + 64u;
    hist = (uint32_t *)zgec_alloc((size_t)n_tables * ZGEC_NSYM_LIT *
                                      sizeof(uint32_t), 64);
    enc_tabs = (zgec_rans_enc_table *)zgec_alloc(
        (size_t)n_tables * sizeof(*enc_tabs), _Alignof(zgec_rans_enc_table));
    desc_buf = (uint8_t *)zgec_alloc(desc_cap, 64);
    if (!hist || !enc_tabs || !desc_buf) { err = ZGEC_ERR_NOMEM; goto done; }

    zgec_rans_histograms(hist, n_tables, zin, lit_n, ctx_mode, cmap, runstart);

    for (t = 0; t < n_tables; t++) {
        int16_t counts[ZGEC_NSYM_LIT];
        uint64_t hsum = 0;
        size_t s;
        size_t nw;
        for (s = 0; s < ZGEC_NSYM_LIT; s++)
            hsum += (uint64_t)hist[(size_t)t * ZGEC_NSYM_LIT + s];
        if (hsum == 0u) {
            /* No literal maps to this context: the decoder never indexes
             * the table, so any valid distribution is fine. */
            counts[0] = (int16_t)ZGEC_RANS_M;
            for (s = 1; s < ZGEC_NSYM_LIT; s++) counts[s] = 0;
        } else {
            err = zgec_rans_normalise(counts, hist + (size_t)t * ZGEC_NSYM_LIT);
            if (err != ZGEC_OK) goto done;
        }
        err = zgec_rans_build_enc(&enc_tabs[t], counts);
        if (err != ZGEC_OK) goto done;
        nw = zgec_fse_write_counts(desc_buf + desc_len, desc_cap - desc_len,
                                   counts, ZGEC_NSYM_LIT, ZGEC_LIT_AL);
        if (nw == 0) { err = ZGEC_ERR_INTERNAL; goto done; }
        desc_len += nw;
    }

    {
        size_t cap = 2u * lit_n + 256u;
        out = (uint8_t *)zgec_alloc(cap, 64);
        if (!out) { err = ZGEC_ERR_NOMEM; goto done; }
        /* S5: out_len == 0 is unambiguous failure here (the empty
         * stream only exists for n_lit == 0, returned above), and the
         * cap always exceeds the 32-byte minimum for lit_n > 0.
         * M3 TODO: zgec_rans_encode (rans.c:194) returns 0 on OOM too,
         * so a deep OOM surfaces here as INTERNAL rather than NOMEM;
         * rans.c would need a distinct error code to propagate it. */
        out_len = zgec_rans_encode(zin, lit_n, out, cap, enc_tabs, k,
                                   ctx_mode, cmap, runstart);
        if (out_len == 0) { err = ZGEC_ERR_INTERNAL; goto done; }
    }

    *desc = desc_buf;
    *desc_size = desc_len;
    *stream = out;
    *stream_size = out_len;
    desc_buf = NULL;
    out = NULL;

done:
    zgec_free(enc_tabs);
    zgec_free(hist);
    zgec_free(desc_buf);
    zgec_free(out);
    zgec_free(z);
    zgec_free(runstart);
    return err;
}

/* ---- one segment payload ---- */

/* Per-segment preparation built ONCE per segment and shared by
 * selection and emission (P1/P2): coded sequence values, exact
 * histograms, repeat offsets, real match lengths and the optional
 * sub-literal residual. The old code built all of these in the block
 * loop for scoring, freed them, then rebuilt them identically inside
 * zgec_emit_segment. */
typedef struct {
    uint32_t *ll;      /* raw LL (owned arena, NULL when n == 0) */
    uint32_t *ml;      /* coded ML-3 (carved from arena) */
    uint32_t *of;      /* coded offbase-1 (carved from arena) */
    uint32_t *rep0;    /* rep0 before each sequence (carved, NULL ok) */
    uint32_t rep0_tail;
    uint32_t h_ll[ZGEC_NSYM_SEQ];
    uint32_t h_ml[ZGEC_NSYM_SEQ];
    uint32_t h_of[ZGEC_NSYM_SEQ];
    uint64_t xbits;
    uint64_t ml_sum;   /* sum of real match lengths (for raw_len) */
    uint32_t *mls;     /* real ML >= 3 (carved from arena) */
    void *arena;       /* single allocation backing ll/ml/of/rep0/mls */
    uint8_t *resid;    /* sub-literal residual (owned, NULL unless built) */
} zgec_seg_prep;

static void zgec_seg_prep_free(zgec_seg_prep *p)
{
    if (!p) return;
    zgec_free(p->arena);
    zgec_free(p->resid);
    memset(p, 0, sizeof(*p));
}

/* Build the prep for segment [s0,s1). want_resid requests the
 * sub-literal residual (needs vb); all arrays stay NULL/0 when n == 0.
 * vb_seg_start is the virtual-buffer position of the slice's first
 * output byte. */
static zgec_err zgec_seg_prep_build(zgec_seg_prep *p,
                                    const zgec_parse *parse,
                                    size_t s0, size_t s1,
                                    int want_resid,
                                    const uint8_t *seg_lit,
                                    size_t n_lit_slice,
                                    const uint8_t *vb,
                                    size_t vb_seg_start)
{
    size_t n = (s1 > s0) ? (s1 - s0) : 0;
    size_t t;
    uint32_t *blk = NULL;
    if (!p || !parse) return ZGEC_ERR_INVAL;
    memset(p, 0, sizeof(*p));
    if (n == 0) return ZGEC_OK;
    if (n > SIZE_MAX / (5 * sizeof(uint32_t))) return ZGEC_ERR_NOMEM;
    blk = (uint32_t *)zgec_alloc(n * 5 * sizeof(uint32_t),
                                _Alignof(uint32_t));
    if (!blk) return ZGEC_ERR_NOMEM;
    p->arena = blk;
    p->ll = blk + 0 * n;
    p->ml = blk + 1 * n;
    p->of = blk + 2 * n;
    p->rep0 = blk + 3 * n;
    p->mls = blk + 4 * n;
    for (t = 0; t < n; t++) {
        const zgec_sequence *q = &parse->seq[s0 + t];
        uint8_t nb = 0;
        uint32_t mlv = (q->ml >= 3) ? (q->ml - 3u) : 0u;
        uint32_t ofv = (q->offbase >= 1) ? (q->offbase - 1u) : 0u;
        p->ll[t] = q->ll;
        p->ml[t] = mlv;
        p->of[t] = ofv;
        p->mls[t] = mlv + 3u;
        p->ml_sum += (uint64_t)q->ml;
        p->h_ll[zgec_seq_code_of(q->ll, &nb)]++;
        p->xbits += (uint64_t)nb;
        p->h_ml[zgec_seq_code_of(mlv, &nb)]++;
        p->xbits += (uint64_t)nb;
        p->h_of[zgec_seq_code_of(ofv, &nb)]++;
        p->xbits += (uint64_t)nb;
    }
    zgec_enc_rep0_before(p->rep0, &parse->seq[s0], n);
    p->rep0_tail = zgec_enc_rep0_after(&parse->seq[s0], n);
    if (want_resid && n_lit_slice > 0 && seg_lit && vb) {
        p->resid = (uint8_t *)zgec_alloc(n_lit_slice, 64);
        if (!p->resid) {
            zgec_seg_prep_free(p);
            return ZGEC_ERR_NOMEM;
        }
        zgec_enc_build_resid(p->resid, seg_lit, n_lit_slice, p->ll, p->ml,
                             n, p->rep0, p->rep0_tail, vb, vb_seg_start);
    }
    return ZGEC_OK;
}

/* D7: pick the RLE-vs-NEW table mode by measurement instead of
 * unconditionally taking RLE for single-valued streams. The RLE
 * descriptor is 1 byte; the NEW descriptor is zgec_enc_seq_desc_size
 * bytes and both streams carry the same extra bits, so RLE wins
 * unless a NEW description is pathologically 1 byte. */
static unsigned zgec_pick_tbl_mode(const uint32_t *hist, int rle_symbol)
{
    if (rle_symbol < 0) return ZGEC_TBL_NEW;
    /* Fast distinct-symbol check before paying for normalise+serialise. */
    {
        int s;
        for (s = 0; s < ZGEC_NSYM_SEQ; s++) {
            if (hist[s] != 0 && s != rle_symbol) break;
        }
        if (s == ZGEC_NSYM_SEQ) return ZGEC_TBL_RLE;
    }
    if (zgec_enc_seq_desc_size(hist) <= 1) return ZGEC_TBL_NEW;
    return ZGEC_TBL_RLE;
}

/* Emit one segment DIRECTLY into the block payload (P6): the caller has
 * already reserved the directory at payload[0..dir]; this appends the
 * segment at *off (growing), fills *dir and *raw_len_out. prep carries
 * the arrays/histograms selection already built (P1), so they are not
 * re-extracted here. */
static zgec_err zgec_emit_segment(uint8_t **payload, size_t *cap, size_t *off,
                                  zgec_seg_dir_entry *dir,
                                  uint32_t *raw_len_out,
                                  const zgec_parse *parse, size_t s0, size_t s1,
                                  size_t l0, size_t n_lit_slice,
                                  const zgec_block_params *bp,
                                  const zgec_coder_choice *choice,
                                  const zgec_seg_prep *prep,
                                  const uint8_t *vb, size_t vb_base,
                                  size_t seg_out_start)
{
    size_t n = (s1 > s0) ? (s1 - s0) : 0;
    const uint32_t *ll = NULL;
    const uint32_t *ml = NULL;
    const uint32_t *of = NULL;
    const uint32_t *hist_ll = NULL;
    const uint32_t *hist_ml = NULL;
    const uint32_t *hist_of = NULL;
    size_t i;
    zgec_err err = ZGEC_OK;
    int eff_coder;
    int eff_form;

    if (!payload || !cap || !off || !dir || !bp || !choice || !prep)
        return ZGEC_ERR_INVAL;
    /* S2: explicit guard; the old clamp below underflows size_t when
     * l0 > parse->n_lit. */
    if (!parse) return ZGEC_ERR_INVAL;
    if (l0 > parse->n_lit) return ZGEC_ERR_INTERNAL;

    /* Effective literal coding for this segment. Sub-literals are only
     * ever chosen with sequences present, and a segment with no literals
     * always codes raw (there is nothing to code, and no tables are
     * emitted for it). */
    eff_coder = choice->lit_coder;
    eff_form = choice->lit_form;
    if (n_lit_slice == 0) {
        eff_coder = 0;
        eff_form = 0;
    }

    if (n_lit_slice > parse->n_lit - l0) n_lit_slice = parse->n_lit - l0;

    if (n > 0) {
        ll = prep->ll;
        ml = prep->ml;
        of = prep->of;
        hist_ll = prep->h_ll;
        hist_ml = prep->h_ml;
        hist_of = prep->h_of;
        if (!ll || !ml || !of) return ZGEC_ERR_INTERNAL;
        (void)i;
    }

    if (n > 0) {
        int rle_ll = zgec_seq_rle_symbol(ll, n);
        int rle_ml = zgec_seq_rle_symbol(ml, n);
        int rle_of = zgec_seq_rle_symbol(of, n);
        unsigned mode_ll = zgec_pick_tbl_mode(hist_ll, rle_ll);
        unsigned mode_ml = zgec_pick_tbl_mode(hist_ml, rle_ml);
        unsigned mode_of = zgec_pick_tbl_mode(hist_of, rle_of);
        zgec_fse_enc_table *enc_ll = NULL;
        zgec_fse_enc_table *enc_ml = NULL;
        zgec_fse_enc_table *enc_of = NULL;
        zgec_fse_dec_table *dec_ll = NULL;
        zgec_fse_dec_table *dec_ml = NULL;
        zgec_fse_dec_table *dec_of = NULL;
        uint8_t desc[4096];
        size_t desc_size = 0;
        uint8_t *ll_tmp = NULL;
        uint8_t *ml_tmp = NULL;
        uint8_t *of_tmp = NULL;
        uint8_t *tb_blk = NULL;
        size_t tb_cap = 0;
        size_t ll_size = 0;
        size_t ml_size = 0;
        size_t of_size = 0;
        uint8_t *lit_desc = NULL;
        size_t lit_desc_size = 0;
        uint8_t *lit_stream = NULL;
        size_t lit_size = 0;
        uint8_t *all_desc = NULL;
        size_t all_desc_size = 0;
        uint8_t *hdr_buf = NULL;
        size_t hdr_cap = 0;
        const uint32_t *rep0b = prep->rep0;
        const zgec_ctx_desc *lit_cd = eff_form ? &bp->sub : &bp->plain;
        int lit_k = (eff_coder != 0) ? (int)lit_cd->ctx_count : 1;
        const uint32_t *mls = prep->mls; /* real match lengths (>= 3) */
        zgec_fse_enc_table *enc_of3[3];   /* conditioned OF tables */
        zgec_fse_dec_table *dec_of3[3];
        zgec_fse_enc_table *enc_ll3[3];   /* conditioned LL tables */
        zgec_fse_dec_table *dec_ll3[3];
        int cond_of;
        int cond_ll;
        int ci;

        /* Conditioning needs a coded (NEW) table and the selector's
         * choice; an RLE stream (one symbol) gains nothing from it. */
        cond_of = (choice->seq_cond_of && mode_of == ZGEC_TBL_NEW) ? 1 : 0;
        cond_ll = (choice->seq_cond_ll && mode_ll == ZGEC_TBL_NEW) ? 1 : 0;
        memset(enc_of3, 0, sizeof(enc_of3));
        memset(dec_of3, 0, sizeof(dec_of3));
        memset(enc_ll3, 0, sizeof(enc_ll3));
        memset(dec_ll3, 0, sizeof(dec_ll3));
        if (!mls) { err = ZGEC_ERR_INTERNAL; goto seg_fail_tables; }

        /* ---- Literal stream (section 9): raw or rANS, plain or sub.
         * rep0/mls come from the shared prep (P1); the residual is
         * shared too when selection built it (P2). ---- */
        {
            err = zgec_emit_lit_stream(&lit_desc, &lit_desc_size,
                                       &lit_stream, &lit_size,
                                       parse->lit + l0, n_lit_slice,
                                       eff_form, eff_coder, lit_k,
                                       (int)lit_cd->ctx_mode,
                                       (lit_k > 1) ? lit_cd->class_map : NULL,
                                       ll, ml, n, rep0b, prep->rep0_tail,
                                       vb, vb_base, seg_out_start,
                                       (eff_form && eff_coder)
                                           ? prep->resid
                                           : NULL);
            if (err != ZGEC_OK && eff_coder != 0) {
                /* D8: an rANS literal failure (table build or the
                 * 2*lit_n+256 cap) falls back to raw plain literals
                 * rather than aborting the whole segment. Raw carries
                 * no tables, so the header below reflects raw plain. */
                zgec_free(lit_desc);
                zgec_free(lit_stream);
                lit_desc = NULL;
                lit_stream = NULL;
                lit_desc_size = 0;
                lit_size = 0;
                eff_coder = 0;
                eff_form = 0;
                lit_cd = &bp->plain;
                lit_k = 1;
                err = zgec_emit_lit_stream(
                    &lit_desc, &lit_desc_size, &lit_stream, &lit_size,
                    parse->lit + l0, n_lit_slice, 0, 0, 1, 0, NULL, NULL,
                    NULL, 0, NULL, 0, NULL, 0, 0, NULL);
            }
        }
        if (err != ZGEC_OK) goto seg_fail_tables;

        if (mode_ll == ZGEC_TBL_RLE) {
            if (desc_size + 1 > sizeof(desc)) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc[desc_size++] = (uint8_t)rle_ll;
        } else if (cond_ll) {
            /* Three LL tables conditioned on the ML class of the
             * PREVIOUS sequence (section 8.6); class 0 for i == 0. The
             * decoder reads ML before LL, so the class is known. */
            uint32_t hc[3][ZGEC_NSYM_SEQ];
            int c2;
            memset(hc, 0, sizeof(hc));
            for (i = 0; i < n; i++) {
                uint8_t nb = 0;
                unsigned cl = (i == 0) ? 0u : zgec_mlclass(mls[i - 1]);
                hc[cl][zgec_seq_code_of(ll[i], &nb)]++;
            }
            for (c2 = 0; c2 < 3; c2++) {
                int16_t cc[ZGEC_NSYM_SEQ];
                size_t nw;
                err = zgec_seq_build_tables_counts(&dec_ll3[c2], &enc_ll3[c2],
                                                     hc[c2], 10, cc);
                if (err != ZGEC_OK) goto seg_fail_tables;
                nw = zgec_fse_write_counts(desc + desc_size,
                                           sizeof(desc) - desc_size, cc, 66, 10);
                if (nw == 0) {
                    err = ZGEC_ERR_INTERNAL;
                    goto seg_fail_tables;
                }
                desc_size += nw;
            }
        } else {
            int16_t c[ZGEC_NSYM_SEQ];
            size_t nw;
            err = zgec_seq_build_tables_counts(&dec_ll, &enc_ll, hist_ll, 10, c);
            if (err != ZGEC_OK) goto seg_fail_tables;
            nw = zgec_fse_write_counts(desc + desc_size,
                                       sizeof(desc) - desc_size, c, 66, 10);
            if (nw == 0) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc_size += nw;
        }
        if (mode_ml == ZGEC_TBL_RLE) {
            if (desc_size + 1 > sizeof(desc)) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc[desc_size++] = (uint8_t)rle_ml;
        } else {
            int16_t c[ZGEC_NSYM_SEQ];
            size_t nw;
            err = zgec_seq_build_tables(&dec_ml, &enc_ml, hist_ml, 10);
            if (err != ZGEC_OK) goto seg_fail_tables;
            (void)zgec_normalize_counts(c, hist_ml, 66, 10);
            nw = zgec_fse_write_counts(desc + desc_size,
                                       sizeof(desc) - desc_size, c, 66, 10);
            if (nw == 0) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc_size += nw;
        }
        if (mode_of == ZGEC_TBL_RLE) {
            if (desc_size + 1 > sizeof(desc)) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc[desc_size++] = (uint8_t)rle_of;
        } else if (cond_of) {
            /* Three OF tables conditioned on the ML class of the current
             * sequence (section 8.6); the decoder reads ML first. */
            uint32_t hc[3][ZGEC_NSYM_SEQ];
            int c2;
            memset(hc, 0, sizeof(hc));
            for (i = 0; i < n; i++) {
                uint8_t nb = 0;
                unsigned cl = zgec_mlclass(mls[i]);
                hc[cl][zgec_seq_code_of(of[i], &nb)]++;
            }
            for (c2 = 0; c2 < 3; c2++) {
                int16_t cc[ZGEC_NSYM_SEQ];
                size_t nw;
                err = zgec_seq_build_tables(&dec_of3[c2], &enc_of3[c2],
                                            hc[c2], 10);
                if (err != ZGEC_OK) goto seg_fail_tables;
                (void)zgec_normalize_counts(cc, hc[c2], 66, 10);
                nw = zgec_fse_write_counts(desc + desc_size,
                                           sizeof(desc) - desc_size, cc, 66, 10);
                if (nw == 0) {
                    err = ZGEC_ERR_INTERNAL;
                    goto seg_fail_tables;
                }
                desc_size += nw;
            }
        } else {
            int16_t c[ZGEC_NSYM_SEQ];
            size_t nw;
            err = zgec_seq_build_tables(&dec_of, &enc_of, hist_of, 10);
            if (err != ZGEC_OK) goto seg_fail_tables;
            (void)zgec_normalize_counts(c, hist_of, 66, 10);
            nw = zgec_fse_write_counts(desc + desc_size,
                                       sizeof(desc) - desc_size, c, 66, 10);
            if (nw == 0) {
                err = ZGEC_ERR_INTERNAL;
                goto seg_fail_tables;
            }
            desc_size += nw;
        }

        /* D8: one retry on a 4x stream buffer. n*8+64 bytes is already
         * ~64 bits/symbol (worst case is far below that), so overflow
         * is not expected; retrying beats aborting the segment. Single
         * allocation carved x3 for locality (one free). */
        if (n > (SIZE_MAX - 64) / 8) { err = ZGEC_ERR_NOMEM; goto seg_fail_tables; }
        tb_cap = n * 8 + 64;
        ll_tmp = NULL;
        ml_tmp = NULL;
        of_tmp = NULL;
        tb_blk = NULL;
        {
            int seq_attempt = 0;
        seq_retry_streams:
            zgec_free(tb_blk);
            tb_blk = NULL;
            ll_tmp = NULL;
            ml_tmp = NULL;
            of_tmp = NULL;
            if (tb_cap > SIZE_MAX / 3) { err = ZGEC_ERR_NOMEM; goto seg_fail_tables; }
            tb_blk = (uint8_t *)zgec_alloc(tb_cap * 3, 1);
            if (!tb_blk) {
                err = ZGEC_ERR_NOMEM;
                goto seg_fail_tables;
            }
            ll_tmp = tb_blk + 0 * tb_cap;
            ml_tmp = tb_blk + 1 * tb_cap;
            of_tmp = tb_blk + 2 * tb_cap;
            {
                zgec_bw bw;
                int streams_ok = 1;
                zgec_bw_init(&bw, ll_tmp, tb_cap);
                if (cond_ll) {
                    const zgec_fse_enc_table *et3[3];
                    et3[0] = enc_ll3[0];
                    et3[1] = enc_ll3[1];
                    et3[2] = enc_ll3[2];
                    ll_size = zgec_seq_stream_encode_cond(
                        ll, n, mls, 1, et3, &bw, zgec_seq_base,
                        zgec_seq_nbits);
                } else {
                    ll_size = zgec_seq_stream_encode(ll, n, enc_ll, rle_ll,
                                                     &bw, zgec_seq_base,
                                                     zgec_seq_nbits);
                }
                if (ll_size == 0 || bw.overflow) streams_ok = 0;
                if (streams_ok) {
                    zgec_bw_init(&bw, ml_tmp, tb_cap);
                    ml_size = zgec_seq_stream_encode(ml, n, enc_ml, rle_ml,
                                                     &bw, zgec_seq_base,
                                                     zgec_seq_nbits);
                    if (ml_size == 0 || bw.overflow) streams_ok = 0;
                }
                if (streams_ok) {
                    zgec_bw_init(&bw, of_tmp, tb_cap);
                    if (cond_of) {
                        const zgec_fse_enc_table *et3[3];
                        et3[0] = enc_of3[0];
                        et3[1] = enc_of3[1];
                        et3[2] = enc_of3[2];
                        of_size = zgec_seq_stream_encode_cond(
                            of, n, mls, 0, et3, &bw, zgec_seq_base,
                            zgec_seq_nbits);
                    } else {
                        of_size = zgec_seq_stream_encode(
                            of, n, enc_of, rle_of, &bw, zgec_seq_base,
                            zgec_seq_nbits);
                    }
                    if (of_size == 0 || bw.overflow) streams_ok = 0;
                }
                if (!streams_ok) {
                    if (seq_attempt == 0 && tb_cap <= ((size_t)1 << 30) / 4) {
                        seq_attempt = 1;
                        tb_cap *= 4;
                        goto seq_retry_streams;
                    }
                    zgec_free(tb_blk);
                    tb_blk = NULL;
                    ll_tmp = NULL;
                    ml_tmp = NULL;
                    of_tmp = NULL;
                    err = ZGEC_ERR_INTERNAL;
                    goto seg_fail_tables;
                }
            }
        }
        zgec_fse_free_enc(enc_ll);
        zgec_fse_free_dec(dec_ll);
        zgec_fse_free_enc(enc_ml);
        zgec_fse_free_dec(dec_ml);
        zgec_fse_free_enc(enc_of);
        zgec_fse_free_dec(dec_of);
        for (ci = 0; ci < 3; ci++) {
            zgec_fse_free_enc(enc_of3[ci]);
            zgec_fse_free_dec(dec_of3[ci]);
            enc_of3[ci] = NULL;
            dec_of3[ci] = NULL;
            zgec_fse_free_enc(enc_ll3[ci]);
            zgec_fse_free_dec(dec_ll3[ci]);
            enc_ll3[ci] = NULL;
            dec_ll3[ci] = NULL;
        }
        enc_ll = enc_ml = enc_of = NULL;
        dec_ll = dec_ml = dec_of = NULL;
        {
            zgec_seg_header sh;
            size_t hz;
            size_t seg_total;
            size_t wpos;

            /* Literal table descriptors come first, then LL/ML/OF (7.3). */
            all_desc_size = lit_desc_size + desc_size;
            all_desc = (uint8_t *)zgec_alloc(all_desc_size ? all_desc_size : 1,
                                             64);
            if (!all_desc) { err = ZGEC_ERR_NOMEM; goto seg_fail_all; }
            if (lit_desc_size > 0) memcpy(all_desc, lit_desc, lit_desc_size);
            if (desc_size > 0) memcpy(all_desc + lit_desc_size, desc, desc_size);

            memset(&sh, 0, sizeof(sh));
            sh.segment_flags = (uint8_t)(
                (unsigned)(eff_form ? ZGEC_SEG_LIT_FORM : 0u) |
                (unsigned)(eff_coder ? ZGEC_SEG_LIT_CODER_RANS
                                     : ZGEC_SEG_LIT_CODER_RAW) |
                (unsigned)(cond_of ? ZGEC_SEG_SEQ_CTX_OF : 0u) |
                (unsigned)(cond_ll ? ZGEC_SEG_SEQ_CTX_LL : 0u));
            sh.table_modes =
                (uint8_t)((mode_ll << 2) | (mode_ml << 4) | (mode_of << 6));
            sh.n_seq = (uint32_t)n;
            sh.n_lit = (uint32_t)n_lit_slice;
            sh.lit_size = (uint32_t)lit_size;
            sh.ll_size = (uint32_t)ll_size;
            sh.ml_size = (uint32_t)ml_size;
            sh.of_size = (uint32_t)of_size;

            hdr_cap = all_desc_size + 64u;
            hdr_buf = (uint8_t *)zgec_alloc(hdr_cap, 64);
            if (!hdr_buf) { err = ZGEC_ERR_NOMEM; goto seg_fail_all; }
            hz = zgec_seg_header_emit(hdr_buf, hdr_cap, &sh, all_desc,
                                      all_desc_size);
            if (hz == 0) { err = ZGEC_ERR_INTERNAL; goto seg_fail_all; }

            seg_total = hz + lit_size + ll_size + ml_size + of_size;
            /* P6: append directly into the block payload instead of a
             * per-segment buffer that the caller would memcpy again. */
            if (zgec_add_overflows(*off, seg_total)) { err = ZGEC_ERR_INTERNAL; goto seg_fail_all; }
            err = zgec_payload_grow(payload, cap, *off, *off + seg_total);
            if (err != ZGEC_OK) goto seg_fail_all;

            wpos = *off;
            memcpy(*payload + wpos, hdr_buf, hz);
            wpos += hz;
            if (lit_size > 0) {
                memcpy(*payload + wpos, lit_stream, lit_size);
                wpos += lit_size;
            }
            if (ll_size > 0) {
                memcpy(*payload + wpos, ll_tmp, ll_size);
                wpos += ll_size;
            }
            if (ml_size > 0) {
                memcpy(*payload + wpos, ml_tmp, ml_size);
                wpos += ml_size;
            }
            if (of_size > 0) {
                memcpy(*payload + wpos, of_tmp, of_size);
                wpos += of_size;
            }

            zgec_free(hdr_buf);
            zgec_free(all_desc);
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            zgec_free(tb_blk);
            dir->comp_len = (uint32_t)seg_total;
            dir->raw_len = (uint32_t)(n_lit_slice + (size_t)prep->ml_sum);
            *off = wpos;
            *raw_len_out = dir->raw_len;
            return ZGEC_OK;
        }
    seg_fail_all:
        zgec_free(hdr_buf);
        hdr_buf = NULL;
        zgec_free(all_desc);
        all_desc = NULL;
        zgec_free(lit_desc);
        lit_desc = NULL;
        zgec_free(lit_stream);
        lit_stream = NULL;
        zgec_free(tb_blk);
        tb_blk = NULL;
        ll_tmp = NULL;
        ml_tmp = NULL;
        of_tmp = NULL;
    seg_fail_tables:
        /* S1: lit_desc/lit_stream are allocated before every goto
         * above (table builds, desc serialisation, stream encodes),
         * so they must be released here; the tmp block is always
         * freed (and NULLed) at its failure sites. */
        zgec_free(lit_desc);
        zgec_free(lit_stream);
        zgec_free(tb_blk);
        for (ci = 0; ci < 3; ci++) {
            zgec_fse_free_enc(enc_of3[ci]);
            zgec_fse_free_dec(dec_of3[ci]);
            zgec_fse_free_enc(enc_ll3[ci]);
            zgec_fse_free_dec(dec_ll3[ci]);
        }
        zgec_fse_free_enc(enc_ll);
        zgec_fse_free_dec(dec_ll);
        zgec_fse_free_enc(enc_ml);
        zgec_fse_free_dec(dec_ml);
        zgec_fse_free_enc(enc_of);
        zgec_fse_free_dec(dec_of);
        return err;
    }

    /* n_seq == 0: literals-only segment (tail/incompressible region). */
    {
        zgec_seg_header sh;
        size_t hz;
        uint8_t *lit_desc = NULL;
        size_t lit_desc_size = 0;
        uint8_t *lit_stream = NULL;
        size_t lit_size = 0;
        uint8_t *hdr = NULL;
        size_t seg_total;
        size_t hdr_cap;
        const zgec_ctx_desc *lit_cd = eff_form ? &bp->sub : &bp->plain;
        int lit_k = (eff_coder != 0) ? (int)lit_cd->ctx_count : 1;

        err = zgec_emit_lit_stream(&lit_desc, &lit_desc_size,
                                   &lit_stream, &lit_size,
                                   parse->lit + l0, n_lit_slice,
                                   eff_form, eff_coder, lit_k,
                                   (int)lit_cd->ctx_mode,
                                   (lit_k > 1) ? lit_cd->class_map : NULL,
                                   NULL, NULL, 0, NULL, 1u, NULL, 0, 0,
                                   NULL);
        if (err != ZGEC_OK && eff_coder != 0) {
            /* D8: fall back to raw plain literals (see above). */
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            lit_desc = NULL;
            lit_stream = NULL;
            lit_desc_size = 0;
            lit_size = 0;
            eff_coder = 0;
            eff_form = 0;
            err = zgec_emit_lit_stream(
                &lit_desc, &lit_desc_size, &lit_stream, &lit_size,
                parse->lit + l0, n_lit_slice, 0, 0, 1, 0, NULL, NULL,
                NULL, 0, NULL, 0, NULL, 0, 0, NULL);
        }
        if (err != ZGEC_OK) {
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            return err;
        }
        memset(&sh, 0, sizeof(sh));
        sh.segment_flags = (uint8_t)(
            (unsigned)(eff_form ? ZGEC_SEG_LIT_FORM : 0u) |
            (unsigned)(eff_coder ? ZGEC_SEG_LIT_CODER_RANS
                                 : ZGEC_SEG_LIT_CODER_RAW));
        sh.table_modes = 0;
        sh.n_seq = 0;
        sh.n_lit = (uint32_t)n_lit_slice;
        sh.lit_size = (uint32_t)lit_size;
        sh.ll_size = 0;
        sh.ml_size = 0;
        sh.of_size = 0;
        hdr_cap = lit_desc_size + 64u;
        hdr = (uint8_t *)zgec_alloc(hdr_cap, 64);
        if (!hdr) {
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            return ZGEC_ERR_NOMEM;
        }
        hz = zgec_seg_header_emit(hdr, hdr_cap, &sh, lit_desc, lit_desc_size);
        if (hz == 0) {
            zgec_free(hdr);
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            return ZGEC_ERR_INTERNAL;
        }
        seg_total = hz + lit_size;
        /* P6: append directly into the block payload. */
        if (zgec_add_overflows(*off, seg_total)) {
            zgec_free(hdr);
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            return ZGEC_ERR_INTERNAL;
        }
        err = zgec_payload_grow(payload, cap, *off, *off + seg_total);
        if (err != ZGEC_OK) {
            zgec_free(hdr);
            zgec_free(lit_desc);
            zgec_free(lit_stream);
            return err;
        }
        memcpy(*payload + *off, hdr, hz);
        *off += hz;
        if (lit_size > 0) {
            memcpy(*payload + *off, lit_stream, lit_size);
            *off += lit_size;
        }
        zgec_free(hdr);
        zgec_free(lit_desc);
        zgec_free(lit_stream);
        dir->comp_len = (uint32_t)seg_total;
        dir->raw_len = (uint32_t)n_lit_slice;
        *raw_len_out = dir->raw_len;
        return ZGEC_OK;
    }
}

/* ---- block pre-filter sampled gate (section 11.11) ---- */

#define ZGEC_ENC_FILTER_GROUPS 16u
#define ZGEC_ENC_FILTER_GROUP  64u
/* Minimum sample-entropy saving, in bits per sampled byte, that a
 * candidate filter must show before the gate accepts it. It covers the
 * 2-byte descriptor and the extra inverse pass of section 7.5. */
#define ZGEC_ENC_FILTER_MARGIN 0.10

/* Gather about 1 KiB of the block in 16 groups of 64 bytes spread
 * across it (section 11.11). Returns the number of sample bytes. */
static size_t zgec_enc_gather_sample(const uint8_t *src, size_t n,
                                     uint8_t *samp, size_t cap)
{
    size_t total = 0;
    size_t g;
    if (src == NULL || samp == NULL || n == 0 || cap == 0) return 0;
    for (g = 0; g < (size_t)ZGEC_ENC_FILTER_GROUPS; g++) {
        size_t off = (n * g) / (size_t)ZGEC_ENC_FILTER_GROUPS;
        size_t len = (size_t)ZGEC_ENC_FILTER_GROUP;
        if (off >= n) break;
        if (off + len > n) len = n - off;
        if (total + len > cap) len = cap - total;
        if (len == 0) break;
        memcpy(samp + total, src + off, len);
        total += len;
    }
    return total;
}

/* Sample statistics for the gate: order-0 entropy in bits and the
 * number of adjacent equal byte pairs (a run-length proxy). A shuffle
 * is a permutation of the sample, so it cannot change the order-0
 * entropy; the run count is the tiebreak that lets a shuffle exposing
 * columnar runs win (11.11). */
static void zgec_enc_sample_stats(const uint8_t *p, size_t n,
                                  double *h0_out, size_t *runs_out)
{
    uint32_t hist[ZGEC_NSYM_LIT];
    size_t i;
    size_t runs = 0;
    memset(hist, 0, sizeof(hist));
    for (i = 0; i < n; i++) hist[p[i]]++;
    for (i = 1; i < n; i++) {
        if (p[i] == p[i - 1]) runs++;
    }
    *h0_out = zgec_enc_entropy_bits_u32(hist, ZGEC_NSYM_LIT);
    *runs_out = runs;
}

/* Estimate the sample as-is and after the byte-wise delta and the
 * shuffle strides {2,4,8,16,32,64}. Return 1 and set mode_out and
 * param_out to the best filter when it lowers the sample entropy by the
 * margin, or (permutation case) forms materially longer runs; 0 to
 * leave the block unfiltered. */
static int zgec_enc_choose_filter(const uint8_t *src, size_t n,
                                  unsigned *mode_out, unsigned *param_out)
{
    static const unsigned strides[6] = { 2u, 4u, 8u, 16u, 32u, 64u };
    uint8_t samp[ZGEC_ENC_FILTER_GROUPS * ZGEC_ENC_FILTER_GROUP];
    uint8_t tmp[ZGEC_ENC_FILTER_GROUPS * ZGEC_ENC_FILTER_GROUP];
    size_t sn;
    size_t i;
    double raw_h0;
    double best_h0;
    size_t raw_runs;
    size_t best_runs;
    unsigned best_mode = 0;
    unsigned best_param = 0;

    sn = zgec_enc_gather_sample(src, n, samp, sizeof(samp));
    if (sn < (size_t)ZGEC_ENC_FILTER_GROUP) return 0;
    zgec_enc_sample_stats(samp, sn, &raw_h0, &raw_runs);
    best_h0 = raw_h0;
    best_runs = raw_runs;

    for (i = 0; i < 7u; i++) {
        unsigned mode = (i == 0u) ? ZGEC_FILTER_DELTA : ZGEC_FILTER_SHUFFLE;
        unsigned param = (i == 0u) ? 0u : strides[i - 1u];
        double h0;
        size_t runs;
        if (zgec_filter_apply(tmp, samp, sn, mode, param) != ZGEC_OK) {
            continue;
        }
        zgec_enc_sample_stats(tmp, sn, &h0, &runs);
        if (h0 < best_h0 || (h0 <= best_h0 && runs > best_runs)) {
            best_h0 = h0;
            best_runs = runs;
            best_mode = mode;
            best_param = param;
        }
    }
    if (best_mode == 0) return 0;
    if (best_h0 + ZGEC_ENC_FILTER_MARGIN * (double)sn < raw_h0) {
        /* order-0 entropy saving covers the descriptor and inverse pass */
    } else if (best_h0 <= raw_h0 && best_runs > raw_runs + sn / 8u) {
        /* permutation form: material run formation (columnar shuffle) */
    } else {
        return 0;
    }
    *mode_out = best_mode;
    *param_out = best_param;
    return 1;
}

/* ---- encode one block (COMPRESSED payload) ---- */

static int zgec_payload_exportable(const uint8_t *payload, size_t size,
                                   uint32_t segment_count);

/* Everything the frame assembler needs about one encoded block, beyond
 * the payload bytes: the segment count for the record header, whether
 * the pre-filter was applied, whether the block is literal-exportable
 * (section 6.3), and its literal buffer in output order for use as a
 * later block's LITREF region. */
typedef struct {
    uint8_t *payload;
    size_t   payload_size;
    uint32_t segment_count;
    int      filtered;
    int      exportable;
    uint8_t *lit;
    size_t   lit_size;
} zgec_block_result;

static void zgec_block_result_init(zgec_block_result *r)
{
    memset(r, 0, sizeof(*r));
}

/* Encode one block. litref/litref_size is the optional LITREF region of
 * section 6.1, laid out between the dictionary and the block. */
static void zgec_block_result_init(zgec_block_result *r);
static zgec_err zgec_params_sanitise(zgec_params *p);
static zgec_err zgec_encode_block_full(zgec_encoder *e,
                            const uint8_t *src, size_t raw_size,
                            uint32_t block_index,
                            const uint8_t *dict, size_t dict_size,
                            const uint8_t *litref, size_t litref_size,
                            zgec_block_result *res)
{
    zgec_parse *parse = NULL;
    size_t *bounds = NULL;
    size_t n_segments = 0;
    size_t *lit_bounds = NULL;
    uint8_t *payload = NULL;
    size_t payload_cap = 0;
    size_t payload_off = 0;
    size_t dir_off = 0;
    zgec_seg_dir_entry *dir = NULL;
    zgec_block_params bp;
    zgec_err err;
    size_t s;
    uint32_t *ll_all = NULL;
    uint8_t *vb_all = NULL; /* dict ++ block, for sub-literal scoring */
    size_t vb_seg_base = 0;
    const uint8_t *parse_src = src;
    const uint8_t *parse_dict = dict;
    size_t parse_dict_size = dict_size;
    uint8_t *filt = NULL;
    int filtered = 0;
    unsigned filter_mode = 0;
    unsigned filter_param = 0;
    zgec_params params;

    (void)block_index;
    if (!e || !src || !res) return ZGEC_ERR_INVAL;
    if (raw_size == 0) return ZGEC_ERR_INVAL;
    if (raw_size > ((size_t)1 << 26) + 256) return ZGEC_ERR_INVAL;
    if (dict_size > ((size_t)1 << 26) || litref_size > ((size_t)1 << 26)) return ZGEC_ERR_INVAL;
    zgec_block_result_init(res);
    params = e->params;
    err = zgec_params_sanitise(&params);
    if (err != ZGEC_OK) return err;
    /* Section 6.3: a literal-reference predecessor must keep its
     * segments literal-exportable, i.e. plain literals (lit_form 0) and
     * no LL conditioning. When the caller asks for literal references,
     * the encoder therefore does not emit the optional sub-literal form
     * (9.6), so the chain the caller asked for stays alive; the
     * per-block measurement in the frame pass then decides whether each
     * block actually uses its LITREF region. D6: conditioning is NOT
     * disabled wholesale: OF conditioning keeps a block exportable
     * (zgec_payload_exportable only forbids LL), so LL conditioning is
     * suppressed per segment below while OF conditioning stays live. */
    if (params.use_litref) {
        params.use_sublit = 0;
    }

    /* Optional block pre-filter (7.5) gated on a sample (11.11). The
     * transform is position-dependent, so a filtered block does not
     * reference a dictionary in this encoder (7.5 permits a dictionary
     * but does not require one). */
    if (params.use_filter && raw_size >= 256) {
        unsigned fm = 0;
        unsigned fp = 0;
        if (zgec_enc_choose_filter(src, raw_size, &fm, &fp)) {
            filt = (uint8_t *)zgec_alloc(raw_size, 64);
            if (!filt) return ZGEC_ERR_NOMEM;
            err = zgec_filter_apply(filt, src, raw_size, fm, fp);
            if (err != ZGEC_OK) {
                zgec_free(filt);
                return err;
            }
            parse_src = filt;
            parse_dict = NULL;
            parse_dict_size = 0;
            filtered = 1;
            filter_mode = fm;
            filter_param = fp;
        }
    }

    /* Parse against the block dictionary and LITREF region when they are
     * assigned: the virtual buffer is [dict][litref][block]. A filtered
     * block references neither (7.5 permits a dictionary but this
     * encoder does not combine one with the filter, and V11 forbids
     * FILTERED together with literal references). */
    err = zgec_parse_block_ex(&parse, parse_src, raw_size, parse_dict,
                              parse_dict_size,
                              filtered ? NULL : litref,
                              filtered ? 0 : litref_size,
                              params.tier, params.lambda);
    zgec_free(filt); /* parse copied the literals it needs */
    filt = NULL;
    if (err != ZGEC_OK) return err;

    /* Segmentation (11.5): granules + greedy merge. */
    err = zgec_segment_greedy(parse, &bounds, &n_segments);
    if (err != ZGEC_OK) {
        zgec_parse_free(parse);
        return err;
    }

    /* Make the parse's offbase values agree with the decoder's
     * per-segment repeat-offset reset (8.2). Must happen before any
     * histogram or emission reads the sequences.
     * D3: the greedy merge above priced OF histograms/extra bits from
     * the whole-block rep chain, but emission uses per-segment chains.
     * So resolve the explicit distances once, rewrite for the first
     * bounds, RE-SEGMENT on the post-rewrite OF statistics, and rewrite
     * again for the final bounds. The second merge pass prices the OF
     * classes actually emitted (up to the residual difference between
     * the two bound sets). */
    if (parse->n_seq > 0) {
        uint32_t *seg_dist = (uint32_t *)zgec_alloc(
            parse->n_seq * sizeof(*seg_dist), _Alignof(uint32_t));
        if (!seg_dist) {
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_NOMEM;
        }
        {
            zgec_reps whole;
            zgec_reps_init(&whole);
            for (s = 0; s < parse->n_seq; s++) {
                seg_dist[s] =
                    zgec_reps_resolve(&whole, parse->seq[s].offbase);
            }
        }
        int offbase_changed = 0;
        zgec_enc_rewrite_offbase_from_dist(parse, bounds, n_segments,
                                           seg_dist, &offbase_changed);
        /* P6a: the re-merge exists to price the OF classes actually
         * emitted under per-segment rep chains. If the first rewrite left
         * every offbase untouched, the OF statistics are identical to what
         * the first merge already priced, so a deterministic re-merge would
         * reproduce the same bounds and the second rewrite would again be a
         * no-op. Skip both; the result is provably identical. */
        if (offbase_changed) {
            size_t *bounds2 = NULL;
            size_t n_seg2 = 0;
            if (zgec_segment_greedy(parse, &bounds2, &n_seg2) == ZGEC_OK) {
                zgec_free(bounds);
                bounds = bounds2;
                n_segments = n_seg2;
            }
            /* On re-segment failure keep the first bounds (still valid,
             * just priced from pre-rewrite OF stats). */
            zgec_enc_rewrite_offbase_from_dist(parse, bounds, n_segments,
                                               seg_dist, NULL);
        }
        zgec_free(seg_dist);
    }

    lit_bounds = (size_t *)zgec_alloc((n_segments + 1) * sizeof(size_t),
                                     _Alignof(size_t));
    if (!lit_bounds) {
        zgec_free(bounds);
        zgec_parse_free(parse);
        return ZGEC_ERR_NOMEM;
    }
    zgec_lit_bounds(parse, bounds, n_segments, lit_bounds);

    /* Block context maps (11.6): one map per block, or mode0/k1. */
    memset(&bp, 0, sizeof(bp));
    bp.plain.ctx_mode = 0;
    bp.plain.ctx_count = 1;
    bp.sub.ctx_mode = 0;
    bp.sub.ctx_count = 1;
    if (params.use_contexts && parse->n_lit > 0) {
        ll_all = (uint32_t *)zgec_alloc(
            (parse->n_seq ? parse->n_seq : 1) * sizeof(*ll_all),
            _Alignof(uint32_t));
        if (!ll_all) {
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_NOMEM;
        }
        for (s = 0; s < parse->n_seq; s++) ll_all[s] = parse->seq[s].ll;
        {
            uint8_t mode = 0;
            uint8_t k = 1;
            uint8_t cmap[ZGEC_ENC_NCLASS];
            memset(cmap, 0, sizeof(cmap));
            err = zgec_select_contexts(parse->lit, parse->n_lit, ll_all,
                                       parse->n_seq, &mode, &k, cmap);
            if (err != ZGEC_OK) {
                zgec_free(ll_all);
                zgec_free(lit_bounds);
                zgec_free(bounds);
                zgec_parse_free(parse);
                return err;
            }
            bp.plain.ctx_mode = mode;
            bp.plain.ctx_count = k;
            if (k > 1) memcpy(bp.plain.class_map, cmap, 64);
        }
    }
    /* Virtual buffer for sub-literal residuals: [dict][litref][block].
     * Built before sub-context training (D6), which predicts from it. */
    if (params.use_sublit && !filtered) {
        size_t pre = (dict && dict_size > 0) ? dict_size : 0;
        size_t lr = litref_size;
        size_t vb_need = pre;
        if (zgec_add_overflows(vb_need, lr) || zgec_add_overflows(vb_need + lr, raw_size + 64)) {
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_INTERNAL;
        }
        vb_need = pre + lr + raw_size + 64;
        vb_all = (uint8_t *)zgec_alloc(vb_need, 64);
        if (!vb_all) {
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_NOMEM;
        }
        if (pre > 0) memcpy(vb_all, dict, pre);
        if (lr > 0) memcpy(vb_all + pre, litref, lr);
        memcpy(vb_all + pre + lr, src, raw_size);
        vb_seg_base = pre + lr;
    }

    /* D6: the sub descriptor gets its own trained context map. Block
     * residuals are built exactly as emission builds them (per-segment
     * rep0 restart at the final bounds), then clustered like plain
     * literals; segments whose residual contexts win are emitted with
     * k = sub.ctx_count. Stays order-0 when training is off. */
    if (params.use_contexts && params.use_sublit && !filtered &&
        parse->n_seq > 0 && parse->n_lit > 0 && vb_all && ll_all) {
        uint8_t *rb = (uint8_t *)zgec_alloc(parse->n_lit, 64);
        if (!rb) {
            zgec_free(vb_all);
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_NOMEM;
        }
        {
            size_t bo2 = 0; /* raw output offset of the segment start */
            size_t s2;
            int train_ok = 1;
            for (s2 = 0; s2 < n_segments && train_ok; s2++) {
                size_t s0 = bounds[s2];
                size_t s1 = bounds[s2 + 1];
                size_t nn = (s1 > s0) ? (s1 - s0) : 0;
                size_t l0 = lit_bounds[s2];
                size_t l1 = lit_bounds[s2 + 1];
                size_t nl = (l1 > l0) ? (l1 - l0) : 0;
                size_t tt;
                uint64_t seg_ml = 0;
                if (nn > 0 && nl > 0) {
                    uint32_t *tll =
                        (uint32_t *)zgec_alloc(nn * sizeof(*tll),
                                               _Alignof(uint32_t));
                    uint32_t *tml =
                        (uint32_t *)zgec_alloc(nn * sizeof(*tml),
                                               _Alignof(uint32_t));
                    uint32_t *tr0 =
                        (uint32_t *)zgec_alloc(nn * sizeof(*tr0),
                                               _Alignof(uint32_t));
                    if (!tll || !tml || !tr0) {
                        zgec_free(tll);
                        zgec_free(tml);
                        zgec_free(tr0);
                        train_ok = 0;
                        break;
                    }
                    for (tt = 0; tt < nn; tt++) {
                        const zgec_sequence *qq = &parse->seq[s0 + tt];
                        tll[tt] = qq->ll;
                        tml[tt] = (qq->ml >= 3) ? (qq->ml - 3u) : 0u;
                        seg_ml += (uint64_t)qq->ml;
                    }
                    zgec_enc_rep0_before(tr0, &parse->seq[s0], nn);
                    zgec_enc_build_resid(
                        rb + l0, parse->lit + l0, nl, tll, tml, nn, tr0,
                        zgec_enc_rep0_after(&parse->seq[s0], nn), vb_all,
                        vb_seg_base + bo2);
                    zgec_free(tll);
                    zgec_free(tml);
                    zgec_free(tr0);
                } else {
                    for (tt = 0; tt < nn; tt++)
                        seg_ml += (uint64_t)parse->seq[s0 + tt].ml;
                }
                bo2 += nl + (size_t)seg_ml;
            }
            if (train_ok) {
                uint8_t mode = 0;
                uint8_t kk = 1;
                uint8_t cmap[ZGEC_ENC_NCLASS];
                memset(cmap, 0, sizeof(cmap));
                err = zgec_select_contexts(rb, parse->n_lit, ll_all,
                                           parse->n_seq, &mode, &kk, cmap);
                if (err == ZGEC_OK) {
                    bp.sub.ctx_mode = mode;
                    bp.sub.ctx_count = kk;
                    if (kk > 1) memcpy(bp.sub.class_map, cmap, 64);
                }
            }
        }
        zgec_free(rb);
        if (err != ZGEC_OK) {
            zgec_free(vb_all);
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return err;
        }
    }

    if (zgec_add_overflows(raw_size, 1024)) {
        zgec_free(vb_all);
        zgec_free(ll_all);
        zgec_free(lit_bounds);
        zgec_free(bounds);
        zgec_parse_free(parse);
        return ZGEC_ERR_INTERNAL;
    }
    payload_cap = raw_size + 1024;
    if (payload_cap < 256) payload_cap = 256;
    payload = (uint8_t *)zgec_alloc(payload_cap, 64);
    if (!payload) {
        zgec_free(vb_all);
        zgec_free(ll_all);
        zgec_free(lit_bounds);
        zgec_free(bounds);
        zgec_parse_free(parse);
        return ZGEC_ERR_NOMEM;
    }
    payload_off = 0;
    if (filtered) {
        /* Spec 4.3: a FILTERED COMPRESSED payload begins with the 2-byte
         * filter descriptor, before the block parameters. */
        payload[0] = (uint8_t)filter_mode;
        payload[1] = (uint8_t)filter_param;
        payload_off = 2;
    }
    {
        size_t pn = zgec_block_params_emit(payload + payload_off,
                                           payload_cap - payload_off, &bp);
        if (pn == 0) {
            zgec_free(payload);
            zgec_free(vb_all);
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_INTERNAL;
        }
        payload_off += pn;
    }
    dir_off = payload_off;
    if (n_segments > (SIZE_MAX - dir_off) / 8) {
        zgec_free(payload);
        zgec_free(vb_all);
        zgec_free(ll_all);
        zgec_free(lit_bounds);
        zgec_free(bounds);
        zgec_parse_free(parse);
        return ZGEC_ERR_INTERNAL;
    }
    err = zgec_payload_grow(&payload, &payload_cap, dir_off, dir_off + n_segments * 8);
    if (err != ZGEC_OK) {
        zgec_free(payload);
        zgec_free(vb_all);
        zgec_free(ll_all);
        zgec_free(lit_bounds);
        zgec_free(bounds);
        zgec_parse_free(parse);
        return err;
    }
    payload_off = dir_off + n_segments * 8;
    if (n_segments > 0) {
        dir = (zgec_seg_dir_entry *)zgec_alloc(n_segments * sizeof(*dir),
                                              _Alignof(zgec_seg_dir_entry));
        if (!dir) {
            zgec_free(payload);
            zgec_free(vb_all);
            zgec_free(ll_all);
            zgec_free(lit_bounds);
            zgec_free(bounds);
            zgec_parse_free(parse);
            return ZGEC_ERR_NOMEM;
        }
        memset(dir, 0, n_segments * sizeof(*dir));
    }

    /* Per-segment coder selection (11.7) then emission, appended
     * directly after the reserved directory (P6). Selection and
     * emission share one prep per segment (P1/P2); the class and
     * conditioning histograms reuse block-level scratch (M2). The
     * loop is sequential today; segments of a block share only the
     * block tables, so per-segment work is parallel-ready (11.8). */
    {
        size_t out_base = 0; /* output offset of the segment start */
        uint32_t *scratch_cls = NULL;
        uint32_t *scratch_cond = NULL;
        if (params.use_contexts &&
            (bp.plain.ctx_count > 1 || bp.sub.ctx_count > 1)) {
            scratch_cls = (uint32_t *)zgec_alloc(
                (size_t)64 * 256u * sizeof(uint32_t), 64);
            if (!scratch_cls) {
                zgec_free(dir);
                zgec_free(payload);
                zgec_free(vb_all);
                zgec_free(ll_all);
                zgec_free(lit_bounds);
                zgec_free(bounds);
                zgec_parse_free(parse);
                return ZGEC_ERR_NOMEM;
            }
        }
        if (params.use_conditioning) {
            scratch_cond = (uint32_t *)zgec_alloc(
                3u * ZGEC_NSYM_SEQ * sizeof(uint32_t), 64);
            if (!scratch_cond) {
                zgec_free(scratch_cls);
                zgec_free(dir);
                zgec_free(payload);
                zgec_free(vb_all);
                zgec_free(ll_all);
                zgec_free(lit_bounds);
                zgec_free(bounds);
                zgec_parse_free(parse);
                return ZGEC_ERR_NOMEM;
            }
        }
        for (s = 0; s < n_segments; s++) {
            size_t s0 = bounds[s];
            size_t s1 = bounds[s + 1];
            size_t n = (s1 > s0) ? (s1 - s0) : 0;
            size_t l0 = lit_bounds[s];
            size_t l1 = lit_bounds[s + 1];
            size_t n_lit_slice = (l1 > l0) ? (l1 - l0) : 0;
            uint32_t raw_len = 0;
            zgec_coder_choice choice;
            zgec_seg_prep prep;

            /* S2: explicit guard; the old clamp underflows when
             * l0 > parse->n_lit. */
            if (l0 > parse->n_lit) {
                err = ZGEC_ERR_INTERNAL;
                goto fail_seg_scratch;
            }
            if (l0 + n_lit_slice > parse->n_lit) {
                n_lit_slice = parse->n_lit - l0;
            }
            memset(&prep, 0, sizeof(prep));
            err = zgec_seg_prep_build(
                &prep, parse, s0, s1,
                params.use_sublit && vb_all != NULL && n_lit_slice > 0,
                (n_lit_slice > 0) ? parse->lit + l0 : NULL, n_lit_slice,
                vb_all, vb_seg_base + out_base);
            if (err != ZGEC_OK) goto fail_seg_scratch;
            err = zgec_select_coder(
                &params, parse->lit + l0, n_lit_slice, prep.ll, prep.ml,
                prep.of, n, prep.rep0 ? prep.rep0 : prep.ll,
                prep.rep0_tail, vb_all, vb_seg_base + out_base,
                (bp.plain.ctx_count > 1) ? bp.plain.class_map : NULL,
                (int)bp.plain.ctx_count, (int)bp.plain.ctx_mode,
                (bp.sub.ctx_count > 1) ? bp.sub.class_map : NULL,
                (int)bp.sub.ctx_count, (int)bp.sub.ctx_mode,
                prep.h_ll, prep.h_ml, prep.h_of, prep.xbits, prep.resid,
                scratch_cls, scratch_cond,
                params.use_litref ? 0 : 1, &choice);
            if (err != ZGEC_OK) {
                zgec_seg_prep_free(&prep);
                goto fail_seg_scratch;
            }
            err = zgec_emit_segment(&payload, &payload_cap, &payload_off,
                                    &dir[s], &raw_len, parse, s0, s1, l0,
                                    n_lit_slice, &bp, &choice, &prep, vb_all,
                                    vb_seg_base, out_base);
            zgec_seg_prep_free(&prep);
            if (err != ZGEC_OK) goto fail_seg_scratch;
            out_base += (size_t)raw_len;
            continue;
        fail_seg_scratch:
            zgec_free(scratch_cls);
            zgec_free(scratch_cond);
            goto fail_seg;
        }
        zgec_free(scratch_cls);
        zgec_free(scratch_cond);
    }
    zgec_seg_dir_emit(payload + dir_off, dir, (uint32_t)n_segments);
    zgec_free(dir);
    zgec_free(vb_all);
    zgec_free(ll_all);
    zgec_free(lit_bounds);
    zgec_free(bounds);
    /* The block's literal buffer, for a later block's LITREF region. */
    if (parse->n_lit > 0) {
        uint8_t *lc = (uint8_t *)zgec_alloc(parse->n_lit, 64);
        if (!lc) {
            zgec_parse_free(parse);
            zgec_free(payload);
            return ZGEC_ERR_NOMEM;
        }
        memcpy(lc, parse->lit, parse->n_lit);
        res->lit = lc;
        res->lit_size = parse->n_lit;
    }
    res->exportable = (!filtered &&
                       zgec_payload_exportable(payload, payload_off,
                                               (uint32_t)n_segments)) ? 1 : 0;
    zgec_parse_free(parse);
    res->payload = payload;
    res->payload_size = payload_off;
    res->segment_count = (uint32_t)n_segments;
    res->filtered = filtered;
    return ZGEC_OK;

fail_seg:
    zgec_free(dir);
    zgec_free(payload);
    zgec_free(vb_all);
    zgec_free(ll_all);
    zgec_free(lit_bounds);
    zgec_free(bounds);
    zgec_parse_free(parse);
    return err;
}

/* Thin wrapper over zgec_encode_block_full for the single-block API:
 * no literal references, no auxiliary outputs. */
static zgec_err zgec_encode_block_ex(zgec_encoder *e,
                            const uint8_t *src, size_t raw_size,
                            uint32_t block_index,
                            const uint8_t *dict, size_t dict_size,
                            uint8_t **dst, size_t *dst_size,
                            uint32_t *segment_count, int *filtered_out)
{
    zgec_block_result r;
    zgec_err err;
    zgec_block_result_init(&r);
    if (segment_count) *segment_count = 0;
    if (filtered_out) *filtered_out = 0;
    err = zgec_encode_block_full(e, src, raw_size, block_index, dict,
                                 dict_size, NULL, 0, &r);
    if (err != ZGEC_OK) return err;
    zgec_free(r.lit);
    *dst = r.payload;
    if (dst_size) *dst_size = r.payload_size;
    if (segment_count) *segment_count = r.segment_count;
    if (filtered_out) *filtered_out = r.filtered;
    return ZGEC_OK;
}

/* Public wrapper: encodes a single block (filter applied internally
 * when params.use_filter selects one). */
zgec_err zgec_encode_block(zgec_encoder *e,
                            const uint8_t *src, size_t raw_size,
                            uint32_t block_index,
                            const uint8_t *dict, size_t dict_size,
                            uint8_t **dst, size_t *dst_size,
                            uint32_t *segment_count)
{
    return zgec_encode_block_ex(e, src, raw_size, block_index, dict,
                                dict_size, dst, dst_size, segment_count,
                                NULL);
}

/* Cost model note (P3): this runs one FULL block encode (parse,
 * segment, emit) per call. The external-dict pass calls it 1 + n_ext
 * times per block and the epoch machinery up to ~4 more times per
 * sampled/decided block, so dictionary probing dominates encode time
 * when use_dicts/external dicts are on. That is the price of the 5.5
 * measurement gate (no proxy); callers could cache the dict=NULL
 * result per block, but the passes currently share no cache. */
size_t zgec_estimate_block(zgec_encoder *e,
                            const uint8_t *src, size_t raw_size,
                            uint32_t block_index,
                            const uint8_t *dict, size_t dict_size)
{
    uint8_t *tmp = NULL;
    size_t tmp_size = 0;
    zgec_err err;
    if (!e || !src || raw_size == 0) return raw_size;
    err = zgec_encode_block(e, src, raw_size, block_index, dict, dict_size,
                            &tmp, &tmp_size, NULL);
    if (err != ZGEC_OK || !tmp) return raw_size;
    {
        size_t est = tmp_size;
        zgec_free(tmp);
        return est;
    }
}

/* ---- frame encode ---- */

/* One assembled record (header fields + payload bytes). */
typedef struct {
    zgec_record_header hdr;
    uint8_t *payload; /* payload_size bytes (NULL when 0... always set) */
} zgec_enc_record;

/* Scan a COMPRESSED payload: 1 if every segment is plain-literal
 * (lit_form 0) with no LL conditioning (bit 4), i.e. the block is
 * literal-exportable per 6.3. */
static int zgec_payload_exportable(const uint8_t *payload, size_t size,
                                   uint32_t segment_count)
{
    zgec_block_params bp;
    size_t pl;
    size_t dir_off;
    size_t seg_off;
    uint32_t i;
    memset(&bp, 0, sizeof(bp));
    if (!payload || segment_count == 0) return 0;
    pl = zgec_block_params_parse(&bp, payload, size);
    if (pl == 0 || pl > size) return 0;
    dir_off = pl;
    if (size < dir_off + (size_t)segment_count * 8u) return 0;
    seg_off = dir_off + (size_t)segment_count * 8u;
    for (i = 0; i < segment_count; i++) {
        uint32_t comp_len = zgec_rd32(payload + dir_off + (size_t)i * 8u);
        zgec_seg_header sh;
        size_t hz;
        int lit_form;
        int k;
        memset(&sh, 0, sizeof(sh));
        if (comp_len == 0 || seg_off + (size_t)comp_len > size) return 0;
        /* How many literal tables a segment carries depends on its
         * lit_form (low bit of segment_flags), which selects the plain
         * or sub block-parameter descriptor. */
        lit_form = (int)(payload[seg_off] & 0x01u);
        k = (int)(lit_form ? bp.sub.ctx_count : bp.plain.ctx_count);
        if (k != 1 && k != 2 && k != 4 && k != 8) return 0;
        hz = zgec_seg_header_parse_ex(&sh, payload + seg_off,
                                      (size_t)comp_len, NULL, 0, NULL, k);
        if (hz == 0) return 0;
        if ((sh.segment_flags & ZGEC_SEG_LIT_FORM) != 0) return 0;
        if ((sh.segment_flags & ZGEC_SEG_SEQ_CTX_LL) != 0) return 0;
        seg_off += (size_t)comp_len;
    }
    return 1;
}

/* Sanitise parameters: header-reserved 0 values select defaults. */
static zgec_err zgec_params_sanitise(zgec_params *p)
{
    if (!p) return ZGEC_ERR_INVAL;
    if (p->block_log2 == 0) p->block_log2 = 21;
    if (p->block_log2 < ZGEC_BLOCK_LOG2_MIN ||
        p->block_log2 > ZGEC_BLOCK_LOG2_MAX) {
        return ZGEC_ERR_INVAL;
    }
    if (p->epoch_blocks == 0) p->epoch_blocks = 10;
    if (p->epoch_blocks < 0 || p->epoch_blocks > (int)ZGEC_MAX_EPOCH_BLOCKS) return ZGEC_ERR_INVAL;
    if (p->max_dict_log2 == 0) p->max_dict_log2 = 20;
    if (p->max_dict_log2 < 0 || p->max_dict_log2 > ZGEC_MAX_DICT_LOG2) return ZGEC_ERR_INVAL;
    if (p->seg_hint_log2 == 0) p->seg_hint_log2 = 18;
    if (p->seg_hint_log2 < ZGEC_SEG_HINT_LOG2_MIN ||
        p->seg_hint_log2 > ZGEC_SEG_HINT_LOG2_MAX) {
        return ZGEC_ERR_INVAL;
    }
    if (p->tier != ZGEC_TIER_FAST && p->tier != ZGEC_TIER_MAIN &&
        p->tier != ZGEC_TIER_HIGH) {
        return ZGEC_ERR_INVAL;
    }
    /* 0 selects one worker per core (11.8); the pool is capped below
     * the static worker limit, and a single worker stays serial. */
    if (p->n_threads < 0) p->n_threads = 1;
    if (p->n_threads == 0) p->n_threads = (int)zgec_cpu_count();
    if (p->n_threads > (int)ZGEC_MAX_WORKERS)
        p->n_threads = (int)ZGEC_MAX_WORKERS;
    /* S4: a negative or NaN lambda inverts the bits+lambda*cycles
     * objective (slowest decoder wins, or NaN poisons every candidate
     * score). !(x >= 0) catches both negatives and NaN; clamp huge
     * values to a large finite dial. */
    if (!(p->lambda >= 0.0)) p->lambda = 0.0;
    if (p->lambda > 1e12) p->lambda = 1e12;
    return ZGEC_OK;
}

/* ---- parallel block pass (11.8) ----
 *
 * Blocks are independent once their dictionaries are known, so phase A
 * does every block's parse/segment/entropy-code/emit work on the pool.
 * Two steps are order-dependent and stay in the serial phase B: the
 * literal-reference chain of 6.3, which reads the accepted literal
 * buffer of each predecessor, and the RAW fallback, which compares
 * against the payload the chain settled on. */

/* One block's phase-A result. */
typedef struct {
    uint32_t          index;
    const uint8_t    *src;      /* this block's raw bytes */
    size_t            raw_size;
    const uint8_t    *dict;     /* dictionary bytes for this block (may be NULL) */
    size_t            dict_len;
    int               external; /* dict is an external dictionary (5.8) */
    int               kind;     /* ZGEC_REC_RLE, else a COMPRESSED candidate */
    zgec_err          err;
    zgec_block_result r;
} zgec_enc_blockjob;

typedef struct {
    zgec_encoder      *e;
    zgec_enc_blockjob *jobs;
} zgec_enc_blockpass;

static void zgec_enc_block_phaseA(void *ctx, size_t i)
{
    zgec_enc_blockpass *bp = (zgec_enc_blockpass *)ctx;
    zgec_enc_blockjob *j = &bp->jobs[i];
    j->err = ZGEC_OK;
    if (j->raw_size > 0 && zgec_enc_all_same_byte(j->src, j->raw_size)) {
        uint8_t *rp = (uint8_t *)zgec_alloc(1, 1);
        if (!rp) {
            j->err = ZGEC_ERR_NOMEM;
            return;
        }
        rp[0] = j->src[0];
        j->kind = ZGEC_REC_RLE;
        j->r.payload = rp;
        j->r.payload_size = 1;
        return;
    }
    j->kind = ZGEC_REC_COMPRESSED;
    j->err = zgec_encode_block_full(bp->e, j->src, j->raw_size, j->index,
                                    j->dict, j->dict_len, NULL, 0, &j->r);
}

/* External-dictionary pass (5.8): best of {none, each registered dict}
 * per block, by measurement (5.5). */
typedef struct {
    zgec_encoder         *e;
    const uint8_t        *src;
    size_t                src_size;
    size_t                block_size;
    uint16_t             *block_dict;
    const uint16_t       *ext_ids;
    const uint8_t *const *ext_data;
    const size_t         *ext_sizes;
    size_t                n_ext;
} zgec_enc_extpass;

static void zgec_enc_ext_block(void *ctx, size_t i)
{
    zgec_enc_extpass *x = (zgec_enc_extpass *)ctx;
    size_t bo = i * x->block_size;
    size_t bs = x->src_size - bo;
    size_t best;
    uint16_t best_id = 0;
    size_t k;
    if (bs > x->block_size) bs = x->block_size;
    best = zgec_estimate_block(x->e, x->src + bo, bs, (uint32_t)i, NULL, 0);
    for (k = 0; k < x->n_ext; k++) {
        size_t b = zgec_estimate_block(x->e, x->src + bo, bs, (uint32_t)i,
                                       x->ext_data[k], x->ext_sizes[k]);
        if (b < best) {
            best = b;
            best_id = x->ext_ids[k];
        }
    }
    x->block_dict[i] = best_id;
}

/* Per-block decision for a trained epoch dictionary (5.5): keep it only
 * where the block is smaller with it. */
typedef struct {
    zgec_encoder  *e;
    const uint8_t *src;
    size_t         src_size;
    size_t         block_size;
    uint16_t      *block_dict;
    const uint8_t *cand;
    size_t         cand_len;
    uint16_t       id;
    size_t         begin;
} zgec_enc_epochpass;

static void zgec_enc_epoch_block(void *ctx, size_t k)
{
    zgec_enc_epochpass *p = (zgec_enc_epochpass *)ctx;
    size_t t = p->begin + k;
    size_t bo = t * p->block_size;
    size_t bs = p->src_size - bo;
    size_t a;
    size_t b;
    if (bs > p->block_size) bs = p->block_size;
    if (p->block_dict[t] != 0) return; /* an external dict already wins */
    a = zgec_estimate_block(p->e, p->src + bo, bs, (uint32_t)t, NULL, 0);
    b = zgec_estimate_block(p->e, p->src + bo, bs, (uint32_t)t, p->cand,
                            p->cand_len);
    p->block_dict[t] = (b < a) ? p->id : 0;
}

zgec_err zgec_encode_frame(zgec_encoder *e,
                            const uint8_t *src, size_t src_size,
                            uint8_t **dst, size_t *dst_size)
{
    zgec_params params;
    size_t block_size = 0;
    size_t n_blocks = 0;
    zgec_frame_header fh;
    zgec_enc_record *recs = NULL;
    size_t n_recs = 0;
    size_t recs_cap = 0;
    zgec_enc_blockjob *jobs = NULL;
    uint16_t *block_dict = NULL; /* per-block dict id (0 = none) */
    uint8_t **dict_bytes = NULL; /* per-epoch dict content (epoch k -> bytes) */
    size_t *dict_lens = NULL;
    uint16_t *epoch_dict_id = NULL;
    size_t n_epochs = 0;
    uint16_t next_dict_id = 0;
    uint32_t dict_count = 0;
    uint32_t dict_total = 0;
    size_t n_ext = 0;
    uint16_t ext_ids[ZGEC_ENC_MAX_EXT];
    const uint8_t *ext_data[ZGEC_ENC_MAX_EXT];
    size_t ext_sizes[ZGEC_ENC_MAX_EXT];
    uint8_t *lr_buf[ZGEC_MAX_LITREF_DEPTH]; /* newest first */
    size_t lr_len[ZGEC_MAX_LITREF_DEPTH];
    size_t lr_n = 0;                        /* consecutive exportable run */
    uint8_t *lr_region = NULL; /* running oldest-first concatenation (M4) */
    size_t lr_cap = 0;
    size_t lr_total = 0;       /* live bytes in lr_region == sum(lr_len) */
    size_t i;
    zgec_err err;

    if (!e || !src || !dst || src_size == 0) return ZGEC_ERR_INVAL;
    params = e->params;
    err = zgec_params_sanitise(&params);
    if (err != ZGEC_OK) return err;

    /* Registered external dictionaries (5.8), excluded from the frame. */
    {
        int s2;
        for (s2 = 0; s2 < ZGEC_ENC_MAX_EXT; s2++) {
            if (e->ext[s2].used && e->ext[s2].data && e->ext[s2].size > 0) {
                ext_ids[n_ext] = e->ext[s2].id;
                ext_data[n_ext] = e->ext[s2].data;
                ext_sizes[n_ext] = e->ext[s2].size;
                n_ext++;
            }
        }
    }

    block_size = (size_t)1 << (unsigned)params.block_log2;
    n_blocks = (src_size + block_size - 1) / block_size;
    if (n_blocks == 0 || n_blocks > UINT32_MAX) return ZGEC_ERR_INVAL;
    if (n_blocks > SIZE_MAX / sizeof(uint16_t)) return ZGEC_ERR_NOMEM;

    memset(&fh, 0, sizeof(fh));
    fh.version_major = ZGEC_VERSION_MAJOR;
    fh.version_minor = ZGEC_VERSION_MINOR;
    /* Frame flags: CONTENT_SIZE is known, HAS_FOOTER always (we write
     * footer+trailer), BLOCK_CHECKSUMS when asked, EXTERNAL_DICT when
     * an external dictionary is registered, and EXTENDED_LITREF when
     * any block has lit_ref_depth > 0 (set after the block pass). */
    fh.flags = (uint16_t)(ZGEC_FLAG_HAS_FOOTER | ZGEC_FLAG_CONTENT_SIZE);
    if (n_ext > 0) {
        fh.flags = (uint16_t)((unsigned)fh.flags | ZGEC_FLAG_EXTERNAL_DICT);
    }
    if (params.block_checksums) {
        fh.flags = (uint16_t)((unsigned)fh.flags | ZGEC_FLAG_BLOCK_CHECKSUMS);
    }
    fh.block_log2 = (uint8_t)params.block_log2;
    fh.epoch_blocks = (uint8_t)params.epoch_blocks;
    fh.max_dict_log2 = (uint8_t)params.max_dict_log2;
    /* 3.3/5.8: the header declares the largest dictionary a block may
     * reference, and a decoder rejects a dictionary larger than that.
     * Raise the declared cap to cover the registered external
     * dictionaries, so a frame is always decodable with the very inputs
     * that produced it. */
    {
        size_t si;
        for (si = 0; si < n_ext; si++) {
            size_t want = ext_sizes[si];
            uint8_t need = 0;
            while (need < (uint8_t)ZGEC_MAX_DICT_LOG2 &&
                   ((size_t)1 << need) < want)
                need = (uint8_t)(need + 1);
            if (need > fh.max_dict_log2) fh.max_dict_log2 = need;
        }
    }
    fh.seg_hint_log2 = (uint8_t)params.seg_hint_log2;
    fh.content_size = (uint64_t)src_size;
    fh.block_count = (uint32_t)n_blocks;

    block_dict = (uint16_t *)zgec_alloc(n_blocks * sizeof(*block_dict),
                                       _Alignof(uint16_t));
    if (!block_dict) return ZGEC_ERR_NOMEM;
    memset(block_dict, 0, n_blocks * sizeof(*block_dict));

    /* ---- external dictionary pass (5.8) ----
     * The bytes are supplied by the application, so they cost the frame
     * only a footer entry; a block uses one only when a measurement
     * shows a smaller payload (the same 5.5 rule as an epoch
     * dictionary). Epoch ids are seeded above every external id so the
     * two spaces cannot collide. */
    if (n_ext > 0) {
        size_t e2;
        zgec_enc_extpass xp;
        for (e2 = 0; e2 < n_ext; e2++) {
            if (ext_ids[e2] > next_dict_id) next_dict_id = ext_ids[e2];
        }
        xp.e = e;
        xp.src = src;
        xp.src_size = src_size;
        xp.block_size = block_size;
        xp.block_dict = block_dict;
        xp.ext_ids = ext_ids;
        xp.ext_data = (const uint8_t *const *)ext_data;
        xp.ext_sizes = ext_sizes;
        xp.n_ext = n_ext;
        zgec_enc_parallel_for(params.n_threads, n_blocks,
                              zgec_enc_ext_block, &xp);
    }

    /* ---- dictionary pass (5.3-5.5, 11.1) ---- */
    n_epochs = (n_blocks + (size_t)params.epoch_blocks - 1) /
               (size_t)params.epoch_blocks;
    if (params.use_dicts && params.epoch_blocks > 0 && n_epochs > 1) {
        dict_bytes = (uint8_t **)zgec_alloc(n_epochs * sizeof(*dict_bytes),
                                           _Alignof(void *));
        dict_lens = (size_t *)zgec_alloc(n_epochs * sizeof(*dict_lens),
                                        _Alignof(size_t));
        epoch_dict_id = (uint16_t *)zgec_alloc(n_epochs * sizeof(*epoch_dict_id),
                                              _Alignof(uint16_t));
        if (!dict_bytes || !dict_lens || !epoch_dict_id) {
            zgec_free(block_dict);
            zgec_free(dict_bytes);
            zgec_free(dict_lens);
            zgec_free(epoch_dict_id);
            return ZGEC_ERR_NOMEM;
        }
        memset(dict_bytes, 0, n_epochs * sizeof(*dict_bytes));
        memset(dict_lens, 0, n_epochs * sizeof(*dict_lens));
        memset(epoch_dict_id, 0, n_epochs * sizeof(*epoch_dict_id));

        for (i = 1; i < n_epochs; i++) {
            size_t prev_start = (i - 1) * (size_t)params.epoch_blocks * block_size;
            size_t prev_end = i * (size_t)params.epoch_blocks * block_size;
            size_t prev_len;
            size_t budget;
            uint8_t *cand = NULL;
            size_t cand_len = 0;
            size_t comp_dict;
            size_t ep_start;
            size_t ep_end;
            size_t ep_n;
            size_t sample;
            size_t measured;
            size_t s_without = 0;
            size_t s_with = 0;
            size_t t;
            double saving;

            if (prev_end > src_size) prev_end = src_size;
            if (prev_start >= prev_end) continue;
            prev_len = prev_end - prev_start;
            budget = zgec_dict_default_size(prev_len, params.max_dict_log2);
            err = zgec_dict_train(&cand, &cand_len, src + prev_start, prev_len,
                                  budget);
            if (err != ZGEC_OK || !cand || cand_len == 0) {
                zgec_free(cand);
                continue;
            }
            /* Compressed dictionary cost: outer DICT header + inner
             * RAW record (24 bytes) + bytes. */
            comp_dict = 24 + 24 + cand_len;

            /* Sample up to 2 blocks of epoch i with/without. */
            ep_start = i * (size_t)params.epoch_blocks;
            ep_end = ep_start + (size_t)params.epoch_blocks;
            if (ep_end > n_blocks) ep_end = n_blocks;
            ep_n = (ep_end > ep_start) ? (ep_end - ep_start) : 0;
            sample = (ep_n > 2) ? 2 : ep_n;
            measured = 0;
            for (t = 0; t < sample; t++) {
                size_t bi = ep_start + t;
                size_t bo = bi * block_size;
                size_t bs = src_size - bo;
                if (bs > block_size) bs = block_size;
                /* A block that an external dictionary already wins keeps
                 * its id, so it cannot judge this candidate. */
                if (block_dict[bi] != 0) continue;
                s_without += zgec_estimate_block(e, src + bo, bs,
                                                (uint32_t)bi, NULL, 0);
                s_with += zgec_estimate_block(e, src + bo, bs, (uint32_t)bi,
                                             cand, cand_len);
                measured++;
            }
            if (measured == 0) {
                zgec_free(cand);
                continue;
            }
            /* Extrapolate the saving to the whole epoch (5.5) and
             * apply the 1.25x ratio-neutrality gate. */
            saving = (double)(uint64_t)s_without - (double)(uint64_t)s_with;
            saving *= (double)(uint64_t)ep_n / (double)(uint64_t)measured;
            if (saving < 1.25 * (double)(uint64_t)comp_dict) {
                zgec_free(cand);
                continue;
            }
            if (next_dict_id >= ZGEC_MAX_DICT_ID - 1) {
                zgec_free(cand);
                continue;
            }
            next_dict_id++;
            epoch_dict_id[i] = next_dict_id;
            dict_bytes[i] = cand;
            dict_lens[i] = cand_len;
            dict_count++;
            /* Per-block decision: dict_id 0 fallback when the block is
             * not smaller with the dictionary (5.5). */
            {
                zgec_enc_epochpass epj;
                epj.e = e;
                epj.src = src;
                epj.src_size = src_size;
                epj.block_size = block_size;
                epj.block_dict = block_dict;
                epj.cand = cand;
                epj.cand_len = cand_len;
                epj.id = next_dict_id;
                epj.begin = ep_start;
                zgec_enc_parallel_for(params.n_threads, ep_end - ep_start,
                                      zgec_enc_epoch_block, &epj);
            }
            /* A dictionary no block of the epoch references never gets a
             * DICT record, so counting it or emitting its footer entry
             * would produce a frame with a zero-id dictionary entry that
             * the decoder rejects. Drop it completely (5.5). */
            {
                int used = 0;
                for (t = ep_start; t < ep_end; t++) {
                    if (block_dict[t] == next_dict_id) {
                        used = 1;
                        break;
                    }
                }
                if (!used) {
                    zgec_free(dict_bytes[i]);
                    dict_bytes[i] = NULL;
                    dict_lens[i] = 0;
                    epoch_dict_id[i] = 0;
                    dict_count--;
                    next_dict_id--;
                }
            }
        }
    }
    /* use_dicts == 0 (default): every block_dict stays 0 and no DICT
     * records are emitted, so the default path is unchanged. */

    /* ---- block pass (11.8) ----
     * Phase A (parallel): every block's independent work. Phase B
     * (serial, in file order): the literal-reference chain of 6.3 and
     * the RAW fallback. */
    if (zgec_add_overflows(n_blocks, (size_t)dict_count + 1)) return ZGEC_ERR_NOMEM;
    recs_cap = n_blocks + (size_t)dict_count + 1;
    if (recs_cap > SIZE_MAX / sizeof(*recs)) return ZGEC_ERR_NOMEM;
    if (n_blocks > SIZE_MAX / sizeof(*jobs)) return ZGEC_ERR_NOMEM;
    recs = (zgec_enc_record *)zgec_alloc(recs_cap * sizeof(*recs),
                                        _Alignof(zgec_enc_record));
    jobs = (zgec_enc_blockjob *)zgec_alloc(n_blocks * sizeof(*jobs),
                                          _Alignof(zgec_enc_blockjob));
    if (!recs || !jobs) {
        zgec_free(recs);
        zgec_free(jobs);
        zgec_free(block_dict);
        if (dict_bytes) {
            for (i = 0; i < n_epochs; i++) zgec_free(dict_bytes[i]);
        }
        zgec_free(dict_bytes);
        zgec_free(dict_lens);
        zgec_free(epoch_dict_id);
        return ZGEC_ERR_NOMEM;
    }
    memset(recs, 0, recs_cap * sizeof(*recs));
    memset(jobs, 0, n_blocks * sizeof(*jobs));
    memset(lr_buf, 0, sizeof(lr_buf));
    memset(lr_len, 0, sizeof(lr_len));

    /* Resolve each block's dictionary bytes before phase A. An
     * unresolvable id becomes "no dictionary" so the block is still
     * emitted (the id spaces cannot collide, so this is defensive). */
    for (i = 0; i < n_blocks; i++) {
        size_t bo = i * block_size;
        size_t bs = src_size - bo;
        uint16_t did = block_dict[i];
        if (bs > block_size) bs = block_size;
        jobs[i].index = (uint32_t)i;
        jobs[i].src = src + bo;
        jobs[i].raw_size = bs;
        if (did != 0) {
            size_t k;
            int found = 0;
            for (k = 0; k < n_ext; k++) {
                if (ext_ids[k] == did) {
                    jobs[i].dict = ext_data[k];
                    jobs[i].dict_len = ext_sizes[k];
                    jobs[i].external = 1;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                size_t ep = i / (size_t)params.epoch_blocks;
                if (ep < n_epochs && epoch_dict_id &&
                    epoch_dict_id[ep] == did && dict_bytes && dict_bytes[ep]) {
                    jobs[i].dict = dict_bytes[ep];
                    jobs[i].dict_len = dict_lens[ep];
                } else {
                    block_dict[i] = 0;
                }
            }
        }
    }
    {
        zgec_enc_blockpass bpx;
        bpx.e = e;
        bpx.jobs = jobs;
        zgec_enc_parallel_for(params.n_threads, n_blocks,
                              zgec_enc_block_phaseA, &bpx);
    }
    /* M1: without literal references no later block needs any phase-A
     * literal buffer, so release them all before the serial pass
     * instead of holding every block's literals until its turn. */
    if (!params.use_litref) {
        for (i = 0; i < n_blocks; i++) {
            zgec_free(jobs[i].r.lit);
            jobs[i].r.lit = NULL;
            jobs[i].r.lit_size = 0;
        }
    }

    for (i = 0; i < n_blocks; i++) {
        size_t bo = i * block_size;
        size_t bs = src_size - bo;
        size_t dict_len = jobs[i].dict_len;
        uint16_t did = block_dict[i];
        uint8_t *payload = NULL;
        size_t psize = 0;
        uint32_t segcount = 0;
        zgec_enc_record *rec = NULL;
        int filtered = 0;
        int exportable = 0;
        int depth = 0;
        size_t lr_region_len = 0;
        size_t lr_use = 0;

        if (bs > block_size) bs = block_size;
        if (jobs[i].err != ZGEC_OK) {
            err = jobs[i].err;
            goto frame_fail_out;
        }
        /* Resolve this block's dictionary bytes. D2: no DICT record is
         * emitted here: an RLE block ignores the dictionary in phase A
         * and a COMPRESSED candidate may still fall back to RAW, so a
         * DICT emitted now could end up unreferenced (bytes + footer
         * entry, and dict_len wrongly tightening the LITREF bound).
         * The record is emitted lazily below, only for a COMPRESSED
         * record that actually references the dictionary. An external
         * dictionary is never stored in the frame (5.8). */
        if (did != 0 && !jobs[i].external) {
            size_t ep = i / (size_t)params.epoch_blocks;
            if (ep < n_epochs && epoch_dict_id && epoch_dict_id[ep] == did &&
                dict_bytes && dict_bytes[ep]) {
                dict_len = dict_lens[ep];
            } else {
                /* Fallback: unknown dict id (should not happen); treat
                 * as no dictionary rather than emitting garbage. */
                did = 0;
                block_dict[i] = 0;
                dict_len = 0;
            }
        }
        if (did != 0 && !jobs[i].external &&
            jobs[i].kind == ZGEC_REC_RLE) {
            /* Phase A ignores dictionaries for one-byte runs: drop the
             * id so no unreferenced DICT is emitted for this block. */
            did = 0;
            block_dict[i] = 0;
            dict_len = 0;
        }

        /* ---- LITREF region of the exportable predecessors (6.3) ----
         * Its LITREF region is the concatenation, oldest first, of the
         * literal buffers of blocks b-D .. b-1, so D may not exceed the
         * length of the unbroken run of exportable predecessors. The
         * region is also bounded so that dict + litref + block stays
         * inside the P24 position limit; D shrinks rather than failing.
         * M4: lr_region is maintained incrementally (append/reset/drop-
         * prefix) as the run evolves, so per block only the P24-bound
         * suffix is referenced -- no O(depth) copy per block. */
        const uint8_t *lr_use_ptr = NULL;
        if (params.use_litref && lr_n > 0 && lr_region) {
            size_t prefix = dict_len;
            size_t d;
            size_t sum = 0;
            size_t q;
            /* lr_buf/lr_len are newest first (index 0 is block b-1), and
             * 6.3 defines the region as blocks b-D .. b-1, so candidates
             * are taken from the newest end: index d is block b-1-d. D
             * shrinks (never to a different, non-contiguous set) when the
             * P24 position bound is reached. */
            for (d = 0; d < lr_n; d++) {
                size_t cand = lr_len[d];
                if (prefix + cand + bs > (size_t)(1u << 24)) break;
                prefix += cand;
            }
            for (q = 0; q < d; q++) sum += lr_len[q];
            if (d > 0 && sum <= lr_total) {
                lr_use_ptr = lr_region + (lr_total - sum);
                lr_region_len = sum;
                lr_use = d;
            }
        }

        /* Phase A already decided the one-byte-run (RLE) case. */
        if (jobs[i].kind == ZGEC_REC_RLE) {
            payload = jobs[i].r.payload;
            jobs[i].r.payload = NULL;
            rec = &recs[n_recs++];
            memset(&rec->hdr, 0, sizeof(rec->hdr));
            rec->hdr.record_type = ZGEC_REC_RLE;
            rec->hdr.raw_size = (uint32_t)bs;
            rec->hdr.payload_size = 1;
            rec->payload = payload;
        } else {
            zgec_block_result r0 = jobs[i].r;
            zgec_block_result r1;
            zgec_block_result_init(&jobs[i].r);
            zgec_block_result_init(&r1);
            /* Literal references are enabled only when a measurement on
             * the block shows a gain (6.3): encode again with the LITREF
             * region and keep the smaller payload. P4: this full
             * re-encode (parse+emit with the region) is the measurement
             * itself, so worst case it ~doubles the block-pass cost;
             * approximating with the phase-A parse/histograms would risk
             * ratio, since matches change with the region. */
            if (lr_region_len > 0 && lr_use_ptr && !r0.filtered) {
                err = zgec_encode_block_full(e, jobs[i].src, bs, (uint32_t)i,
                                             jobs[i].dict, jobs[i].dict_len,
                                             lr_use_ptr, lr_region_len, &r1);
                if (err != ZGEC_OK) {
                    zgec_free(r0.payload);
                    zgec_free(r0.lit);
                    goto frame_fail_out;
                }
                if (r1.payload_size < r0.payload_size) {
                    zgec_free(r0.payload);
                    zgec_free(r0.lit);
                    r0 = r1;
                    zgec_block_result_init(&r1);
                    depth = (int)lr_use;
                } else {
                    zgec_free(r1.payload);
                    zgec_free(r1.lit);
                    zgec_block_result_init(&r1);
                }
            }
            payload = r0.payload;
            psize = r0.payload_size;
            segcount = r0.segment_count;
            filtered = r0.filtered;
            exportable = r0.exportable;
            r0.payload = NULL;
            /* RAW if incompressible: comp >= raw. A filtered block that
             * did not shrink also falls back to RAW without the filter
             * (7.5, 11.10). D2: a RAW block references no dictionary,
             * so the id is dropped (no DICT is emitted for it below). */
            if (psize >= bs) {
                uint8_t *rp = (uint8_t *)zgec_alloc(bs ? bs : 1, 64);
                if (!rp) {
                    zgec_free(payload);
                    zgec_free(r0.lit);
                    err = ZGEC_ERR_NOMEM;
                    goto frame_fail_out;
                }
                if (bs > 0) memcpy(rp, jobs[i].src, bs);
                zgec_free(payload);
                payload = rp;
                psize = bs;
                segcount = 0;
                exportable = 0;
                depth = 0;
                did = 0;
                block_dict[i] = 0;
                rec = &recs[n_recs++];
                memset(&rec->hdr, 0, sizeof(rec->hdr));
                rec->hdr.record_type = ZGEC_REC_RAW;
                rec->hdr.raw_size = (uint32_t)bs;
                rec->hdr.payload_size = (uint32_t)bs;
                rec->payload = payload;
            } else {
                /* D2: epoch dictionaries keep their bytes in the frame
                 * and need a DICT record before their first user -- but
                 * only for a COMPRESSED record that actually references
                 * the dictionary (5.5: every DICT is referenced). */
                if (did != 0 && !jobs[i].external) {
                    size_t ep = i / (size_t)params.epoch_blocks;
                    if (ep < n_epochs && epoch_dict_id &&
                        epoch_dict_id[ep] == did && dict_bytes &&
                        dict_bytes[ep]) {
                        int already = 0;
                        size_t r;
                        for (r = 0; r < n_recs; r++) {
                            if (recs[r].hdr.record_type == ZGEC_REC_DICT &&
                                recs[r].hdr.dict_id == did) {
                                already = 1;
                                break;
                            }
                        }
                        if (!already) {
                            uint8_t *dp = NULL;
                            size_t dl = dict_lens[ep];
                            zgec_record_header inner;
                            dp = (uint8_t *)zgec_alloc(24 + dl, 64);
                            if (!dp) {
                                zgec_free(payload);
                                zgec_free(r0.lit);
                                err = ZGEC_ERR_NOMEM;
                                goto frame_fail_out;
                            }
                            /* Inner RAW record holding the dictionary
                             * bytes: dict_id 0, lit_ref_depth 0 (never
                             * depends). */
                            memset(&inner, 0, sizeof(inner));
                            inner.record_type = ZGEC_REC_RAW;
                            inner.raw_size = (uint32_t)dl;
                            inner.payload_size = (uint32_t)dl;
                            zgec_record_header_emit(dp, &inner);
                            memcpy(dp + 24, dict_bytes[ep], dl);
                            rec = &recs[n_recs++];
                            memset(&rec->hdr, 0, sizeof(rec->hdr));
                            rec->hdr.record_type = ZGEC_REC_DICT;
                            rec->hdr.dict_id = did;
                            rec->hdr.raw_size = (uint32_t)dl;
                            rec->hdr.payload_size = (uint32_t)(24 + dl);
                            rec->payload = dp;
                            rec = NULL;
                        }
                    } else {
                        did = 0;
                        block_dict[i] = 0;
                    }
                }
                rec = &recs[n_recs++];
                memset(&rec->hdr, 0, sizeof(rec->hdr));
                rec->hdr.record_type = ZGEC_REC_COMPRESSED;
                rec->hdr.raw_size = (uint32_t)bs;
                rec->hdr.payload_size = (uint32_t)psize;
                rec->hdr.segment_count = segcount;
                rec->hdr.dict_id = did;
                rec->hdr.lit_ref_depth = (uint8_t)depth;
                rec->payload = payload;
                /* FILTERED (7.5) or, when plain and unconditioned,
                 * LIT_EXPORTABLE (6.3). A FILTERED block MUST NOT set
                 * LIT_EXPORTABLE (V11). */
                if (filtered) {
                    rec->hdr.rflags = (uint8_t)((unsigned)rec->hdr.rflags |
                                               ZGEC_RFLAG_FILTERED);
                } else if (exportable) {
                    rec->hdr.rflags = (uint8_t)((unsigned)rec->hdr.rflags |
                                               ZGEC_RFLAG_LIT_EXPORTABLE);
                }
            }
            /* Keep this block's literal buffer for the next block only
             * while the record is exportable and not FILTERED, and only
             * when literal references can use it at all (6.3). M4: the
             * running region mirrors the run (oldest first): append the
             * newest block, drop the evicted oldest prefix, or reset. */
            if (params.use_litref) {
                if (lr_n == (size_t)ZGEC_MAX_LITREF_DEPTH) {
                    size_t drop = lr_len[lr_n - 1];
                    zgec_free(lr_buf[lr_n - 1]);
                    lr_buf[lr_n - 1] = NULL;
                    lr_len[lr_n - 1] = 0;
                    lr_n--;
                    if (drop > 0 && drop <= lr_total) {
                        memmove(lr_region, lr_region + drop,
                                lr_total - drop);
                        lr_total -= drop;
                    } else if (drop > 0) {
                        lr_total = 0; /* defensive: never negative */
                    }
                }
                if (exportable && r0.lit_size > 0) {
                    size_t q;
                    for (q = lr_n; q > 0; q--) {
                        lr_buf[q] = lr_buf[q - 1];
                        lr_len[q] = lr_len[q - 1];
                    }
                    if (lr_total + r0.lit_size > lr_cap) {
                        size_t ncap = lr_cap ? lr_cap : 256;
                        uint8_t *nb;
                        while (ncap < lr_total + r0.lit_size) {
                            if (ncap > (size_t)1 << 30) {
                                ncap = lr_total + r0.lit_size;
                                break;
                            }
                            ncap *= 2;
                        }
                        nb = (uint8_t *)zgec_alloc(ncap, 64);
                        if (!nb) {
                            zgec_free(r0.lit);
                            r0.lit = NULL;
                            err = ZGEC_ERR_NOMEM;
                            goto frame_fail_out;
                        }
                        if (lr_total > 0) memcpy(nb, lr_region, lr_total);
                        zgec_free(lr_region);
                        lr_region = nb;
                        lr_cap = ncap;
                    }
                    memcpy(lr_region + lr_total, r0.lit, r0.lit_size);
                    lr_total += r0.lit_size;
                    lr_buf[0] = r0.lit;
                    lr_len[0] = r0.lit_size;
                    r0.lit = NULL;
                    lr_n++;
                } else {
                    size_t q;
                    for (q = 0; q < lr_n; q++) {
                        zgec_free(lr_buf[q]);
                        lr_buf[q] = NULL;
                        lr_len[q] = 0;
                    }
                    lr_n = 0;
                    lr_total = 0; /* keep the buffer for the next run */
                }
            }
            zgec_free(r0.lit);
        }
        /* A RAW/RLE block is not literal-exportable and breaks the run. */
        if (rec != NULL && rec->hdr.record_type != ZGEC_REC_COMPRESSED) {
            size_t q;
            for (q = 0; q < lr_n; q++) {
                zgec_free(lr_buf[q]);
                lr_buf[q] = NULL;
                lr_len[q] = 0;
            }
            lr_n = 0;
            lr_total = 0;
        }
        /* Block checksums (rflags HAS_CHECKSUM + CRC32C). */
        if (params.block_checksums) {
            rec->hdr.rflags = (uint8_t)((unsigned)rec->hdr.rflags |
                                       ZGEC_RFLAG_HAS_CHECKSUM);
            rec->hdr.checksum = zgec_crc32c(jobs[i].src, bs, 0u);
        }
        continue;
    }
    {
        size_t q;
        for (q = 0; q < lr_n; q++) {
            zgec_free(lr_buf[q]);
            lr_buf[q] = NULL;
        }
        lr_n = 0;
        zgec_free(lr_region);
        lr_region = NULL;
        /* Release any phase-A result that the serial pass did not use
         * (an early error path leaves the rest untouched). */
        for (q = 0; q < n_blocks; q++) {
            zgec_free(jobs[q].r.payload);
            zgec_free(jobs[q].r.lit);
            jobs[q].r.payload = NULL;
            jobs[q].r.lit = NULL;
        }
        zgec_free(jobs);
        jobs = NULL;
    }

    /* Frame flags EXTENDED_LITREF if any lit_ref_depth > 0. */
    {
        size_t r;
        for (r = 0; r < n_recs; r++) {
            if (recs[r].hdr.lit_ref_depth > 0) {
                fh.flags = (uint16_t)((unsigned)fh.flags |
                                     ZGEC_FLAG_EXTENDED_LITREF);
                break;
            }
        }
    }

    /* D2: only dictionaries with an actual DICT record get footer
     * entries. A trained dictionary whose blocks all came out RLE/RAW
     * has no record (lazy emission above), so counting it would leave
     * a zero-filled footer entry the decoder rejects. */
    {
        size_t r;
        uint32_t n_dict_recs = 0;
        for (r = 0; r < n_recs; r++) {
            if (recs[r].hdr.record_type == ZGEC_REC_DICT) n_dict_recs++;
        }
        dict_total = n_dict_recs + (uint32_t)n_ext;
    }

    /* ---- assemble: header + records + footer + trailer ---- */
    {
        zgec_footer footer;
        size_t footer_size = zgec_footer_size((uint32_t)n_blocks, dict_total);
        size_t frame_size = 32;
        uint8_t *frame = NULL;
        size_t off;
        size_t r;
        size_t bi;
        size_t e5;

        for (r = 0; r < n_recs; r++) {
            size_t rec = 24 + (size_t)recs[r].hdr.payload_size;
            if (rec < 24 || zgec_add_overflows(frame_size, rec)) {
                err = ZGEC_ERR_INTERNAL;
                goto frame_fail_out;
            }
            frame_size += rec;
        }
        if (zgec_add_overflows(frame_size, footer_size + 16)) {
            err = ZGEC_ERR_INTERNAL;
            goto frame_fail_out;
        }
        frame_size += footer_size + 16;
        frame = (uint8_t *)zgec_alloc(frame_size, 64);
        if (!frame) {
            err = ZGEC_ERR_NOMEM;
            goto frame_fail_out;
        }
        zgec_frame_header_emit(frame, &fh);

        memset(&footer, 0, sizeof(footer));
        footer.block_count = (uint32_t)n_blocks;
        footer.dict_count = dict_total;
        footer.content_size = (uint64_t)src_size;
        if (n_blocks > 0) {
            footer.blocks = (zgec_footer_block_entry *)zgec_alloc(
                n_blocks * sizeof(zgec_footer_block_entry),
                _Alignof(zgec_footer_block_entry));
            if (!footer.blocks) {
                zgec_free(frame);
                err = ZGEC_ERR_NOMEM;
                goto frame_fail_out;
            }
            memset(footer.blocks, 0,
                   n_blocks * sizeof(zgec_footer_block_entry));
        }
        if (dict_total > 0) {
            footer.dicts = (zgec_footer_dict_entry *)zgec_alloc(
                (size_t)dict_total * sizeof(zgec_footer_dict_entry),
                _Alignof(zgec_footer_dict_entry));
            if (!footer.dicts) {
                zgec_free(footer.blocks);
                zgec_free(frame);
                err = ZGEC_ERR_NOMEM;
                goto frame_fail_out;
            }
            memset(footer.dicts, 0,
                   (size_t)dict_total * sizeof(zgec_footer_dict_entry));
        }

        off = 32;
        bi = 0;
        {
            uint32_t di = 0;
            for (r = 0; r < n_recs; r++) {
                zgec_record_header_emit(frame + off, &recs[r].hdr);
                if (recs[r].hdr.payload_size > 0) {
                    memcpy(frame + off + 24, recs[r].payload,
                           (size_t)recs[r].hdr.payload_size);
                }
                /* M1: release each record payload as it lands in the
                 * frame, so peak is ~frame + remaining records rather
                 * than ~2x the compressed size. */
                zgec_free(recs[r].payload);
                recs[r].payload = NULL;
                if (recs[r].hdr.record_type == ZGEC_REC_DICT) {
                    /* Footer dictionary entry (embedded). */
                    size_t ep;
                    uint64_t hash = 0;
                    for (ep = 0; ep < n_epochs; ep++) {
                        if (epoch_dict_id && epoch_dict_id[ep] ==
                                recs[r].hdr.dict_id) {
                            hash = zgec_xxh64(dict_bytes[ep], dict_lens[ep],
                                              0);
                            break;
                        }
                    }
                    footer.dicts[di].dict_id = recs[r].hdr.dict_id;
                    footer.dicts[di].kind = 0;
                    footer.dicts[di].reserved = 0;
                    footer.dicts[di].raw_size = recs[r].hdr.raw_size;
                    footer.dicts[di].offset = (uint64_t)off;
                    footer.dicts[di].content_hash = hash;
                    di++;
                } else {
                    footer.blocks[bi].offset = (uint64_t)off;
                    footer.blocks[bi].record_size =
                        (uint32_t)(24 + (size_t)recs[r].hdr.payload_size);
                    footer.blocks[bi].dict_id = recs[r].hdr.dict_id;
                    footer.blocks[bi].lit_ref_depth =
                        recs[r].hdr.lit_ref_depth;
                    footer.blocks[bi].rflags = recs[r].hdr.rflags;
                    bi++;
                }
                off += 24 + (size_t)recs[r].hdr.payload_size;
            }
            /* External dictionary entries (kind 1, no stored offset). */
            for (e5 = 0; e5 < n_ext; e5++) {
                footer.dicts[di].dict_id = ext_ids[e5];
                footer.dicts[di].kind = 1;
                footer.dicts[di].reserved = 0;
                footer.dicts[di].raw_size = (uint32_t)ext_sizes[e5];
                footer.dicts[di].offset = 0;
                footer.dicts[di].content_hash =
                    zgec_xxh64(ext_data[e5], ext_sizes[e5], 0);
                di++;
            }
        }
        {
            /* Spec 4.5: footer_offset is the offset of the footer
             * from the start of the frame, i.e. before it is written,
             * not the offset of the trailer. */
            size_t footer_off = off;
            size_t fsz = zgec_footer_emit(frame + off, &footer);
            zgec_free(footer.blocks);
            zgec_free(footer.dicts);
            if (fsz != footer_size) {
                zgec_free(frame);
                err = ZGEC_ERR_INTERNAL;
                goto frame_fail_out;
            }
            off += fsz;
            {
                zgec_trailer tr;
                tr.footer_offset = (uint64_t)footer_off;
                tr.footer_size = (uint32_t)fsz;
                zgec_trailer_emit(frame + off, &tr);
            }
        }
        for (r = 0; r < n_recs; r++) zgec_free(recs[r].payload);
        zgec_free(recs);
        zgec_free(block_dict);
        if (dict_bytes) {
            for (i = 0; i < n_epochs; i++) zgec_free(dict_bytes[i]);
        }
        zgec_free(dict_bytes);
        zgec_free(dict_lens);
        zgec_free(epoch_dict_id);
        *dst = frame;
        if (dst_size) *dst_size = frame_size;
        return ZGEC_OK;
    }

frame_fail_out:
    {
        size_t q;
        for (q = 0; q < (size_t)ZGEC_MAX_LITREF_DEPTH; q++) zgec_free(lr_buf[q]);
        zgec_free(lr_region);
    }
    if (jobs) {
        size_t q;
        for (q = 0; q < n_blocks; q++) {
            zgec_free(jobs[q].r.payload);
            zgec_free(jobs[q].r.lit);
        }
        zgec_free(jobs);
    }
    if (recs) {
        size_t r;
        for (r = 0; r < n_recs; r++) zgec_free(recs[r].payload);
        zgec_free(recs);
    }
    zgec_free(block_dict);
    if (dict_bytes) {
        for (i = 0; i < n_epochs; i++) zgec_free(dict_bytes[i]);
    }
    zgec_free(dict_bytes);
    zgec_free(dict_lens);
    zgec_free(epoch_dict_id);
    return err;
}
