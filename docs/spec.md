# zGEC Format and Design Specification

z Good Enough Compression: a random-access LZ77 + ANS block compressor

| Item | Value |
|---|---|
| Document | zGEC Format and Design Specification |
| Version | 1.0 |
| Date | 4 October 2026 |
| Reference platform | AMD Ryzen 5 5600X (Zen 3), x86-64 with AVX2 and BMI2; reference implementation in C |
| Normative language | The words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Sections marked (informative) do not constrain conforming decoders. |
| Status of numbers | All speed and ratio figures in this document are design budgets and targets, not measurements. |

## Contents

1 Introduction, 2 Conventions and terminology, 3 Format overview and limits, 4 Container format,
5 Dictionaries and epochs, 6 The virtual buffer and literal references, 7 Block parameters and segments,
8 Sequence coding, 9 Literal coding, 10 Decoding procedure, 11 Encoder guidance (informative),
12 Performance model and targets (informative), 13 Robustness and security, 14 Versioning and extensibility,
15 Verification plan and milestones, 16 Open issues and risks, Annex A tANS table construction,
Annex B rANS details, Annex C Context classification functions, Annex D Reference copy routines, References.

## 1 Introduction

### 1.1 Purpose

zGEC is a byte-oriented lossless compression format in the family of zstd and LZ4: an LZ77 parse
produces sequences of literal runs and matches, and the symbols are entropy coded with asymmetric
numeral systems (ANS). It differs from zstd in one structural respect that drives every other
decision: random access is a hard requirement. Any block of the compressed file can be decoded
without decoding any other block, apart from a bounded, explicitly stored dictionary.

This document defines the file format (normative), the decoding procedure (normative), and guidance
for encoders and implementers (informative).

### 1.2 Goals

| ID | Goal |
|---|---|
| G1 | Random access at block granularity, always. Decoding block N MUST NOT require decoding the contents of any other block. |
| G2 | Compression ratio close to zstd -6 at encoder speed close to zstd -4 on the reference platform, for text, source code and JSON; binary data is expected to trail somewhat. |
| G3 | Decode speed at least as high as zstd at comparable ratio, with a branch-light, vectorisable execute loop. |
| G4 | Near-linear multi-core scaling for both encode and decode, because blocks are independent. |
| G5 | A small implementation: the reference encoder and decoder together should be of the same order of size as zstd's core, written in portable C with optional x86-64 SIMD paths. |

### 1.3 Non-goals

- Maximum compression ratio. The format is "good enough": it trades the last few percent for speed and random access.
- Unbounded history. Matches that reach outside the current block, the block's dictionary, or (optionally) a small fixed number of preceding blocks' literals are not representable. This is what makes G1 possible.
- Streaming without a block buffer. A decoder needs the whole compressed block in memory.

### 1.4 Summary of key design decisions

| Decision | Rationale |
|---|---|
| Independent blocks with an explicit dictionary table | Gives random access and full parallelism. Cross-block redundancy is recovered through dictionaries rather than through history. |
| Epoch dictionaries: trained on one epoch, applied to the next | The dictionary is stored in the file, so there is no decode-time dependency between blocks, and the encoder can train every dictionary from raw input before any block is encoded, which keeps encoding fully parallel. |
| Optional literal references (Extended) | Allows a block to see the literals of up to three preceding blocks with a dependency depth of one. Kept because it is logically consistent with random access. |
| Segments inside blocks | Entropy tables adapt every 128-256 KiB without shrinking the random-access unit. |
| Learned literal contexts with a run-start context | Literals that start a run follow a match in the real output, so the previous literal is meaningless as context for them. |
| Sub-literals | Per-segment option to code literals as residuals against the byte at the current repeat offset; large gains on structured binary data. |
| Three independent sequence bitstreams (LL, ML, OF) | Removes the single shared bit reader that serialises zstd's sequence decoding. |
| Phase split: decode and validate, then execute | Makes the execute loop check-free and prefetchable. |
| Scalar-first rANS literal coder with 8 lanes | Gather-based vector rANS is slow on Zen 3; interleaved scalar lanes hide latency and are portable. |
| Optional block pre-filter (byte delta or stride shuffle) | A whole-block transform recovers structure that byte order hides on numeric, columnar and image data; a sampled gate keeps it off, so it is free where it does not help. |
| Per-block record type chosen from work already done | Raw, RLE and compressed records already exist; choosing between them after encoding skips the entropy stage on incompressible blocks, a net speedup for both encoder and decoder. |

## 2 Conventions and terminology

### 2.1 Numeric conventions

- All multi-byte integers are little-endian. u8, u16, u32, u64 denote unsigned integers of 8, 16, 32 and 64 bits.
- varint is an unsigned LEB128 integer: seven payload bits per byte, least significant group first, bit 7 set on every byte except the last. A varint encoding a 32-bit value occupies at most five bytes; longer encodings MUST be rejected.
- KiB = 1024 bytes, MiB = 1024 KiB. Block sizes are powers of two.
- CRC32C is the Castagnoli CRC-32 (polynomial 0x1EDC6F41, reflected, initial value and final XOR 0xFFFFFFFF) as used by iSCSI and the x86 crc32 instruction.

### 2.2 Terminology

| Term | Definition |
|---|---|
| Frame | A complete zGEC file: header, records, footer and trailer. |
| Record | A 24-byte record header followed by a payload. Records are blocks or dictionaries. |
| Block | A record holding 2^block_log2 bytes of original data (the last block may be shorter). The unit of random access. |
| Segment | A run of consecutive sequences inside a block that shares entropy tables; the unit of table adaptation, typically 128-256 KiB of output. |
| Sequence | A triple (literal length LL, match length ML, offset base OF) describing LL literal bytes followed by an ML-byte match. |
| Tail literals | Literals after the last sequence of a segment (count derived, may be zero). |
| Run start | The first literal of a literal run, that is, a literal immediately preceded in the output by a match (or by the start of a segment). |
| Dictionary | A byte string stored in the frame and referenced by id, placed before the block output in the virtual buffer. |
| Epoch | A group of epoch_blocks consecutive blocks that share the same dictionary by encoder convention. |
| Virtual buffer (VB) | The conceptual byte array in which distances are measured: dictionary, then referenced literals, then the block output (section 6). |
| Lane | One of the 8 interleaved rANS decoders used for the literals of a segment. |
| Context | The index of the literal frequency table used for a literal. |
| Pre-filter | An optional reversible, length-preserving transform of a block's raw data, applied before parsing and undone after decoding (section 7.5). |

## 3 Format overview and limits

### 3.1 Frame layout

```
+----------------+----------+-----------+-----+----------+----------+
| Frame header   | Record 0 | Record 1  | ... | Footer   | Trailer  |
| 32 bytes       | (DICT or | (BLOCK)   |     | (index)  | 16 bytes |
|                |  BLOCK)  |           |     |          |          |
+----------------+----------+-----------+-----+----------+----------+

A BLOCK record:   [24-byte record header][block parameters]
                   [segment directory][segment 0]...[segment n-1]

A segment:        [segment header][literal stream][LL][ML][OF streams]
```

Records appear in the order in which a sequential reader needs them: a dictionary record precedes
the first block that references it. The footer holds an index of every record so that a reader that
starts at the end of the file can reach any block with two reads (trailer, then footer) and one
more read per block.

### 3.2 Decoding a block at a glance

1. Locate the block through the footer index; read its record, and the dictionary record it names (if any).
2. Build the virtual buffer: dictionary bytes, then (if literal references are used) the literals of the referenced blocks, then space for the block output.
3. Phase A for each segment: decode the sequence streams, resolve repeat offsets, validate, then decode the literals using the run-start information.
4. Phase B: execute the sequences, copying literals and matches into the output with no further checks.
5. Verify the checksum if present.

### 3.3 Limits

| Quantity | Format limit | Reference profile (P24) |
|---|---|---|
| Block size | 2^16 to 2^26 bytes | 2^16 to 2^23; default 2^21 (2 MiB) |
| Dictionary size | at most 2^26 bytes and at most 2^max_dict_log2 | at most 1 MiB; default sized per section 5.5 |
| Virtual buffer (dictionary + literal references + block) | at most 2^32 - 1 bytes | at most 2^24 bytes (16 MiB) |
| Distance (offset) | 1 to 2^32 - 4 | at most 2^24 |
| Match length | 3 to 2^32 - 1, limited by block size | same |
| Segments per block | 1 to 4096 | typically 8-32 |
| Sequences per segment | at most 2^22 | same |
| Dictionaries per frame | ids 1 to 65535 | same |
| epoch_blocks | 0 to 255 | default 10 |
| Literal reference depth | 0 to 3 | default 0 |
| Filter stride (mode 2) | 2 to 64 | 2, 4, 8, 16, 32 or 64 |

The reference profile P24 exists because the reference encoder packs a 24-bit virtual-buffer position
and an 8-bit tag into each 32-bit hash-table entry. It is an encoder property, not a format property:
a decoder MUST accept any stream within the format limits, and an encoder MAY use larger virtual
buffers with wider table entries.

## 4 Container format

