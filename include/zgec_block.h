#ifndef ZGEC_BLOCK_H
#define ZGEC_BLOCK_H

#include "zgec_common.h"

/*
 * Block parameters and segments per zGEC section 7.
 *
 * A COMPRESSED block payload is:
 *   [block parameters][segment directory][segment 0]...[segment n-1]
 *
 * Block parameters (section 7.1): two context-map descriptors
 * (plain and sub), each 2 bytes (k == 1) or 26 bytes (k > 1).
 *
 * Segment directory (section 7.2): segment_count entries of
 * 8 bytes (comp_len u32, raw_len u32).
 *
 * Segment structure (section 7.3): segment header, then the
 * literal, LL, ML and OF streams.
 */

/* ---- block parameters (section 7.1) ---- */
typedef struct {
    uint8_t ctx_mode;    /* ZGEC_CTX_* */
    uint8_t ctx_count;   /* 1, 2, 4 or 8 */
    uint8_t class_map[64];  /* class -> context (0..k-1) */
} zgec_ctx_desc;

typedef struct {
    zgec_ctx_desc plain;   /* for lit_form 0 segments */
    zgec_ctx_desc sub;     /* for lit_form 1 segments */
} zgec_block_params;

/* Parse the block parameters from buf (up to 52 bytes).
 * Returns the number of bytes consumed, or 0 on error. */
size_t zgec_block_params_parse(zgec_block_params *bp,
                                         const uint8_t *buf, size_t size);

/* Serialise the block parameters into buf (capacity cap).
 * Returns the number of bytes written, or 0 on error. */
size_t zgec_block_params_emit(uint8_t *buf, size_t cap,
                                        const zgec_block_params *bp);

/* ---- segment directory (section 7.2) ---- */
typedef struct {
    uint32_t comp_len;   /* total size of the segment incl. header */
    uint32_t raw_len;    /* output bytes produced */
} zgec_seg_dir_entry;

/* Parse segment_count directory entries from buf.
 * Returns ZGEC_OK or an error. */
zgec_err zgec_seg_dir_parse(zgec_seg_dir_entry *dir,
                                      uint32_t segment_count,
                                      const uint8_t *buf, size_t size);

/* Serialise the directory into buf. */
void zgec_seg_dir_emit(uint8_t *buf,
                             const zgec_seg_dir_entry *dir,
                             uint32_t segment_count);

/* ---- segment header (section 7.3) ---- */
typedef struct {
    uint8_t  segment_flags;
    uint8_t  table_modes;
    uint32_t n_seq;       /* varint */
    uint32_t n_lit;       /* varint */
    /* table descriptors follow in the payload; this struct
       holds only the fixed fields plus the stream sizes */
    uint32_t lit_size;
    uint32_t ll_size;
    uint32_t ml_size;
    uint32_t of_size;
    /* offset of the streams within the segment (after the
       header and table descriptors) */
    size_t   streams_off;
    size_t   header_size;  /* total header size incl. descriptors */
} zgec_seg_header;

/* Parse a segment header from buf (up to the segment size).
 * The table descriptors are parsed and their total size is
 * returned in *descriptors_size (the caller then knows where
 * the streams begin). Returns the number of bytes consumed
 * (the header size), or 0 on error.
 *
 * The table descriptors themselves are returned in
 * descriptors (a byte buffer the caller provides, capacity
 * descriptors_cap; the function writes the raw descriptor
 * bytes there for later interpretation). If descriptors is
 * NULL, the descriptors are skipped but still counted.
 *
 * Single-context form: assumes k == 1 (one literal table).
 * Callers for blocks with k > 1 MUST use
 * zgec_seg_header_parse_ex with the real k from the block
 * parameters.
 */
size_t zgec_seg_header_parse(zgec_seg_header *sh,
                                       const uint8_t *buf, size_t size,
                                       uint8_t *descriptors,
                                       size_t descriptors_cap,
                                       size_t *descriptors_size);

