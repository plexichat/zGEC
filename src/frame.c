#include "zgec_frame.h"

#include "zgec_crc32c.h"

/* Block size in bytes: 2^block_log2 (callers validate range 16..26 first). */
static inline uint32_t zgec_frame_block_size(const zgec_frame_header *fh)
{
    return 1u << fh->block_log2;
}

void zgec_frame_header_emit(uint8_t *buf, const zgec_frame_header *h)
{
    /* Review entry 8: the signature carries no destination capacity and
       returns nothing, so it cannot report an error; a NULL pointer must
       still not be dereferenced. */
    if (buf == NULL || h == NULL) return;
    zgec_wr32(buf, ZGEC_MAGIC_U32);
    buf[4] = h->version_major;
    buf[5] = h->version_minor;
    zgec_wr16(buf + 6, h->flags);
    buf[8] = h->block_log2;
    buf[9] = h->epoch_blocks;
    buf[10] = h->max_dict_log2;
    buf[11] = h->seg_hint_log2;
    zgec_wr64(buf + 12, h->content_size);
    zgec_wr32(buf + 20, h->block_count);
    zgec_wr32(buf + 24, 0u);
    zgec_wr32(buf + 28, zgec_crc32c(buf, 28u, 0u));
}

zgec_err zgec_frame_header_parse(zgec_frame_header *h, const uint8_t *buf)
{
    if (h == NULL || buf == NULL) return ZGEC_ERR_INVAL;
    if (zgec_rd32(buf) != ZGEC_MAGIC_U32) return ZGEC_ERR_MAGIC;
    /* Review entry 9: verify the header CRC before interpreting any field. A
       corrupt header is then rejected without the version, flag and range
       checks and without a partial fill of *h, which does less work on random
       data. Bytes 0..27 are untouched by the reorder, so every caller sees
       the same accept/reject decision as before.

       Contract note, because the reorder changed it: *h is modified only on
       the success path. At baseline a CRC failure returned after all eleven
       fields had been written, so a caller reading *h on error saw the corrupt
       parsed values; now it reads back exactly what it passed in. In-tree
       callers never read *h after a failure (src/decode.c:2815 returns the
       error immediately, tests/test.c:481 returns 0), which is why no test
       pins either behaviour. The same note belongs on the declaration in
       include/zgec_frame.h for external callers; that header is outside this
       slice. */
    if (zgec_crc32c(buf, 28u, 0u) != zgec_rd32(buf + 28))
        return ZGEC_ERR_HEADER_CRC;
    h->version_major = buf[4];
    /* Spec 4.1/14: reject a larger major version. Minor versions only add
       features gated by flag bits, which are rejected below if unknown. */
    if (h->version_major == 0u || h->version_major > ZGEC_VERSION_MAJOR)
        return ZGEC_ERR_VERSION;
    h->version_minor = buf[5];
    h->flags = zgec_rd16(buf + 6);
    if ((h->flags & ~ZGEC_FLAG_ALL_KNOWN) != 0u) return ZGEC_ERR_FLAGS;
    h->block_log2 = buf[8];
    h->epoch_blocks = buf[9];
    h->max_dict_log2 = buf[10];
    h->seg_hint_log2 = buf[11];
    h->content_size = zgec_rd64(buf + 12);
    h->block_count = zgec_rd32(buf + 20);
    if (zgec_rd32(buf + 24) != 0u) return ZGEC_ERR_RESERVED;
    if (h->block_log2 < ZGEC_BLOCK_LOG2_MIN ||
        h->block_log2 > ZGEC_BLOCK_LOG2_MAX) return ZGEC_ERR_BLOCK_SIZE;
    /* Review entry 10: max_dict_log2 out of range is a dictionary-size fault
       and seg_hint_log2 is a segment-size hint (spec 4.1), so report the
       specific existing code and leave ZGEC_ERR_BLOCK_SIZE for block_log2,
       which is the only one of the three that is actually a block size. */
    if (h->max_dict_log2 > ZGEC_MAX_DICT_LOG2) return ZGEC_ERR_DICT_SIZE;
    if (h->seg_hint_log2 < ZGEC_SEG_HINT_LOG2_MIN ||
        h->seg_hint_log2 > ZGEC_SEG_HINT_LOG2_MAX) return ZGEC_ERR_SEGMENT_SIZE;
    return ZGEC_OK;
}