### 4.1 Frame header (32 bytes)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | magic | ASCII "zGEC" (0x7A 0x47 0x45 0x43). |
| 4 | 1 | version_major | Format major version; this document defines 1. Decoders MUST reject a larger major version. |
| 5 | 1 | version_minor | Minor version; decoders MUST accept a larger minor version if every feature bit it uses is understood. |
| 6 | 2 | flags | See table below. |
| 8 | 1 | block_log2 | Block size is 2^block_log2 bytes; range 16 to 26. |
| 9 | 1 | epoch_blocks | Blocks per dictionary epoch (informative for readers, used by encoders); 0 means no epoch convention. |
| 10 | 1 | max_dict_log2 | No dictionary exceeds 2^max_dict_log2 bytes; range 0 to 26 (0 means no dictionaries). Lets a decoder size caches up front. |
| 11 | 1 | seg_hint_log2 | Encoder hint for the target segment size; range 14 to 22. No effect on decoding. |
| 12 | 8 | content_size | Total original size if flag bit 0 is set, otherwise 0. |
| 20 | 4 | block_count | Number of BLOCK records if known, otherwise 0. |
| 24 | 4 | reserved | MUST be zero. |
| 28 | 4 | header_crc | CRC32C of bytes 0 to 27. |

| Flag bit | Name | Meaning |
|---|---|---|
| 0 | CONTENT_SIZE | content_size holds the total original size. |
| 1 | HAS_FOOTER | The frame ends with a footer and trailer. Streaming encoders that cannot seek MAY clear this and write the footer last anyway; if the bit is clear a reader MUST fall back to scanning. |
| 2 | BLOCK_CHECKSUMS | Every BLOCK record carries a CRC32C of its original data. |
| 3 | EXTENDED_LITREF | At least one block uses literal references (section 6.3). Decoders that do not implement the Extended level MUST reject the frame. |
| 4 | EXTERNAL_DICT | At least one dictionary is of kind external (section 5.8). |
| 5 to 15 | reserved | MUST be zero. Decoders MUST reject frames with unknown bits set. |

### 4.2 Record header (24 bytes)

Blocks and dictionaries share one header layout so that a sequential reader can skip any record using
only its first 24 bytes: the total record length is 24 + payload_size.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | record_type | 0 COMPRESSED block, 1 RAW block, 2 RLE block, 3 DICT. Values 0x80 to 0xFF are skippable records: a reader MUST ignore them using payload_size. Other values are reserved and MUST be rejected. |
| 1 | 1 | rflags | Bit 0 HAS_CHECKSUM. Bit 1 LIT_EXPORTABLE (section 6.3). Bit 2 FILTERED (section 7.5). Bits 3 to 7 reserved, MUST be zero. |
| 2 | 2 | dict_id | BLOCK: the dictionary the block uses, 0 for none. DICT: the id this record defines (never 0). |
| 4 | 4 | raw_size | Original size of the block (or the dictionary length for DICT). For blocks, MUST equal 2^block_log2 except for the final block, which MUST be between 1 and 2^block_log2. |
| 8 | 4 | payload_size | Bytes following the header. |
| 12 | 4 | segment_count | COMPRESSED only: 1 to 4096. Otherwise zero. |
| 16 | 4 | checksum | CRC32C of the original data when HAS_CHECKSUM is set, otherwise zero. |
| 20 | 1 | lit_ref_depth | Number of preceding blocks whose literals are prepended to the virtual buffer (0 to 3). |
| 21 | 3 | reserved | MUST be zero. |

Payload by record type:

- RAW: payload_size = raw_size; the payload is the original data.
- RLE: payload_size = 1; the original data is that byte repeated raw_size times. Decoders SHOULD refuse raw_size above the declared block size.
- COMPRESSED: see section 4.3. When rflags bit 2 (FILTERED) is set, the payload begins with the filter descriptor of section 7.5.
- DICT: the payload is a complete inner record (24-byte header plus payload) of type RAW, RLE or COMPRESSED holding the dictionary bytes. The inner record MUST have dict_id 0 and lit_ref_depth 0, so a dictionary never depends on another dictionary, and neither the DICT record nor its inner record may set FILTERED.

### 4.3 COMPRESSED payload

```
[Filter descriptor]     0 or 2 bytes      (section 7.5; present iff rflags bit 2)
[Block parameters]      2 to 52 bytes     (section 7.1)
[Segment directory]     8 x segment_count (section 7.2)
[Segment 0] ... [Segment n-1]             (section 7.3)
```

The sum of the segment lengths in the directory plus the sizes of the filter descriptor, the block
parameters and the directory MUST equal payload_size exactly, and the sum of the directory raw lengths
MUST equal raw_size.

### 4.4 Footer

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | magic | ASCII "zGEF". |
| 4 | 4 | block_count | Number of BLOCK records (types 0, 1, 2). |
| 8 | 4 | dict_count | Number of DICT records. |
| 12 | 8 | content_size | Total original size. |
| 20 | 16 x block_count | block entries | See below. Entry b describes block b in file order. |
| next | 24 x dict_count | dictionary entries | See below. |
| next | 4 | footer_crc | CRC32C of all preceding footer bytes. |

Block entry:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 8 | offset | Byte offset of the record header from the start of the frame. |
| 8 | 4 | record_size | 24 + payload_size of the block record. |
| 12 | 2 | dict_id | Copy of the record header field, so the dictionary can be fetched in parallel with the block. |
| 14 | 1 | lit_ref_depth | Copy of the record header field. |
| 15 | 1 | rflags | Copy of the record header field. |

Dictionary entry:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 2 | dict_id | The id (1 to 65535). |
| 2 | 1 | kind | 0 embedded, 1 external. |
| 3 | 1 | reserved | MUST be zero. |
| 4 | 4 | raw_size | Dictionary length in bytes. |
| 8 | 8 | offset | Embedded: offset of the DICT record header. External: zero. |
| 16 | 8 | content_hash | XXH64 (seed 0) of the dictionary bytes. Used to identify external dictionaries and to verify embedded ones after decoding. |

Because every block except the last has raw_size = 2^block_log2, the block holding original offset X is
simply X >> block_log2; no search is needed.

### 4.5 Trailer (16 bytes)

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 8 | footer_offset | Offset of the footer from the start of the frame. |
| 8 | 4 | footer_size | Size of the footer in bytes. |
| 12 | 4 | magic | ASCII "CEGz" (the frame magic reversed), so the trailer is recognisable from the end of the file. |

### 4.6 Random-access read procedure

1. Read the 16 trailer bytes at the end of the file and check the magic; read and verify the footer.
2. Compute b = X >> block_log2 and read block entry b.
3. If dict_id is non-zero, look up the dictionary entry; fetch the decoded dictionary from the cache, or read its DICT record, decode the inner record, verify content_hash, and cache it (section 5.9).
4. If lit_ref_depth is D, read blocks b - D to b - 1 and run the literal-export procedure of section 6.3 on each.
5. Read and decode block b (section 10), then return the requested bytes.

### 4.7 Sequential decoding and recovery

A reader without a footer proceeds record by record from offset 32 using the 24-byte headers,
remembering each DICT record by dict_id as it passes. This is also the recovery path for a truncated
file: all records before the damage remain decodable. A reader that finds an unknown record_type below
0x80 MUST stop with an error.

## 5 Dictionaries and epochs

### 5.1 Concept

A dictionary is a byte string stored once in the frame and referenced by blocks through a 16-bit id.
When a block names a dictionary, the dictionary bytes form the start of the block's virtual buffer, so
matches may point into them exactly as they point into earlier output. The dictionary is part of the
file; a decoder obtains it by decoding one DICT record. Consequently a block never depends on another
block's data, only on a stored dictionary that can be fetched, decoded and cached independently.

### 5.2 Dictionary records and ids

- Ids 1 to 65535 are valid. Id 0 means "no dictionary" in a block record and MUST NOT appear in a DICT record.
- A DICT record MUST precede the first block that references it in file order, and MUST be listed in the footer.
- A block references at most one dictionary. An encoder that wants the effect of two (for example an external base dictionary plus an epoch dictionary) concatenates them into one.
- The dictionary is stored compressed (section 4.2) without a dictionary of its own, so storing it costs roughly its compressed size, typically 40-60% of its length on text-like data.

### 5.3 Epoch convention

The format allows any assignment of dictionaries to blocks. Encoders SHOULD use the following epoch
convention, which is what epoch_blocks in the frame header advertises. Let E = epoch_blocks (default 10).
Epoch e consists of blocks e*E to e*E + E - 1. The dictionary used by the blocks of epoch e is trained
from the original data of epoch e - 1 and is stored immediately before the first block of epoch e.

| Epoch | Blocks | Dictionary used | Trained from |
|---|---|---|---|
| 0 | 0 to 9 | none (or an external dictionary) | not applicable |
| 1 | 10 to 19 | dictionary 1 | blocks 0 to 9 |
| 2 | 20 to 29 | dictionary 2 | blocks 10 to 19 |
| k | k*E to k*E + E - 1 | dictionary k | blocks (k-1)*E to k*E - 1 |

Training on the previous epoch rather than the current one exploits locality: neighbouring data tends to
share vocabulary, structure and boilerplate, and the dictionary reflects what the file has been like
recently. The final epoch may be shorter than E blocks.

### 5.4 Why random access and encoder parallelism are preserved

