# zGEC

A lossless, random-access block compressor written in C.

zGEC parses input with LZ77-style match finding and encodes the result with
asymmetric numeral systems (ANS / rANS) entropy coding. Input is split into
independently decodable blocks, and each frame carries a footer index, so any
single block can be decompressed without reading the rest of the file.

## Features

- Random-access decompression — decode one block, not the whole frame
- ANS entropy coding (rANS + FSE), optional sub-literals and context modelling
- External dictionaries and literal references for cross-block history
- Optional per-frame integrity checksums (CRC-32C)
- Multi-threaded encoding; output is byte-identical regardless of thread count

## Build

```sh
cmake -B build
cmake --build build
```

## Usage

```sh
zgec c input output     # compress
zgec d input output     # decompress
zgec t input            # round-trip test
zgec -h                 # all options, levels and feature flags
```

Run `zgec -h` for compression levels (`-l 1..9`), thread count (`-T`) and the
individual feature toggles. See `docs/cli.md` for the full CLI guide with
examples (levels table, threads, dictionaries, verification).

## Documentation

The bitstream format is specified in `docs/spec.md`.
`docs/cli.md` is the command-line usage guide.
`docs/architecture.md` maps the shared vs encoder-only vs decoder-only
split (including the intentional `dict.c`/`decode.c` fork).
`docs/testing.md` describes the test suites and how to add tests.

## License

Apache License 2.0 — see [LICENSE](LICENSE).
