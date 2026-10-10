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

## 2026-10-12 - Phase B Decompression Fast Match Copying and Pattern Broadcast

**Learning:**
1. In LZ77 decompression Phase B (`exec_match`), small match offsets (`off < 8`, such as `off = 2, 3, 4..7`) and short match lengths (`len == 3`) previously fell through to scalar single-byte loops (`for (i=0; i<len; i++) d[i] = src[i]`), creating a major decoder bottleneck.
2. For 2-byte offsets (`off == 2`), replicating `zgec_rd16(src)` across a 64-bit register (`uint64_t v64 = v16 * 0x0001000100010001ULL`) and copying 8-byte chunks in-phase at `d + i` (where `i` is a multiple of 8) eliminates scalar iterations safely.
3. For 3-byte offsets (`off == 3`), unrolling 3-byte assignments `(s0, s1, s2)` avoids byte-by-byte loops.
4. For offsets `off >= 4`, copying in unaligned 4-byte, 8-byte, and 16-byte chunks (e.g. `memcpy(d+i, src+i, 4)`) is safe because `src + i + 4 <= d + i`, meaning source data is strictly finalized before reading.

**Action:**
In LZ77 match reconstructors, handle `off == 2` with 64-bit pattern broadcast and `off >= 4` with chunked unaligned `memcpy` steps while avoiding phase-shift errors on odd target offsets.