- Decode. Block b needs exactly one dictionary, and a dictionary needs no other block. To read block 25 the decoder fetches dictionary 2 (a record of at most about 1 MiB) and decodes block 25. A cache keyed by dict_id makes later reads in the same epoch free.
- Encode. Dictionary k depends only on the raw input of epoch k - 1, not on the compressed output. The encoder can therefore train all dictionaries in a cheap first pass over the raw data, after which every block in the file can be encoded in parallel. This is a deliberate advantage over designs in which block N is parsed against the parse of block N - 1.

### 5.5 Sizing and the ratio-neutrality gate (informative)

A dictionary costs space, so it must pay for itself. Encoders SHOULD apply the following policy so that
using dictionaries does not reduce the ratio.

- Default size: clamp(epoch_raw_bytes / 32, 64 KiB, 1 MiB), never above 2^max_dict_log2. With 2 MiB blocks and E = 10 this is about 640 KiB for a 20 MiB epoch.
- Before emitting dictionary k, estimate its benefit: encode a sample of blocks of epoch k (for example two of ten) with and without the dictionary and extrapolate the saving to the whole epoch.
- Emit the dictionary only if the extrapolated saving is at least 1.25 times the compressed dictionary size. Otherwise omit the record and set dict_id to 0 for those blocks.
- Decide per block: a block whose measured size with the dictionary is not smaller than without it MUST be written with dict_id 0 if the encoder cares about ratio. The stored dictionary then still serves the other blocks of the epoch.

### 5.6 Dictionary content layout (informative)

The virtual buffer places the dictionary immediately before the block output, so the end of the
dictionary is the nearest history. Distances are entropy coded on a logarithmic scale, so near content
is cheaper. Encoders SHOULD therefore place the most valuable (most frequently referenced) chunks at
the end of the dictionary and the least valuable at the start.

### 5.7 Training algorithm (informative)

Training must be fast enough for an inline encoder; COVER-style optimisation is too slow. The
reference approach is a sampled greedy selection:

1. Split the epoch's data into content-defined chunks using a gear-style rolling hash with a mask giving an average chunk of about 512 bytes (minimum 64, maximum 4 KiB).
2. Fingerprint each chunk with a 64-bit hash and count occurrences in an open-addressed table, in one pass over the epoch.
3. Score each chunk that occurs at least twice by (occurrences - 1) x length. Optionally, mark chunks of poorly compressible content (high-entropy bytes) as ineligible.
4. Select chunks in descending score order until the size budget is reached, removing duplicates.
5. Order the selected chunks by ascending score so that the best content ends up at the end (section 5.6). Where two selected chunks were adjacent in the source, keep them adjacent so that longer matches survive.

Because the chunker is content-defined, it also finds repeats that are far apart in the original file.
This is how the format captures the benefit that a long-distance matcher would give in a streaming
design.

### 5.8 External dictionaries

A dictionary entry of kind 1 is not stored in the frame. The decoder must be given the bytes by the
application; it MUST check that their length equals raw_size and that their XXH64 equals
content_hash, and MUST fail otherwise. The frame flag EXTERNAL_DICT is set. This supports
corpus-specific base dictionaries shared by many files.

### 5.9 Decoder dictionary cache

Decoders SHOULD keep at least two decoded dictionaries (the current and the next epoch) in an LRU
cache keyed by dict_id, shared between worker threads, with a once-only decode per id. Decoding a
1 MiB dictionary at about 1.5 GB/s takes under a millisecond, so even an uncached random read pays
little.

## 6 The virtual buffer and literal references

### 6.1 Layout

Distances are measured in the virtual buffer VB, which is the concatenation, in this order, of three
regions:

```
VB = [ DICT: Ld bytes ][ LITREF: Ll bytes ][ OUT: raw_size bytes ]
       dictionary         literals of the    the block being
       (may be empty)     referenced blocks  decoded
                          (may be empty)
```

Let p be the number of bytes already written to OUT. The write position in VB is Ld + Ll + p. A match
with offset d copies from VB position (Ld + Ll + p) - d, byte by byte in increasing order, so
overlapping copies (d smaller than the match length) replicate a pattern. A valid offset satisfies
1 <= d <= Ld + Ll + p at the first byte of the match.

### 6.2 Consequences

- A decoder needs no special case for a match that reaches back into the dictionary or literal region: all three regions are laid out contiguously in memory, so the only addressing rule is the distance.
- A match may cross from LITREF into OUT or from DICT into LITREF when the source range spans the junction. The bytes at a junction are artificial neighbours, but the decoder reproduces exactly the same buffer as the encoder, so such matches are valid and decode correctly.
- The decoder allocates Ld + Ll + raw_size + 64 bytes per worker. The 64 bytes are the slack required by section 10.5.

### 6.3 Literal references (Extended level)

A block MAY set lit_ref_depth = D (1 to 3). Its LITREF region is then the concatenation, oldest
first, of the literal buffers of blocks b - D to b - 1. The literal buffer of a block is the
sequence of all literal bytes that its sequences and tail literals copy to the output, in output order.

Literal references recover recently seen content that the previous block coded as new, without any
dependency on that block's matches. A decoder obtains a literal buffer without executing the block, by
the literal-export procedure: parse the block parameters and segment directory, and for each segment
decode the LL stream and then the literal stream (section 9), appending the literals. The ML and OF
streams are not touched and no output is produced.

To make this possible a referenced block MUST satisfy all of:

- its record has LIT_EXPORTABLE set and is not FILTERED (section 7.5);
- every segment uses plain literals (lit_form = 0), because sub-literals need the block's output to reconstruct the literal bytes;
- no segment has seq_ctx_mode bit 4 set (LL conditioned on the previous ML), because that would require decoding the ML stream to obtain LL.

The block that references them has a dependency depth of exactly one: it needs the exportable literal
sections of D predecessors and nothing else, so random access costs at most D extra literal-export
passes (roughly 0.5 ms each for 1 MiB of literals). A block with lit_ref_depth > 0 may itself be
exported by a later block. Decoders that implement only the Core level MUST reject any frame with
EXTENDED_LITREF set.

Encoders MUST keep Ld + Ll + raw_size within the format limit, and SHOULD stay within their profile
limit (16 MiB for P24), reducing D or declining the reference when the buffers would not fit.
Encoders SHOULD enable literal references only when a measurement on the file shows a gain; the
expected gain is smaller than that of an epoch dictionary on most data, and the format keeps the
option mainly for data whose repetition is short-range and block-aligned.

## 7 Block parameters and segments

### 7.1 Block parameters

The block parameters describe the literal context maps shared by all segments of the block. They
consist of two consecutive descriptors, plain (used by segments with lit_form 0) and sub (used by
segments with lit_form 1):

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | ctx_mode | Context classification function (Annex C): 0 none, 1 LSB6, 2 MSB6, 3 TEXT, 4 SIGNED. Values above 4 are reserved. |
| 1 | 1 | ctx_count | Number of contexts k: 1, 2, 4 or 8. If ctx_mode is 0, k MUST be 1. |
| 2 | 24 | class_map | Present only if k > 1. Sixty-four 3-bit entries, packed least significant bit first, giving the context (0 to k - 1) of each class. |

A descriptor therefore occupies 2 bytes (k = 1) or 26 bytes (k > 1). Each class_map entry MUST be less
than k. The two descriptors are independent; a block using no sub-literals still carries both, with the
sub descriptor typically 00 01. Run starts use an additional dedicated table (section 9.3), so the
number of literal tables per segment is k + 1 when k > 1.

### 7.2 Segment directory

The directory contains segment_count entries of 8 bytes: comp_len (u32), the total size in bytes of the
segment including its header, and raw_len (u32), the number of output bytes it produces. Prefix sums
give each segment's byte offset in the payload and its start position in OUT, which allows segments to
be decoded by different threads once the segment headers have been parsed (section 10.6).

### 7.3 Segment structure

```
segment_flags   u8
table_modes     u8
n_seq           varint
n_lit           varint
[table descriptors, in the order: literal, LL, ML, OF]
lit_size        varint     bytes in the literal stream
ll_size         varint     bytes in the LL stream     (0 if n_seq = 0)
ml_size         varint     bytes in the ML stream     (0 if n_seq = 0)
of_size         varint     bytes in the OF stream     (0 if n_seq = 0)
[literal stream][LL stream][ML stream][OF stream]
```

segment_flags byte bits:

| Byte | Bits | Meaning |
|---|---|---|
| segment_flags | 0 | lit_form: 0 plain literals, 1 sub-literals (section 9.6). |
| | 1-2 | lit_coder: 0 raw, 1 rANS, 2 and 3 reserved (a Huffman coder is reserved for a later minor version). |
| | 3 | seq_ctx_mode bit 3: OF conditioned on the ML class (section 8.6). |
| | 4 | seq_ctx_mode bit 4: LL conditioned on the previous ML class. |
| | 5-7 | reserved, MUST be zero. |

table_modes byte bits:

| Bits | Meaning |
|---|---|
| 0-1 | literal tables: 0 new, 1 repeat the previous segment's tables. |
| 2-3 | LL table: 0 new FSE description, 1 repeat, 2 RLE. |
| 4-5 | ML table, same coding. |
| 6-7 | OF table, same coding. |

Table descriptors:

