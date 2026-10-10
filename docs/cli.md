# zGEC CLI guide

Binary: `zgec` (build with `cmake -B build && cmake --build build`).
All timings and size summaries go to stderr; `t` prints `round-trip ok`
to stdout. Exit code `0` on success, `1` on any error.

## Commands

```sh
zgec c input output   # compress input into output
zgec d input output   # decompress input into output
zgec t input          # compress + decompress in memory and verify (no output file)
zgec -h               # full option list
zgec -V               # version string
```

Command names are exact (`c`, `d`, `t`, case-insensitive). `c`/`d` need
exactly one output path; `t` takes none (`zgec: t takes no output path`).

## Quick start

```sh
zgec c book.txt book.zgec
zgec d book.zgec book.out
zgec t book.txt
```

Typical output (stderr, `--quiet` suppresses it):

```text
zgec: input 12.0 MiB, one worker per core
zgec: level 6, 12582912 -> 4210812 bytes (2.9884x)  210.5 ms  57.0 MiB/s
```

## Levels (`-l 1..25`, default 6)

A level is a preset; any explicit flag overrides its column. Every preset
uses `--block-log2 21` (2 MiB blocks) and `lambda 0`.

Slots 1-3 and 11 are reserved and rejected with `level N is not implemented`; the implemented ladder is 4-10 and 12-25.
Levels 13-14 add epoch dictionaries and pre-filtering; levels 15-25 progressively increase block window sizes up to 16 MiB (`block-log2` 24) for maximum compression ratio.

| level | tier | contexts | sub-lit | conditioning | litref | dicts | filter | checksums | block_log2 |
|------:|------|----------|---------|--------------|--------|-------|--------|-----------|------------|
| 4 | fast | - | - | - | - | - | - | - | 21 (2 MiB) |
| 5 | fast | x | - | - | - | - | - | - | 21 (2 MiB) |
| 6 | main | - | - | - | - | - | - | - | 21 (2 MiB) |
| 7 | main | x | - | - | - | - | - | - | 21 (2 MiB) |
| 8 | main | x | - | - | x | - | - | - | 21 (2 MiB) |
| 9 | main | x | - | x | x | - | - | - | 21 (2 MiB) |
| 10 | high | x | x | x | x | - | - | - | 21 (2 MiB) |
| 12 | high | x | x | x | x | - | - | x | 21 (2 MiB) |
| 13 | high | x | x | x | x | x | - | x | 21 (2 MiB) |
| 14 | high | x | x | x | x | x | x | x | 21 (2 MiB) |
| 15..16 | high | x | x | x | x | x | x | x | 22 (4 MiB) |
| 17..18 | high | x | x | x | x | x | x | x | 23 (8 MiB) |
| 19..25 | high | x | x | x | x | x | x | x | 24 (16 MiB) |

```sh
zgec c -l 4 big.log big.zgec      # fastest implemented
zgec c -l 25 src.tar src.tar.zgec # maximum ratio level
```

## Threads (`-T N`, default 0)

`0` means one worker per core. Workers are clamped to one per block, so a
small input reports e.g. `threads 8 clamped to 2 (one per block)`.
Output is byte-identical regardless of thread count.

```sh
zgec c -T 0 input output
zgec c -T 1 input output   # serial, for benchmarking
```

## Feature flags (override the level)

```sh
--contexts      --no-contexts
--sub-lit       --no-sub-lit
--conditioning  --no-conditioning
--litref        --no-litref
--dicts         --no-dicts
--filter        --no-filter
--checksums     --no-checksums
```

Example: level 12 without checksums, or level 7 with contexts:

```sh
zgec c -l 12 --no-checksums input output
zgec c -l 7 --contexts input output
```

Note: `--litref` replaces `--sub-lit` (§6.3): a literal-reference predecessor
must stay literal-exportable, and sub-literal segments break that chain. The
CLI prints a warning and clears `--sub-lit` so the reported config is the one
that ran. `--conditioning` is **not** cleared: the encoder keeps OF
conditioning on the literal-reference path and drops only LL conditioning,
which is the part that would break exportability.

## Checksum cost

`--checksums` (on by default only at level 12) adds a per-block CRC32C
pass: `src/encode.c:3891` checksums the source block on encode and
`src/decode.c:1917` re-checksums the decoded block on decode, both via
`zgec_crc32c` (`src/crc32c.c:155` `zgec_crc32c_hw`, which uses the x86
SSE4.2 `crc32` instruction when available and falls back to a sliced
software table otherwise). The pass costs ~0.7 line-hits/byte (~6% of
the level-9 decode budget, measured on the 211938580-byte Silesia
corpus). It applies only when block checksums are on (`block_checksums`
/ `--checksums`); with `--no-checksums` — or any level below 9 without
`--checksums` — there is no CRC pass. Interleaving the CRC with the
copy pass was considered and rejected: it risks cache pollution for
little gain, since the checksum path is already HW-accelerated
(`crc32` path ~155-176 vs sliced software ~108-142).

## Tuning dials

```sh
--tier fast|main|high        # match finder tier (default: level preset)
--block-log2 16..26         # block size (default 21 = 2 MiB)
--lambda X                  # speed/ratio dial >= 0 (default: level preset)
```

```sh
zgec c --block-log2 22 --tier high input output
```

## External dictionaries (`-D FILE`, repeatable, max 4)

Files must each fit in 64 MiB. Ids follow argument order, so decode with
the same files in the same order:

```sh
zgec c -D dict1 -D dict2 input output
zgec d -D dict1 -D dict2 output restored
```

A dictionary the encoder rejects (`dictionary ... rejected: ...`) aborts
the run. Decoding a frame that needs a dictionary without supplying it
fails instead of producing wrong bytes.

## Decode level, quiet

```sh
zgec d --core input.zgec output     # CORE level (default is EXTENDED)
zgec c --quiet input output        # no size summary
```

`--core` applies to `d` only.

## Verifying (`t`)

```sh
zgec t input
zgec t -l 12 --checksums input
```

Compresses and decompresses in memory, byte-compares, prints decode timing
and `round-trip ok`. `round-trip mismatch` means a bug, not a usage error.

## Errors

Common failures (all exit `1` with a `zgec: ...` message):

- `unknown option` / `unknown command` / `too many arguments` — check
  `zgec -h`.
- `c needs an output path` / `d needs an output path`.
- `cannot read <path>` / `cannot write <path>`.
- `level must be 1..25`, `thread count must be 0..1024`,
  `block-log2 must be 16..26`, `tier must be fast, main or high`,
  `lambda must be a number >= 0`, `at most 4 dictionaries`.
- `compress failed: ...` / `decompress failed: ...` — the suffix is the
  `zgec_strerror` reason (e.g. checksum or dictionary mismatch).
