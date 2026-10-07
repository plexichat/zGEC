#include "zgec_frame.h"

#include "zgec_crc32c.h"

/* Block size in bytes: 2^block_log2 (callers validate range 16..26 first). */
static inline uint32_t zgec_frame_block_size(const zgec_frame_header *fh)
{
    return 1u << fh->block_log2;
}

void zgec_frame_header_emit(uint8_t *buf, const zgec_frame_header *h)
{
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
    if (h->max_dict_log2 > ZGEC_MAX_DICT_LOG2) return ZGEC_ERR_BLOCK_SIZE;
    if (h->seg_hint_log2 < ZGEC_SEG_HINT_LOG2_MIN ||
        h->seg_hint_log2 > ZGEC_SEG_HINT_LOG2_MAX) return ZGEC_ERR_BLOCK_SIZE;
    if (zgec_crc32c(buf, 28u, 0u) != zgec_rd32(buf + 28)) return ZGEC_ERR_HEADER_CRC;
    return ZGEC_OK;
}

void zgec_record_header_emit(uint8_t *buf, const zgec_record_header *r)
{
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
    if (f == NULL || buf == NULL) return ZGEC_ERR_INVAL;
    if (size < 24u) return ZGEC_ERR_TRUNCATED;
    if (zgec_rd32(buf) != ZGEC_FOOTER_MAGIC_U32) return ZGEC_ERR_MAGIC;
    uint32_t block_count = zgec_rd32(buf + 4);
    uint32_t dict_count = zgec_rd32(buf + 8);
    uint64_t content_size = zgec_rd64(buf + 12);
    /* Spec 13: bound allocations from hostile counts before malloc. */
    if (block_count > (1u << 24) || dict_count > (1u << 24))
        return ZGEC_ERR_TRUNCATED;
    size_t need = zgec_footer_size(block_count, dict_count);
    if (need == 0 || need != size) return ZGEC_ERR_TRUNCATED;
    if (zgec_crc32c(buf, size - 4u, 0u) != zgec_rd32(buf + size - 4u))
        return ZGEC_ERR_FOOTER_CRC;

    zgec_footer_block_entry *blocks = NULL;
    zgec_footer_dict_entry *dicts = NULL;
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
            zgec_free(blocks);
            return ZGEC_ERR_NOMEM;
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
        /* Footer copies of the record header fields get the same range
           checks as the record header itself (spec 4.2/4.4). */
        if (blocks[i].lit_ref_depth > ZGEC_MAX_LITREF_DEPTH) {
            zgec_free(blocks);
            zgec_free(dicts);
            return ZGEC_ERR_LITREF_DEPTH;
        }
        if ((blocks[i].rflags & (uint8_t)~ZGEC_RFLAG_ALL_KNOWN) != 0u) {
            zgec_free(blocks);
            zgec_free(dicts);
            return ZGEC_ERR_RESERVED;
        }
    }
    for (uint32_t i = 0; i < dict_count; i++) {
        dicts[i].dict_id = zgec_rd16(p);
        dicts[i].kind = p[2];
        dicts[i].reserved = p[3];
        dicts[i].raw_size = zgec_rd32(p + 4);
        dicts[i].offset = zgec_rd64(p + 8);
        dicts[i].content_hash = zgec_rd64(p + 16);
        p += 24;
        if (dicts[i].reserved != 0u) {
            zgec_free(blocks);
            zgec_free(dicts);
            return ZGEC_ERR_RESERVED;
        }
        if (dicts[i].kind > 1u || dicts[i].dict_id == 0u) {
            zgec_free(blocks);
            zgec_free(dicts);
            return ZGEC_ERR_DICT_ID;
        }
    }

    f->block_count = block_count;
    f->dict_count = dict_count;
    f->content_size = content_size;
    f->blocks = blocks;
    f->dicts = dicts;
    return ZGEC_OK;
}

size_t zgec_footer_emit(uint8_t *buf, const zgec_footer *f)
{
    size_t total = zgec_footer_size(f->block_count, f->dict_count);
    if (total == 0) return 0;
    if ((f->block_count > 0u && f->blocks == NULL) ||
        (f->dict_count > 0u && f->dicts == NULL)) return 0;
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
