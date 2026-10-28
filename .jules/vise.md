## 2024-10-09 - Repeat Match Price-Gate Cost Evaluation and Stack-Allocated rANS Lookup Table
**Learning:** Selecting candidate matches based purely on length in the match finder discards repeat offsets when a non-repeat match is slightly longer. Repeat offsets carry 0 extra bits for distance encoding, so evaluating repeat-match price scores (`parse_score`) directly against long/short table matches significantly improves bitstream density without sacrificing seekability or speed. Additionally, single-context rANS decoding table (`ntab == 1`) fits safely in an 8 KB stack buffer, eliminating dynamic memory allocations during hot-path symbol decoding.
**Action:** Always evaluate candidate price scores (`parse_score`) for repeat offsets independently of match-finder candidate length rankings, and prefer stack allocations for single-context entropy decoding tables.

2026-10-10 - Elimination of rANS Decoder Hot-Path Heap Allocations and Price-Gate Cost Fix
Learning: Allocating rANS packed lookup tables (`pk`) on the heap via `zgec_alloc` for multi-context decoding (`ntab > 1`) caused repeated dynamic memory allocation overhead and footprint thrashing during block decompression. Since the maximum context count `ntab` is bounded at 9 (k <= 8 plus 1 run-start table), allocating a fixed 73.7 KB stack buffer (`9 * ZGEC_RANS_M` uint32_t elements) completely eliminates dynamic heap allocations on the rANS decoding hot path without exceeding standard thread stack limits. In addition, fixing an argument type mismatch in repeat match price score calculation (`parse_score`) in `src/parse.c` ensured that precomputed float cost tables are used instead of raw histogram counts.
Action: Stack-allocate rANS packed lookup tables up to the maximum context bound `ntab <= 9`, and verify argument types in price-gate cost evaluation functions.

2026-10-11 - Elimination of FSE/Sequence Encoder Heap Allocations and Fast-Tier Repeat-Match Ranking
Learning: Allocating temporary occurrence tables (`occ`) on the heap during FSE encoder table construction (`zgec_fse_build_enc`) and temporary symbol arrays (`syms`) during sequence stream encoding (`zgec_seq_stream_encode`) produced heavy dynamic memory allocation churn and RSS fragmentation across multi-threaded encoding loops. Stack-allocating `occ` (12 KB for S <= 2048) and providing a stack fallback buffer (2 KB) for symbol conversion completely eliminates heap allocations on hot sequence encoding paths. Furthermore, in `fast_parse()`, preferring earlier repeat offsets in MTF order (`rep0` > `rep1` > `rep2`) for equal match lengths avoids unnecessary MTF state thrashing and reduces offbase symbol entropy.
Action: Stack-allocate FSE occurrence tables and bounded sequence symbol buffers during entropy encoding, and preserve MTF repeat-offset preference when match lengths are equal.




2026-10-12 - Sequence Array Allocation Consolidation and Lazy Lookahead Repeat Evaluation
Learning: Allocating four separate sequence arrays (`ll`, `ml`, `of`, `rep0_before`) per segment in `src/decode.c` and `src/dict.c` produced heavy heap allocation churn and fragmentation during decompression. Consolidating them into a single 64-byte aligned allocation `4 * aligned_seqs * sizeof(uint32_t)` (with `aligned_seqs = (n_seq + 15) & ~15`) reduces heap allocations by 75% per segment while preserving 64-byte SIMD alignment across all four pointers. Furthermore, in `zgec_parse_block_ex`, evaluating repeat offsets at `ip + 1` during lazy lookahead probing using `zgec_matcher_find_lens` ensures zero-distance-cost repeat matches at lookahead positions are selected, improving compression density.
Action: Align consolidated array section offsets to 64-byte boundaries, and evaluate repeat offset candidate scores during lazy lookahead match parsing.

2026-10-17 - Elimination of Hot-Path Encoder Allocations and Deferral of Fast-Tier Parser Sequences
Learning: Allocating temporary rANS output word buffers (`words`), class histograms (`hist4`), context selection scratch arrays (`scratch_cls`, `scratch_cond`), and granule cost arrays (`gcost`) dynamically on the heap during block and segment encoding caused significant memory churn, RSS fragmentation, and allocator mutex contention across multi-threaded encoding loops. Providing stack fallback buffers (`stack_words[2048]`, `stack_rs[8192]`, `stack_gcost[64]`) and thread-local scratch buffers (`tls_hist4`, `tls_scratch_cls`, `tls_scratch_cond`) completely eliminates heap allocations on hot entropy encoding paths. Furthermore, in `zgec_parse_block_ex`, deferring `p->seq` allocation until after checking `tier == ZGEC_TIER_FAST` avoids a redundant heap allocation and immediate free cycle per block during fast-tier parsing.
Action: Prefer bounded stack fallbacks for per-segment symbol/word buffers and thread-local storage for large per-worker histogram scratch buffers, and defer parser array allocations until after tier selection.

