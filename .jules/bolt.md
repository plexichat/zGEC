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
1. In fast_parse (src/parse.c), checking repeat offset candidates previously used a 32-bit compare followed by fast_match_len. Replacing with a single 64-bit load XOR zgec_rd64(vb + ip - r) ^ cur8 and branchless __builtin_ctzll(diff) >> 3 for non-zero differences evaluates repeat offset match lengths without calling general match extension loops.
2. Unrolling the inside-match table insertion loop 2x with dual 64-bit loads (iv0, iv1) and dual hash calculations (hs0/hl0, hs1/hl1) reduces loop control overhead and increases instruction-level parallelism when updating short and long match tables during match emission.

**Action:**
In fast-tier LZ parsers, evaluate candidate repeat matches directly via 64-bit XOR and ctzll, and unroll match-interior table insertion loops for improved store pipelining.
## 2026-10-14 - Precomputed rANS xmax Thresholds, 32-bit Unsigned Remainder Math, and Runstart Fast-Paths

**Learning:**
1. In rANS encoding (`zgec_rans_encode_symbol` in `src/rans.c`), precomputing `xmax[s] = (uint32_t)eff << 21` in `zgec_rans_build_enc` eliminates repeated 64-bit shifts (`f << 21`) per symbol. Replacing 64-bit signed division (`int64_t rems = (int64_t)x - (int64_t)quo * (int64_t)f`) with 32-bit unsigned wraparound math (`uint32_t rems = x - quo * f; if (rems >= f) { quo--; rems += f; }`) removes 64-bit integer conversion overhead in the inner encoding loop.
2. In `zgec_select_contexts` (`src/encode.c`) and `zgec_rans_histograms` (`src/rans.c`), checking `zgec_rs_is_empty(runstart, n_lit)` before iterating over literals bypasses bitwise `zgec_rs_get` queries whenever no run-starts exist.
3. In `zgec_rans_histograms` for single-context streams (`n_tables == 1`), unrolling the 4-way loop to 16-way increases instruction-level parallelism and L1 cache throughput.

**Action:**
Precompute rANS normalization thresholds (`xmax`), use 32-bit unsigned wraparound remainder logic in rANS symbol encoding, and bypass bit-level runstart lookups when the runstart bitmap is empty.





## 2026-10-16 - Target-Len Pre-Filtering, Inlined Match Extension and Lazy Probe Score Thresholding

**Learning:**
1. In `zgec_matcher_find_lens` (`src/match.c`), pre-filtering candidate short bucket and long table matches against `target_len = max(best.length, hash_min)` byte equality (`vb[ip - d + target_len - 1] == vb[ip + target_len - 1]`) before issuing 64-bit loads or diff mask arithmetic prunes invalid candidates branchlessly even when `best.length == 0`. In addition, returning `best` immediately when `mask == 0u` skips popcount and mask rotation calculations.
2. Inlining `mf_match_len_slow` with `always_inline` eliminates function call frame setup and register spilling overhead across 8+ byte match extensions.
3. In `zgec_parse_block_ex` (`src/parse.c`), computing `min_lazy_len = (uint32_t)(cur_score / lbar) + 1u` and passing it as `min_len` to `zgec_matcher_find_lens` during lazy evaluation probes at `ip + 1` filters out non-viable short match candidates early, dropping match finding runtime by >60%.

**Action:**
Pre-filter all candidate match positions against `target_len = max(best.length, hash_min)` byte equality before loading 64-bit prefixes, inline match length extension routines, and derive `min_len` from `cur_score / lbar` for lazy lookahead probes.

## 2026-10-16 - FSE Local Bitwriter Register State and SIMD Histogram Additions

**Learning:**
1. In `zgec_fse_encode` (`src/fse.c`), maintaining bitwriter accumulators and pointers (`bw_acc`, `bw_nacc`, `bw_ptr`) in local scalar registers with 32-bit threshold flushes (`zgec_bw_flush`) eliminates struct pointer dereference overhead, reducing `zgec_fse_encode` runtime from 0.48s (8.07%) to 0.20s (3.44%) in gprof profiling.
2. In `zgec_granule_merge` and `zgec_cluster_greedy` (`src/encode.c`), vectorizing 256-element uint32_t histogram array additions with 256-bit AVX2 SIMD (`_mm256_loadu_si256`, `_mm256_add_epi32`, `_mm256_storeu_si256`) speeds up granule merging and greedy context clustering.

**Action:**
Always cache bitwriter state in local registers during entropy encoding loops, and vectorize 256-element histogram array additions with 256-bit AVX2 SIMD.

## 2026-10-15 - Register-Backed FSE Bitwriting and Specialised Bitstream Flushes

**Learning:**
1. In `zgec_fse_encode` (`src/fse.c`) and `zgec_seq_stream_encode_cond` (`src/seq.c`), maintaining bitwriter accumulator and pointer state in local register variables (`bw_acc`, `bw_nacc`, `bw_ptr`) and performing 32-bit threshold flushes (`zgec_wr32`) when `bw_nacc >= 32` removes struct pointer dereferences and function call overhead per symbol.
2. In `zgec_bw_flush` (`include/zgec_bitstream.h`), specializing 1, 2, and 4 byte flushes with direct unaligned store primitives (`zgec_wr16`, `zgec_wr32`) avoids scalar byte loop iterations during bitstream flushes.
3. Before finalizing backward bitstream encoding in `zgec_bw_finish`, all complete bytes (`rem_bytes = bw_nacc >> 3`) must be flushed so that `bw_nacc < 8` and `bw_acc` holds only remaining sub-byte bits, ensuring sentinel bit insertion in `zgec_bw_finish` functions correctly.

**Action:**
In entropy and bitstream encoding routines, localize bitwriter state into registers, flush 32-bit chunks via `zgec_wr32`, and ensure remaining full bytes are flushed down to `bw_nacc < 8` prior to calling `zgec_bw_finish`.

## 2026-10-15 - Fast Sequence Code Lookup Table, Float Price Gate Score Calculations, and Match Candidate Pre-Filtering

**Learning:**
1. In `include/zgec_seq.h` and `src/seq.c`, replacing scalar `zgec_highbit32` bit-scans in `zgec_seq_code_of` with a static 512-entry lookup table (`zgec_seq_lut512`) accelerates symbol code and extra-bit count determination for values `v < 512` across all candidate match cost evaluations and sequence preparation loops.
2. In `src/parse.c` (`zgec_parse_block_ex`), converting price-gate cost and score calculations (`parse_seq_cost`, `parse_score`, `parse_lbar`, `zgec_fast_log2_u32_f`) from 64-bit `double` to 32-bit `float` eliminates floating-point unit type conversion overhead and lowers register pressure during LZ77 parsing.
3. In `src/match.c` (`zgec_matcher_find_lens`) and `src/parse.c` (`fast_parse`), pre-testing candidate byte equality at `check_idx = max(best_len, hash_min) - 1` before issuing 64-bit loads and `ctzll` scans filters out failing candidate positions branchlessly.

