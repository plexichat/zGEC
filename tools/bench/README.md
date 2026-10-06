# Benchmark suite

zGEC against zstd, the zstd seekable format and xz, on real corpora, with the
raw numbers as CSV and the comparison as an HTML report with Pareto frontiers.

Three pieces:

| file | what it does |
|---|---|
| `corpora.json` | where each corpus comes from and what to feed the compressors |
| `run_bench.py` | downloads, prepares, measures, writes a long-format CSV |
| `report.py` | CSV to `report.html`, `summary.md`, `results.json`, SVG/PNG charts |

`.github/workflows/bench.yml` runs the whole thing on a schedule and on demand,
and publishes a downloadable `benchmark-report` artifact.

## Running it locally

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel

python3 tools/bench/run_bench.py --list                    # the corpora
python3 tools/bench/run_bench.py --only redis --reps 3     # one corpus, quick
python3 tools/bench/run_bench.py --tool zgec --no-baseline # zGEC only

python3 tools/bench/report.py --csv results.csv --out report
```

Useful flags: `--max-input-mb N` truncates every input (fast smoke runs, but
the ratios stop being comparable to a full-corpus run), `--heavy-mb N` is the
size above which the slowest baseline levels are skipped, `--all-levels`
overrides that, `--baseline-cache DIR` reuses `<corpus>.csv` baseline rows
instead of re-measuring them, and `--reps N` sets how many times each
measurement runs (the best is reported).

## What is measured

* **zGEC** — levels 1..9 from the preset ladder, plus one configuration that
  turns on the features the presets leave out (`-l 9 --dicts --filter`). Each is
  measured for encode and decode at one thread and at all threads, and the
  encode is round-tripped once against the input before its numbers are
  reported; a mismatch is recorded in the row rather than thrown away.
* **zstd** — levels 1, 3, 4, 6, 9, 12 and 19, one thread and all threads.
  Level 19 is skipped above `--heavy-mb` unless `--all-levels` is given.
* **zstd-seekable** — the seekable format's example program, at levels 1, 3, 6
  and 9. Its option spelling has moved between zstd releases, so the driver
  probes candidate invocations once on a 1 MiB sample, verifies the one it
  picks actually produces output and round-trips it, and records the spelling
  in the row's notes. If none works the row says so instead of failing the run.
* **xz** — level 2, one thread and all threads.

## Corpora

Linux 7.1, LLVM 23.1.2, Clang 23.1.2, CPython 3.14.8, Redis 8.10.2 and the
Silesia corpus, each from its project's own release host, each pinned by
version. A `tree` corpus is re-tarred without compression into one file with
sorted names and zeroed ownership and timestamps, so the measured bytes are
identical on every machine and a cached baseline stays valid. Silesia
contributes one input per file, as `<corpus>/<file>`.

## The CSV

```
corpus,tool,tool_version,config,threads,op,input_bytes,output_bytes,ratio,seconds,mbps,max_rss_kb,cpu_model,cpu_count,loadavg,runner,notes
```

* `ratio` is data / frame and is the **same number on the encode and the decode
  row** of a configuration, so a chart can plot either direction against it.
  Higher is better.
* `mbps` is always in terms of the **uncompressed** bytes, for decode as well.
  Higher is better.
* `input_bytes`/`output_bytes` are what the operation actually read and wrote.
* `max_rss_kb` is the child's peak resident set, from `/usr/bin/time -v`; it is
  empty where that tool does not exist (the Windows developer machines), never
  guessed.
* `notes` carries the machine calibration and a `roundtrip-ok` / `-MISMATCH`
  marker, and starts with `error: ` when a measurement failed. `report.py`
  keeps error rows out of the charts and still lists them in the table.

## Pareto frontiers

For each corpus and direction, a point is on the frontier when no other point
has both a higher ratio and a higher throughput. The charts plot ratio against
log10 MB/s, draw the frontier as a polyline and label every point with its tool
and configuration, so the interesting question -- what does the last 1% of
ratio cost, and which tool gives the best trade at each point on the curve --
is readable directly.

## Reading the environment columns

Compression numbers are only comparable on an idle machine, so every row
records the CPU model, the logical CPU count, the load average at that moment,
the runner name and `cal=<N>MiB/s`: the best of three SHA-256 passes over a
64 MiB buffer, measured by the driver itself. `cal` is the contention proxy --
compare it across runs of the same workflow on the same hardware class. The
matrix runs with `max-parallel: 1`, so no two measurements deliberately share a
runner.

## Caching

* Baseline results (zstd, zstd-seekable, xz) are cached per corpus, keyed by the
  manifest, the driver and the measured tool versions, which is why the
  workflow builds those tools from pinned release tags instead of using the
  image's copies. `force_baseline` re-measures them anyway.
* Corpus downloads and the materialised inputs are cached too, so a rerun does
  no network work and re-uses byte-identical inputs.

## Limits worth knowing

* The throughput figures come from the CLI binaries, so they include process
  start and file I/O around the measured region for the baseline tools. zGEC's
  own numbers are the same way; `build/zgec_bench` measures zGEC in-process
  without it, and is the right tool for a zGEC-only A/B.
* A GitHub-hosted runner is a shared VM of a few cores. Absolute MB/s there is
  not the reference platform's; the comparisons and the calibration are what
  transfer.
