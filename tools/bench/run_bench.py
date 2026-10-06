#!/usr/bin/env python3
"""Run the zGEC benchmark matrix and write a long-format CSV.

Measured tools: zGEC itself (every level, plus one all-features configuration),
zstd, the zstd seekable format and xz. Each is measured for encode and decode,
at one thread and at all threads, and every zGEC encode is round-tripped
against the input before its numbers are reported.

Corpora come from tools/bench/corpora.json. A corpus of kind "tree" is
re-tarred without compression into a single input file so the measured bytes
are identical on every machine; kind "files" contributes one input per file.
Downloads and materialised inputs live in the cache directory, so a rerun (and
a CI cache restore) does no network work.

CSV schema (fixed; tools/bench/report.py reads exactly this):

    corpus,tool,tool_version,config,threads,op,input_bytes,output_bytes,
    ratio,seconds,mbps,max_rss_kb,cpu_model,cpu_count,loadavg,runner,notes

Semantics, so the charts mean something:

  * `ratio` is the compression ratio of the configuration, data_size /
    frame_size, and is the SAME NUMBER on the encode and the decode row of a
    configuration. Higher is better.
  * `mbps` is always throughput in terms of the UNCOMPRESSED data:
    data_size / seconds for encode and for decode. Higher is better.
  * `input_bytes`/`output_bytes` are what the operation actually read and
    wrote, so the decode row's input is the frame and its output is the data.
  * `max_rss_kb` is the peak resident set of the child process, from
    /usr/bin/time -v; it is empty where that tool does not exist.
  * `notes` always carries the machine calibration (see calibrate()) and the
    repetition count, and starts with "error: " when a measurement failed.
    report.py keeps error rows out of the charts but shows them in the table.

Baselines do not change, so zstd/xz/seekable rows can be reused: pass
--baseline-cache DIR and any <corpus>.csv there is copied in instead of
re-measured. --no-baseline skips those tools entirely.
"""

import argparse
import csv
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
import zipfile

FIELDS = [
    "corpus", "tool", "tool_version", "config", "threads", "op",
    "input_bytes", "output_bytes", "ratio", "seconds", "mbps",
    "max_rss_kb", "cpu_model", "cpu_count", "loadavg", "runner", "notes",
]

MIB = 1024 * 1024
ZSTD_LEVELS = [1, 3, 4, 6, 9, 12, 19]
ZSTD_SEEKABLE_LEVELS = [1, 3, 6, 9]
XZ_LEVELS = [2]
HEAVY_MB = 256
HTTP_TIMEOUT = 120
HTTP_TRIES = 4


def log(msg):
    sys.stderr.write("[bench] %s\n" % msg)
    sys.stderr.flush()


# --------------------------------------------------------------------------
# environment
# --------------------------------------------------------------------------

def cpu_model():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine() or "unknown"


def load_avg():
    try:
        a, b, c = os.getloadavg()
        return "%.2f/%.2f/%.2f" % (a, b, c)
    except (OSError, AttributeError):
        return ""


def runner_name():
    if os.environ.get("GITHUB_ACTIONS") == "true":
        return "%s/%s" % (os.environ.get("RUNNER_NAME", "gh"),
                          os.environ.get("RUNNER_OS", "?"))
    return "local/%s" % platform.system()


def calibrate(rounds=3):
    """Best-of-N MiB/s of SHA-256 over a fixed buffer.

    A number from a shared runner is only comparable if the machine was idle
    while it ran, so every row records this. Well below the value another run
    on the same hardware produced means the run was contended.
    """
    buf = b"\x5a" * (16 * MIB)
    best = 0.0
    for _ in range(rounds):
        t0 = time.perf_counter()
        h = hashlib.sha256()
        for _ in range(4):
            h.update(buf)
        dt = time.perf_counter() - t0
        if dt > 0:
            best = max(best, (16 * 4) / dt)
    return best


# --------------------------------------------------------------------------
# process helpers
# --------------------------------------------------------------------------

def find_time_v():
    for cand in ("/usr/bin/time", "/bin/time"):
        if os.path.exists(cand):
            return cand
    return None


TIME_V = find_time_v()


