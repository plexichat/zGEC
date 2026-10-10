## 2026-10-10 - rANS and Match Copying Hot Loop Fast-Paths

**Learning:**
In zGEC, default single-context literal streams (`k = 1`) previously shared decoding, encoding, and histogram counting loops with multi-context streams (`k > 1`). Evaluating `if (k > 1)` and computing context table indices (`ti * 2048`) inside the 8-lane inner loops introduced unnecessary branch instructions and index math per literal byte. Specializing the `k = 1` loops removes these branches and allows tight, branchless lane iteration. In addition, when `n_tables = 1`, `zgec_rans_histograms` can bypass 8-lane geometry and run a simple sequential 4-unrolled loop `tables_counts[Z[i]]++`. For Phase B LZ77 execution, handling `off == 1` with `memset` and `off >= 16` with 16-byte unaligned `memcpy` significantly reduces match reconstruction latency.

**Action:**
When optimizing entropy coders or LZ77 match executors, always separate the single-context or common default fast-paths from multi-context fallback loops before unrolling or vectorizing.


## 2026-10-11 - Conditioned Sequence Decoding, rANS ILP Unrolling and FSE Loop Specialization

**Learning:**
1. `seq_decode_cond` (conditioned sequence decoding used on levels 5..9) previously called `zgec_br_read` in a loop, causing struct dereferences and per-bit function call overhead. Transferring the register-held bitreader variables (`acc`, `.nacc`, `ptr`, `left`) with 64-bit fast refill (`ZGEC_BRF_TAKE`) directly into `seq_decode_cond` dramatically accelerates conditioned FSE sequence stream decoding.
2. In `zgec_rans_decode` (`k = 1`), during the `round < full` phase where all 8 lanes are active, unrolling the 8 lanes with explicit scalar variables (`x0..x7`) allows the compiler to exploit instruction-level parallelism (ILP) across independent rANS state transitions in register memory.
3. Specializing `zgec_fse_decode` for sequence decoding (`!syms && out && base && nbits`) removes redundant condition checks and ternary branches from the inner FSE loop.
4. Leveraging output/literal buffer slack (`ZGEC_OUTPUT_SLACK`, `ZGEC_LIT_SLACK`) in Phase B literal and match copying (`exec_copy_literals` and `exec_match`) allows replacing multi-branch small-length dispatches with direct unaligned 8-byte `memcpy` operations.

**Action:**
In FSE/rANS entropy decoders and bitstream readers, always keep bitreader accumulators in local scalar registers, unroll independent multi-lane states for ILP, and leverage allocated buffer slack for fast unaligned 64-bit word copies.
## 2026-10-11 - LZ Match Finder and Parsing Cost Table Optimization

**Learning:**
1. In `parse_seq_cost`, calling `zgec_fast_log2_u32` per candidate match symbol inside the LZ parser's inner price-gate loop introduced millions of redundant log calculations per block. Precomputing `ll_cost`, `ml_cost`, and `of_cost` tables updated whenever running histograms change turns `parse_seq_cost` into direct table lookups, cutting its runtime by >60%.
2. In `mf_match_len` and `mf_match_len_slow`, checking the first 8 bytes in both inline `mf_match_len` and `mf_match_len_slow` caused duplicate 64-bit XOR and trailing zero count operations for matches >= 8 bytes.
3. In `zgec_matcher_insert_match`, calculating sample offsets `(len - 1) * t / (nsamp - 1)` invoked 64-bit integer division (`idiv`) per sample. Specializing for fixed sample counts (16, 3, 1) or `nsamp == len` replaces `idiv` with fast constant division (`/ 15u`) or bit shifts.

**Action:**
In LZ parsing price gates and sample generators, pre-compute log-probability costs into per-symbol float arrays and eliminate variable integer divisions in sample offset calculations.

## 2026-10-12 - Phase B Decompression Fast Match Copying and Pattern Broadcast

**Learning:**
1. In LZ77 decompression Phase B (`exec_match`), small match offsets (`off < 8`, such as `off = 2, 3, 4..7`) and short match lengths (`len == 3`) previously fell through to scalar single-byte loops (`for (i=0; i<len; i++) d[i] = src[i]`), creating a major decoder bottleneck.
2. For 2-byte offsets (`off == 2`), replicating `zgec_rd16(src)` across a 64-bit register (`uint64_t v64 = v16 * 0x0001000100010001ULL`) and copying 8-byte chunks in-phase at `d + i` (where `i` is a multiple of 8) eliminates scalar iterations safely.
3. For 3-byte offsets (`off == 3`), unrolling 3-byte assignments `(s0, s1, s2)` avoids byte-by-byte loops.
4. For offsets `off >= 4`, copying in unaligned 4-byte, 8-byte, and 16-byte chunks (e.g. `memcpy(d+i, src+i, 4)`) is safe because `src + i + 4 <= d + i`, meaning source data is strictly finalized before reading.