void zgec_record_header_emit(uint8_t *buf, const zgec_record_header *r)
{
    if (buf == NULL || r == NULL) return; /* review entry 8 */
    buf[0] = r->record_type;
    buf[1] = r->rflags;
    zgec_wr16(buf + 2, r->dict_id);
    zgec_wr32(buf + 4, r->raw_size);
    zgec_wr32(buf + 8, r->payload_size);
    zgec_wr32(buf + 12, r->segment_count);
    zgec_wr32(buf + 16, r->checksum);
    buf[20] = r->lit_ref_depth;
    buf[21] = 0u;
    buf[22] = 0u;
    buf[23] = 0u;
}

zgec_err zgec_record_header_parse(zgec_record_header *r, const uint8_t *buf,
                                  const zgec_frame_header *fh)
{
    if (r == NULL || buf == NULL || fh == NULL) return ZGEC_ERR_INVAL;
    r->record_type = buf[0];
    r->rflags = buf[1];
    if ((r->rflags & ~ZGEC_RFLAG_ALL_KNOWN) != 0u) return ZGEC_ERR_RESERVED;
    /* Spec 7.5/V11: FILTERED is defined for COMPRESSED blocks only; a
     * DICT record (with its RAW/RLE inner record) may never set it. The
     * inner COMPRESSED record's own FILTERED is rejected in dict.c. */
    if ((r->rflags & ZGEC_RFLAG_FILTERED) != 0u &&
        r->record_type != ZGEC_REC_COMPRESSED) {
        return ZGEC_ERR_RESERVED;
    }
    r->dict_id = zgec_rd16(buf + 2);
    r->raw_size = zgec_rd32(buf + 4);
    r->payload_size = zgec_rd32(buf + 8);
    r->segment_count = zgec_rd32(buf + 12);
    r->checksum = zgec_rd32(buf + 16);
    r->lit_ref_depth = buf[20];
    if (buf[21] != 0u || buf[22] != 0u || buf[23] != 0u) return ZGEC_ERR_RESERVED;
    if (r->lit_ref_depth > ZGEC_MAX_LITREF_DEPTH) return ZGEC_ERR_LITREF_DEPTH;
    if (r->record_type > ZGEC_REC_DICT && r->record_type < ZGEC_REC_SKIPPABLE)
        return ZGEC_ERR_RECORD_TYPE;
    if (fh->block_log2 < ZGEC_BLOCK_LOG2_MIN ||
        fh->block_log2 > ZGEC_BLOCK_LOG2_MAX) return ZGEC_ERR_BLOCK_SIZE;
    if (r->record_type <= ZGEC_REC_RLE) {
        /* Spec 4.2: 1 <= raw_size <= 2^block_log2 for every block. Whether a
           non-final block equals 2^block_log2 exactly (only the final block
           may be shorter) needs the block's index in the frame, which this
           24-byte header parser does not know; decode.c enforces that
           final-block rule where the frame position is known. */
        if (r->raw_size == 0u || r->raw_size > zgec_frame_block_size(fh))
            return ZGEC_ERR_BLOCK_SIZE;
    }
    if (r->record_type == ZGEC_REC_DICT) {
        /* Spec 3.3/4.1: a dictionary is at most 2^max_dict_log2 bytes
           (and never more than the format limit 2^26). */
        uint64_t dcap = (fh->max_dict_log2 == 0u)
                            ? ((uint64_t)1 << ZGEC_MAX_DICT_LOG2)
                            : ((uint64_t)1 << fh->max_dict_log2);
        if (r->dict_id == 0u) return ZGEC_ERR_DICT_ID;
        if ((uint64_t)r->raw_size > dcap) return ZGEC_ERR_DICT_SIZE;
    }
    if (r->record_type == ZGEC_REC_COMPRESSED) {
        if (r->segment_count < 1u || r->segment_count > ZGEC_MAX_SEGMENTS)
            return ZGEC_ERR_SEGMENT_COUNT;
    } else if (r->segment_count != 0u) {
        return ZGEC_ERR_SEGMENT_COUNT;
    }
    return ZGEC_OK;
}