def run(cmd, stdout=None, timeout=None):
    """Run cmd; return (seconds, peak_rss_kb, returncode, stderr_text)."""
    cmd = [str(c) for c in cmd]
    wrapped = ([TIME_V, "-v"] + cmd) if TIME_V else cmd
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            wrapped,
            stdout=stdout if stdout is not None else subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return (time.perf_counter() - t0, None, -9, "timeout")
    except OSError as exc:
        return (0.0, None, -1, "exec failed: %s" % exc)
    dt = time.perf_counter() - t0
    text = proc.stderr.decode("utf-8", "replace") if proc.stderr else ""
    rss = None
    if TIME_V:
        for line in text.splitlines():
            if "Maximum resident set size" in line:
                try:
                    rss = int(line.split(":", 1)[1].strip())
                except ValueError:
                    rss = None
    return (dt, rss, proc.returncode, text)


def tool_version(exe, args=("--version",)):
    if not exe:
        return ""
    try:
        proc = subprocess.run([str(exe)] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=30)
        lines = proc.stdout.decode("utf-8", "replace").strip().splitlines()
        return lines[0].strip() if lines else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def exe_path(path):
    """The path to an executable, allowing for the suffix Windows appends.

    subprocess already finds build/zgec.exe for the name build/zgec, so the
    existence checks have to agree with it or a perfectly usable binary is
    reported as missing.
    """
    if not path or os.path.exists(path):
        return path
    if os.name == "nt" and os.path.exists(path + ".exe"):
        return path + ".exe"
    return path


def file_equal(a, b):
    if a.stat().st_size != b.stat().st_size:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        while True:
            ca = fa.read(4 * MIB)
            cb = fb.read(4 * MIB)
            if ca != cb:
                return False
            if not ca:
                return True


# --------------------------------------------------------------------------
# corpora
# --------------------------------------------------------------------------

def ensure_dirs(cache):
    for name in ("downloads", "extracted", "inputs", "tmp"):
        (cache / name).mkdir(parents=True, exist_ok=True)


def download(url, dest):
    if dest.exists() and dest.stat().st_size > 0:
        return
    tmp = dest.with_name(dest.name + ".part")
    last = None
    for attempt in range(1, HTTP_TRIES + 1):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "zgec-bench"})
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp, \
                    open(tmp, "wb") as out:
                shutil.copyfileobj(resp, out, 1024 * 1024)
            if tmp.stat().st_size == 0:
                raise IOError("empty download")
            tmp.replace(dest)
            log("downloaded %s (%.0f MiB)" % (dest.name, dest.stat().st_size / MIB))
            return
        except (urllib.error.URLError, IOError, OSError) as exc:
            last = exc
            log("download %s attempt %d/%d failed: %s"
                % (url, attempt, HTTP_TRIES, exc))
            time.sleep(2 * attempt)
    raise SystemExit("cannot download %s: %s" % (url, last))


def check_members(names):
    for name in names:
        norm = os.path.normpath(name)
        if norm.startswith("..") or os.path.isabs(norm) or ":" in norm[:2] or \
                norm.startswith("/"):
            raise SystemExit("refusing unsafe archive member %r" % name)


def _tar_extract_all(tf, dest, members=None):
    """Extract members, pinning tarfile's extraction filter.

    From 3.12 tarfile warns when no filter is passed, and 3.14 changes the
    default, which would quietly alter what an archive unpacks into. The names
    are vetted by check_members() first, so the permissive filter is the one
    intended here; interpreters older than 3.12 take no filter at all.
    """
    try:
        tf.extractall(dest, members=members, filter="fully_trusted")
    except TypeError:
        tf.extractall(dest, members=members)


def extract(archive, dest, only=None):
    """Unpack archive into dest, or only the subtree named by `only`.

    `only` is a path inside the archive. A manifest entry that names a nested
    payload uses it, so a corpus that is a subtree of a large tree unpacks on
    its own instead of writing that whole tree out a second time.

    The member list is materialised before anything is written: extracting
    while iterating the archive makes tarfile load its index, which drains the
    stream and silently ends the loop after the first member.
    """
    dest.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zf:
            names = zf.namelist()
            check_members(names)
            if only:
                names = [n for n in names
                         if n == only or n.startswith(only + "/")]
            zf.extractall(dest, members=names)
        return
    with tarfile.open(archive, "r:*") as tf:
        members = None
        if only:
            members = [m for m in tf.getmembers()
                       if m.name == only or m.name.startswith(only + "/")]
            check_members([m.name for m in members])
        else:
            check_members(tf.getnames())
        _tar_extract_all(tf, dest, members)


