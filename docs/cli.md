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
zgec: level 3, 12582912 -> 4210812 bytes (2.9884x)  210.5 ms  57.0 MiB/s
```

## Levels (`-l 1..9`, default 3)

A level is a preset; any explicit flag overrides its column. Every preset
uses `--block-log2 21` (2 MiB blocks) and `lambda 0`.

| level | tier | contexts | sub-lit | conditioning | litref | dicts | filter | checksums |
|------:|------|----------|---------|--------------|--------|-------|--------|-----------|
| 1 | fast | - | - | - | - | - | - | - |
| 2 | fast | x | - | - | - | - | - | - |
| 3 | main | - | - | - | - | - | - | - |
| 4 | main | x | - | - | - | - | - | - |
| 5 | main | x | - | - | x | - | - | - |
| 6 | main | x | - | x | x | - | - | - |
| 7 | high | x | - | x | x | - | - | - |
| 8 | high | x | x | x | x | - | - | - |
| 9 | high | x | x | x | x | - | - | x |

```sh
zgec c -l 1 big.log big.zgec      # fastest
zgec c -l 9 src.tar src.tar.zgec  # best ratio in the preset ladder
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

Example: level 9 without checksums, or level 3 with contexts:

```sh
zgec c -l 9 --no-checksums input output
zgec c -l 3 --contexts input output
```

Note: `--litref` replaces `--sub-lit` (§6.3): a literal-reference predecessor
must stay literal-exportable, and sub-literal segments break that chain. The
CLI prints a warning and clears `--sub-lit` so the reported config is the one
that ran. `--conditioning` is **not** cleared: the encoder keeps OF
conditioning on the literal-reference path and drops only LL conditioning,
which is the part that would break exportability.

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
zgec t -l 9 --checksums input
```

Compresses and decompresses in memory, byte-compares, prints decode timing
and `round-trip ok`. `round-trip mismatch` means a bug, not a usage error.

## Errors

Common failures (all exit `1` with a `zgec: ...` message):

- `unknown option` / `unknown command` / `too many arguments` — check
  `zgec -h`.
- `c needs an output path` / `d needs an output path`.
- `cannot read <path>` / `cannot write <path>`.
- `level must be 1..9`, `thread count must be 0..1024`,
  `block-log2 must be 16..26`, `tier must be fast, main or high`,
  `lambda must be a number >= 0`, `at most 4 dictionaries`.
- `compress failed: ...` / `decompress failed: ...` — the suffix is the
  `zgec_strerror` reason (e.g. checksum or dictionary mismatch).