**Action:**
In LZ77 match reconstructors, handle `off == 2` with 64-bit pattern broadcast and `off >= 4` with chunked unaligned `memcpy` steps while avoiding phase-shift errors on odd target offsets.
## 2026-10-12 - Long-Table Candidate Filtering and FSE/rANS Loop Specialization

**Learning:**
1. In `zgec_matcher_find`, long-table match candidates previously triggered `mf_match_len` scans without checking if the candidate's character at offset `best.length - 1` matched the target byte. Adding `if (best.length == 0u || ... || vb[ip - d + best.length - 1] == vb[ip + best.length - 1])` before calling `mf_match_len` eliminates millions of redundant match length comparisons per block when `best.length > 0`.
2. In `zgec_fse_decode` and `zgec_fse_encode`, sequence stream operations always pass `syms == NULL`, `out != NULL`, `base != NULL`, and `nbits != NULL`. Specializing the inner FSE decode/encode loops for this case removes several per-symbol branch evaluations inside high-frequency loops.
3. In `zgec_rans_encode` for `k = 1`, separating full rounds (`round < len[7]`) from tail rounds removes the `if (round >= len[lane])` check across all 8 lanes in the main encoding loop.

**Action:**
Always filter long-table match candidates against the current `best.length` character before invoking full match length scans, and specialize FSE/rANS inner loops for non-NULL sequence argument fast-paths.
## 2026-10-12 - LZ Match Finder Single-Load Consolidation and SIMD Lane Masking

**Learning:**
1. In `zgec_matcher_find` and `mf_insert_pos`, issuing separate 32-bit and 64-bit unaligned memory reads (`zgec_rd32`/`zgec_rd64`) at `vb + ip` for repeat offset checks, long table 8-byte hashing, and short table 5-byte hashing caused up to 4 redundant memory loads per position. Consolidating into a single 64-bit load `v8 = zgec_rd64(vb + ip)` and deriving `cur4` (`(uint32_t)v8`), `mf_hash8_v`, and `mf_hash5_v`/`mf_hash4_v` from `v8` eliminates redundant memory accesses.
2. In `mf_bucket_hits`, 4-lane short buckets (fast tier) previously fell back to a scalar 4-iteration loop. Adding a 128-bit SIMD (`_mm_load_si128`) match mask path evaluates all 4 lanes simultaneously.
3. In `mf_match_len_slow`, when an 8-byte chunk comparison found a mismatch, falling through into a scalar byte-by-byte comparison loop added unnecessary branch overhead. Returning `len + (__builtin_ctzll(diff) >> 3)` directly provides fast branchless termination.

**Action:**
When probing multi-table hash indices or checking match lengths, consolidate input loads into a single 64-bit register and use bit-count intrinsics (`ctzll`) to determine byte offsets branchlessly.

## 2026-10-12 - Fast Parser Candidate Match Filtering and 64-bit Length Bypassing

**Learning:**
1. In `fast_parse` (`src/parse.c`) and `zgec_matcher_find` (`src/match.c`), candidate match probes previously set up full `fast_match_len` loop structures even when candidate 64-bit prefixes differed or failed `hash_min` / `best_len` thresholds.
2. Comparing `zgec_rd64(vb + ip - d) ^ v8` directly evaluates 8-byte candidate prefix equality. When `diff != 0`, `(uint32_t)((unsigned)__builtin_ctzll(diff) >> 3)` branchlessly determines match length without invoking function calls or slow loops.
3. For long table candidates (8-byte hash key) and short table candidates (5-byte hash key), verifying character equality at `ip + best_len` and checking 64-bit load prefix mismatch prior to match extension skips millions of failing candidate scans per block.

**Action:**
In LZ match parsers and match finders, compare 64-bit candidate loads with `ctzll` to determine match lengths branchlessly and filter candidates against `ip + best_len` before invoking match extension routines.

## 2026-10-12 - Lock-Free Task Dispatching and Two-Tier Block/Segment Multithreading Scaling