def deterministic_tar(payload, out):
    """Re-tar a tree without compression, byte-identically on every run."""
    tmp = out.with_name(out.name + ".part")
    entries = []
    for root, dirs, files in os.walk(payload):
        dirs.sort()
        for name in sorted(files):
            full = os.path.join(root, name)
            if os.path.islink(full) or not os.path.isfile(full):
                continue
            rel = os.path.relpath(full, payload).replace(os.sep, "/")
            entries.append((rel, full))
    entries.sort(key=lambda e: e[0])
    with tarfile.open(tmp, "w") as tf:
        for rel, full in entries:
            info = tf.gettarinfo(full, arcname=rel)
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            with open(full, "rb") as fh:
                tf.addfile(info, fh)
    tmp.replace(out)
    return out


def head_copy(src, dest, limit):
    with open(src, "rb") as fin, open(dest, "wb") as fout:
        left = limit
        while left > 0:
            chunk = fin.read(min(left, 4 * MIB))
            if not chunk:
                break
            fout.write(chunk)
            left -= len(chunk)
    return dest


def prepare(entry, cache, max_input_mb):
    """Materialise a manifest entry into [(corpus_id, input_path)] units."""
    cid = entry["id"]
    url = entry["url"]
    archive = cache / "downloads" / url.rsplit("/", 1)[-1]
    download(url, archive)
    root = cache / "extracted" / cid
    rel = entry.get("payload", ".")
    # A payload below the archive root ("a/b") is unpacked on its own, so two
    # corpora can share one download without writing the tree out twice.
    only = rel.strip("/") if "/" in rel.strip("/") else None
    if not root.exists():
        log("extracting %s%s"
            % (archive.name, " (%s only)" % only if only else ""))
        extract(archive, root, only)
    payload = root / rel

    units = []
    if entry.get("kind") == "files":
        names = entry.get("files")
        if not names:
            names = sorted(p.name for p in payload.iterdir() if p.is_file())
        for name in names:
            path = payload / name
            if path.is_file():
                units.append(("%s/%s" % (cid, name), path))
            else:
                log("warning: %s missing from %s" % (name, archive.name))
    else:
        if not payload.exists():
            raise SystemExit("payload %s missing from %s" % (payload, archive))
        out = cache / "inputs" / ("%s.tar" % cid)
        if not out.exists():
            log("building %s" % out.name)
            deterministic_tar(payload, out)
        units.append((cid, out))

    if max_input_mb:
        limit = max_input_mb * MIB
        trimmed = []
        for name, path in units:
            if path.stat().st_size <= limit:
                trimmed.append((name, path))
            else:
                cut = cache / "inputs" / ("%s.head-%dMiB" % (path.name, max_input_mb))
                if not cut.exists():
                    head_copy(path, cut, limit)
                trimmed.append((name, cut))
        units = trimmed
    return units


# --------------------------------------------------------------------------
# measurements
# --------------------------------------------------------------------------

