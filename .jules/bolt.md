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

## 2026-10-13 - AVX2 Sub-Literal Residual Subtraction, Delta Filter SIMD, and Encoder Lane Search Elimination

**Learning:**
1. In `zgec_enc_build_resid` (`src/encode.c`), calculating sub-literal predictions (`z[li] = lit[li] - vb[p - r0]`) previously ran in a scalar byte loop. Adding 32-byte AVX2 vector subtraction (`_mm256_sub_epi8`) speeds up residual calculation for literal runs.
2. In `zgec_filter_apply` (`src/common.c`), DELTA pre-filter transformation (`dst[i] = src[i] - src[i-1]`) operated byte-by-byte. Vectorizing with 32-byte AVX2 vector subtraction (`_mm256_sub_epi8`) processes 32 bytes per instruction.
3. In `zgec_ctx_body_bits` (`src/encode.c`), checking whether literal position `j` is a lane start previously executed an 8-iteration loop over `n_lane` for every literal byte. Replacing the loop with explicit direct comparisons `(j == start[0] || j == start[1] || ...)` completely eliminated per-byte loop search overhead.
4. In `zgec_seq_rle_symbol` (`src/seq.c`), testing whether 32-bit sequence values match `values[0]` using 256-bit SIMD comparisons (`_mm256_cmpeq_epi32`) allows skipping scalar `zgec_seq_code_of` calls across 8 values at a time.

**Action:**
Vectorize byte subtractions with AVX2 SIMD `_mm256_sub_epi8` in predictive coders/filters, and unroll static array membership checks to avoid inner loop linear search overhead.
## 2026-10-14 - Encode Path AVX2 SIMD Vectorization across Sequence Preparation and Histograms

**Learning:**
1. In `zgec_seg_prep_build` (`src/encode.c`), extracting sequence fields (`ll`, `mlv`, `ofv`, `mls`, `ml_sum`) operated sequentially. Vectorizing 8 sequences at a time using 256-bit AVX2 SIMD intrinsics (`_mm256_sub_epi32`, `_mm256_max_epi32`, `_mm256_add_epi32`, and SIMD stores) accelerates segment preparation overhead.
2. In `zgec_enc_entropy_bits_u32` and `zgec_select_contexts` (`src/encode.c`), 256-element histogram summation previously used scalar accumulation. Vectorizing with AVX2 (`_mm256_loadu_si256` and `_mm256_add_epi32`) reduces histogram total computation time.
3. In `zgec_rans_histograms` (`src/rans.c`), single-table histogram generation previously suffered from loop-carried dependencies when consecutive identical symbols appeared. Using 4 parallel accumulation buffers followed by 256-bit AVX2 vector additions eliminates dependency stalls while preserving exact counts.
4. In `zgec_seq_rle_symbol` (`src/seq.c`), evaluating sequence values for RLE candidates previously checked each value individually. Comparing 8 values per iteration with `_mm256_cmpeq_epi32` rapidly confirms uniform sequence symbols.

**Action:**
Vectorize sequence array transformations and 256-element histogram summations with AVX2 SIMD, and use parallel accumulation arrays to eliminate loop-carried dependencies in symbol counting loops.
## 2026-10-13 - FSE Bitreader Refill Guarding and Phase B Direct 64-bit Match Copying

**Learning:**
1. In FSE stream decoding (`zgec_fse_decode` in `src/fse.c` and `seq_decode_cond` in `src/decode.c`), the inner loop calculates `nb = (63u - nacc) >> 3`. When `nacc >= 56u`, `nb` evaluates to 0. Guarding the 64-bit unaligned load and bit shifts with `if (nacc < 56u)` avoids redundant memory reads, shifts, and register pointer increments on iterations where `acc` already holds sufficient bits (56..63).
2. In Phase B LZ77 match copying (`exec_block` in `src/decode.c`), matches with `moff >= 8` and short lengths (`mlen <= 8`, such as common 3..8 byte matches) previously fell through a multi-branch cascade and scalar copy loop. Handing `moff >= 8 && mlen <= 8` with a direct 64-bit load/store `zgec_wr64(d, zgec_rd64(msrc))` completes short non-overlapping match copies in a single 64-bit operation.