2026-10-18 - Large-block reuse in the allocator (churn as a memory cost) and FSE table pooling

Learning: Repeatedly allocating and freeing the same multi-hundred-KiB block
is a memory cost, not only a speed one: the system allocator decommits such a
block on free, so the next allocation re-faults every page. The encoder
entropy tables (`nsym * 2^al * 8` bytes, ~540 KiB for the sequence alphabet)
and the per-segment scratch (sequence-prep arenas ~5 * n_seq * 4 B, the stream
temp block, the per-block sequence array, the decoder tables) were all
churning this way. `zgec_alloc`/`zgec_free` now keep a bounded per-thread cache
of freed blocks in [128 KiB, 4 MiB] (at most 8, best fit), and
`zgec_fse_build_enc` rents its table from a thread-local free list. The header
now records the block's *requested* total beside the saved malloc pointer --
always a safe lower bound on the physical size, so a reused block is never
written past what it owns.

Action: When a hot path allocates and frees a large transient block in a loop,
reuse it from a thread-local pool rather than malloc/free. Keep the pool
explicitly bounded; make a released block's size recoverable from the block
itself; and prove no double-free can reach it (NULL-initialise every pooled
pointer and clear it after its single free). Blocks held at thread exit are
still reachable from the thread-local pointer, so they are not unreachable
leaks -- but the pools are deliberate retention, so they must stay capped.

Also verified for this change: no encoder-table free can double-free --
`enc_ll/ml/of` and the `enc_ll3/of3` clusters are NULLed after their one free,
so the `seg_fail_tables` error path only frees NULLs when the success path
already freed them. A -T 4 encode/decode round-trip and ASan-clean ctest pass.