/* Spec 4.2/4.4: the invariants a block index entry must satisfy. The footer
 * parser and the footer emitter share this one definition so that both sides
 * apply one definition of the *index* invariants, which is not the same as a
 * valid frame -- see the note on zgec_footer_emit() for what a 16-byte index
 * entry cannot express (review entries 6 and 8). `prev` is the preceding
 * entry, or NULL for the first one. */
static zgec_err zgec_footer_block_check(const zgec_footer_block_entry *e,
                                        const zgec_footer_block_entry *prev)
{
    if (e->lit_ref_depth > ZGEC_MAX_LITREF_DEPTH) return ZGEC_ERR_LITREF_DEPTH;
    if ((e->rflags & (uint8_t)~ZGEC_RFLAG_ALL_KNOWN) != 0u)
        return ZGEC_ERR_RESERVED;
    /* Spec 7.5/V11: FILTERED and a literal-reference duty are mutually
       exclusive -- "a FILTERED block MUST have lit_ref_depth 0 and MUST NOT
       set LIT_EXPORTABLE". Both fields live in this index entry, so the
       combination is decidable here instead of only from the record header a
       layer further in. The encoder cannot trip it: the LITREF re-encode is
       skipped outright for a filtered block, so its depth stays 0
       (src/encode.c:3604), and the two flags are set as an if/else
       (src/encode.c:3724-3727). */
    if ((e->rflags & ZGEC_RFLAG_FILTERED) != 0u &&
        (e->lit_ref_depth != 0u ||
         (e->rflags & ZGEC_RFLAG_LIT_EXPORTABLE) != 0u)) {
        return ZGEC_ERR_RESERVED;
    }
    /* The offset is the record header's offset from the start of the frame, so
       it can never fall inside the 32-byte frame header (spec 4.1, 4.7). */
    if (e->offset < 32u) return ZGEC_ERR_OFFSET;
    /* record_size is 24 + payload_size (spec 4.2). */
    if (e->record_size < 24u) return ZGEC_ERR_RECORD_SIZE;
    /* Bound the offset + record_size range later stages compute. */
    if (e->offset > UINT64_MAX - (uint64_t)e->record_size)
        return ZGEC_ERR_OFFSET;
    /* Entry b describes block b in file order (spec 4.4) and every record is at
       least 24 bytes, so the offsets strictly increase. */
    if (prev != NULL && e->offset <= prev->offset) return ZGEC_ERR_OFFSET;
    return ZGEC_OK;
}

/* Spec 4.4: dictionary entry invariants. Embedded entries carry the offset of
 * the DICT record header, external entries carry zero. Shared by the parser
 * and the emitter. */
static zgec_err zgec_footer_dict_check(const zgec_footer_dict_entry *e)
{
    if (e->reserved != 0u) return ZGEC_ERR_RESERVED;
    /* kind is a mode field (0 embedded, 1 external) and dict_id is an
       identity, so report them separately: a bad mode is a value this format
       does not define, the same fault class the flag-mask check and
       src/dict.c:1165 report as ZGEC_ERR_RESERVED, whereas a bad identity is
       specifically ZGEC_ERR_DICT_ID. Collapsing the two (as the baseline did)
       reports an undefined mode as an identity fault. */
    if (e->kind > 1u) return ZGEC_ERR_RESERVED;
    if (e->dict_id == 0u) return ZGEC_ERR_DICT_ID;
    if (e->kind == 1u) {
        if (e->offset != 0u) return ZGEC_ERR_OFFSET;
    } else if (e->offset < 32u) {
        return ZGEC_ERR_OFFSET;
    }
    return ZGEC_OK;
}

size_t zgec_footer_size(uint32_t block_count, uint32_t dict_count)
{
    size_t b = (size_t)block_count;
    size_t d = (size_t)dict_count;
    if (b > (SIZE_MAX - 24u) / 16u) return 0;
    if (d > (SIZE_MAX - 24u - 16u * b) / 24u) return 0;
    return 24u + 16u * b + 24u * d;
}