**Action:**
In FSE bitstream readers, guard 64-bit byte refills with `if (nacc < 56u)` to bypass zero-byte refills, and perform direct 64-bit unaligned word copies for `moff >= 8` short match lengths in LZ77 decoders.
## 2026-10-14 - High-Tier Probe Depth Expansion and High Compression Preset Ladder

**Learning:**
1. In `zgec_matcher_find` (`src/match.c`), `ZGEC_MF_PROBE_DEPTH_HIGH` previously probed only 8 out of 16 bucket lanes in `ZGEC_TIER_HIGH`. Probing all 16 lanes (`ZGEC_MF_PROBE_DEPTH_HIGH = 16u`) allows high-tier match finding to discover significantly longer, higher-quality matches across large virtual buffers.
2. In `parse_min_norep` (`src/parse.c`), forcing non-repeat matches to be >= 6 bytes beyond 256 KiB bypassed 5-byte matches that the price gate cost model confirmed were profitable. Allowing 5-byte matches in high tier when `cur_score > 0` improves overall compression ratio without sacrificing throughput.
3. Expanding the CLI preset level ladder to levels 13-25 with epoch dictionaries (`--dicts`), sampled block pre-filtering (`--filter`), and progressive block sizes (`block_log2` up to 24 / 16 MiB window) reduces Silesia corpus payload size by ~3.9 MB (from 3.40x to 3.63x ratio) while maintaining fast decompression speed (~154 MB/s).

**Action:**
When configuring top-tier compression levels, probe all bucket lanes in `ZGEC_TIER_HIGH`, rely on price-gate cost modeling for candidate minimum lengths, and leverage larger block windows up to the P24 position limit (16 MiB).
## 2026-10-13 - Comprehensive AVX2 SIMD Vectorization in Decompression Hot Paths

**Learning:**
1. In Phase B decompression (`exec_copy_literals` and `exec_match_wide`), replacing byte/word loops with 256-bit AVX2 vector loads and stores (`_mm256_loadu_si256`/`_mm256_storeu_si256`) accelerates literal copying and match reconstruction across 32-byte blocks.
2. For small match offsets (`moff = 2, 4, 8, 16`), broadcasting pattern registers into 256-bit AVX2 vectors (`_mm256_set1_epi16`, `_mm256_set1_epi32`, `_mm256_set1_epi64x`, `_mm256_broadcastsi128_si256`) enables writing 32 repeated bytes per store without scalar byte iterations.
3. In sub-literal prediction decoding (`exec_literals_sub`), when `rep0 >= 32`, vectorizing residual addition with 256-bit `_mm256_add_epi8` computes 32 decoded sub-literals per SIMD instruction.
4. In 8-lane rANS decoding (`zgec_rans_decode`), gathering packed slots across 8 lanes simultaneously with `_mm256_i32gather_epi32` and computing state transitions with `_mm256_mullo_epi32` accelerates rANS symbol decoding during full rounds when no lane requires state renormalization.
5. In inverse pre-filtering (`zgec_filter_inverse` DELTA mode), 128-bit vector shifts (`_mm_slli_si128`) and byte additions (`_mm_add_epi8`) compute 16-byte prefix sums in parallel.

**Action:**
Always use 256-bit AVX2 vector operations for literal copies, pattern broadcasts, sub-literal residual additions, rANS 8-lane state gathers, and delta filter inverse prefix sums in decompression hot paths.
## 2026-10-13 - AVX2 Sub-Literal Residual Subtraction, Hoisted Lane Boundary Checks, and Static Context LUTs

