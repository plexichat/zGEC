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
