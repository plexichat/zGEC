#ifndef ZGEC_FSE_H
#define ZGEC_FSE_H

#include "zgec_common.h"
#include "zgec_bitstream.h"

/*
 * tANS (FSE) coder per zGEC section 8 and Annex A.
 *
 * The decode table T[state] = {symbol, nbBits, baseline} is built from
 * normalised counts N[s] (a count of -1 means "less than one", occupying
 * one cell). The construction follows Annex A exactly.
 *
 * The encode table enc[symbol][state] = {nbBits, newState, baseline} is the
 * reverse mapping: given the next state (the state the decoder transitions
 * to) and the symbol, it gives the state the decoder uses for this symbol
 * and the transition bits to write.
 *
 * Key property (verified against Annex A): for every symbol s, the transition
 * ranges of its states tile the entire state space [0, 2^AL). Therefore
 * enc[s][st] is defined for every (s, st) pair, and the encoder's state
 * chain is valid for any symbol sequence.
 */

typedef struct {
    int16_t  symbol;    /* symbol decoded at this state */
    uint8_t  nb_bits;   /* bits to read for the next state */
    uint8_t  pad;
    int32_t  baseline;  /* baseline for the next state */
} zgec_fse_dec_entry;   /* 8 bytes */

typedef struct {
    zgec_fse_dec_entry *e;  /* 2^al entries */
    int al;
    int nsym;
    int first_state[];     /* flexible array: first state with each symbol (nsym entries) */
} zgec_fse_dec_table;

typedef struct {
    uint8_t  nb_bits;   /* bits to write for the transition */
    uint16_t new_state; /* state the decoder uses for this symbol */
    uint16_t baseline;  /* baseline for the transition */
    uint16_t pad;
} zgec_fse_enc_entry;   /* 8 bytes */

typedef struct {
    zgec_fse_enc_entry *e;  /* nsym * 2^al entries, indexed [s * 2^al + st] */
    int al;
    int nsym;
} zgec_fse_enc_table;

/* Build the decode table from normalised counts.
   counts[s] is the normalised count (-1 for "less than one", 0 for never,
   positive otherwise). The sum of effective counts (count == -1 ? 1 : count)
   must equal 2^al. nsym is the alphabet size. */
zgec_err zgec_fse_build_dec(zgec_fse_dec_table **out, const int16_t *counts,
                                int nsym, int al);
void zgec_fse_free_dec(zgec_fse_dec_table *t);

/* Build the encode table from a decode table. */
zgec_err zgec_fse_build_enc(zgec_fse_enc_table **out, const zgec_fse_dec_table *dec);
void zgec_fse_free_enc(zgec_fse_enc_table *t);

/* ---- normalised-count serialisation (RFC 8878 section 4.1.1) ---- */

/* Forward bitstream reader for table descriptions (bits consumed LSB-first
   from little-endian words). */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    uint64_t acc;
    int nacc;
} zgec_fwd_bits;

void zgec_fb_init(zgec_fwd_bits *f, const uint8_t *p, size_t size);
/* Read n bits (n <= 32). Returns the value, or sets *err if truncated. */
uint32_t zgec_fb_read(zgec_fwd_bits *f, unsigned n, zgec_err *err);

/* Read a normalised-count description.
   On success, fills counts[0..*nsym-1], sets *nsym to the number of symbols
   read, and *al to the accuracy log. Returns the number of bytes consumed,
   or 0 on error. counts must have room for max_nsym entries. */
size_t zgec_fse_read_counts(int16_t *counts, int *nsym, int *al,
                                int max_nsym, const uint8_t *buf, size_t size);

/* Write a normalised-count description. Returns the number of bytes written,
   or 0 on error (buffer too small). */
size_t zgec_fse_write_counts(uint8_t *buf, size_t cap,
                                 const int16_t *counts, int nsym, int al);

/* ---- stream decode / encode ---- */

/* Decode n symbols from the backward bitstream.
   For each symbol s, the output value is base[s] + read(nbits[s]).
   If syms is non-NULL, the raw symbols are written to syms[0..n-1].
   The bitstream is left exactly consumed on success (V5). */
zgec_err zgec_fse_decode(const zgec_fse_dec_table *t, zgec_br *br,
                             uint32_t *out, uint8_t *syms, size_t n,
                             const uint32_t *base, const uint8_t *nbits);

/* Encode n symbols into the backward bitstream.
   values[0..n-1] are the values to encode; syms[0..n-1] are the symbols
   (the caller maps values to symbols via the section 8.1 code table).
   The bitstream is written in reverse decode order. */
zgec_err zgec_fse_encode(const zgec_fse_enc_table *t, zgec_bw *bw,
                             const uint32_t *values, const uint8_t *syms, size_t n,
                             const uint32_t *base, const uint8_t *nbits);

#endif /* ZGEC_FSE_H */