- A new FSE description follows the normalised-count format used by zstd (RFC 8878 section 4.1.1), which begins with the accuracy log. For sequence tables the accuracy log AL MUST be between 5 and 11; for literal tables it MUST be exactly 11. The alphabet is 66 symbols for LL, ML and OF, and 256 symbols for literals.
- When seq_ctx_mode bit 3 is set and the OF mode is "new", three OF descriptions follow, one per ML class (section 8.6), all with the same AL. Likewise three LL descriptions when bit 4 is set. With mode "repeat" the previous segment's sets are reused and its seq_ctx_mode bits MUST match.
- For lit_coder 1 and k = 1 there is one literal table. For k > 1 there are k + 1 literal tables: contexts 0 to k - 1, then the run-start table. Each is an FSE description over 256 symbols with AL = 11.
- An RLE mode stream is described by a single byte, the symbol code, and its bitstream contains only extra bits (section 8.5).
- Literal tables are omitted when lit_coder is 0. Sequence tables are omitted when n_seq is 0.

### 7.4 Inheritance and constraints

- "Repeat" refers to the immediately preceding segment of the same block. The first segment of a block MUST use "new" or "RLE" for every table. A repeat of the literal tables requires the same lit_form and k as the previous segment.
- Repeat chains never cross a block boundary, so block-level random access is unaffected. Decoders resolve all inheritance in a quick pass over the segment headers before decoding streams. Reuse of the previous block's tables is deliberately excluded: it would make block N depend on the segment headers of block N - 1, and an encoder could chain such repeats back across the file, so a random read could be forced to walk every earlier block. The saving is also small, because a segment's table descriptions are only a few hundred bytes against a 128 KiB to 2 MiB block. Table reuse therefore stays inside one block.
- raw_len from the directory MUST equal n_lit plus the sum of all match lengths in the segment.
- The tail literal count is n_lit minus the sum of the LL values and MUST be non-negative.
- A segment with n_seq = 0 consists only of tail literals; it is valid and is used for incompressible regions.

### 7.5 Block pre-filter (optional)

A COMPRESSED block MAY be pre-filtered. The filter is a reversible, length-preserving transform of
the block's raw data that the encoder applies before parsing and the decoder undoes after phase B. Its
purpose is to expose structure that byte order hides in numeric arrays, columnar data, fixed-stride
records and images. Because it transforms the whole block, it helps the match finder as well as the
literal coder, unlike the sub-literal option of section 9.6, which touches literals only.

The filter is announced by record-header rflags bit 2 (FILTERED, section 4.2). When that bit is set the
first two bytes of the COMPRESSED payload are the filter descriptor:

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 1 | filter_mode | 1 byte-wise delta, 2 stride shuffle. 0 is reserved and MUST NOT appear with FILTERED set; 3 to 255 are reserved. |
| 1 | 1 | filter_param | mode 2: the stride N, 2 to 64. mode 1: MUST be 0. |

A FILTERED block MUST have lit_ref_depth 0 and MUST NOT set LIT_EXPORTABLE, and it MUST NOT be a
literal-reference predecessor (section 6.3). It MAY use a dictionary. The filter is block-local: block
N is still decoded from its own record, its dictionary and its literal references alone, so random
access (G1) is unaffected. A block chosen for filtering that does not shrink still falls back to a RAW
or RLE record without the bit set (sections 11.10 and 11.11).

With x the raw bytes, y the filtered bytes and n the block length, the encoder produces y as follows and
the decoder applies the exact inverse to the output of phase B:

- mode 1, byte-wise delta: y[0] = x[0], and y[i] = (x[i] - x[i-1]) mod 256 for 1 <= i < n. The inverse is
  x[0] = y[0] and x[i] = (y[i] + x[i-1]) mod 256, a sequential prefix-sum scan.
- mode 2, stride shuffle: let N = filter_param and R = ceil(n / N). The block is viewed as R rows of N
  bytes in row-major order, the last row possibly short. The shuffle writes the columns in order, skipping
  positions past n: iterate c = 0 to N - 1 and, for each c, r = 0 to R - 1 with r*N + c < n, taking the
  next output byte as y[k] = x[r*N + c]. The inverse walks the same index sequence and stores
  x[r*N + c] = y[k]. The length is preserved.

Both filters preserve length and are one linear pass; the mode-1 inverse is a scan (a serial dependency,
vectorisable with a prefix-sum carry) and the mode-2 inverse is a permutation, for which a decoder MAY
use a scratch buffer of at most the block size. A decoder MUST reject an out-of-range descriptor and MUST
NOT invert a filter outside OUT (V11).

## 8 Sequence coding

### 8.1 Value codes

Each of the three fields is mapped to a non-negative value v and coded as a symbol (a "code") from a
66-symbol alphabet plus raw extra bits. The mapping is the same for all three fields:

```
value -> (code, extra_bits, extra_value)
  if v < 8:   code = v;  nbits = 0
  else:       e     = floor(log2(v))           (e >= 3)
              m     = (v >> (e - 1)) & 1
              code  = 8 + 2 * (e - 3) + m
              nbits = e - 1
              extra = v & ((1 << nbits) - 1)

code -> value
  if code < 8:  v = code
  else:         e = 3 + ((code - 8) >> 1);  m = (code - 8) & 1
                v = (1 << e) + (m << (e - 1)) + extra     (nbits = e - 1)
```

For example: v = 8..11 is code 8 with 2 extra bits; v = 12..15 is code 9; v = 16..23 is code 10
with 3 extra bits; v = 2^31 + ... reaches code 65. One extra mantissa bit is thus carried in the code
itself, which keeps the distribution of the code symbols informative while the remaining bits are close
to uniform.

| Field | Value v | Range of the field |
|---|---|---|
| LL | LL | 0 to 2^32 - 1 |
| ML | ML - 3 | ML from 3 |
| OF | offbase - 1 | offbase from 1; see section 8.2 |

### 8.2 Offsets and repeat offsets

The decoded offbase has three meanings: offbase 1, 2 and 3 select the repeat offsets rep0, rep1 and
rep2; offbase >= 4 is an explicit offset d = offbase - 3. At the start of every segment the repeat
offsets are (rep0, rep1, rep2) = (1, 4, 8); they do not carry over from the previous segment. This
costs a negligible amount of ratio (one reset per 128-256 KiB) and makes the phase A of different
segments fully independent. After each sequence they are updated by move-to-front:

```
offbase == 1:  d = rep0                                   (no change)
offbase == 2:  d = rep1;  (rep0, rep1) = (rep1, rep0)
offbase == 3:  d = rep2;  (rep0, rep1, rep2) = (rep2, rep0, rep1)
offbase >= 4:  d = offbase - 3;  (rep0, rep1, rep2) = (d, rep0, rep1)
```

Unlike zstd there is no special case when LL = 0. A sequence with LL = 0 and offbase 1 is legal; it
simply continues the previous match. Encoders normally avoid it by extending the previous match.

### 8.3 Streams

A segment has three independent sequence bitstreams, LL, ML and OF, each holding n_seq symbols. They
are separate so that three decoders can run with no shared bit-reader dependency. Each stream carries
its own extra bits, interleaved with its state transitions (section 8.4).

### 8.4 The tANS bitstream

Each stream uses the backward bitstream of RFC 8878 section 4.1: the last byte of the stream MUST be
non-zero, its highest set bit is a sentinel, and the bits below the sentinel followed by the bits of
earlier bytes (from bit 7 down to bit 0 in each) are consumed in that order. A read of k bits returns
an integer in which the first bit consumed is the most significant. After the last read, the consumed
position MUST be exactly the start of the stream, otherwise the stream is invalid.

Decoding uses a tANS table built from the normalised counts by the procedure of Annex A. For a stream
of n symbols:

```
state = read(AL)
for i in 0 .. n-1:
    e   = T[state]                 // table T chosen per section 8.6
    sym = e.symbol
    v   = base(sym) + read(nbits(sym))     // extra bits, section 8.1
    output v
    if i < n-1:  state = e.baseline + read(e.nbits)
```

The encoder processes symbols from last to first, writing the bits so that this order is reproduced.
Because the sentinel and the final-position check make the stream length exact, no length is stored
for the stream beyond its byte size.

### 8.5 RLE streams

In RLE mode every symbol of the stream is the one given code. There is no state; the bitstream
contains only the extra bits of each value, in order, using the same backward format. A stream of codes
below 8 therefore has no bits at all and a size of 1 byte (the bare sentinel 0x01).

### 8.6 Context conditioning (optional per segment)

When bit 3 or bit 4 of segment_flags is set, the OF and/or LL streams use three tables each, selected
per symbol by a small class of the match length:

```
mlclass(ML) = 0 if ML - 3 < 4      (ML 3..6)
              1 if ML - 3 < 16     (ML 7..18)
              2 otherwise
OF(i) uses the OF table of class mlclass(ML(i))               (bit 3)
LL(i) uses the LL table of class mlclass(ML(i-1)); for i = 0, class 0   (bit 4)
```

All tables of one stream MUST share the same accuracy log, because the state produced by one table is
used as an index into the next. This is sound for tANS: the encoder, working backwards, picks for each
symbol a state in the table that the decoder will use for that symbol, and the symbol's ranges always
partition the state space. The decode order within a sequence is ML, then OF, then LL, so that every
class is known when it is needed.