/* Extended form: k is the block's context count from the block
 * parameters (1, 2, 4 or 8; anything else is rejected). It selects
 * how many literal-table descriptors follow (1 for k == 1, k + 1
 * otherwise). Same return convention as above. */
size_t zgec_seg_header_parse_ex(zgec_seg_header *sh,
                                          const uint8_t *buf, size_t size,
                                          uint8_t *descriptors,
                                          size_t descriptors_cap,
                                          size_t *descriptors_size,
                                          int k);

/* Serialise a segment header into buf (capacity cap).
 * The descriptors bytes (descriptors_size of them) are
 * copied verbatim after the fixed fields. Returns the
 * number of bytes written, or 0 on error. */
size_t zgec_seg_header_emit(uint8_t *buf, size_t cap,
                                      const zgec_seg_header *sh,
                                      const uint8_t *descriptors,
                                      size_t descriptors_size);

/* ---- table descriptor interpretation ---- */

/* The table modes byte packs four 2-bit modes:
 * bits 0-1 literal, 2-3 LL, 4-5 ML, 6-7 OF. */
static inline unsigned zgec_tbl_mode(uint8_t table_modes, unsigned which) {
    return (table_modes >> (2 * which)) & ZGEC_TBL_MASK;
}

/* A table descriptor is either:
 * - NEW: an FSE description (RFC 8878 normalised counts),
 * - RLE: a single byte (the symbol code; sequence tables only),
 * - REPEAT: nothing (the previous segment's tables).
 *
 * Literal tables are NEW or REPEAT only (section 7.3); a literal
 * RLE mode does not exist and is rejected.
 *
 * For sequence tables (LL, ML, OF) the alphabet is 66
 * symbols; for literal tables it is 256 symbols with
 * AL = 11. When context conditioning is enabled (section
 * 8.6), three descriptions follow for the conditioned
 * stream, all with the same AL.
 *
 * For literals with k > 1 contexts, k+1 descriptions
 * follow (contexts 0..k-1, then the run-start table).
 */

/* Parse the table descriptors for one segment.
 * Fills the caller's arrays of FSE descriptions (as raw
 * byte slices) and RLE symbols.
 *
 * On success, returns the number of descriptor bytes
 * consumed. The caller then builds the tables.
 *
 * The arrays are sized by the caller:
 *   lit_desc[0..n_lit_tables-1]   (n_lit_tables = k or k+1)
 *   ll_desc[0..n_ll_tables-1]     (1 or 3)
 *   ml_desc[0..1]                 (always 1)
 *   of_desc[0..n_of_tables-1]     (1 or 3)
 * Each desc is a {const uint8_t *ptr; size_t size;} slice
 * for NEW mode, or a single-byte RLE symbol (sequence
 * tables only; literal entries are always NEW).
 *
 * This is a convenience that walks the descriptor bytes;
 * the caller may also walk them directly.
 */
typedef struct {
    const uint8_t *ptr;   /* NEW: the FSE description bytes */
    size_t size;          /* NEW: their length */
    int mode;             /* ZGEC_TBL_NEW / RLE / REPEAT */
    int rle_symbol;       /* RLE: the symbol code */
} zgec_tbl_desc;

/* Walk the table descriptors for a segment.
 * buf/size: the descriptor bytes (from the segment header).
 * Returns the number of bytes consumed, or 0 on error.
 *
 * lit_form, lit_coder, k, seq_ctx_of, seq_ctx_ll determine
 * the descriptor layout. The caller provides arrays sized
 * as above; unused entries are left untouched. */
size_t zgec_seg_descriptors_walk(zgec_tbl_desc *lit_desc, int n_lit_tables,
                                            zgec_tbl_desc *ll_desc, int n_ll_tables,
                                            zgec_tbl_desc *ml_desc,
                                            zgec_tbl_desc *of_desc, int n_of_tables,
                                            const uint8_t *buf, size_t size,
                                            int lit_form, int lit_coder, int k,
                                            int seq_ctx_of, int seq_ctx_ll);

#endif /* ZGEC_BLOCK_H */
