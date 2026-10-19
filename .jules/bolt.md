## 2026-10-19 - Candidate Pre-filtering in Fast Parse & Always-Inline Match Finder Helpers

**Learning:**
1. In `fast_parse` (`src/parse.c`), pre-filtering candidate repeat matches (`r0`, `r1`, `r2`) against `check_idx_r = (best_len >= 4u) ? (size_t)best_len : 3u` byte equality (`vb[ip - r + check_idx_r] == vb[ip + check_idx_r]`) before issuing 64-bit loads or diff mask arithmetic prunes invalid candidates branchlessly even when `best_len < 4u`.
2. Similarly, pre-filtering long-table match candidates (`cl`) against `check_idx_l = (best_len >= ZGEC_FAST_MIN_LONG) ? (size_t)best_len : (size_t)ZGEC_FAST_MIN_LONG - 1u` (where `ZGEC_FAST_MIN_LONG = 8u`) before reading 64-bit words skips 64-bit diff calculations for candidate positions that cannot beat `best_len` or reach the long-table minimum match length.
3. Adding `__attribute__((always_inline))` to hot match finder functions in `src/match.c` (`mf_hash4_v`, `mf_hash5_v`, `mf_hash8_v`, `mf_hash4`, `mf_hash5`, `mf_hash8`, `mf_hash_short`, `mf_insert_short`, `mf_insert_long`, `mf_insert_pos`, `mf_update_best`, `mf_bucket_hits`) guarantees inlining across translation units and reduces register spilling in hot match finding loops.

**Action:**
Pre-filter repeat-offset and long-table match candidates at `max(best_len, min_match - 1)` index in fast match parsers, and mark hot hashing and insertion helpers with `always_inline`.