**Action:**
Use precomputed 512-entry LUTs for small sequence symbol lookups, compute LZ parser price-gate scores using 32-bit single-precision floats, and filter candidate match positions at `max(best_len, min_match) - 1` before performing 64-bit diff scans.

## 2026-10-15 - Unaligned Memcpy Inline Reads, Hoisted Parser Literal Costs, and SSE2 16-byte Match Extensions

**Learning:**
1. Implementing `zgec_rd16`/`32`/`64` and `zgec_wr16`/`32`/`64` with `memcpy` on little-endian architectures allows compilers (GCC/Clang) to emit single unaligned load/store instructions (`mov`/`movdqu`) instead of multi-instruction byte shift/OR loops while maintaining portable C standards.
2. In `zgec_parse_block_ex` (`src/parse.c`), hoisting literal length cost (`parse_ll_cost`) per position `ip` and using `parse_score_fast` eliminates redundant calls to `zgec_seq_code_of(ll)` across main and repeat offset candidate match scoring loops.
3. In `zgec_matcher_find_lens` (`src/match.c`), checking candidate bytes against `target_len = max(best.length, hash_min)` prior to 64-bit loads/diffs skips non-matching candidates branchlessly.
4. In `mf_match_len_slow` (`src/match.c`), adding a 16-byte SSE2 SIMD comparison loop (`_mm_loadu_si128` + `_mm_cmpeq_epi8` + `_mm_movemask_epi8`) accelerates match length extensions when match lengths fall between 16 and 39 bytes where 32-byte AVX2 loops do not fire.

**Action:**
Use `memcpy` for unaligned little-endian memory access helpers, hoist per-position literal length sequence costs in optimal parser price gates, and use 16-byte SSE2 SIMD steps for short-to-medium match extension loops.

## 2026-10-16 - Encoder AVX2 SIMD Gather, Stack-Allocated rANS Words and Tracked Lane Boundaries

**Learning:**
1. In `src/rans.c` (`zgec_rans_encode`), allocating the output words buffer on the stack for small literal counts (`words_cap <= 2048`) completely eliminates per-call heap allocation (`zgec_alloc`/`zgec_free`) overhead on hot rANS literal encoding paths.
2. In `src/encode.c` (`zgec_ctx_body_bits`), replacing an 8-way per-byte equality comparison chain (`j == start[0] || ... || j == start[7]`) with a single monotonically advancing lane index check (`j == start[next_lane_idx]`) and guarding `zgec_rs_get` with `zgec_rs_is_empty` reduces inner loop branch instructions per literal byte from up to 8 down to 1.
3. In `src/encode.c` (`zgec_seg_prep_build`), using 256-bit AVX2 gather SIMD (`_mm256_i32gather_epi32`) loads sequence fields (`ll`, `ml`, `offbase`) 8-at-a-time directly into SIMD registers.
4. In `src/rans.c` (`zgec_rans_histograms`), expanding single-table literal histogram counting to 8-wide unrolled accumulators combined with 256-bit AVX2 SIMD additions reduces store-forwarding stalls during entropy model generation.

**Action:**
Stack-allocate small scratch buffers in hot entropy coders, track monotonically advancing lane indices instead of multi-way equality checks, and use AVX2 gather SIMD for sequence array preparation.

## 2026-10-17 - Inlined Context Classification LUTs for Encoder Hot Paths

**Learning:**
1. In `src/encode.c` (`zgec_select_contexts`), building class histograms for literal context selection invoked `zgec_classify` millions of times per block across 4 context modes (`LSB6`, `MSB6`, `TEXT`, `SIGNED`).
2. Exposing the static lookup tables `zgec_ctx_text_lut` and `zgec_ctx_signed_lut` in `include/zgec_lit.h` and making `zgec_classify` an inline function eliminated non-inlined cross-translation-unit function call frame setup and switch statement branching.
3. Replacing `zgec_classify` calls with direct LUT array accesses (`zgec_ctx_text_lut[b]` and `zgec_ctx_signed_lut[b]`) in `zgec_select_contexts` completely removed classification overhead from gprof profile hotspots and increased level 6 single-threaded encoding throughput from 33.4 MiB/s to 60.2 MiB/s (+80%).

**Action:**
Always expose classification LUTs in public internal headers to allow direct array indexing in entropy context selection loops.

## 2026-10-18 - Fast-Tier Speed Ladder: Runtime Parser Dial (Levels 1-3)

**Context:** The fast parser was the encode bottleneck (58% of level 4 encode:
2.44 s of a 2.33 s run on the 211.9 MB reference corpus, ~6.7 ns/byte). Shedding
its optional passes at compile time was not where its time goes; the cost is the
mandatory per-position body (two hashes, two table loads, two table stores, three
repeat-offset probes) and the working set is latency-bound on the two hash tables.

**Learning:**
1. The two compile-time levers that actually move throughput are the table size
   (`ZGEC_FAST_BITS_SHIFT`): one bucket per 16 positions (2^15 buckets, 128 KiB
   per table, fits L2) gave -17% encode for +4.5% size; and dropping the
   per-position passes (repeat-offset probes, short table) gave -21% for +14%.
   Optional-pass shedding measured -6.8%/+1.7% (reps only) and -12%/+8.5%
   (short table only) - the short table carries most of the ratio.
2. A runtime dial reproduces the compile-time envelopes exactly with no measured
   cost to the pinned levels: interleaved A/B (min of 9) put level 4 at -0.7% and
   level 6 at -0.3% versus the pinned binary - the loop-invariant branches the
   dial adds are fully predicted.
3. Surfacing the dial required no public-API break: `zgec_parse_block_ex` stays
   the byte-identical entry (it now wraps a new `zgec_parse_block_dial` with
   dial 0), and `zgec_params.fast_dial` threads the value from the CLI preset.

**Measurement (reference corpus, 211,938,580 B, -T 1, min of 4):**
`l1 79,091,618 / 1,629 ms` `l2 68,006,258 / 1,990 ms` `l3 66,061,264 / 2,100 ms`
`l4 65,059,955 / 2,306 ms`. All pinned rungs 4-10 unchanged; level 7 SHA-256
unchanged.

**Action:** New fast levels 1-3 set `zgec_params.fast_dial` (1..3) through
`zgec_parse_block_dial`: dial 1 shrinks the fast hash tables, 2 shrinks them
further, 3 also drops the repeat-offset and short-table probes. Never set by the
pinned levels. A from-scratch single-table parser remains the only path to close
the remaining gap to zstd's fast tier.

## 2026-10-18 - Fast Tier: Single-Table Lean Parser Core (Level 1)

**Context:** The dial ladder left level 1 (dial 3) at 1,629 ms vs level 4's
2,306 ms, but level 1 still ran the two-table `fast_parse` loop with its short
hash computed and discarded and a lazy check that cannot fire without the short
table. The remaining parser cost at the top rung was the short/long fan-out and
the O(best_len) interior-insert walk, which touched nearly as many table
positions as the main loop (1.12 M inserts vs 1.00 M loop iterations on one
2 MiB block).