The expected gain is about 1-3% of sequence bits on data where offsets correlate with match lengths.
The option exists so that the cost can be paid only by segments that benefit.

## 9 Literal coding

### 9.1 The literal stream

The literals of a segment are coded as a sequence Z of n_lit bytes, in output order. For plain
segments Z[j] is the literal byte itself; for sub-literal segments it is a residual (section 9.6).
Literals are coded either raw (the stream is the n_lit bytes of Z, lit_size = n_lit) or by rANS.

### 9.2 Lane partition

For rANS the sequence Z is cut into 8 contiguous slices, one per lane. With q = floor(n_lit / 8) and
r = n_lit mod 8, lane k holds q + 1 symbols if k < r and q symbols otherwise; slice k starts at
start(k) = k*q + min(k, r). Lanes are independent entropy decoders, so their dependency chains overlap
in the processor's out-of-order window.

### 9.3 Contexts and run starts

Let LL values be known (the sequence streams are decoded first). The literal at index j is a run start
if it is the first literal of a literal run: that is, if there is a sequence i with LL(i) > 0 such that
j equals the sum of LL(t) over t < i, or if tail literals exist and j equals the sum of all LL values.
The context of literal j is chosen as follows when k > 1:

```
if j == start(lane) or runstart[j]:   context = k            // run-start table
else:                                 context = class_map[ classify(Z[j-1]) ]
```

classify is the function selected by ctx_mode (Annex C), applied to the previous coded byte of the same
slice. For k = 1 there is a single table and no context logic. The reason for the run-start table is
that the previous literal of a run start is the last literal of an unrelated earlier run, so using it
as a context would add noise to roughly a quarter of all literals in a typical parse. The context never
depends on the decoded output bytes, only on Z and on the LL values, so literals can be decoded before
the sequences are executed.

### 9.4 rANS parameters

| Parameter | Value |
|---|---|
| Probability scale M | 2^11 = 2048 (accuracy log 11 in the table description) |
| State | 32 bits, invariant range [2^16, 2^32) |
| Renormalisation | 16-bit words, little-endian, b = 2^16 |
| Frequency of symbol s | f(s) = count(s), with a count of -1 ("less than one") read as 1; zero counts mean the symbol never occurs |
| Cumulative start c(s) | sum of f(t) for t < s |

Decoding one symbol from state x:

```
slot = x & 2047
s    = symbol_of_slot[slot]
x    = f(s) * (x >> 11) + slot - c(s)
if x < 65536:  x = (x << 16) | next_word()
```

### 9.5 Literal stream layout

```
bytes 0..31     : initial states x0..x7 (u32 each)
bytes 32..end   : 16-bit renormalisation words, consumed in decode order
```

Decoding proceeds in rounds r = 0, 1, ...; within a round the lanes k = 0 to 7 decode in order the
symbol at index start(k) + r, provided r is less than the lane's length, and perform renormalisation
immediately, taking words from a single shared cursor. After the last symbol, each lane's state MUST
equal 2^16 and the cursor MUST be at the end of the stream; otherwise the stream is invalid. When
n_lit = 0 the stream is empty (lit_size = 0). When 0 < n_lit < 8 the unused lanes still have a state,
2^16, in the header. The encoder produces the stream by encoding the symbols in reverse decode order
starting from state 2^16 in each lane and then reversing the emitted words.

### 9.6 Sub-literals

With lit_form = 1 the coded value is a residual against the byte at the current repeat offset. Let the
literal run belonging to sequence i be given and let rep0 be the value of rep0 in effect before sequence
i (for tail literals, after the last sequence). For t = 0, 1, ... within the run, with VB positions
measured as in section 6.1:

```
pred = (rep0 <= vbpos) ? VB[vbpos - rep0] : 0     // vbpos = write position of this byte
out  = (Z[j] + pred) mod 256
```

When rep0 is smaller than the run length, later bytes of the run use earlier bytes of the same run as
predictors, so the reconstruction is sequential. Sub-literals typically help on structured binary data
(tables, records with fixed strides, images, audio) where the byte one stride back predicts the current
byte. Phase B performs the addition during the literal copy with one vector add per 32 bytes when
rep0 >= 32.

### 9.7 Implementation note: fused decode tables (informative)

Each decode step needs the symbol, its frequency and cumulative start, and the next context. A
practical layout is a 2048-entry slot table of 16-bit entries holding (symbol, context of the next
literal) so that one load yields both, plus a 256-entry table of (f, slot offset) per context. The
table set of one context is about 3 KiB, so eight contexts plus the run-start table occupy roughly
27 KiB, close to the 32 KiB L1 data cache of the reference platform; segments with fewer contexts are
correspondingly cheaper. The dependency chain per lane is slot load, frequency load, multiply, add and
conditional renormalisation, which is about 20 cycles and is hidden by the eight lanes down to roughly
2.5 cycles per symbol.

## 10 Decoding procedure

### 10.1 Overview

Decoding a COMPRESSED block has two phases. Phase A turns each segment's bitstreams into plain arrays
and validates them completely. Phase B executes the sequences and writes the output. Because phase A
establishes every invariant that phase B relies on, phase B contains no bounds checks and, apart from
rarely taken length loops, no data-dependent branches. A conforming decoder MUST NOT produce output
that depends on an unvalidated input value, but MAY organise the work in any way that yields the same
result.

### 10.2 Phase A (per segment)

1. Parse the segment header and resolve table inheritance (section 7.4). Build the decoding tables (Annexes A and B), or reuse tables from the previous segment.
2. Decode the LL stream fully into an array of n_seq u32 values. Compute the prefix sums to obtain the run-start bitmap of the literal stream.
3. Decode the literal stream into Z (section 9), choosing contexts with the bitmap. For plain segments Z is the literal buffer; for sub-literal segments Z holds residuals.
4. Decode the ML and OF streams in batches of about 1024 sequences into structure-of-arrays form (ml, ofbase). If conditioning is enabled, decode ML first within each sequence, then OF, as in section 8.6.
5. Resolve repeat offsets sequentially, starting from (1, 4, 8) at the start of the segment, producing the explicit offset off[i] and the value rep0_before[i] needed by sub-literals.
6. Run the validation pass of section 10.3 over the arrays.

Because repeat offsets restart at every segment, the phase A of different segments of one block share
nothing but the stored tables and can run on different threads.

### 10.3 Validation rules

A decoder MUST reject the block if any of the following fails. Items V2 to V4 are checked in a single
vectorisable pass over the arrays, after the prefix sums of LL + ML have been computed.

| Rule | Condition |
|---|---|
| V1 | raw_len of the segment equals n_lit plus the sum of ML; the sum of LL does not exceed n_lit; all running sums fit in 32 bits. |
| V2 | For every sequence, off[i] <= Ld + Ll + (output position of the first match byte). |
| V3 | ML[i] >= 3 and off[i] >= 1 (the latter holds by construction, but MUST be checked for repeat offsets after resolution). |
| V4 | The total output of the block does not exceed raw_size, and the sum of segment raw_len equals raw_size. |
| V5 | Every bitstream has a non-zero final byte and is consumed exactly (sections 8.4 and 9.5), and the rANS lanes end at state 2^16. |
| V6 | Every FSE description is well formed (counts sum to 2^AL, AL in range, no truncation); RLE symbol codes are below 66. |
| V7 | All reserved bits are zero; record_type, ctx_mode, ctx_count and lit_coder hold defined values; class_map entries are below k. |
| V8 | Segment sizes, stream sizes and varints are consistent with the payload size and do not run past it. |
| V9 | Literal references (if any) satisfy the conditions of section 6.3; dictionary lookups succeed and hashes match. |
| V10 | The block checksum, if present, matches the decoded output. |
| V11 | A block carrying FILTERED is COMPRESSED with lit_ref_depth 0 and no LIT_EXPORTABLE flag, is not a literal-reference predecessor, and its filter descriptor holds a defined mode with an in-range parameter; the inverse filter (section 7.5) stays inside OUT. |

### 10.4 Phase B (execute)

For each sequence i the executor copies LL[i] literals from the literal buffer, then copies a match of
ML[i] bytes from distance off[i]. After the last sequence it copies the tail literals. For sub-literal
segments the literal copy adds the predictor.

| Case | Method |
|---|---|
| Literals, plain | Copy in 32-byte chunks without regard to the exact length (a loop that normally runs once); the surplus is overwritten later or lies in the slack. |
| Literals, sub-literal, rep0 >= 32 | For each 32-byte chunk: store(load(lit) + load(dst - rep0)), byte-wise addition. |
| Literals, sub-literal, rep0 < 32 | Byte-sequential reconstruction; slower, and the encoder's coder selection accounts for it (section 11.9). |
| Match, offset >= 32 | 32-byte chunks from dst - off. |
| Match, 16 <= offset < 32 | 16-byte chunks; 32-byte chunks would read bytes of the same store. |
| Match, offset < 16 | Build the repeating pattern in a vector register with a byte-shuffle (or a broadcast for 1, 2, 4, 8), store it repeatedly with a stride equal to the largest multiple of the offset not above 16. |
| Prefetch | Eight sequences ahead, prefetch the match source if the offset exceeds about 384 KiB (roughly the L2 size of the reference platform); for nearer sources select a hot dummy address instead of branching. |
| Pre-filter inverse | For a FILTERED block, after all literals and matches are written, one linear pass over OUT that reverses section 7.5 (a prefix-sum scan for mode 1, a permutation for mode 2). |