zgec_err zgec_footer_parse(zgec_footer *f, const uint8_t *buf, size_t size)
{
    zgec_footer_block_entry *blocks = NULL;
    zgec_footer_dict_entry *dicts = NULL;
    zgec_err err = ZGEC_OK; /* only read on the `fail` path, which sets it */

    if (f == NULL || buf == NULL) return ZGEC_ERR_INVAL;
    if (size < 24u) return ZGEC_ERR_TRUNCATED;
    if (zgec_rd32(buf) != ZGEC_FOOTER_MAGIC_U32) return ZGEC_ERR_MAGIC;
    uint32_t block_count = zgec_rd32(buf + 4);
    uint32_t dict_count = zgec_rd32(buf + 8);
    uint64_t content_size = zgec_rd64(buf + 12);
    /* Spec 13: bound the counts from the caller-supplied size before any
       allocation arithmetic (review entry 5). A block entry is 16 bytes and a
       dictionary entry 24, so a count that cannot fit in `size` is
       incoherent; the exact-size test below is then the tighter bound, which
       is what really pins the counts to the buffer the caller handed over. */
    if ((uint64_t)block_count > ((uint64_t)size - 24u) / 16u)
        return ZGEC_ERR_TRUNCATED;
    if ((uint64_t)dict_count > ((uint64_t)size - 24u) / 24u)
        return ZGEC_ERR_TRUNCATED;
    size_t need = zgec_footer_size(block_count, dict_count);
    if (need == 0 || need != size) return ZGEC_ERR_TRUNCATED;
    if (zgec_crc32c(buf, size - 4u, 0u) != zgec_rd32(buf + size - 4u))
        return ZGEC_ERR_FOOTER_CRC;

    if (block_count > 0u) {
        blocks = (zgec_footer_block_entry *)zgec_alloc(
            (size_t)block_count * sizeof(zgec_footer_block_entry),
            _Alignof(zgec_footer_block_entry));
        if (blocks == NULL) return ZGEC_ERR_NOMEM;
    }
    if (dict_count > 0u) {
        dicts = (zgec_footer_dict_entry *)zgec_alloc(
            (size_t)dict_count * sizeof(zgec_footer_dict_entry),
            _Alignof(zgec_footer_dict_entry));
        if (dicts == NULL) {
            err = ZGEC_ERR_NOMEM;
            goto fail;
        }
    }

    const uint8_t *p = buf + 20;
    for (uint32_t i = 0; i < block_count; i++) {
        blocks[i].offset = zgec_rd64(p);
        blocks[i].record_size = zgec_rd32(p + 8);
        blocks[i].dict_id = zgec_rd16(p + 12);
        blocks[i].lit_ref_depth = p[14];
        blocks[i].rflags = p[15];
        p += 16;
        /* Footer copies of the record header fields get the same range checks
           as the record header itself (spec 4.2/4.4), plus the index
           invariants of review entry 6. */
        err = zgec_footer_block_check(&blocks[i],
                                      (i > 0u) ? &blocks[i - 1u] : NULL);
        if (err != ZGEC_OK) goto fail;
    }
    for (uint32_t i = 0; i < dict_count; i++) {
        dicts[i].dict_id = zgec_rd16(p);
        dicts[i].kind = p[2];
        dicts[i].reserved = p[3];
        dicts[i].raw_size = zgec_rd32(p + 4);
        dicts[i].offset = zgec_rd64(p + 8);
        dicts[i].content_hash = zgec_rd64(p + 16);
        p += 24;
        err = zgec_footer_dict_check(&dicts[i]);
        if (err != ZGEC_OK) goto fail;
    }

    /* Review entry 7: fill a local and install it only on success, so a
       failed parse leaves *f exactly as the caller passed it and nothing
       stays allocated.

       The caller's previous content is NOT released here, deliberately: *f
       is whatever the caller passed in, and callers legitimately hand over an
       uninitialised stack object (tests/test.c:475 t_frame_parts is called
       with a fresh `zgec_footer` and the result is freed afterwards, e.g.
       tests/test.c:796). Releasing it would free indeterminate pointers -- it
       segfaulted under gdb on Windows with the caller's stack garbage.
       Ownership stays with the caller: free and clear *f (tests/test.c:803
       and :804) before parsing a second time into the same object. */
    {
        zgec_footer tmp;
        tmp.block_count = block_count;
        tmp.dict_count = dict_count;
        tmp.content_size = content_size;
        tmp.blocks = blocks;
        tmp.dicts = dicts;
        *f = tmp;
    }
    return ZGEC_OK;

fail:
    zgec_free(blocks);
    zgec_free(dicts);
    return err;
}