**Learning:**
1. Gating the walk / lazy check behind a *runtime* dial in the shared loop cost
   the pinned path 2% (measured against a self-A/B noise floor of +-0.3%). The
   fix is structural, not a branch: the lean core is a separate function, so
   `fast_parse` and the pinned levels keep their exact code shape.
2. The single-table core keeps only the mandatory per-position body -- one
   8-byte hash, one table load, one store -- plus a full-word candidate probe
   (a collision is dropped with no length scan). It drops the short table, the
   repeat-offset probes, the lazy check and the second hash, and inserts a
   constant few interior positions per sequence instead of walking the match.
3. Larger fast hash tables did not help once the short table was gone: fbits
   13/11 were neutral-to-slower than fbits 15 at the top rung, so the lean core
   keeps the dial-3 table sizing.

**Measurement (211,938,580 B, -T 1, interleaved A/B, min of 7):** level 1
(lean core) 80,567,687 B / 1,483 ms against the dial-3 rung it replaces
(79,091,618 B / 1,751 ms): **-15.3% time, +1.9% size**. Level 4 byte-identical
(65,059,955 B) and within the self-A/B noise floor. Pins 4-10 and the l7
SHA-256 unchanged; ctest 4/4; l1-l3 round-trip and `-T 1` == `-T 4`.

**Action:** `fast_parse_lite` is the new dial 4, selected by level 1; the fast
ladder is now 1 = dial 4 (lean core), 2 = dial 3, 3 = dial 2, 4 = dial 0. The
`fast_insert_sparse_long` helper is out-of-line so no pinned level pays for it.
The remaining gap to zstd -1 is entropy-bound, not parser-bound.

### Follow-up: hash tag in the lean-core table entry

Storing `(tag << 24) | pos` in the lean core's single table (tag = the hash
bits the bucket index does not use, 0 marks empty) lets a bucket hit whose full
hash differs be rejected by a register compare instead of the random candidate
load the probe would otherwise issue. It is provably output-neutral -- equal
8-byte keys hash equally, so a tag mismatch implies different bytes, which the
byte compare rejects anyway (l1 stays 80,567,687 B).

Interleaved A/B (min of 9): l1 -0.8% (med -1.1%). Small: the 2 MiB input buffer
is L2-resident, so the skipped load was not the bottleneck. Kept because it is
free of ratio and correctness risk.

### Branchless entropy encoders (rANS literals + FSE sequences)

With the parser at its floor the fast rung's time is entropy-bound, so a
phase profile was taken (temporary `ZGEC_ENC_PROFILE` rdtsc instrumentation,
since removed). At level 1 the encode splits roughly parse 28%, granule
segmentation 14%, literal stream 18-26%, sequence FSE build 8%, sequence FSE
streams 10-12%, prep 6%. The two entropy encoders dominated the fast tiers.

Both encoders tested a renormalisation condition with a data-dependent branch
that mispredicts far too often to keep:

1. `rans.c` `zgec_rans_encode_symbol`: the `while (x >= f<<21)` is provably a
   single shift (`x < 2^32` so one `>>16` lands below `2^16 <= f<<21`), so it
   is now an unconditional word store plus a conditional commit, with the
   reciprocal correction as a cmov. The caller reserves 8 words of slack and
   the per-write bound test is dropped. The decoder made the same change long
   ago (see `zgec_rans_renorm`).
2. `fse.c` `zgec_fse_encode`: the `if (nb > 0)` gate around the extra-bits
   field becomes a width mask (`extra & mask`, `cn += nb`), since a symbol
   with no extra bits stands for exactly `base[s]` and the symbol is derived
   from the value. The two range checks are unchanged for `nb > 0`.

**Measurement (211,938,580 B, -T 1, interleaved A/B, min of 7):** rANS gives
level 1 **-11.1%** and level 4 -1.7%; FSE gives a further level 1 -2.2% and
level 4 -2.3%. Both are bitstream-identical by construction; pins 4-10 and the
l7 SHA-256 are unchanged and ctest is 4/4.

**Negative results (measured, reverted):** zeroing only the absent symbols'
blocks of the FSE enc table (instead of one big memset) was *slower* (l1
+4.3%) -- the many small memsets lose to the single call. A branchless FSE
bitwriter flush (unconditional store + conditional advance, with slack) was
within noise on l1/l4 and adds an unchecked store, so it was dropped.

## 2026-10-18 - Encoder allocation churn (FSE tables, block cache) and the FSE bitwriter

After the branchless encoders the profile showed the sequence FSE *table
build* at ~8% of level 1 and ~5% of level 4 -- far more than its fill loop
could explain. The fill loop, a range-fill variant and an absent-symbols-only
memset had all already been tried and moved nothing. The real cost was the
allocate/free of the ~540 KiB encoder table (`nsym * 2^al * 8` bytes at
al = 10) once per stream per segment: the system allocator returns such a
block to the OS on free, so the next build re-faults every page.

Three changes landed, all bitstream-identical:

1. `fse.c`: `zgec_fse_build_enc` rents its table from a thread-local free list
   (`zgec_fse_enc_rent` / `zgec_fse_enc_return`) instead of malloc/free. The
   list is threaded through the tables' own `e` field while `al`/`nsym` stay
   intact, so a slot's size is recovered on reuse. No double-free is possible:
   every encoder-table pointer is NULL-initialised and cleared after its one
   free, so `seg_fail_tables` only ever frees NULLs after the success path
   already freed them. (commit 7890ba9)
2. `common.c`: `zgec_alloc`/`zgec_free` gained a per-thread cache of *any*
   freed block in [128 KiB, 4 MiB], at most 8, best-fit reuse. The word before
   the saved malloc pointer now records the block's requested total, a safe
   lower bound on its physical size. This subsumes the same churn in the
   sequence-prep arrays, the stream temp block, the per-block sequence array
   and the decoder tables. (commit 4a2e714)
3. `fse.c` `zgec_fse_bw_write_local`: the `*bw_nacc >= 32` drain is now
   branchless (unconditional store, conditional advance), matching the rANS
   renormalisation treatment, with the rare second completed word keeping the
   bound-checked path. The caller reserves 16 bytes past the three contiguous
   stream regions so the uncommitted store stays inside the allocation.
   (commit d058f97)

Interleaved A/B (min of 7). The box was cleaned up mid-session and the noise
floor fell to ~0.3% (self-A/B), so small effects became measurable again:
   FSE table pool:        l1 -6.3%, l2 -5.5%, l3 -4.5%, l4 ~0%, l7 -2.0%
   block cache on top:    l1 -2.2%, l4 -2.3%
   FSE bitwriter on top:  l1 -0.6%, l4 -0.2%
Pins 4-10, l7 SHA-256, ctest 4/4 and a -T 4 round-trip are unchanged.