class Bench(object):
    def __init__(self, args, env):
        self.args = args
        self.env = env
        self.rows = []
        self.tmp = args.cache / "tmp"
        self.tmp.mkdir(parents=True, exist_ok=True)
        self.cal = calibrate()
        self.cpu_count = env["cpu_count"]

    def note(self, extra=""):
        txt = "cal=%.0fMiB/s" % self.cal
        if extra:
            txt += " " + extra
        return txt

    def row(self, corpus, tool, ver, config, threads, op,
            in_bytes, out_bytes, data_bytes, frame_bytes, seconds, rss,
            extra="", error=None):
        ratio = (float(data_bytes) / float(frame_bytes)) if frame_bytes else 0.0
        mbps = (float(data_bytes) / MIB) / seconds if seconds and seconds > 0 else 0.0
        notes = self.note(extra)
        if error:
            notes = "error: %s | %s" % (error, notes)
        self.rows.append({
            "corpus": corpus, "tool": tool, "tool_version": ver,
            "config": config, "threads": threads, "op": op,
            "input_bytes": in_bytes, "output_bytes": out_bytes,
            "ratio": "%.4f" % ratio, "seconds": "%.4f" % seconds,
            "mbps": "%.2f" % mbps,
            "max_rss_kb": "" if rss is None else rss,
            "cpu_model": self.env["cpu_model"], "cpu_count": self.cpu_count,
            "loadavg": load_avg(), "runner": self.env["runner"],
            "notes": notes,
        })

    def _best_of(self, cmd, reps, stdout_factory=None):
        """Run cmd reps times; return (best_seconds, rss) or (None, error)."""
        best = None
        for i in range(reps):
            handle = stdout_factory() if stdout_factory else None
            try:
                sec, rss, rc, text = run(cmd, stdout=handle)
            finally:
                if handle is not None:
                    handle.close()
            if rc != 0:
                return (None, "rc=%d %s" % (rc, text.strip()[-200:]))
            if best is None or sec < best[0]:
                best = (sec, rss)
        return (best, None)

    # ---- zGEC -----------------------------------------------------------

    def zgec(self, corpus, src, exe, ver):
        data = src.stat().st_size
        frame = self.tmp / "frame.zgec"
        back = self.tmp / "back.bin"
        configs = ["-l %d" % lvl for lvl in range(1, 10)]
        # The presets leave the sampled pre-filter and the epoch dictionaries
        # out on purpose, so exercise them together once on the top level.
        configs.append("-l 9 --dicts --filter")
        for cfg in configs:
            extra = cfg.split()
            for threads in (1, self.cpu_count):
                base = ["--quiet", "-T", str(threads)]
                enc = [exe, "c"] + base + extra + [str(src), str(frame)]
                dec = [exe, "d"] + base + [str(frame), str(back)]
                (best, err) = self._best_of(enc, self.args.reps)
                if best is None:
                    self.row(corpus, "zgec", ver, cfg, threads, "encode",
                             data, 0, data, 0, 0.0, None, error=err)
                    continue
                fsize = frame.stat().st_size
                self.row(corpus, "zgec", ver, cfg, threads, "encode",
                         data, fsize, data, fsize, best[0], best[1])

                # The same best-of-N path as encode, so the first decode is not
                # a cold special case that always counts.
                (best_dec, derr) = self._best_of(dec, self.args.reps)
                if best_dec is None:
                    self.row(corpus, "zgec", ver, cfg, threads, "decode",
                             fsize, 0, data, fsize, 0.0, None,
                             error="roundtrip %s" % derr)
                    continue
                rt = "roundtrip-ok"
                if not back.exists() or not file_equal(back, src):
                    rt = "roundtrip-MISMATCH"
                self.row(corpus, "zgec", ver, cfg, threads, "decode",
                         fsize, data, data, fsize, best_dec[0], best_dec[1],
                         extra=rt)

    # ---- tools with -o, and tools that write to stdout -------------------

    def _generic(self, corpus, src, spec, to_stdout):
        exe = spec["exe"]
        data = src.stat().st_size
        ext = ".out"
        out = self.tmp / ("%s%s" % (spec["tool"], ext))
        back = self.tmp / ("%s.back" % spec["tool"])
        if to_stdout:
            enc = [exe] + spec["args"] + [str(src)]
            factory = lambda: open(out, "wb")
        else:
            enc = [exe] + spec["args"] + [str(src), "-o", str(out)]
            factory = None
        (best, err) = self._best_of(enc, self.args.reps, factory)
        if best is None or not out.exists() or out.stat().st_size == 0:
            self.row(corpus, spec["tool"], spec["ver"], spec["config"],
                     spec["threads"], "encode", data, 0, data, 0, 0.0, None,
                     error=err or "no output")
            return
        fsize = out.stat().st_size
        self.row(corpus, spec["tool"], spec["ver"], spec["config"],
                 spec["threads"], "encode", data, fsize, data, fsize,
                 best[0], best[1])
        if spec.get("no_decode"):
            return
        if to_stdout:
            dcmd = [exe] + spec["dec_args"] + [str(out)]
            dfactory = lambda: open(back, "wb")
        else:
            dcmd = [exe] + spec["dec_args"] + [str(out), "-o", str(back)]
            dfactory = None
        # Decode through the same best-of-N path as encode. Each round opens its
        # stdout target and closes it again, so the round-trip check below reads
        # a file nothing still holds open.
        (best_dec, derr) = self._best_of(dcmd, self.args.reps, dfactory)
        if best_dec is None:
            self.row(corpus, spec["tool"], spec["ver"], spec["config"],
                     spec["threads"], "decode", fsize, 0, data, fsize, 0.0,
                     None, error="decode %s" % derr)
            return
        rt = "roundtrip-ok"
        if not back.exists() or not file_equal(back, src):
            rt = "roundtrip-MISMATCH"
        self.row(corpus, spec["tool"], spec["ver"], spec["config"],
                 spec["threads"], "decode", fsize, data, data, fsize,
                 best_dec[0], best_dec[1], extra=rt)

    def generic(self, corpus, src, spec):
        self._generic(corpus, src, spec, to_stdout=False)

    def generic_stdout(self, corpus, src, spec):
        self._generic(corpus, src, spec, to_stdout=True)