size_t zgec_footer_emit(uint8_t *buf, const zgec_footer *f)
{
    /* Review entry 8: the name reads as a public entry point, so it checks its
       own inputs instead of trusting the caller's structures. It shares the
       parser's index-invariant helpers, which is the set of properties the two
       sides must agree on -- not an equivalence. The parser additionally
       requires need == size and a valid footer CRC, and the record headers
       carry constraints a 16-byte index entry cannot express at all: FILTERED
       only on a COMPRESSED record, and 1 <= raw_size <= 2^block_log2
       (src/frame.c:90-120). A footer accepted here is therefore a valid index,
       not a proof that the frame decodes. A rejected footer writes nothing and
       returns 0. */
    if (buf == NULL || f == NULL) return 0;
    size_t total = zgec_footer_size(f->block_count, f->dict_count);
    if (total == 0) return 0;
    if ((f->block_count > 0u && f->blocks == NULL) ||
        (f->dict_count > 0u && f->dicts == NULL)) return 0;
    for (uint32_t i = 0; i < f->block_count; i++) {
        if (zgec_footer_block_check(&f->blocks[i],
                                    (i > 0u) ? &f->blocks[i - 1u] : NULL) !=
            ZGEC_OK)
            return 0;
    }
    for (uint32_t i = 0; i < f->dict_count; i++) {
        if (zgec_footer_dict_check(&f->dicts[i]) != ZGEC_OK) return 0;
    }
    zgec_wr32(buf, ZGEC_FOOTER_MAGIC_U32);
    zgec_wr32(buf + 4, f->block_count);
    zgec_wr32(buf + 8, f->dict_count);
    zgec_wr64(buf + 12, f->content_size);
    uint8_t *p = buf + 20;
    for (uint32_t i = 0; i < f->block_count; i++) {
        zgec_wr64(p, f->blocks[i].offset);
        zgec_wr32(p + 8, f->blocks[i].record_size);
        zgec_wr16(p + 12, f->blocks[i].dict_id);
        p[14] = f->blocks[i].lit_ref_depth;
        p[15] = f->blocks[i].rflags;
        p += 16;
    }
    for (uint32_t i = 0; i < f->dict_count; i++) {
        zgec_wr16(p, f->dicts[i].dict_id);
        p[2] = f->dicts[i].kind;
        p[3] = f->dicts[i].reserved;
        zgec_wr32(p + 4, f->dicts[i].raw_size);
        zgec_wr64(p + 8, f->dicts[i].offset);
        zgec_wr64(p + 16, f->dicts[i].content_hash);
        p += 24;
    }
    zgec_wr32(p, zgec_crc32c(buf, total - 4u, 0u));
    return total;
}

void zgec_footer_free(zgec_footer *f)
{
    if (f == NULL) return;
    zgec_free(f->blocks);
    zgec_free(f->dicts);
    f->blocks = NULL;
    f->dicts = NULL;
    f->block_count = 0u;
    f->dict_count = 0u;
}

void zgec_trailer_emit(uint8_t *buf, const zgec_trailer *t)
{
    if (buf == NULL || t == NULL) return; /* review entry 8 */
    zgec_wr64(buf, t->footer_offset);
    zgec_wr32(buf + 8, t->footer_size);
    zgec_wr32(buf + 12, ZGEC_TRAILER_MAGIC_U32);
}

zgec_err zgec_trailer_parse(zgec_trailer *t, const uint8_t *buf)
{
    if (t == NULL || buf == NULL) return ZGEC_ERR_INVAL;
    if (zgec_rd32(buf + 12) != ZGEC_TRAILER_MAGIC_U32) return ZGEC_ERR_MAGIC;
    t->footer_offset = zgec_rd64(buf);
    t->footer_size = zgec_rd32(buf + 8);
    return ZGEC_OK;
}