Negative result: vectorising the rANS output reverse-copy (SSE `pshufb` over
16-byte blocks) was neutral-to-worse (l1 +0.4%), so it was reverted -- that
copy is memory-bound, not instruction-bound.

## Remote agent branches (origin/*, 2026-10-17..18) -- what is worth taking

Reviewed all 13 bolt/jules/vise branches. Most content is already in main or
superseded: the context LUT inlining (`zgec_ctx_text_lut`), `next_lane_idx` in
`zgec_ctx_body_bits`, the AVX2 merge in `zgec_cluster_greedy`, the 8-way unroll
in `zgec_select_contexts`, AVX2 rANS decode and the decode stack fallbacks are
all present already.

Still absent from main and now imported (all bitstream-identical, pins + SHA +
ctest unchanged; interleaved A/B min of 7 on the quiet box: l4 -0.6%, l7/l9
within noise):
  - `zgec_cluster_greedy` initial copy+sum loop vectorised (AVX2 load/store/add,
    exact 32-bit sum since the total is <= n_lit).
  - `zgec_select_coder`: break out of the `rep0_before[t] < 32` scan as soon as
    one is found (`slow_sublit` is a flag, so the rest of the scan is dead work).
  - `mf_bucket_hits`: explicit `nlanes == 8` / `== 16` AVX2 paths (no loop, no
    lane shift); these are exactly the main/high tier widths, so it is the one
    that shows on l4.

Do NOT take without a byte-identity gate:
  - the entropy `clogc` refactor (bolt/entropy-encode-opt): regrouping
    `sum c*log2(c)` versus `total*log2(total) - sum` reassociates floating point
    and can move coder selection;
  - the parse candidate pre-filter branches (jules-210087, bolt-fast-parse-*,
    bolt-opt-inline-match-helpers): they reorder/short-circuit probes and must
    be proven output-neutral before landing.
  - `bolt/fuzz-leak-and-enc-perf` commits compiled binaries to the repo and
    changes no source; ignore it.

2026-10-19 note -- `zgec_ctx_body_bits` rewrite (from
`origin/bolt/avx2-encode-throughput-opt`): iterate the histogram once per lane
(`start[lane]..start[lane+1]`), count the lane's first byte as a runstart, and
in the no-runstart case run a 4-wide unrolled interior loop with no per-byte
lane-boundary test or `zgec_rs_get` call. Byte-identical; interleaved A/B at l7
(min/median of 9) -1.2% / -1.2%. Kept. (The same branch's seq.c
`_mm256_testz_si256` gate around the syms loop was not taken: it still does the
scalar LUT lookups, so it only hoists a branch.)

Measured NEGATIVE result -- `fast_parse` candidate pre-filters (tried directly
on l4, the parse-bound tier): adding a single-byte gate at
`max(best_len, MIN-1)` before the 64-bit xor in the rep and long probes is
byte-identical but **slower** (l4 +3.7%, l6 +2.5%). The common case is a byte
that *does* match, so the extra load+branch is pure overhead on top of the xor
the original already skipped. Do not re-attempt without a different structure.

Measured NEGATIVE result -- AVX2 gather over `zgec_seq_lut512` (branch
`bolt-fse-seq-opt-task2`): pad the LUT to 513 entries, load 8 values with
`_mm256_i32gather_epi32(..., scale 2)`, shift/and to the code byte and pack.
Byte-identical, but **neutral at l1** (0.0%) and **+0.4% at l4**; the gather
costs as much as the eight scalar L1 loads it replaces on this CPU. The same
branch's `binfo[256]` (base+nbits in one 8-byte record) and the folded `next[]`
pass in `zgec_fse_build_dec` are also in the noise. Reverted.

WINDOWS GOTCHA: `grp1`, `grp2`, `grp3` are macros in the MinGW/Windows headers
(they expand to numeric resource constants), so local variables with those
names do not compile here with `-Werror`. Use `ga`/`gb`/`gc`/`gd`.

REJECTED for correctness -- multi-context rANS full-lane hoist
(`jules-143888259400267764`, `bolt-opt-rans-fse-encode-throughput`): the
unguarded "round < len[7]" fast path drops the `idx_lut[Z[j-1]] == 0xFF`
sentinel test before indexing `tables[table_idx]`. `tables` has k+1 entries,
so a 0xFF sentinel indexes far out of bounds. Do not take it unless the
sentinel is proven impossible for k>1 and an assert/fuzz run backs that up.

REJECTED for safety -- decode branch `jules-decode-opt-rans-match-copy`:
the seq_decode_cond changes delete the `symbol` range test, the `pad > 32`
test and the whole `bad_symbol` path, so a malformed stream can index the
symbol/extra tables out of range (and shift by an unbounded amount). Its
reported l7/l10 decode gain (~10%) comes partly from that removed work. The
match-copy dispatch rewrite in the same commit is worth porting *with* the
validation kept, if a decode pass is ever done.

`bolt/fuzz-leak-and-enc-perf` commits compiled binaries and changes no source.

Session 2026-10-19 -- closing the l7 emit gap and the parse split.

PROFILED, no byte-identical win -- the ~15% of l7 inside emit that the phase
timers did not name is the block-level context work: zgec_select_contexts on
the plain literals 5.1%, the D6 sub-context residual build 2.2%, and
zgec_select_contexts on the residuals 6.7%. Inside one call the split is
runstart 0.7%, 4x64x256 histogram 2.5%, mode ranking 0.3%, and
zgec_cluster_greedy 8.0% of total. The clustering is the cost: the seed
evaluates 2016 pair deltas, each a 256-term floating-point accumulation whose
order is part of the bitstream, so the dependent add chain (4 cycles/term)
cannot be reassociated or vectorised without changing output. Fusing the
sum into the seed (no 1 KiB temp row), hoisting the outer row into L1, and
dropping the per-symbol zero test all measured neutral; reverted. Treat the
greedy merge as a floor.

PROFILED -- parse at l7 is main find 42% (36.8M calls), the lazy block 28%
(13.05M probes at ip+1), and the rest of the loop body 29%. (Careful: a
timer placed after the LL/ML/OF mode picks includes zgec_emit_lit_stream,
which sits in the same else-block -- it is not all table build.)

Measured NEGATIVE -- prefetching the bucket of the position after next
(ip + 2*nstep) in the parse skip schedule: byte-identical but l7 min +1.5%.
One find of lead time is already enough; the extra hash+prefetch is overhead.

ACCEPTED -- zgec_fse_build_enc (commit 68ad5ec): only absent-symbol rows of
the nsym*2^al encoder table are zeroed (present rows are filled completely by
the occurrence loop), the per-range fill uses an AVX2 8-byte broadcast store
four entries at a time, and the coverage/duplicate checks are debug-only.
Byte-identical (pinned sizes and l7 SHA unchanged). Interleaved A/B, min of 7
on a pinned core: l7 7429.2 -> 7191.7 ms (-3.2%), l4 2030.1 -> 2000.9 ms
(-1.4%); medians -3.0% / -2.1%. ctest 4/4 and a -T 4 round-trip clean.