After the tail literals are copied, a block whose record has FILTERED set is passed through the inverse
filter in place over OUT, and the block checksum (V10) is then checked against these unfiltered bytes.

Annex D contains reference routines. A scalar implementation with identical results is conforming; the
SIMD forms exist for speed only.

### 10.5 Buffer slack

Because chunked copies may write past the logical end of a literal run or a match, the output buffer
MUST have at least 64 bytes of writable slack beyond raw_size, and the literal buffer at least 32 bytes
beyond n_lit. The slack content is undefined and is never read as input. The last few sequences of a
block (those within 64 bytes of the end) SHOULD be executed with a safe scalar path; this costs one
branch per block.

### 10.6 Parallelism inside and across blocks

- Across blocks: blocks are independent. A decoder may decode any number of blocks concurrently; the only shared state is the dictionary cache.
- Within a block: phase A of all segments may run concurrently once the segment headers have been parsed. Phase B is sequential within a block because a match may reference earlier segments' output. A worker pool can therefore run phase A ahead of phase B, which lowers latency for single-block random reads.
- Within a segment: eight rANS lanes and three tANS streams give instruction-level parallelism; no thread synchronisation is needed.

### 10.7 Streaming consumers

A consumer that reads the whole file sequentially decodes blocks in order using a worker pool, with
output buffers returned in order. Working-set guidance is in section 12.3.

## 11 Encoder guidance (informative)

Nothing in this section constrains a conforming encoder; any stream that satisfies the format is valid.
The guidance describes the reference encoder, which is designed to reach the goals of section 1.2.

### 11.1 Pipeline

1. Dictionary pass: for each epoch, train a candidate dictionary from the preceding epoch's raw data (section 5.7) and decide by sampling whether to keep it (section 5.5).
2. Block pass, in parallel across blocks: choose the block type (section 11.10); optionally pre-filter the block (section 11.11); parse into sequences; segment; build context maps and tables; entropy code; choose per-segment options; emit the record.
3. Assemble records in order, write dictionary records before their first user, write footer and trailer.

### 11.2 Match finder tiers

| Tier | Table layout | Probes | Lazy | Skip shift |
|---|---|---|---|---|
| fast | 4-entry buckets, packed 32-bit entries | rep0, long table, short table (depth 2) | none | 5-6 |
| main | 8-entry buckets, packed | rep0, rep1, rep2, long, short | one step | 7-8 |
| high | 16-slot rows with separate 8-bit tag arrays | rep0, rep1, rep2, long, short, deeper | one step, price-based | 8 |

- Entries in packed tables are 32 bits: a 24-bit position in the virtual buffer and an 8-bit tag from spare hash bits. Eight packed entries fill one 32-byte vector, so a bucket is one aligned load and one SIMD tag comparison. With 16-slot rows, separate tag arrays are better: 16 tags compare in one 128-bit operation and only hit positions are loaded.
- Hashing: the short table hashes 5 bytes (4 bytes for data classified as binary); the long table hashes 8 bytes with one entry per bucket. A 5-byte hash cannot find 4-byte matches, so the minimum non-repeat match is 5, with 4 permitted for repeat offsets.
- Insertion: insert at visited positions only. Inside a match insert 2-3 sampled positions (fast and main tiers); insert every position only in the high tier once price-based parsing exists. A bucket is loaded once per position and used for both lookup and insert; the insert shifts entries down one lane with a permute and writes the new entry at lane 0 so that buckets stay ordered newest first.
- Table size: begin with 2^16 entries (256 KiB, resident in the 512 KiB L2) for fast and main, and 2^18 for high, and let measurement decide. Cap the per-thread total at roughly 1-1.5 MiB when running one worker per core, because the sum across threads otherwise exceeds the shared L3.
- Memory: allocate tables and buffers 2 MiB aligned and advise huge pages; with 4 KiB pages the 2048-entry L2 TLB covers only 8 MiB.

### 11.3 Per-position order and pipelining

1. Compute the hashes for position ip + D*step and prefetch the bucket line, but only for positions the skip schedule will actually visit.
2. At ip: check rep0 (rep0..rep2 on main/high with one 4-byte compare each; rep0 only on fast). Probe the long table, then the short bucket.
3. Compare tags with a SIMD instruction; extract the first two hits with trailing-zero count and clear-lowest-bit; an empty mask selects a padded dummy slot, so no branch is needed.
4. Load 8 bytes at each candidate, XOR with the current 8 bytes, count trailing zero bytes to get the length (at most 8); select the best by conditional move on the price score; extend beyond 8 bytes with 32-byte compares in a loop that normally runs once.
5. Lazy check at ip + 1 with the same score, again by conditional move.

The one unpredictable branch left is "found or not found". The skip schedule is step = ((ip - anchor) >> shift) + 1, with shift between 5 and 8; the value 8 gives the best ratio, and 5-6 is faster on incompressible data.

### 11.4 Price gate

A match is accepted only if its estimated saving is positive: len * Lbar > cost(LL, ML, OF), where Lbar
is the running average cost in bits of a literal and cost is read from small tables derived from the
previous segment's actual code statistics (offset cost from the leading-zero count of the distance plus
extra bits). The initial estimate is 6 bits per literal. This replaces fixed offset thresholds, adapts
as context modelling makes literals cheaper, and lowers the sequence count, which speeds decoding. The
same score drives the lazy comparison. A fixed fallback (minimum length 5, 6 beyond 256 KiB) is
acceptable in the first implementation.

### 11.5 Segmentation

Split the parse into granules of about 16K sequences. Each granule has literal and sequence histograms,
which are additive. Greedily merge adjacent granules when the entropy-coded size with one set of tables,
including the table header bytes, is not larger than with two. Because merge tests are sums of
256-element histograms, the cost is small. Segment boundaries always fall between sequences so that a
segment owns its literals entirely.

### 11.6 Context map construction

1. For each block choose the context mode (LSB6, MSB6, TEXT, or SIGNED for sub-literal segments) by estimating the entropy with each.
2. Accumulate the next-byte histogram of each of the 64 classes over the whole block (not per segment), skipping run-start literals which use their own table.
3. Cluster the 64 classes greedily, merging the pair whose merge increases coded size least, until k = 1, 2, 4 or 8 contexts remain; pick the k that minimises coded size plus header cost.
4. Emit the map once per block. Segments then only choose frequency tables.

The gain over order-0 depends heavily on the data; 3-6% of literal bits on text, JSON and source is the
working estimate and must be verified by the experiment in section 15.

### 11.7 Per-segment coder selection

For each segment compare the candidates raw, rANS with one table, and rANS with contexts, each with and
without sub-literals, scoring bits + lambda x decode_cycles. The decode cycle estimates are small
constants measured once per platform (the sub-literal path with rep0 < 32 is the slow outlier). Lambda
is the speed/ratio dial: zero maximises ratio, and larger values prefer raw literals and lower-order
models. Each tier of the encoder maps to a lambda.

### 11.8 Parallel encoding and memory

- Blocks are independent once dictionaries are known, so a thread pool takes one block at a time. Per-thread state is the hash tables (section 11.2), the block input, the sequence arrays and the output.
- For each file, hash the dictionary once into a table snapshot and copy it into each worker's table at block start (a memcpy of the table, about 0.1 ms for 2 MiB), rather than re-inserting the dictionary per block.
- Measure SMT (12 threads on the reference CPU) against one thread per core; encoding is bound by probe latency and SMT often gains 20-30%. Also compare interleaving two block parses within a thread, which overlaps cache misses without hardware threading.

### 11.9 Block similarity sketches and optional frame reordering (informative)

An encoder MAY cluster or reorder blocks by content similarity so that similar
blocks share an epoch dictionary or sit near each other for literal references.
Any reordering MUST preserve random access (G1) and MUST stay at RAM speed
with bounded extra RAM; it MUST NOT affect decode correctness or speed.

- Sketch per block (one pass, memory-bandwidth speed, about 0.1 ms per
  256 KiB). Build a 256-bin byte-count table in a single pass over the
  block's raw bytes:

```c
uint32_t h[256] = {0};
for (size_t i = 0; i < n; i++) h[p[i]]++;   // use 4 interleaved tables to avoid store-forwarding stalls
```

- Similarity as a single number: L1 distance sum |ha[i] - hb[i]|
  (0 = identical distribution, 2n = disjoint). Cheap, SIMD-friendly, and
  good enough for clustering.
- If order matters too (it often does for LZ-style compressors),
  histograms miss it. Two upgrades, still fast:
  - Order-1 sketch: hash byte pairs into a 4096-bin table
    ((a << 4 ^ b) & 0xFFF or similar) and L1 that instead. Catches
    text-vs-binary-vs-structured differences that order-0 cannot.
  - MinHash on 4-byte shingles: hash every 4-byte window, keep the
    k smallest (k = 64-128) per block, and estimate Jaccard from overlap.
    This approximates "how many matches would an LZ window find across
    these blocks", which is closer to what actually drives compression
    gain. Costs about 1 multiply-hash per byte, still near RAM speed if
    sampled (for example only hash windows where hash & 7 == 0).
