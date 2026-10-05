#ifndef ZGEC_FRAME_H
#define ZGEC_FRAME_H

#include "zgec_common.h"

/*
 * Container format per zGEC section 4.
 *
 * Frame header (32 bytes), record header (24 bytes), footer,
 * trailer (16 bytes). All multi-byte integers are little-endian.
 */

/* ---- frame header (32 bytes) ---- */
typedef struct {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t flags;
    uint8_t  block_log2;
    uint8_t  epoch_blocks;
    uint8_t  max_dict_log2;
    uint8_t  seg_hint_log2;
    uint64_t content_size;   /* 0 if CONTENT_SIZE not set */
    uint32_t block_count;    /* 0 if unknown */
} zgec_frame_header;

/* Parse and verify a 32-byte frame header. buf must have 32 bytes.
   Verifies magic, version, reserved bits, header CRC, and field
   ranges. Returns ZGEC_OK or an error. */
zgec_err zgec_frame_header_parse(zgec_frame_header *h, const uint8_t *buf);

/* Serialise a frame header into buf (32 bytes), computing the
   header CRC. The caller must set all fields; reserved fields
   are written as zero. */
void zgec_frame_header_emit(uint8_t *buf, const zgec_frame_header *h);

/* ---- record header (24 bytes) ---- */
typedef struct {
    uint8_t  record_type;    /* 0 COMPRESSED, 1 RAW, 2 RLE, 3 DICT, 0x80..0xFF skippable */
    uint8_t  rflags;         /* bit 0 HAS_CHECKSUM, bit 1 LIT_EXPORTABLE */
    uint16_t dict_id;        /* 0 for none */
    uint32_t raw_size;
    uint32_t payload_size;
    uint32_t segment_count;  /* COMPRESSED only */
    uint32_t checksum;       /* when HAS_CHECKSUM */
    uint8_t  lit_ref_depth;  /* 0..3 */
} zgec_record_header;

/* Parse a 24-byte record header. Verifies reserved bits and
   field ranges against the frame header. Returns ZGEC_OK or an error. */
zgec_err zgec_record_header_parse(zgec_record_header *r, const uint8_t *buf,
                                          const zgec_frame_header *fh);

/* Serialise a record header into buf (24 bytes). */
void zgec_record_header_emit(uint8_t *buf, const zgec_record_header *r);

/* ---- footer (section 4.4) ---- */
typedef struct {
    uint64_t offset;       /* byte offset of the record header */
    uint32_t record_size;  /* 24 + payload_size */
    uint16_t dict_id;
    uint8_t  lit_ref_depth;
    uint8_t  rflags;
} zgec_footer_block_entry;

typedef struct {
    uint16_t dict_id;
    uint8_t  kind;         /* 0 embedded, 1 external */
    uint8_t  reserved;
    uint32_t raw_size;
    uint64_t offset;       /* embedded: DICT record offset; external: 0 */
    uint64_t content_hash; /* XXH64 seed 0 */
} zgec_footer_dict_entry;

typedef struct {
    uint32_t block_count;
    uint32_t dict_count;
    uint64_t content_size;
    zgec_footer_block_entry *blocks;  /* block_count entries */
    zgec_footer_dict_entry *dicts;    /* dict_count entries */
} zgec_footer;

/* Parse and verify a footer from buf (footer_size bytes).
   Allocates and fills the entries. Returns ZGEC_OK or an error. */
zgec_err zgec_footer_parse(zgec_footer *f, const uint8_t *buf, size_t size);

/* Compute the footer size in bytes for the given counts. */
size_t zgec_footer_size(uint32_t block_count, uint32_t dict_count);

/* Serialise a footer into buf (must be >= zgec_footer_size),
   computing the footer CRC. Returns the number of bytes written. */
size_t zgec_footer_emit(uint8_t *buf, const zgec_footer *f);

void zgec_footer_free(zgec_footer *f);

/* ---- trailer (16 bytes, section 4.5) ---- */
typedef struct {
    uint64_t footer_offset;
    uint32_t footer_size;
} zgec_trailer;

/* Parse a 16-byte trailer. Verifies the magic. */
zgec_err zgec_trailer_parse(zgec_trailer *t, const uint8_t *buf);

/* Serialise a trailer into buf (16 bytes). */
void zgec_trailer_emit(uint8_t *buf, const zgec_trailer *t);

#endif /* ZGEC_FRAME_H */