Session 2026-10-19 (cont.) -- rANS k == 1 lane-store chain.

PROFILED -- fast-tier literal rANS is ~22% of l1 (ZGEC_RANS_PROFILE split:
nwords/n_lit 0.39, so the output reverse-copy is only ~0.8%; the encode loop
is ~8.2 cycles/literal). zgec_rans_encode k == 1 unrolled eight lanes but
serialised every one through `nwords`: each `words[nwords] = x; nwords += need`
made the next lane's store address depend on the previous commit.

ACCEPTED -- rans.c: a store-free `zgec_rans_encode_core[_simple]` returns the
next state / pre-shift word / commit bit; the k == 1 round computes the eight
lanes and writes them at prefix-summed offsets in lane order 7..0 (lower lanes
overwrite earlier uncommitted words at a shared offset, as the serial version
did). The unconditional store for f == M is harmless: a committed slot at that
offset is overwritten by the lane that commits it, and an uncommitted one is
slack. Forcing always_inline matters -- without it clang emitted a real
zgec_rans_encode_core call per symbol and the change was slower. A one-time
scan of the 256 frequencies selects `_simple` (no f==0/1/M tests) when all are
in [2, M-1]; otherwise the checked core keeps the old error behaviour.

Byte-identical: pinned sizes l1..l10 all exact, l7 SHA
13AF5E256B1731FEAC2FD795A51B9E9A8073F75E14990B75D2006D79FD1A4CDE, ctest 4/4,
-T 4 round-trip clean. Interleaved A/B (pinned core, min of 9): l1 1135.8 ->
1107.0 ms (-2.5% min; medians 1149.0 -> 1127.7, -1.9%); the hoisted simple
path alone was ~1% of that.

Session 2026-10-19 (cont.) -- literal histogram built once per segment.

PROFILED -- segmentation is ~10% of l1. Its own split (temp rdtsc counters):
literal/sequence histogram construction ~65%, greedy merge ~6%, seg_dist
resolve ~9%, the two offbase rewrites ~19%. The block literal stream was
being scanned three times: once for the granule histograms in
zgec_segment_greedy, once in zgec_select_coder for scoring, and once more in
zgec_rans_histograms at emission.

ACCEPTED -- zgec_seg_prep_build now builds the plain-literal byte histogram
(zgec_hist_lit_slice, the same 4-accumulator loop) and prep.h_lit is passed to
both zgec_select_coder (copied instead of rescanned) and
zgec_emit_lit_stream, which uses it directly when lit_form == 0 and k <= 1
(exactly the single-table histogram zgec_rans_histograms would produce).
Literals-only segments (n_seq == 0) return early from prep before the arrays,
so they build h_lit in that early-return path too -- missing that changed the
l1 size by +1.3 MB until fixed.

Byte-identical: l1 80,567,687 / l2 79,091,618 / l3 68,006,258 / l4 65,059,955 /
l7 61,909,039 and l7 SHA 13AF5E...4CDE, ctest 4/4, -T 4 round-trip clean.
Interleaved A/B vs bb45bc8 (min of 9/7): l1 1122.2 -> 1115.0 ms (-0.6% min,
medians 1142.1 -> 1119.8, -1.9%); l3 1794.7 -> 1772.0 ms (-1.3% min, median
-0.3%). Emission's third scan is gone; prep still scans each segment once.

Session 2026-10-19 (cont.) -- raw bit-writer for fused sequence fields.

PROFILED -- zgec_fse_encode's serial loop (seqtbl encode ~15% of l1, ~14% of l3)
dominates on the state chain but the inlined bit-writer still paid two
data-dependent costs per symbol: an if (n == 0) return branch (n is the fused
transition+extra width, and n == 0 is common: a high-count state paired with
a no-extra-bits code) and a redundant mask recompute
(n == 32 ? ... : (1ull << n) - 1) + AND.

REJECTED -- a preflight that hoisted the row[0].pad marker, the nsym check and
the nbits mask into a per-symbol table, with a three-stream lockstep encoder
on top. The 3-lane version spilled badly (304 stack refs, 6.6 KB body) and was
~20% slower; the preflight alone was neutral-to-slower because the mask table
is a stack load and the pad/nsym scans cost real work. Also tried a full n-byte
max(syms) preflight: pure overhead, removed.

ACCEPTED -- zgec_fse_bw_write_raw: same drain as zgec_fse_bw_write_local, but
no mask and no zero test. Valid because the caller passes a transition field
fused with its extra bits, which is < 2^n by construction (bits < 2^cn_entry,
extra < 2^nb, shifted by cn_entry). n == 0 stays branchless: val == 0, nacc
unchanged, the (already unconditional) 32-bit store is simply overwritten.
Used only in the base && nbits branch; the two-write fallback is untouched.

Byte-identical: pinned sizes l1..l10 exact, l7 SHA
13AF5E256B1731FEAC2FD795A51B9E9A8073F75E14990B75D2006D79FD1A4CDE, ctest 4/4,
-T 4 round-trip clean. Isolated encode-phase rdtsc (min of 6): l1 424.5M ->
395.5M ticks (-6.8%), l3 770.7M -> 687.7M (-10.8%). Whole-file interleaved A/B
(pinned core, min of 9): l1 902.7 -> 898.9 ms (-0.4% min, medians -1.1%);
l3 1569.7 -> 1551.0 ms (-1.2% min, medians -1.8%).

Session 2026-10-20 -- inbound agent branch triage (12 single-commit branches).

MEASUREMENT METHOD FIX (important). A fresh `git worktree` has no CMake
compiler cache, so `cmake -G Ninja` silently picked gcc from PATH while the
main build tree uses clang. The gcc binaries were ~12% slower at l1 than the
clang ones, which initially made every candidate look like a regression at
l1. Always configure worktrees with the explicit clang path and verify with
`ninja -t commands ... | select-string clang` before trusting any A/B.

ACCEPTED -- `avx2-encode-optimization` (d9e16b7), fast-forwarded. AVX2
histogram banks in `zgec_hist_lit_slice`/`zgec_enc_sample_stats`, factored
`total*log(total)` entropy estimate, vectorised runstart sum and seq code
lookups. Byte-identical (all pinned sizes + l7 SHA), ctest 4/4, -T 4
round-trip clean. Paired interleaved l7 (10 reps, median of pair deltas):
-192 ms on ~7.3 s (-2.6%), 8/10 wins; l1/l3 neutral.

ACCEPTED (match.c only) -- `jules-2393332296035115024` (c60687c). Candidate
filter takes the 64-bit difference first and derives short-target length from
ctz, dropping the pre-filter gate. Byte-identical at l1..l10 + l7 SHA, ctest
4/4. Paired l7 (10 reps): median -1.53%, 7/10 wins; l3 neutral. The branch's
`cli.c` change (filter = 0 for levels 15-25) was dropped: it is a
compression-quality regression on the top tiers and unrelated to speed.