2026-10-19 - Thread-Local Pool Retention in Short-Lived Worker Threads
Learning: When parallel encoding or decoding loops spawn transient POSIX/Windows worker threads, retaining cached memory blocks in thread-local storage (`zgec_pool` in `src/common.c` and `zgec_fse_enc_pool` in `src/fse.c`) across worker thread completion causes memory leaks upon thread termination under LeakSanitizer (LSan). Explicitly draining and releasing all cached blocks in thread-local pools (`zgec_thread_cleanup`) at worker thread exit eliminates the leak. (These are real leaks, not sanitizer false positives: the blocks become unreachable when the thread's TLS is torn down.)
Action: Always invoke `zgec_thread_cleanup()` before worker threads terminate in parallel work dispatch routines.

2026-10-19 - Audit of the inbound agent branches: two memory-safety regressions to reject
Learning: Two of the 2026-10-19 bolt/jules branches buy speed by deleting
validation on the *decompression* path, which consumes untrusted bytes:
  - `jules-decode-opt-rans-match-copy` (src/decode.c, `seq_decode_cond`) removes
    the `symbol` range test, the `pad > 32` test and the `bad_symbol` cleanup
    label. A corrupt stream can then drive `zgec_seq_extra` / symbol tables out
    of range and shift by an unbounded amount. The reported ~10% decode gain is
    partly the removed checks.
  - `jules-143888...` and `bolt-opt-rans-fse-encode-throughput` hoist the
    multi-context rANS loop without the `idx_lut[...] == 0xFF` sentinel test,
    so `tables[255]` can be indexed when the table array holds k+1 <= 4 entries.
Action: The encoder may assume its own buffers, but the decoder must keep every
range check. Treat "deleted bounds check" as a failed review even if the
reported speedup is large; a decode optimisation must be re-done with the checks
in place. (The match-copy dispatch rewrite in that branch is otherwise sound and
can be ported separately.)

2026-10-20 - Gating Frame Literal History Allocation and Stack Fallbacks
Learning: Unconditionally allocating multi-hundred-KiB literal history buffers (`lb`) during sequential frame decoding when `EXTENDED_LITREF` is disabled in `fh.flags` caused massive heap allocation churn and peak RSS inflation across all multi-block decompression paths, only to free the buffers without ever reading them. Checking `(fh.flags & ZGEC_FLAG_EXTENDED_LITREF)` before allocating literal history buffers completely eliminates this overhead. Additionally, providing stack fallback buffers for segment directory parsing (`stack_dir[64]`), filter inversion (`stack_tmp[8192]`), and dictionary chunk selection (`stack_sel[256]`) eliminates per-block heap allocation churn across hot decode and training loops.
Action: Always check `EXTENDED_LITREF` flag before storing literal history during frame decoding, and use stack fallback buffers for bounded segment directory and filter temporary buffers.

2026-10-20 - Reintegration and verification of the decode memory branch
Measured after cherry-picking fb530e2 onto current main: single-threaded l7
decode 495.3 -> 488.7 ms in the acceptance run; rigorous paired interleaved
(8 reps l7 / 6 reps l4, pinned core) median delta -11.3 ms (-2.3%, 7/8 wins)
at l7 and -12.9 ms (-2.0%, 5/6 wins) at l4. Decoded output SHA-256 matches
exactly, ctest 4/4, encode pinned sizes and l7 SHA unchanged.
Note: branches that delete decoder range checks remain rejected regardless of
speed -- see the 2026-10-19 audit entry.

2026-10-27 - Memory work is finished; the next lever is layout, not fewer allocations

Learning: the 2026-10-27 intake had thirteen branches in this journal's area
(vise-pool-hardening x3, vise-1-decode-stack-alloc x3, vise-2-pool-hardening,
vise-decode-stack-fallbacks, vise-decoder-stack-fallbacks,
vise-stack-fallback-buffers, vise-decoder-leak-fix-and-encoder-sbo,
jules-13906346593682127824, jules-16465769277812124083,
jules-16667722021506175466, jules-13222170761254327584). All were reviewed and
none was taken: they are allocator hardening, leak/CI fixes, or one more
malloc moved to the stack, and none of them is measurable on the current
profile. Allocation churn is no longer a throughput term - the last two
stack-fallback branches measured neutral at 9 interleaved pairs. The cycle
budget now sits in the parser (70% of l4, 56% of l5, 43% of l6) and in the
high-tier search (l7 is 116 cycles per input byte against zstd -6's ~37).

Action: do not open another "remove a malloc / add a stack fallback" branch and
do not expect it to show up on the clock. If a future allocation change is to
be justified it must come with a profile showing allocator time as a line item,
not with a code-shape argument. Large-transient reuse (the TLS pool) and the
thread-exit drain stay as they are: they are memory-cost fixes, already done.

What memory work *is* still worth doing is layout, because it changes misses
rather than call counts:
  - the fast-tier match finder's two 1 MiB tables are random-touch and
    L3-latency-bound; interleaving the two entries for one hash into a single
    32-byte slot turns one probe from two misses into one. Same logical slots,
    same candidate set, same output.
  - alignment and page behaviour of the 31 MiB literal path and the per-block
    scratch: keep 64-byte alignment (the AVX2 loads assume it is safe to use
    unaligned ops) and prefer one allocation over four for arrays consumed in
    the same pass (SoA sections of one arena, as done for the sequence arrays).
  - prefetch distance for lookups, not inserts: the rejected dense-insert
    prefetch was the wrong access to hide. The lookup line for position i+K is
    the one on the dependency chain.
These are noted in .jules/bolt.md with the numbers; coordinate with that file so
the same experiment is not run twice under two names.

Safety invariants that are not negotiable, regardless of the speed claim:
  - the decoder keeps every range check (symbol < nsym, pad <= 32, table index
    sentinel, header length vs. buffer), because it consumes untrusted bytes;
  - pooled blocks stay bounded and a released block's size stays recoverable
    from the block itself; every pooled pointer is NULLed at its single free;
  - zgec_thread_cleanup() runs before any worker thread exits;
  - every change ships with pinned sizes at all ten levels, the l7 SHA-256,
    ctest 4/4, and a measured cycle delta - a memory change that cannot show a
    cycle delta is either free (take it with the proof) or pointless (leave it).

Where the remaining budget is (cross-reference, same measurement session):
l1 16.0, l4 33.1, l5 41.5, l6 51.7, l7 115.6, l10 425.0 cycles per input byte.
Anything in this journal's area has to move the parser or the match finder to
matter; below that, the only open question is the v2 format, where interleaved
entropy states and a different block/segment policy are allowed.

2026-10-28 - Hardening Allocator Pools and FSE Table Lifetime Audit

Learning: Bounding thread-local memory retention pools and guarding against double-returns prevents memory leaks and linked-list cycles during multi-threaded encoding/decoding loops. `zgec_pool` in `src/common.c` now rejects duplicate pointer offers and enforces allocator layout assertions in debug builds. `zgec_fse_enc_pool` in `src/fse.c` is now strictly bounded to <= 8 table slots and <= 32 MiB per thread, releasing excess tables via `zgec_free` and validating pointer uniqueness to prevent list cycles. Furthermore, setting table pointers (`enc_ll`, `dec_ll`, `enc_ml`, `dec_ml`, `enc_of`, `dec_of`, `enc_of3`, `dec_of3`, `enc_ll3`, `dec_ll3`, `llt`, `oft`, `mlt`) to NULL immediately upon freeing in `src/encode.c` and `src/dict.c` ensures re-entrant error path safety.

Action: Always bound thread-local storage pools in both slot count and byte capacity, check candidate pointers for duplicates before insertion to prevent list cycles/double-frees, and explicitly clear freed table pointers to NULL.