- Compute each block's sketch once (O(n) over blocks), then run all
  pairwise comparisons on the tiny sketches, not on the block data. That
  makes reordering/clustering over thousands of blocks essentially free.

Constraints when reordering is used:

- Footer block entries remain indexed by logical block number
  (b = X >> block_log2); the entry offset gives the physical record
  position, so physical file order MAY differ from logical order without
  changing the random-access procedure of section 4.6.
- Sequential readers MUST reassemble output in logical block order, not
  file order.
- A DICT record MUST still precede its first user in file order, so a
  single-pass reader without a footer keeps working.
- Encoder SHOULD cap sketch memory (order-0: 1 KiB per block;
  order-1: up to 16 KiB per block; MinHash: k u32/u64 per block),
  keep the clustering pass itself at RAM speed, and only keep a
  permutation when measured saving exceeds its footer/index cost.
  Decoder working set and speed are unchanged.

### 11.10 Cheap block-type selection (informative)

Before entropy coding a block, the encoder SHOULD choose among the three record types of section 4.2
using quantities it has already computed, so the choice costs almost nothing:

- RLE (record type 2) when every byte of the block is equal. One pass establishes this, and the record is
  a single byte in a 24-byte header, so an all-same block becomes almost free.
- RAW (record type 1) when the best COMPRESSED payload is not smaller than the raw size. The encoder knows
  the payload size after encoding the block, so this costs no second pass: if it is not smaller, it
  discards the payload and writes the raw bytes. This is the common case for incompressible data and it
  skips the entropy stage entirely, which is a net speedup rather than a cost.
- Otherwise COMPRESSED (record type 0).

This is a pure encoder policy: the format already provides all three record types and a decoder needs no
change. It is the cheapest ratio-preserving speed win available, and it is the reason a decoder never
spends entropy-decode cycles on data that carries no entropy.

Repcodes are the other near-free win and are already normative in section 8.2. Checking rep0, and where
the tier allows rep1, before the hash lookup costs one compare each and often skips the search, so an
encoder SHOULD probe them first, as section 11.2 does. Structured data (records, tables, source) repeats
offsets heavily, which is much of why the format can approach zstd on that data.

### 11.11 Sampled filter gating (informative)

Section 7.5 lets a block be pre-filtered, but a filter helps only some data and costs one extra decode
pass when it is used. An encoder SHOULD gate it on a measurement and enable it only where it pays:

- Sample about 1 KiB of the block, for example 16 groups of 64 bytes spread across it, and estimate the
  order-0 entropy of the sample as-is and after each candidate filter.
- Consider the byte-wise delta (mode 1) and a small set of shuffle strides (mode 2), for example
  N in {2, 4, 8, 16, 32, 64}, and enable the filter whose filtered sample entropy is lowest, but only if
  it is lower than the raw entropy by a margin that covers the descriptor and the inverse pass (a few
  hundred cycles per block).
- On text, source and JSON the gate normally declines, so those blocks pay only the sample, which is
  noise against a 128 KiB to 2 MiB block. On numeric arrays, columnar data, fixed-stride records and
  images it accepts, and the whole-block transform lets the match finder see repeated structure that
  byte-order data hides.

The gate keeps the stage free where it does not help, so the filter is a data-dependent upside rather
than a fixed cost and never touches a block that does not opt in. A filtered block still passes through
the RAW/RLE fallback of section 11.10: if the filtered payload is not smaller than the raw data, the
encoder emits the unfiltered block as RAW and drops the filter.

## 12 Performance model and targets (informative)

### 12.1 Cycle budgets

At 4.6 GHz, 350 MB/s is about 13 cycles per input byte. With an average parse step of 1.5-2 bytes that
is roughly 20-26 cycles per match-finder iteration, within which the loop of section 11.3 must fit. If
it does not, the tier table lets the encoder shed work: shallower buckets, no lazy step, fewer inserts.

### 12.2 Decode cost structure

- Execution cost scales with the number of sequences, not bytes. The price gate and the minimum-match rule therefore improve decode speed as well as ratio.
- The literal lanes cost roughly 2.5 cycles per symbol (section 9.7), about 1.8 GB/s of literal output at 4.6 GHz if the estimate holds. With literals at about 30% of the output, literal decode takes a large share of a 2 GB/s budget, which is why the lane layout and fused tables matter.
- Separate LL, ML and OF streams remove the single shared bit reader that serialises zstd's sequence decoding.
- Branch mispredictions on lengths, not copy bandwidth, limit phase B. The unconditional first chunk and the offset-class dispatch are well predicted on typical data.

### 12.3 Working sets

A decode worker needs roughly the block output, its literal buffer (30-40% of the output), the
dictionary, and small sequence arrays: about 1.3 B + Ld for block size B. With six workers on the
reference platform's 32 MiB L3, a 2 MiB block gives about 22 MiB in total and fits; 4 MiB blocks give
about 37 MiB and spill. The default block size is therefore 2 MiB, with 1 MiB worth testing in
combination with a larger dictionary.

### 12.4 Targets

| Metric | Target (budget, to be tested) |
|---|---|
| Encode, main tier | about zstd -4 speed (roughly 300-350 MB/s per thread), with table caps and huge pages; scaling near-linear across blocks. |
| Decode | at least 1.2 times the measured zstd -6 decode speed on the same machine; 2 GB/s per thread is aspirational and depends on a low sequence count. |
| Ratio | roughly zstd -6 parity on text, source and JSON if contexts, segments, repcodes and dictionaries deliver; binary data may trail without sub-literals. |
| Aggregate decode, 6 workers | a hypothesis of 7-10 GB/s, to be checked against L3 behaviour. |
| Random read latency | dictionary fetch plus decode of one 2 MiB block, a few milliseconds from memory. |

Calibrate against zstd -b4 -e6 on the same machine with the CPU frequency pinned, and treat the zstd
figures quoted in earlier notes (about 300-400 MB/s encode at level 4, 100-150 MB/s at level 6,
1.3-1.8 GB/s decode) as placeholders until measured.

## 13 Robustness and security

- Hostile input. All sizes, counts and offsets come from the stream. Phase A validates every one before phase B uses it (section 10.3), so phase B cannot read or write out of bounds regardless of the input. The only unchecked writes are into the declared slack.
- Memory limits. The frame header declares block_log2 and max_dict_log2, so a decoder can compute an upper bound on per-worker memory (about 2^block_log2 + 2^max_dict_log2 + literal references + slack) before decoding anything and refuse frames above its configured limit.
- Decompression bombs. raw_size is bounded by the block size; RLE blocks are checked against it; the total content size, if declared, is checked against the sum of blocks.
- Integrity. Header, footer and trailer carry CRC32C. Block checksums are optional; with them, random access can verify the data returned. CRC32C is not a defence against deliberate tampering; applications needing that must authenticate the file separately.
- Fuzzing. The decoder MUST be fuzzed with structure-aware mutation of every header field and stream, with the validation rules as the oracle.
- Timing. The format makes no constant-time claims.

## 14 Versioning and extensibility

- The frame header carries a major and minor version. A major increment may change anything. A minor increment may only add features that are announced by a flag bit or a previously reserved value, so that an older decoder fails cleanly.
- Reserved bits and values MUST be written as zero and MUST be rejected when non-zero, so that later versions can assign them without ambiguity.
- Records with type 0x80 to 0xFF are skippable and may carry metadata (for example file names or encoder statistics) without affecting decoders.
- Reserved candidates for future minor versions: a Huffman literal coder (lit_coder 2), additional context modes, a 64-bit checksum, and a wider record size for blocks above 4 GiB.
- Conformance levels: Core is everything except literal references; Extended adds literal references (section 6.3). A decoder declares the level it implement.

## 15 Verification plan and milestones

### 15.1 Experiments, in order

1. Feed zstd -6's exact sequences (via ZSTD_generateSequences, if the zstd version has it) through zGEC's entropy stage, comparing: order-0, contexts without the run-start fix, and contexts with it. This answers whether contexts are worth building.
2. Repeat with sub-literals on binary and JSON data. Because zstd's parse is tuned to Huffman costs, the result understates zGEC's gain; also run it with zGEC's own parse.
3. On zGEC's parse: table size 2^16 against 2^18; price gate against fixed thresholds; skip shift 5 to 8.
4. Epoch dictionaries: blocks of 256 KiB, 1 MiB and 2 MiB with and without dictionaries, with E = 5, 10, 20; ratio against random-read latency. Include the literal-reference option (D = 0 to 3) as a separate arm to decide whether it earns its place.
5. Block pre-filter (section 7.5): on numeric arrays, columnar data and images, compare the byte-wise delta and a few shuffle strides under the sampled gate of section 11.11 against no filter, measuring both ratio and decode cycles; include the descriptor overhead and the RAW/RLE fallback.
6. Microbenchmarks last: the rANS chain with and without table fusion; three sequence streams against a shared one; the three copy paths; SMT against two-parse interleaving.

Benchmark with lzbench on the Silesia corpus plus JSON, source-tree and log corpora representative of
the intended use, against zstd -b4 -e6, with CPU frequency pinned and the machine otherwise idle.

### 15.2 Conformance testing