REJECTED -- `bolt-3-literal-rans-encode-opt` (manual 8-lane k==1 step): +4.4%
l1 / +3.5% l3, 0-1/6 wins. The compiler's SLP form of the existing round is
better; do not hand-roll the lane step.
REJECTED -- `jules-11811750267246473172` (unrolled k>1 rounds): +3-4% l7/l9.
REJECTED -- `bolt/opt-match-finder-tag-fast-path`: tag!=0 probe fast path,
l1 +0.5% min (median dominated by an outlier), l3/l7 within noise.
REJECTED -- `bolt/encode-throughput-fast-ctx-opt` as-is: its
`ZGEC_FAST_INS_SPAN 0 -> 256` changes the fast-tier parse and moves l2
(79,097,254) and l3 (68,007,671). With parse.c reverted the rest measured
l1 -0.7%, l3 -0.9%, l7 +2.2% (worse) -- not worth the split.
REJECTED -- `avx2-encode-path-simd-opt`: neutral everywhere (l7 median 2/5
wins).
REJECTED (decode) -- `bolt-decode-throughput-opt`, `bolt-decode-rans-match-opt`,
`bolt-opt-decode-cache-atomic-sync`, `bolt/avx2-decode-path-opt`: l7 decode
495 ms baseline -> 516 / 521 / 497 / 501 ms. Only the vise mem branch won.

Session 2026-10-20 -- per-granule literal histogram cache for the D3 re-merge.

IDEA -- zgec_segment_greedy runs twice per block (first from the whole-block
rep chain, then again after the offbase rewrite). Per-granule lit histograms
depend only on granule boundaries and the LL prefix sums, neither of which the
rewrite changes, so the second call can read the first call's histograms.

ATTEMPTED and REJECTED. Implemented with an explicit snapshot (the merge
compacts grans in place, so it must be taken before merging), plus ownership
on every error path. Byte-identical (all pinned sizes, l7 SHA, ctest 4/4).
Confirmed live: 22 re-merges at l1 and the cache is consumed 116 times
(~5 granules per call).

Paired interleaved A/B (min of 10/6, pinned core): l1 +0.45% median, 3/10
wins; l3 +0.07%, 5/10; l7 +0.84%, 2/6. No gain. The 8-accumulator literal
sweep over the block literals (76 MB total at l1) is only a few ms even when
repeated; the snapshot allocation, memcpy and extra live memory cancel it.
Conclusion: the second greedy pass is already cheap -- do not revisit literal
caching, and treat the remaining per-segment histogram rebuild as a ~1%
ceiling, which does not justify caching the full granule build.

Session 2026-10-20 (cont.) -- parse insert prefetch, LTO, FSE entry load,
conditioned-stream drains.

REJECTED -- prefetch pipeline for the dense insert walk (mf_insert_dense):
issue zgec_matcher_prefetch 8 positions ahead while inserting. Insert is 22%
of l7 parse (5.1G of 23.1G ticks), and the write-allocate misses looked
exposed. Paired l7: +2.3% min, 3/8 wins; l10 -0.9% min / +0.35% median, 4/8;
l3 neutral. The extra hash computations and the doubled table touch cost more
than the hidden latency. Do not batch/lead-prefetch the insert grid.

REJECTED -- LTO (-flto on compile and link, separate build dir). l1 -0.3%,
l3 +1.1% (worse), l7 -2.1% min / -1.4% median (4/6). Mixed and the l3
regression is real; not worth the build-time cost. Note: -flto produced
byte-identical output at all pinned levels.

REJECTED -- single 8-byte load of zgec_fse_enc_entry in the plain FSE loop
(__builtin_memcpy of the entry, field extraction by shift). The disassembly
showed three loads (offsets 0/2/4) and a spill of new_state through the stack
on the state chain. One word load did not remove the spill and lengthened the
chain: l1 +2.1% median, l3 +3.0% min (2/8 wins), l7 +3.7% min. The existing
field-wise loads schedule better; leave the loop alone.

REJECTED -- hoisting the per-symbol row[0].pad check to a once-per-row prescan
over [0, nsym). Absent symbols own a zero-filled row and legitimately have
pad == 0, so the prescan rejects valid tables (caught immediately as
"compress failed: internal error" at every level). pad is also per-row only
because every cell of a present row is rewritten; it is not derivable from the
entry that row[state] loads. Keep the per-symbol check.

ACCEPTED -- branchless drains in zgec_seq_stream_encode_cond (seq.c). The
conditioned LL/OF encoder wrote the transition field and the value's extra
bits with two WRITE_BITS_COND flushes per symbol; each branched on the
word-complete coin flip and on nbits[s] > 0, so it mispredicted roughly once
per symbol. Replaced with the same unconditional-store/branchless-advance
drain the plain FSE loop uses (two drains, so the fused 64-bit shift bound is
never approached), and hoisted the three table pointers, the shared nsym and
the use_prev back-offset out of the loop.

Byte-identical at every pinned level (l1-l10 sizes and the l7 SHA).
Isolated emit-phase ticks (4 interleaved pairs, min): cond stream encode
-11.6% at l7 and -4.4% at l3, with the literal phase flat (control). Whole
file l7: -1.1% min / -0.25% median, 7/12 paired wins -- the phase change is
~1.7% of l7 and the wall clock is at the noise floor. ctest 4/4.

Profiling note: an env-gated rdtsc split of zgec_emit_segment is the useful
signal here (lit vs seq separately); the changed phase moved 11% while the
whole-file delta was inside the +/-1.5% run-to-run drift. The emit split at
l1/l3/l7 is lit 612/344/937 Mticks and seq 418/704/1057 Mticks, i.e. the
sequence coder is now the larger half of emit at main/high levels.

Session 2026-10-20 (cont.) -- fast-tier rung: per-segment histograms from the
merge.

ACCEPTED -- reuse the greedy merge's per-segment histograms in
zgec_seg_prep_build. The merge already accumulates exact LL/ML/OF and literal
counts per granule (that is how it prices a merge), and its final granules are
exactly the emitted segments: boundaries fall between sequences and merging
only adds whole granules, so the per-segment literal span is the granule's
lit_of_seq span. zgec_segment_greedy now also returns the final granule array
inside the same allocation as the bounds (one zgec_free(bounds) releases
both), and zgec_seg_prep_build takes it: the emit pass copies the four
histograms plus extra_bits and ml_sum instead of re-deriving three coded
streams and re-sweeping every literal. The gathering of ll/ml/of/mls still
runs; only the counting disappears.

Liveness guard: the D3 rewrite can change offbase values after the merge
priced them. When the final rewrite reports a change (or the re-segment
fails), use_grans is cleared and prep rebuilds everything exactly as before.
Both paths produce identical output (verified by forcing use_grans = 0 and
re-checking the pinned sizes).

