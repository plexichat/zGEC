# zGEC testing

All C tests use the same custom style (no Unity/CUnit): `static int
test_*` or `CHECK`, `printf` on failure, non-zero exit on any failure.
Run with `ctest` or the binaries directly.

## Suites

- `tests/test.c` (`ctest -R conformance`, `zgec_test`): 20-case full
  conformance harness — frame/FSE/rANS round-trips, classify, runstart,
  lane starts, class-map, dict cache, FSE-seq, filter transform + frame,
  coder paths, round-trip, external dict, literal refs, threads,
  block-parallel, litref flags/bounds, dict-no-orphan.
- `tests/test_mini.c` (`smoke`, `zgec_test_mini`): 4-case fast bring-up
  subset (frame/FSE/rANS/round-trip).
- `tests/test_block_check.c` (`block`, `zgec_test_block`): 6 header-level
  groups — block params, raw/FSE/x3/k segments, descriptor walk,
  bitstream. Thorough negatives at the header level, no full-frame
  round-trip.
- `tests/test_dedup.c` (`dedup`, `zgec_test_dedup`): regression tests
  for the deduplicated shared helpers — normalize (empty/even/overfull/
  bad AL), lane geom vs starts + partition, sentinel (ok/zero/empty/
  null), shuffle lengths + filter round-trip, runstart (bits/over-sum/
  empty), class-map round-trip + `>=k` reject, reps encode/resolve
  round-trip.
- `tools/fuzz/fuzz_roundtrip.c` (`zgec_fuzz`, optional
  `zgec_fuzz_libfuzzer`): deterministic seed corpus + libFuzzer shape.
  Round-trip all tiers/params, decoder hardening under random limits,
  checksum corrupt/truncate integrity. `abort()` on finding under
  libFuzzer.
- `tests/bench.c` (`zgec_bench`) + `tools/bench/*.py`: perf only
  (best-of-N, determinism check), not correctness.

## Adding a test

1. Extend the closest suite, or add `tests/test_<topic>.c` in the same
   style (`-Werror -Wstrict-prototypes` clean, no external framework).
2. Register it in `CMakeLists.txt` (`add_executable` +
   `target_link_libraries(... zgec_core)` + `add_test`).
3. Cite the spec section (`§5/7/8/9/10/11`) in a comment, as the
   existing suites do.

## Spec coverage (`docs/spec.md` §15.2)

Round-trip all record/flag/seq features, scalar-vs-SIMD differential,
random-access incl. filtered, filter +/-, V1–V10 negatives.