# --------------------------------------------------------------------------
# the zstd seekable format
# --------------------------------------------------------------------------

# The seekable format is a contrib example, not a zstd CLI mode, and in the
# pinned release it is three separate programs with fixed positional
# interfaces -- none of which accepts an output path:
#
#   seekable_compression    FILE FRAME_SIZE [LEVEL]    -> writes FILE.zst, serial
#   parallel_compression    FILE FRAME_SIZE NB_THREADS -> writes FILE.zst, level 5
#   seekable_decompression  FILE START END             -> writes the range, stdout
#
# The interfaces are read from the pinned sources rather than probed, because
# the frame size is a required positional argument and the output name cannot
# be chosen at all. The input is therefore staged under a private name in the
# scratch directory, and the FILE.zst that appears beside it is what gets
# measured. Encode and decode are two different binaries in this release, which
# is what --seekable-dec is for.
SEEKABLE_FRAME_SIZE = 1 * MIB
SEEKABLE_MT_LEVEL = 5  # parallel_compression hard-codes the level


def default_seekable_dec(exe):
    """The decompressor that ships beside the seekable compressor."""
    if not exe:
        return ""
    cand = os.path.join(os.path.dirname(exe),
                        os.path.basename(exe).replace("compression",
                                                      "decompression"))
    if cand != exe and os.path.exists(cand):
        return cand
    return ""


def stage_seekable_input(src, tmp):
    """Put the input where the seekable example may write beside it.

    The examples derive FILE.zst from the path they are handed, so the input
    cannot point into the cached corpora. A hard link costs no copying; a copy
    is the fallback where links are unavailable.
    """
    staged = tmp / "seek.input"
    for path in (staged, tmp / "seek.input.zst", tmp / "seek.output"):
        if path.exists():
            path.unlink()
    try:
        os.link(src, staged)
    except OSError:
        shutil.copyfile(src, staged)
    return staged