Byte-identical at every pinned level (l1-l10 sizes and the l7 SHA-256).
Isolated prep-phase ticks (5 interleaved pairs, min): l1 325.8 -> 144.5 Mticks
(-56%), l4 518.9 -> 236.1 (-54%). Whole file, 9 interleaved pairs: l1 min
985.2 -> 903.6 ms (-8.3%), median -6.6%, 8/9 wins; l4 min 2057.9 -> 2033.1
(-1.2%), median -2.1%, 7/9. ctest 4/4.

Measured NEGATIVE (reverted) in the same session: 4-way replicated LL/ML/OF
histogram accumulators in zgec_seg_prep_build (to break store-to-load
forwarding on runs of equal codes). l1 prep min 316.7 -> 332.4, l4 516.2 ->
527.7; the replica zero-fill, indexing and final sum cost more than the stalls
they remove. Also NEGATIVE: assigning the FSE loop's next state before the bit
writes to keep it out of a stack spill (same senc ticks as HEAD, 13.5
ticks/symbol either way -- that loop is throughput-bound, not latency-bound,
despite the spill).

Session 2026-10-20 (cont.) -- default worker count for the full fast parser.

ACCEPTED -- resolve n_threads == 0 to one worker per PHYSICAL core for the
fast tier at its default speed dial (rungs 4-6), and keep the logical count
everywhere else. zgec_cpu_physical_count() folds SMT siblings (Windows
GetLogicalProcessorInformation, Darwin hw.physicalcpu, Linux
/.../cpu0/topology/thread_siblings_list) and falls back to the logical count,
i.e. the old behaviour, when the platform cannot name them.

Why only that slice of the ladder: the default fast parse keeps two 256 KiB
tables, which is exactly the 512 KiB L2 a Zen 3 core owns; a second SMT thread
on that core evicts the tables instead of filling the first thread's stalls.
The lean-dial rungs (1-3) use 64-256 KiB of table and are issue-bound, and
the main/high parsers have L3-resident tables and are latency-bound, so all of
those still gain from the SMT threads (measured: l1 1,023 -> 1,160 MB/s and
l7 107 -> 138 MB/s from 6 to 12 workers).

Interleaved A/B, min of 5 (6 vs 12 workers, full corpus): l4 654.1 vs 826.8 ms
(324.0 vs 256.3 MB/s, 1.26x), l5 724.2 vs 866.4 (1.20x), l6 853.5 vs 926.1
(1.09x). Byte-identical at every pinned level (l1-l10 sizes, l7 SHA). ctest
4/4; dedup gained a range check for the new counter.

Scaling diagnostic worth remembering: six single-threaded encoder processes
pinned to six distinct physical cores take 4.95 s wall for the corpus, i.e.
257 MB/s aggregate against 324 MB/s for one in-process six-worker run -- each
process runs ~2.1x slower than alone. The per-worker slowdown at 6 workers is
therefore a shared memory/L3 limit, not thread-pool overhead; the pool is
already near that ceiling, and only the SMT oversubscription above was
recoverable.

Measured NEGATIVE (reverted) in the same session: three-stream fused FSE encode.
zgec_fse_encode3 interleaved the LL, ML and OF state chains (all one symbol per
sequence) in a single loop, on the theory that the standalone loop's ~13.5
ticks/symbol is one dependent encode-table load and three chains would keep
three loads in flight. Pinned single-core A/B against the same binary with the
fused branch forced off (9 interleaved pairs, min): l1 872.5 vs 865.8 ms (+0.8%,
2/9 wins) and l4 1930.4 vs 1923.6 (+0.4%, 4/9). Neutral-to-worse, so the loop
really is store/throughput-bound as the earlier spill experiment suggested,
and the third table's L2 footprint cancels any latency it could have hidden.
Unpinned runs had shown a bogus 4-7% "win" at l2/l4; whole-file wall clock
must be affinity-pinned here, the unpinned medians swing by more than the
effect.

Session 2026-10-20 (cont.) -- fast-tier literal copies.

ACCEPTED -- parse_copy_lit in fast_parse and fast_parse_lite. Both loops called
a variable-length memcpy for the literal run in front of every accepted match,
and those runs are usually only a few bytes, so the call (size dispatch plus
prologue) cost more than the copy. A run of <= 8 or <= 16 bytes now moves a
fixed 8- or 16-byte block instead: the literal buffer keeps ZGEC_LIT_SLACK
past the block, the virtual buffer keeps 64 bytes past its end, anchor <= ip
<= end - 8 when the copy runs, and the bytes past ll are overwritten by the
next copy or the tail copy before the literal region is read. Longer runs keep
the plain memcpy. The main/high parser was left alone: its literal runs are
longer, its per-match work already dominates, and its whole-file A/B at l7 was
inside the noise either way.

Byte-identical at every pinned level (l1-l10 sizes, l7 SHA). Affinity-pinned
interleaved A/B against the same tree without the helper: l1 min -3.0% (5/7
wins) and -3.1% / median -5.9% (7/9) in two sessions, l2 -3.5% min / -3.7%
median (8/15), l4 -3.0% (5/7) when quiet and neutral (-0.6% median, 9/15)
while the box was loaded. ctest 4/4.

Measurement caveat for this session: a game was running on the machine, so
absolute times drifted by up to 60% and unpinned runs were useless; only
affinity-pinned interleaved pairs with min/median over 7-15 repetitions gave a
stable answer. Also re-measured NEGATIVE (reverted): raising ZGEC_POOL_MAX_BLOCK
from 4 MiB to 16 MiB (l1 +0.5%, l4 flat) -- the per-thread pool already covers
every transient the encoder allocates.

Session 2026-10-20 (cont.) -- lean-core match scan out of line, no-literal-ref copy.

ACCEPTED -- fast_match_len_out for fast_parse_lite (level 1). fast_parse_lite
inlined fast_match_len_tail (always_inline, AVX2 scan) into its position loop,
and the live values of the accept path cost more registers than the loop has:
the miss path reloaded `anchor` from the stack, stored it back on every
position, and reloaded the table base, i.e. three extra memory operations on
the ~9 of 10 positions that find no candidate. The scan now sits behind a
noinline wrapper: the miss path lost all three (verified in the objdump:
movq 0x98(%rsp) store, movq 0x98(%rsp) reload and movq 0x88(%rsp) reload gone,
anchor and tl stay in r13/r14), and the scan itself is byte-for-byte the same
code. Pinned interleaved A/B, 9 pairs, thread-cycle counts (ZGEC_CPUTIME, which
excludes descheduled time and is what this box needs): l1 min -0.7% / median
-1.8%, 7/9 wins. Byte-identical (l1 SHA F28B157E..., l7 SHA 13AF5E25...).

Rejected in the same session:

- Same treatment for fast_parse (levels 4-6, all eight tail call sites).
  The frame shrank (0x108 -> 0xc8) but the loop still spills the current word
  and the dial-3 branch's state, so the win did not materialise: l4 min -1.6%
  / median +1.9% (4/9), l6 min -0.4% / median +3.5% (3/9). Reverted.
