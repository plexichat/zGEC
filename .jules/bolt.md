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