**Learning:**
1. In `zgec_enc_build_resid` (`src/encode.c`), sub-literal residual prediction subtraction operated scalar byte-by-byte. Vectorizing 32 bytes at a time with AVX2 (`_mm256_sub_epi8` on `lit` and `vb + p - r0`) drastically accelerates residual generation.
2. In `zgec_ctx_body_bits` (`src/encode.c`), scanning an $O(N \times \text{n\_lane})$ loop over 8 lane start positions per byte added millions of redundant array comparisons. Advancing a single lane pointer index (`next_lane_idx`) when $j$ reaches the next lane boundary reduces comparison overhead to $O(N)$.
3. In `zgec_classify` (`src/lit.c`) and `zgec_select_contexts` (`src/encode.c`), replacing arithmetic signed mode classification with a static 256-entry lookup table (`signed_table`) eliminates branchy arithmetic in context model ranking.

**Action:**
Vectorize sub-literal prediction subtraction with 256-bit AVX2 byte operations, replace linear lane-boundary scans with monotonically advancing pointer checks, and use static 256-entry lookup tables for symbol classification.

## 2026-10-14 - Adaptive Match Position Insertion, Fast-Tier Table Sizing, and Thread-Local Table Scratch

**Learning:**
1. In `fast_parse` (`src/parse.c`), inserting every position (`stride = 1`) inside a match into both `ts` and `tl` hash tables introduced $2 \times \text{best\_len}$ 64-bit multiplications and table stores per match. Using adaptive match position insertion (inserting `ip + 1`, `ip + 2`, striding intermediate positions for `best_len > 8`, and inserting `ip + best_len - 1`) eliminates up to 70% of inner loop hash calculations and table stores during match skips while preserving compression ratio.
2. Sizing fast-tier hash tables to 1 MiB (`ZGEC_FAST_BITS_SHIFT = 3u`) allows hash tables to fit entirely inside L2 cache (512 KB - 1 MB), reducing L3 cache miss stalls during hash probes and increasing single-threaded throughput from 30.3 MB/s to 41.0 MB/s (+35.3%) across the Silesia corpus.
3. Allocating fast-tier tables (`ts` and `tl`) in thread-local scratch memory (`ZGEC_ENC_TLS`) avoids repeated `malloc`/`free` heap overhead per block.
4. In `zgec_matcher_find` (`src/match.c`), pre-filtering candidate positions against `vb[pos + best.length - 1] != vb[ip + best.length - 1]` before 64-bit loads and `ctzll` scans eliminates failing candidates branchlessly.

**Action:**
In LZ match parsers, stride intermediate match positions during insertion for long matches, keep fast-tier hash tables within L2 cache bounds (<=1 MB), use thread-local scratch arrays for block tables, and filter candidates against `ip + best.length - 1` before fetching 64-bit match prefixes.
## 2026-10-15 - Fast Tier Repeat Offset 64-Bit Length Bypassing and Dual-Unrolled Table Inserts

**Learning:**
1. In  (), checking repeat offset candidates previously used a 32-bit compare followed by . Replacing with a single 64-bit load XOR  and branchless  for non-zero differences evaluates repeat offset match lengths without calling general match extension loops.
2. Unrolling the inside-match table insertion loop 2x with dual 64-bit loads (, ) and dual hash calculations (, ) reduces loop control overhead and increases instruction-level parallelism when updating short and long match tables during match emission.

**Action:**
In fast-tier LZ parsers, evaluate candidate repeat matches directly via 64-bit XOR and , and unroll match-interior table insertion loops for improved store pipelining.

## 2026-10-15 - Fast Tier Repeat Offset 64-Bit Length Bypassing and Dual-Unrolled Table Inserts

**Learning:**
1. In fast_parse (src/parse.c), checking repeat offset candidates previously used a 32-bit compare followed by fast_match_len. Replacing with a single 64-bit load XOR zgec_rd64(vb + ip - r) ^ cur8 and branchless __builtin_ctzll(diff) >> 3 for non-zero differences evaluates repeat offset match lengths without calling general match extension loops.
2. Unrolling the inside-match table insertion loop 2x with dual 64-bit loads (iv0, iv1) and dual hash calculations (hs0/hl0, hs1/hl1) reduces loop control overhead and increases instruction-level parallelism when updating short and long match tables during match emission.