- Skipping the per-block literal buffer copy (zgec_alloc + memcpy of n_lit
  bytes in zgec_encode_block_full) when use_litref is off, which looked like
  pure waste. It is a consistent 1% *regression* (l1 min +1.1%, 15 pairs, 4/15
  wins; 9 pairs +1.0%, 2/9), reproducibly, so the copy helps by touching the
  pool block that a later phase reuses. Kept as is. Forcing res->exportable to
  0 in that path is not an option anyway: the flag is emitted as
  ZGEC_RFLAG_LIT_EXPORTABLE, so it changes the frame bytes at equal size.

cli.c now reports `zgec: thread cycles N` under ZGEC_CPUTIME=1 (Windows
QueryThreadCycleTime, rdtsc elsewhere). Wall-clock medians on this box swing by
2-3% while freebuff/GTA5 hold 30-60% of every core, so cycle counts are the
only usable signal; mins over 9+ interleaved pairs still needed.

Session 2026-10-20 (cont.) -- FSE entry width, staging gathers, both NEGATIVE.

Tried and reverted: packing zgec_fse_enc_entry from 8 bytes to 4 (nb_bits 4
bits, new_state and baseline 11 each, bit 26 the old pad/presence marker;
_Static_assert ties the widths to ZGEC_MAX_AL). The sequence table the encoder
walks is nsym * 2^al, i.e. 66 * 1024 * 8 = 540 KiB, just over the 512 KiB L2,
so halving it to 270 KiB looked like the way to stop the serial
state -> table-load -> new_state chain from missing L2 on every symbol.
Byte-identical at every pinned level (sizes, l1 and l7 SHAs, ctest 4/4), but
pinned interleaved A/B, 9 pairs, cycles: l1 min +0.26% (6/9 wins), l4 min
+2.81% / wall +2.28% (2/9). The extra shift/mask on the chain costs more than
the smaller footprint saves, which also says the loop is not L2-miss-bound at
all -- consistent with the earlier three-stream result, the bit-writer store
is the real serialiser. Reverted whole (fse.c, seq.c, zgec_fse.h).

Also tried and reverted: dropping the three vpgatherdd in
zgec_seg_prep_build's staging loop in favour of the scalar path that already
exists below it (same outputs, g != NULL still takes the granule histograms
from the merge). l1 min +0.10% (5/9), l4 min +1.48% (4/9): the gathers are
not the problem either. Reverted.

Standing conclusion for the encode ladder: every remaining phase is within a
few percent of the best shape found so far. The one measured lever left on
this box is the measurement itself -- freebuff/GTA5 hold 30-60% of all cores,
and even on-CPU cycle counts need 9+ interleaved pinned pairs to resolve 1%.

Session 2026-10-21 -- quiet box, fresh phase split, one real win.

freebuff settled to ~8-40% of a core instead of 30-60%, so pinned on-CPU cycle
counts finally resolve 1% with 9 pairs. Re-instrumented encode.c (temporary
ZGEC_PHASE_LOG=1 rdtsc block, removed before committing) and got this split at
-T1, pinned 0x200, 211,938,580-byte corpus (Mticks = 1e6 ticks):

  l1 (3.40G): parse 1325, emit 1287 [lit 660, tbl 74, seq 417], seg 387,
              prep 161, other 156
  l4 (7.69G): parse 5407, emit 1263 [lit 299, tbl 114, seq 800], seg 683,
              prep 260
  l5 (9.52G): parse 5103, ctx 1291, emit 1761 [lit 880, seq 731], seg 647,
              prep 266, sel 329
  l6 (12.9G): parse 5496, d6 2295 (residual build 0.8G + select 0.93G),
              ctx 1319, emit 1792 [lit 865, seq 774], seg 760, prep 683,
              sel 603, vb 132

select_contexts splits [hist 381 / cluster 877] per call at l5, so the class
histogram build (4 modes x 64 KiB, 31.3M literals) and the greedy merge with
its thousands of 256-entry entropies were each ~9% of l5 and ~8% of l6 x2.

ACCEPTED a365f84 encode: exact integer entropy in the cost models.
zgec_fast_log2_u64 is e + mant/256, exactly representable, so c*log2(c) and
every partial sum are exact multiples of 1/256 well under 2^53: accumulate
256*log2(c) as integers (u16 table over 64K counts, four accumulators) and
divide by 256 once. Bit-identical, summation-order independent, and it drops
the 256-step clz + double multiply + add loop per entropy call.
Pinned interleaved A/B, 9 pairs, cycles: l1 -0.55% (5/9), l5 -2.45% (7/9),
l6 -5.72% (9/9), l9 -2.59% (5/9). select_contexts entropy work at l6 fell
from 2220 to 1764 Mticks total (both calls).

ACCEPTED f276867 rans: eight-lane round for the k > 1 literal coder.
The multi-table coder was the last serial path in emit; only the table choice
differs per lane (idx_lut of the predecessor, else the own table at lane/run
start), word commit order stays lane 7 down to lane 0, tail rounds stay
serial. Byte-identical. l6 -0.8% min / -1.4% median, 11/15 pairs; l5, l7
noise. Below the 2% bar but kept: it deletes a duplicate slow implementation
rather than trading one shape for another.

REJECTED (reverted): building the four select_contexts class histograms in
four passes instead of one fused pass, so each pass' table is 64 KiB instead
of 256 KiB. Literal re-reads and 4x loop overhead swamp the locality gain:
l5 min +4.53% (0/9), l6 min +7.30% (1/9). The fused 4-table pass is right,
which also says those increments are not L2-latency-bound.

Still the three biggest blocks after these two commits: fast_parse (43% of l6,
56% of l5, 70% of l4 -- the two 1 MiB hash tables are random-touch and
L3-latency-bound; dense-insert prefetch and the match-length out-line were
already measured negative), the l6 D6 residual build (the same residuals are
built again in zgec_seg_prep_build; sharing the block-level buffer needs a
per-segment "built" flag because D6 skips segments with no sequences), and
emit seq FSE at ~16.6 ticks/symbol, already at the serial state chain floor.

Session 2026-10-21 -- jules-2 context & sub-literal optimization (levels 7-10).

ACCEPTED -- Vectorized 4-wide AVX2 unrolling in `zgec_cluster_greedy` for initial class histogram copying & horizontal summation, singleton delta seeding, candidate delta verification, and cluster merging (`s += 32` per step). Added short-circuiting (`ctot == 0`) for empty classes. In `zgec_select_coder`, tracked `rs_built` to eliminate redundant `runstart` bitmask rebuilds. In `zgec_ctx_body_bits` & `zgec_select_contexts`, pre-computed `grp_off_lut` (`grp * 256u`), added single-context (`k <= 1`) 4-bank parallel accumulation, and replaced per-byte stride multiplications with bitwise shifts (`<< 8`).

Byte-identical across all levels 4-10 (all pinned sizes exact, ctest 4/4 passed).