- Round-trip tests of every record type, every segment flag combination and every sequence feature, on synthetic and real data.
- Differential testing between the scalar reference decoder and each SIMD path.
- Random-access tests: decode every block individually, in random order, and compare against sequential decoding, including filtered blocks.
- Filtered round-trips: one block with each filter mode under the sampled gate, and a negative test that a FILTERED block used as a literal-reference predecessor is rejected (V11).
- Negative tests: one malformed input per validation rule V1 to V10.

### 15.3 Milestones

| Step | Deliverable |
|---|---|
| M1 | Match finder with cost model and a harness that injects external sequences; measurement of candidates per position and estimated coded size. |
| M2 | Container, segments, tANS sequence streams and scalar order-0 rANS literals; round-trip with random access. |
| M3 | Learned contexts with the run-start table, adaptive segmentation, price gate. |
| M4 | Phase A/B decoder with validation, SIMD copy paths, threading. |
| M5 | Epoch dictionaries with training and the ratio gate; sub-literals. |
| M6 | Optional features: sequence conditioning, literal references, the block pre-filter, Huffman comparison; tuning and release. |

## 16 Open issues and risks

| Item | Why it matters / how it is resolved |
|---|---|
| Context gain may be smaller than estimated, or header cost may absorb it | Experiment 1 of section 15.1; per-segment cost check makes the decision data-driven, so the downside is bounded. |
| Cycle budget of the match-finder loop | The tier table allows shedding work; measure per-iteration cycles early (M1). |
| Store-forwarding stalls on short-offset copies | Hardware-dependent; benchmark scalar starts against the shuffle path on real data. |
| Number of outstanding L1 misses on Zen 3 | Believed to be about 22-24, not Intel's 10-12; verify by microbenchmark before sizing prefetch distances. |
| Dictionary benefit versus cost | The ratio gate (section 5.5) makes the dictionary optional per epoch and per block; experiment 4 sets defaults for size and E. |
| Literal references may not justify their complexity | Kept as Extended level; decide after experiment 4 whether the reference encoder implements it. |
| Huffman versus rANS with contexts | Reserved in the format; benchmark once contexts exist. |
| Whether the block pre-filter earns its stage | Section 11.11 gates it on a sample, so the downside is bounded; experiment 5 of section 15.1 decides whether the reference encoder enables it and with which strides. |
| Cross-block entropy-table reuse | Declined in section 7.4: it would weaken G1 random access for a saving of a few hundred bytes per block; revisit only if the default block size drops well below 1 MiB. |
| Default block size | Between 1 and 2 MiB pending experiments 3 and 4 and the L3 working-set analysis. |
| Small files | A file with fewer blocks than workers cannot use all cores; blocks may be split to a smaller block_log2 for such files, at some ratio cost. |

## Annex A tANS table construction

This annex defines how decoding tables are built from normalised counts N[s], s = 0 to nsym - 1, with
accuracy log AL and size S = 2^AL. A count of -1 denotes "probability less than one", which occupies
one cell.

```
table[0..S-1]                       // symbol of each cell
high = S - 1
for s in 0 .. nsym-1:               // place "less than one" symbols at the end
    if N[s] == -1:  table[high--] = s
step = (S >> 1) + (S >> 3) + 3
mask = S - 1
pos  = 0
for s in 0 .. nsym-1:               // spread the rest
    for i in 0 .. N[s]-1  (N[s] > 0):
        table[pos] = s
        do  pos = (pos + step) & mask  while pos > high
// pos must end at 0

next[s] = (N[s] == -1) ? 1 : N[s]
for i in 0 .. S-1:
    s       = table[i]
    ns      = next[s]++
    nbBits  = AL - floor(log2(ns))
    T[i]    = { symbol = s, nbBits = nbBits, baseline = (ns << nbBits) - S }
```

Decoding uses T[state] as in section 8.4. The construction is the one used by zstd's FSE; it MUST be
followed exactly because the encoder's state assignment depends on it.

## Annex B rANS details

### B.1 Decoder tables

```
for s in 0 .. 255:
    f[s] = (N[s] == -1) ? 1 : N[s]          // 0 allowed
    c[s] = sum(f[t] for t < s)
for s in 0 .. 255:
    for slot in c[s] .. c[s] + f[s] - 1:
        symbol_of_slot[slot] = s            // 2048 entries
```

### B.2 Encoder step (informative)

The encoder processes symbols in reverse decode order. With x in [2^16, 2^32), before encoding symbol
s it renormalises, then applies the coding step:

```
xmax = f[s] << 21                           // ((2^16 >> 11) << 16) * f[s]
if x >= xmax:  emit_word(x & 0xFFFF);  x >>= 16
x = ((x / f[s]) << 11) + (x % f[s]) + c[s]
```

The division may be replaced by a multiplication with a precomputed reciprocal. The words are emitted in
reverse order and are reversed once the lane's symbols are finished; with eight lanes the emission order
across lanes is the reverse of the decode order of section 9.5.

## Annex C Context classification functions

classify(b) maps a byte to one of 64 classes. Class map entries are indexed by the result.

| ctx_mode | Function |
|---|---|
| 1 LSB6 | b & 63. |
| 2 MSB6 | b >> 2. |
| 4 SIGNED | Let s be b read as a signed 8-bit value. class = clamp(s, -32, 31) + 32. Intended for residual (sub-literal) bytes, which cluster around zero. |
| 3 TEXT | See the table below. |

| Byte range | Class | Notes |
|---|---|---|
| 0x0A | 0 | line feed |
| 0x09, 0x0D | 1 | tab, carriage return |
| 0x00 | 2 | NUL |
| other 0x01-0x1F | 3 | remaining control characters |
| 0x20-0x2F | 4 + (b - 0x20) | space and punctuation, one class each (4 to 19) |
| 0x30-0x39 | 20 | digits |
| 0x3A-0x40 | 21 + (b - 0x3A) | punctuation (21 to 27) |
| 0x41-0x5A | 28 | upper-case letters |
| 0x5B-0x60 | 29 + (b - 0x5B) | punctuation (29 to 34) |
| a, e, i, o, u | 35, 36, 37, 38, 39 | lower-case vowels |
| other 0x61-0x7A | 40 | lower-case consonants |
| 0x7B-0x7F | 41 + (b - 0x7B) | punctuation and DEL (41 to 45) |
| 0x80-0xBF | 46 | UTF-8 continuation bytes |
| 0xC0-0xDF | 47 | UTF-8 two-byte leads |
| 0xE0-0xEF | 48 | UTF-8 three-byte leads |
| 0xF0-0xFF | 49 | UTF-8 four-byte leads and invalid |
| unused | 50-63 | never produced by TEXT |

## Annex D Reference copy routines

The following routines express the behaviour of phase B in C. store32 and load32 are unaligned 32-byte
vector stores and loads; store16 and load16 are 16-byte. All writes may extend past the logical end
into the slack of section 10.5.

```c
static inline void copy_literals(uint8_t *dst, const uint8_t *lit, ptrdiff_t n) {
    do { store32(dst, load32(lit)); dst += 32; lit += 32; n -= 32; } while (n > 0);
}

static inline void copy_literals_sub(uint8_t *dst, const uint8_t *res,
                                     ptrdiff_t n, size_t rep0, size_t vbpos) {
    if (rep0 >= 32 && rep0 <= vbpos) {              // vector path
        do { store32(dst, add_u8(load32(res), load32(dst - rep0)));
             dst += 32; res += 32; n -= 32; } while (n > 0);
    } else {                                         // sequential path
        for (ptrdiff_t t = 0; t < n; t++, vbpos++) {
            uint8_t pred = (rep0 <= vbpos) ? dst[t - (ptrdiff_t)rep0] : 0;
            dst[t] = (uint8_t)(res[t] + pred);
        }
    }
}

static inline void copy_match(uint8_t *dst, size_t off, ptrdiff_t len) {
    const uint8_t *src = dst - off;
    if (off >= 32) {
        do { store32(dst, load32(src)); dst += 32; src += 32; len -= 32; } while (len > 0);
    } else if (off >= 16) {
        do { store16(dst, load16(src)); dst += 16; src += 16; len -= 16; } while (len > 0);
    } else {                                         // 1 <= off < 16
        vec16 pat = shuffle(load16(src), PATTERN[off]);   // pat[i] = src[i % off]
        size_t stride = STRIDE[off];                     // largest multiple of off <= 16
        do { store16(dst, pat); dst += stride; len -= stride; } while (len > 0);
    }
}
```

For the sub-literal sequential path with a run longer than rep0, the predictor reads bytes written
earlier in the same run; the routine above produces that result because it stores each byte before
computing the next.

## References

- RFC 2119, Key words for use in RFCs to Indicate Requirement Levels.
- RFC 8878, Zstandard Compression and the application/zstd Media Type (FSE table description, backward bitstream, sequence decoding).
- J. Duda, Asymmetric numeral systems: entropy coding combining speed of Huffman coding with compression rate of arithmetic coding, 2013.
- F. Giesen, interleaved rANS coding (ryg_rans and accompanying notes).
- RFC 7932, Brotli Compressed Data Format (literal context modes).
- RFC 3720, Appendix B.4, CRC32C (Castagnoli).
- Y. Collet, XXH64 specification (xxHash).
- Zstandard source documentation on the row-based match finder (version 1.5.0 and later) and ZSTD_generateSequences.