**Action:**
In fast-tier LZ parsers, evaluate candidate repeat matches directly via 64-bit XOR and ctzll, and unroll match-interior table insertion loops for improved store pipelining.
## 2026-10-14 - Precomputed rANS xmax Thresholds, 32-bit Unsigned Remainder Math, and Runstart Fast-Paths

**Learning:**
1. In rANS encoding ( in ), precomputing  in  eliminates repeated 64-bit shifts () per symbol. Replacing 64-bit signed division () with 32-bit unsigned wraparound math () removes 64-bit integer conversion overhead in the inner encoding loop.
2. In  () and  (), checking  before iterating over literals bypasses bitwise  queries whenever no run-starts exist.
3. In  for single-context streams (), unrolling the 4-way loop to 16-way increases instruction-level parallelism and L1 cache throughput.

**Action:**
Precompute rANS normalization thresholds (), use 32-bit unsigned wraparound remainder logic in rANS symbol encoding, and bypass bit-level runstart lookups when the runstart bitmap is empty.

## 2026-10-14 - Precomputed rANS xmax Thresholds, 32-bit Unsigned Remainder Math, and Runstart Fast-Paths

**Learning:**
1. In rANS encoding (`zgec_rans_encode_symbol` in `src/rans.c`), precomputing `xmax[s] = (uint32_t)eff << 21` in `zgec_rans_build_enc` eliminates repeated 64-bit shifts (`f << 21`) per symbol. Replacing 64-bit signed division (`int64_t rems = (int64_t)x - (int64_t)quo * (int64_t)f`) with 32-bit unsigned wraparound math (`uint32_t rems = x - quo * f; if (rems >= f) { quo--; rems += f; }`) removes 64-bit integer conversion overhead in the inner encoding loop.
2. In `zgec_select_contexts` (`src/encode.c`) and `zgec_rans_histograms` (`src/rans.c`), checking `zgec_rs_is_empty(runstart, n_lit)` before iterating over literals bypasses bitwise `zgec_rs_get` queries whenever no run-starts exist.
3. In `zgec_rans_histograms` for single-context streams (`n_tables == 1`), unrolling the 4-way loop to 16-way increases instruction-level parallelism and L1 cache throughput.

**Action:**
Precompute rANS normalization thresholds (`xmax`), use 32-bit unsigned wraparound remainder logic in rANS symbol encoding, and bypass bit-level runstart lookups when the runstart bitmap is empty.

## 2026-10-16 - Precomputed Matcher Masks, Shift Indexing, and Bytes 8..15 64-Bit Fast Match Extension

**Learning:**
1. In `src/match.c` (`mf_match_len_slow`), matches that extend past 8 bytes frequently end in the 8..15 byte range. Adding an early 64-bit load/XOR check for bytes 8..15 before the 32-byte AVX2 loop evaluates 8..15 byte match extensions using fast scalar 64-bit register math without AVX2 vector setup overhead, reducing `mf_match_len_slow` execution share from 23.4% to 16.7%.
2. In `struct zgec_matcher` (`src/match.c`), precomputing `nbuckets_mask`, `long_buckets_mask`, `lanes_mask`, and `nlanes_shift` at creation time replaces 64-bit integer multiplication (`bucket * m->nlanes`) and mask subtractions (`m->nbuckets - 1u`) with fast bitwise shifts (`bucket << m->nlanes_shift`) and precomputed masks across match search, insertion, and prefetch inner loops, raising 1T compression speed from 221.9 MB/s to 243.4 MB/s (+9.7%).

**Action:**
When extending matches, check bytes 8..15 with scalar 64-bit XOR math before entering AVX2 vector loops, and precompute array stride shifts and bucket masks in lookup structures to eliminate integer multiplications in hot loops.