def measure_seekable(bench, corpus, src, exe, dec_exe, mt_exe, ver, levels):
    """Measure the seekable format: the serial levels, plus the parallel example."""
    data = src.stat().st_size
    if not exe or not os.path.exists(exe):
        bench.row(corpus, "zstd-seekable", "", "seekable", 1, "encode",
                  data, 0, data, 0, 0.0, None,
                  error="zstd-seekable compressor not supplied")
        return

    staged = stage_seekable_input(src, bench.tmp)
    frame = bench.tmp / "seek.input.zst"  # the name the examples derive
    back = bench.tmp / "seek.output"
    can_decode = bool(dec_exe and os.path.exists(dec_exe))
    if not can_decode:
        log("seekable: no decompressor available, decode rows will say so")

    configs = [(exe, [str(SEEKABLE_FRAME_SIZE), str(level)], 1, "-%d" % level)
               for level in levels]
    if mt_exe and os.path.exists(mt_exe):
        # parallel_compression takes the thread count where the serial example
        # takes the level, and fixes the level at 5 in its own source.
        configs.append((mt_exe,
                        [str(SEEKABLE_FRAME_SIZE), str(bench.cpu_count)],
                        bench.cpu_count,
                        "parallel -%d" % SEEKABLE_MT_LEVEL))

    try:
        for program, extra, threads, label in configs:
            (best, err) = bench._best_of([program, str(staged)] + extra,
                                         bench.args.reps)
            if best is None or not frame.exists() or frame.stat().st_size == 0:
                bench.row(corpus, "zstd-seekable", ver, label, threads,
                          "encode", data, 0, data, 0, 0.0, None,
                          error=err or "no output")
                continue
            fsize = frame.stat().st_size
            bench.row(corpus, "zstd-seekable", ver, label, threads, "encode",
                      data, fsize, data, fsize, best[0], best[1],
                      extra="frame=%dKiB" % (SEEKABLE_FRAME_SIZE // 1024))
            if not can_decode:
                bench.row(corpus, "zstd-seekable", ver, label, threads,
                          "decode", fsize, 0, data, fsize, 0.0, None,
                          error="zstd-seekable decompressor not supplied")
                continue
            # The decoder is a range reader that writes to stdout. The whole
            # file is the range 0..data, so the capture is the round trip.
            (best_dec, derr) = bench._best_of(
                [dec_exe, str(frame), "0", str(data)], bench.args.reps,
                lambda: open(back, "wb"))
            if best_dec is None:
                bench.row(corpus, "zstd-seekable", ver, label, threads,
                          "decode", fsize, 0, data, fsize, 0.0, None,
                          error="decode %s" % derr)
                continue
            rt = "roundtrip-ok"
            if not back.exists() or not file_equal(back, src):
                rt = "roundtrip-MISMATCH"
            bench.row(corpus, "zstd-seekable", ver, label, threads, "decode",
                      fsize, data, data, fsize, best_dec[0], best_dec[1],
                      extra=rt)
    finally:
        for path in (staged, frame, back):
            if path.exists():
                path.unlink()


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def cached_rows(cache_dir, corpus):
    if not cache_dir:
        return []
    path = pathlib.Path(cache_dir) / ("%s.csv" % corpus.replace("/", "_"))
    if not path.exists():
        return []
    with open(path, "r", newline="", encoding="utf-8") as fh:
        rows = list(csv.DictReader(fh))
    for row in rows:
        row["corpus"] = corpus
    return rows


# A spec's `config` names the options only, and `threads` is how many threads the
# run actually used. report.py composes a point label from the tool, the config
# and that count, so repeating the tool name or the thread count in the config
# would print twice. -T0 means "one worker per core", which is why the column
# records the resolved count rather than the flag value.
def zstd_spec(exe, ver, level, threads_arg, threads_used):
    return {
        "tool": "zstd", "ver": ver, "exe": exe,
        "args": ["-q", "-f", "-%d" % level, "-T%d" % threads_arg],
        "dec_args": ["-q", "-f", "-d"],
        "threads": threads_used, "config": "-%d" % level,
    }


def xz_spec(exe, ver, level, threads_arg, cpu_count):
    return {
        "tool": "xz", "ver": ver, "exe": exe,
        "args": ["-q", "-f", "-c", "-%d" % level, "-T%d" % threads_arg],
        "dec_args": ["-q", "-f", "-d", "-c"],
        "threads": 1 if threads_arg == 1 else cpu_count,
        "config": "-%d" % level,
    }


def parse_args(argv):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpora", default="tools/bench/corpora.json")
    ap.add_argument("--cache", default=".bench-cache")
    ap.add_argument("--out", default="results.csv")
    ap.add_argument("--only", default="", help="comma separated corpus ids")
    ap.add_argument("--tool", default="all",
                    choices=["all", "zgec", "baseline",
                             "zstd", "seekable", "xz"],
                    help="baseline = zstd + zstd-seekable + xz, no zgec")
    ap.add_argument("--zgec", default="build/zgec")
    ap.add_argument("--zstd", default="zstd")
    ap.add_argument("--seekable", default="",
                    help="zstd seekable compressor (seekable_compression)")
    ap.add_argument("--seekable-dec", default="",
                    help="zstd seekable range decoder; defaults to the "
                         "sibling of --seekable")
    ap.add_argument("--seekable-mt", default="",
                    help="parallel seekable compressor (parallel_compression)")
    ap.add_argument("--seekable-version", default="",
                    help="version recorded for the seekable rows; the examples "
                         "have no --version of their own")
    ap.add_argument("--xz", default="xz")
    ap.add_argument("--reps", type=int, default=2,
                    help="repetitions per measurement; the best is reported")
    ap.add_argument("--max-input-mb", type=int, default=0,
                    help="truncate every input (breaks comparability; 0 = full)")
    ap.add_argument("--heavy-mb", type=int, default=HEAVY_MB,
                    help="inputs above this size skip the slowest levels")
    ap.add_argument("--all-levels", action="store_true")
    ap.add_argument("--baseline-cache", default="",
                    help="directory of cached <corpus>.csv baseline rows")
    ap.add_argument("--no-baseline", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--list-json", action="store_true",
                    help="print the corpus ids as a JSON array, for a CI matrix")
    return ap.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    args.cache = pathlib.Path(args.cache).resolve()
    args.cache.mkdir(parents=True, exist_ok=True)
    ensure_dirs(args.cache)

    with open(args.corpora, "r", encoding="utf-8") as fh:
        manifest = json.load(fh)
    entries = manifest["corpora"]
    if args.only:
        want = {c.strip() for c in args.only.split(",") if c.strip()}
        entries = [e for e in entries if e["id"] in want]
    if args.list:
        for e in entries:
            print("%-10s %s" % (e["id"], e["title"]))
        return 0
    if args.list_json:
        print(json.dumps([e["id"] for e in entries]))
        return 0

    env = {
        "cpu_model": cpu_model(),
        "cpu_count": os.cpu_count() or 1,
        "runner": runner_name(),
    }
    log("cpu: %s x%d on %s" % (env["cpu_model"], env["cpu_count"], env["runner"]))

    args.zgec = exe_path(args.zgec)
    args.seekable = exe_path(args.seekable)
    args.seekable_dec = (exe_path(args.seekable_dec) if args.seekable_dec
                         else default_seekable_dec(args.seekable))
    args.seekable_mt = exe_path(args.seekable_mt)
    zgec_ver = tool_version(args.zgec, ("-V",)) or tool_version(args.zgec)
    zstd_ver = tool_version(args.zstd)
    xz_ver = tool_version(args.xz)
    # The seekable examples print no version of their own -- asking for one
    # yields their "wrong arguments" usage text -- so the version is whatever
    # the caller knows the examples were built from, or blank.
    seek_ver = args.seekable_version
    log("versions: zgec=%r zstd=%r xz=%r seekable=%r"
        % (zgec_ver, zstd_ver, xz_ver, seek_ver))

    want = {
        "zgec": args.tool in ("all", "zgec"),
        "zstd": args.tool in ("all", "baseline", "zstd"),
        "seekable": args.tool in ("all", "baseline", "seekable"),
        "xz": args.tool in ("all", "baseline", "xz"),
    }
    have = {
        "zstd": bool(args.zstd and shutil.which(args.zstd)),
        "xz": bool(args.xz and shutil.which(args.xz)),
        "seekable": bool(args.seekable and os.path.exists(args.seekable)),
    }
    if want["zgec"] and not os.path.exists(args.zgec):
        raise SystemExit("zgec binary not found: %s" % args.zgec)

    bench = Bench(args, env)
    log("calibration: %.0f MiB/s SHA-256" % bench.cal)

    for entry in entries:
        for corpus, src in prepare(entry, args.cache, args.max_input_mb):
            size_mb = src.stat().st_size / MIB
            log("=== %s: %.0f MiB ===" % (corpus, size_mb))
            if want["zgec"]:
                bench.zgec(corpus, src, args.zgec, zgec_ver)

            wants_baseline = want["zstd"] or want["seekable"] or want["xz"]
            if wants_baseline and not args.no_baseline:
                rows = cached_rows(args.baseline_cache, corpus)
                if rows:
                    log("%s: reusing %d cached baseline rows" % (corpus, len(rows)))
                    bench.rows.extend(rows)
                    continue
            heavy = size_mb > args.heavy_mb and not args.all_levels
            if want["zstd"] and have["zstd"]:
                for level in ZSTD_LEVELS:
                    if heavy and level >= 19:
                        log("zstd -%d skipped: input above %d MiB"
                            % (level, args.heavy_mb))
                        continue
                    bench.generic(corpus, src, zstd_spec(
                        args.zstd, zstd_ver, level, 1, 1))
                    bench.generic(corpus, src, zstd_spec(
                        args.zstd, zstd_ver, level, 0, env["cpu_count"]))
            elif want["zstd"]:
                log("zstd not available, skipped")
            if want["seekable"]:
                levels = [l for l in ZSTD_SEEKABLE_LEVELS
                          if not (heavy and l >= 9)]
                measure_seekable(bench, corpus, src, args.seekable,
                                 args.seekable_dec, args.seekable_mt,
                                 seek_ver, levels)
            if want["xz"] and have["xz"]:
                for level in XZ_LEVELS:
                    for threads in (1, 0):
                        bench.generic_stdout(corpus, src, xz_spec(
                            args.xz, xz_ver, level, threads, env["cpu_count"]))
            elif want["xz"]:
                log("xz not available, skipped")

    with open(args.out, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=FIELDS)
        writer.writeheader()
        for row in bench.rows:
            writer.writerow(row)
    errors = sum(1 for r in bench.rows if str(r["notes"]).startswith("error"))
    log("wrote %s: %d rows, %d error rows" % (args.out, len(bench.rows), errors))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
