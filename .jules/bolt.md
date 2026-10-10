## 2026-10-10 - rANS and Match Copying Hot Loop Fast-Paths

**Learning:**
In zGEC, default single-context literal streams (`k = 1`) previously shared decoding, encoding, and histogram counting loops with multi-context streams (`k > 1`). Evaluating `if (k > 1)` and computing context table indices (`ti * 2048`) inside the 8-lane inner loops introduced unnecessary branch instructions and index math per literal byte. Specializing the `k = 1` loops removes these branches and allows tight, branchless lane iteration. In addition, when `n_tables = 1`, `zgec_rans_histograms` can bypass 8-lane geometry and run a simple sequential 4-unrolled loop `tables_counts[Z[i]]++`. For Phase B LZ77 execution, handling `off == 1` with `memset` and `off >= 16` with 16-byte unaligned `memcpy` significantly reduces match reconstruction latency.

**Action:**
When optimizing entropy coders or LZ77 match executors, always separate the single-context or common default fast-paths from multi-context fallback loops before unrolling or vectorizing.

## 2026-10-11 - LZ Match Finder and Parsing Cost Table Optimization

**Learning:**
1. In `parse_seq_cost`, calling `zgec_fast_log2_u32` per candidate match symbol inside the LZ parser's inner price-gate loop introduced millions of redundant log calculations per block. Precomputing `ll_cost`, `ml_cost`, and `of_cost` tables updated whenever running histograms change turns `parse_seq_cost` into direct table lookups, cutting its runtime by >60%.
2. In `mf_match_len` and `mf_match_len_slow`, checking the first 8 bytes in both inline `mf_match_len` and `mf_match_len_slow` caused duplicate 64-bit XOR and trailing zero count operations for matches >= 8 bytes.
3. In `zgec_matcher_insert_match`, calculating sample offsets `(len - 1) * t / (nsamp - 1)` invoked 64-bit integer division (`idiv`) per sample. Specializing for fixed sample counts (16, 3, 1) or `nsamp == len` replaces `idiv` with fast constant division (`/ 15u`) or bit shifts.

**Action:**
In LZ parsing price gates and sample generators, pre-compute log-probability costs into per-symbol float arrays and eliminate variable integer divisions in sample offset calculations.

## 2026-10-12 - Long-Table Candidate Filtering and FSE/rANS Loop Specialization

**Learning:**
1. In `zgec_matcher_find`, long-table match candidates previously triggered `mf_match_len` scans without checking if the candidate's character at offset `best.length - 1` matched the target byte. Adding `if (best.length == 0u || ... || vb[ip - d + best.length - 1] == vb[ip + best.length - 1])` before calling `mf_match_len` eliminates millions of redundant match length comparisons per block when `best.length > 0`.
2. In `zgec_fse_decode` and `zgec_fse_encode`, sequence stream operations always pass `syms == NULL`, `out != NULL`, `base != NULL`, and `nbits != NULL`. Specializing the inner FSE decode/encode loops for this case removes several per-symbol branch evaluations inside high-frequency loops.
3. In `zgec_rans_encode` for `k = 1`, separating full rounds (`round < len[7]`) from tail rounds removes the `if (round >= len[lane])` check across all 8 lanes in the main encoding loop.

**Action:**
Always filter long-table match candidates against the current `best.length` character before invoking full match length scans, and specialize FSE/rANS inner loops for non-NULL sequence argument fast-paths.
