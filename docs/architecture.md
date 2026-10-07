# zGEC architecture: shared vs encoder-only vs decoder-only

Public surface: every `zgec_*` declared in `include/` is implemented once
in the matching `src/` file. `static inline` helpers in headers
(`zgec_br_*`, `zgec_seq_code_of/base_of`, `zgec_reps_*`, `zgec_mlclass`,
`zgec_lit_sub_predict`, `zgec_tbl_mode`, LE/varint/fast-log2) are shared
by design for speed.

## Shared core (`fse/rans/seq/lit/bitstream/frame/block/common/crc/xxhash`)

Single implementations live here and are called from both sides:

- `src/seq.c:273` `zgec_normalize_counts` — canonical histogram
  normalisation. The former `src/encode.c` copy was removed; encoder
  call sites use `(void)zgec_normalize_counts(...)` (always succeeds for
  `al == 10`, `nsym == 66`).
- `src/lit.c:212` `zgec_lit_lane_starts` + `zgec_lit_lane_geom` — lane
  geometry. `src/rans.c` decode/encode/histograms call the helper instead
  of inlining `q/r/start/len`.
- `src/lit.c:152` `zgec_lit_runstart` — validated run-start bitmap.
  `src/encode.c:383` `zgec_enc_runstart` is a thin wrapper (clears on the
  impossible error path); encoder call sites were not changed otherwise.
- `src/lit.c:110` `zgec_class_map_decode/encode` — 3-bit class-map codec.
  `src/block.c:14` `zgec_ctx_desc_parse/emit` call it (emit pre-validates
  `>= ctx_count`, preserving the old reject).
- `src/zgec_internal.h` — `zgec_mu_*`, `zgec_cpu_count`,
  `ZGEC_MAX_WORKERS`, `zgec_check_stream_sentinel`,
  `zgec_filter_shuffle_count`, `zgec_normalize_counts` declaration.
  `src/decode.c` and `src/encode.c` share the threading shim;
  `src/decode.c` and `src/dict.c` share the V5 sentinel check;
  `src/common.c` apply/inverse share the shuffle column lengths.
- `include/zgec_seq.h:79` `zgec_reps_encode` + `:61` `zgec_reps_resolve` —
  canonical repeat-offset chain. `src/parse.c:199` `parse_offbase` wraps
  the former; the accept-path MTF cascade calls resolve directly.
- `src/block.c` includes `zgec_fse.h` instead of a local forward
  declaration of `zgec_fse_read_counts`.
- `src/encode.c` entropy calls `zgec_fast_log2_u64` directly; the
  `zgec_enc_log2_u64` wrapper was removed.

## Intentional fork: `src/dict.c` segment decoder vs `src/decode.c`

`src/dict.c:409` decodes inner DICT records with a standalone segment
decoder so `dict.c` does not include `decode.h` (`Ld = 0, Ll = 0`).
Only the leaf V5 sentinel check is shared today. A full merge would
require unifying `zgec_dict_seg`/`zgec_dict_prev` with
`decode.c`'s segment/table structs and is deliberately out of scope;
any V-rule or table-inheritance fix must still be applied in both places
(see `docs/spec.md` §10 vs §5).

## Opaque handles

`zgec_decoder`, `zgec_encoder`, `zgec_matcher`, `zgec_dict_cache` are
never exposed. `cli.c` is outside the library (`main` + `static`
helpers only).