**Learning:**
1. Task index increments in `zgec_enc_work_run` and `zgec_dec_blocks_run` previously used mutex locks (`zgec_mu_lock`), causing mutex lock contention across worker threads. Replacing mutex locks with atomic fetch-and-add (`zgec_atomic_fetch_add_size`) enables lock-free work unit claims across worker pools.
2. For small inputs or inputs with fewer blocks than thread count (`n_blocks < n_threads`), block-level parallelism alone previously clamped worker count to `n_blocks` and left all remaining threads idle. In the encoder, parallelizing per-segment preparation (`zgec_seg_prep_build`), selection (`zgec_select_coder`), and stream emission (`zgec_emit_segment`) across segments in `zgec_encode_block_full` allows all threads to remain active. In the decoder, computing `inner = (n_threads / n_blocks)` when `n_blocks < n_threads` allows inner segment-level parallelism (`decode_segments_parallel`) in Phase A to utilize all remaining CPU cores, achieving near-linear scaling without thread oversubscription.

**Action:**
When designing multi-threaded compressors or decoders, use atomic fetch-add for work unit distribution and combine block-level and segment-level parallelism to prevent core idling on low-block-count inputs while preserving byte-for-byte output determinism.
## 2026-10-13 - AVX2 Match Length Extension, 4-Lane Bucket SIMD Masking, and Single-Load Position Insertion

**Learning:**
1. In `fast_parse` (`src/parse.c`), `fast_match_len` previously relied on a scalar 8-byte XOR loop for match length extension. Adding 32-byte AVX2 SIMD equality scans (`_mm256_loadu_si256` + `_mm256_cmpeq_epi8` + `_mm256_movemask_epi8`) accelerates long match scans across fast-tier encoding.
2. In `mf_bucket_hits` (`src/match.c`), fast-tier buckets (`nlanes == 4`) fell through to a scalar 4-iteration loop. Adding a 128-bit SIMD (`_mm_loadu_si128`) comparison evaluates all 4 packed entries branchlessly in a single vector operation.
3. In `mf_classify_binary` (`src/match.c`), printable ASCII classification operated on scalar single bytes. Vectorizing 32 bytes at a time with AVX2 (`_mm256_cmpgt_epi8`) calculates non-text character counts efficiently.
4. In `mf_insert_pos` (`src/match.c`), short and long hashes previously triggered redundant memory loads. Consolidating into a single 64-bit load (`zgec_rd64(vb + pos)`) when `pos + 8 <= m->vb_size` derives both `mf_hash5_v` / `mf_hash4_v` and `mf_hash8_v` without re-reading memory, dropping `mf_insert_pos` from gprof hotspot lists.
5. In `zgec_rans_encode` (`src/rans.c`) for default `k = 1` streams, separating full rounds (`round < full`) where all 8 lanes are active eliminates per-lane round boundary branches (`round >= len[lane]`) and enables ILP across all 8 rANS state transitions.

**Action:**
Use AVX2 32-byte SIMD equality scans for all match length matchers, evaluate 4-lane hash buckets with 128-bit SIMD masks, and consolidate multi-table hash input loads into a single 64-bit load.

## 2026-10-14 - High-Tier Probe Depth Expansion and High Compression Preset Ladder

**Learning:**
1. In `zgec_matcher_find` (`src/match.c`), `ZGEC_MF_PROBE_DEPTH_HIGH` previously probed only 8 out of 16 bucket lanes in `ZGEC_TIER_HIGH`. Probing all 16 lanes (`ZGEC_MF_PROBE_DEPTH_HIGH = 16u`) allows high-tier match finding to discover significantly longer, higher-quality matches across large virtual buffers.
2. In `parse_min_norep` (`src/parse.c`), forcing non-repeat matches to be >= 6 bytes beyond 256 KiB bypassed 5-byte matches that the price gate cost model confirmed were profitable. Allowing 5-byte matches in high tier when `cur_score > 0` improves overall compression ratio without sacrificing throughput.
3. Expanding the CLI preset level ladder to levels 13-25 with epoch dictionaries (`--dicts`), sampled block pre-filtering (`--filter`), and progressive block sizes (`block_log2` up to 24 / 16 MiB window) reduces Silesia corpus payload size by ~3.9 MB (from 3.40x to 3.63x ratio) while maintaining fast decompression speed (~154 MB/s).

**Action:**
When configuring top-tier compression levels, probe all bucket lanes in `ZGEC_TIER_HIGH`, rely on price-gate cost modeling for candidate minimum lengths, and leverage larger block windows up to the P24 position limit (16 MiB).
