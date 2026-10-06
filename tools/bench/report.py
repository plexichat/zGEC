#!/usr/bin/env python3
"""Turn the benchmark results CSV into charts, an HTML report and a summary.

The input is one long-format CSV, one row per measured operation:

    corpus,tool,tool_version,config,threads,op,input_bytes,output_bytes,
    ratio,seconds,mbps,max_rss_kb,cpu_model,cpu_count,loadavg,runner,notes

`ratio` and `mbps` are both "higher is better". Rows whose `notes` starts with
`error` are excluded from the charts and from the Pareto frontier but still
appear in the results table, so a failed configuration is visible rather than
silently missing.

Two quantities are derived here:

  * the Pareto frontier of a (corpus, op) group is the set of rows that no
    other row in the group dominates: a row q dominates p when q.ratio >=
    p.ratio and q.mbps >= p.mbps and at least one of the two is strictly
    greater. Rows that tie exactly all stay on the frontier.
  * the chart is a hand-written SVG scatter of ratio (x, linear) against
    throughput (y, log10), with the frontier drawn as a highlighted polyline
    through the frontier rows sorted by ratio.

Only the standard library is imported at module scope. matplotlib is imported
defensively inside a guard and merely adds PNG copies of the charts; nothing
in the report depends on it.

Usage:

    python3 tools/bench/report.py --csv results.csv --out out [--title "..."]
"""

from __future__ import annotations

import argparse
import collections
import csv
import datetime
import html
import json
import math
import os
import sys

try:  # optional, never required
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt  # noqa: F401

    HAVE_MPL = True
except Exception:  # pragma: no cover - depends on the machine
    HAVE_MPL = False

# Column order of the results CSV. Parsing is by name, but the required names
# are pinned here so a header typo fails loudly instead of producing an empty
# report.
FIELDS = [
    "corpus",
    "tool",
    "tool_version",
    "config",
    "threads",
    "op",
    "input_bytes",
    "output_bytes",
    "ratio",
    "seconds",
    "mbps",
    "max_rss_kb",
    "cpu_model",
    "cpu_count",
    "loadavg",
    "runner",
    "notes",
]

OPS = ("encode", "decode")

# Marker and colour per tool; anything unknown falls back to a grey circle.
TOOL_STYLE = {
    "zgec": ("circle", "#1f77b4"),
    "zstd": ("square", "#d62728"),
    "zstd-seekable": ("diamond", "#2ca02c"),
    "xz": ("triangle", "#9467bd"),
}
FALLBACK_STYLE = ("circle", "#7f7f7f")

FRONTIER_COLOR = "#ff7f0e"
GRID_COLOR = "#dddddd"
AXIS_COLOR = "#888888"

SVG_W = 900
SVG_H = 560
MARGIN_L = 92
MARGIN_R = 210
MARGIN_T = 52
MARGIN_B = 72


# --------------------------------------------------------------------------
# parsing
# --------------------------------------------------------------------------


class Row(object):
    """One CSV row, with the numeric columns parsed once."""

    __slots__ = (
        "corpus",
        "tool",
        "tool_version",
        "config",
        "threads",
        "op",
        "input_bytes",
        "output_bytes",
        "ratio",
        "seconds",
        "mbps",
        "max_rss_kb",
        "cpu_model",
        "cpu_count",
        "loadavg",
        "runner",
        "notes",
        "index",
    )

    def __init__(self, record, index):
        self.index = index
        self.corpus = (record.get("corpus") or "").strip()
        self.tool = (record.get("tool") or "").strip()
        self.tool_version = (record.get("tool_version") or "").strip()
        self.config = (record.get("config") or "").strip()
        self.threads = _to_int(record.get("threads"))
        self.op = (record.get("op") or "").strip()
        self.input_bytes = _to_int(record.get("input_bytes"))
        self.output_bytes = _to_int(record.get("output_bytes"))
        self.ratio = _to_float(record.get("ratio"))
        self.seconds = _to_float(record.get("seconds"))
        self.mbps = _to_float(record.get("mbps"))
        self.max_rss_kb = _to_float(record.get("max_rss_kb"))
        self.cpu_model = (record.get("cpu_model") or "").strip()
        self.cpu_count = _to_int(record.get("cpu_count"))
        self.loadavg = _to_float(record.get("loadavg"))
        self.runner = (record.get("runner") or "").strip()
        self.notes = (record.get("notes") or "").strip()

    @property
    def label(self):
        """Tool plus configuration, used as the point label."""
        parts = [self.tool]
        if self.config:
            parts.append(self.config)
        if self.threads is not None:
            parts.append("T=%d" % self.threads)
        return " ".join(parts)

    @property
    def failed(self):
        """A run recorded as an error: kept in the table, out of the charts."""
        return self.notes.lower().startswith("error")

    @property
    def plottable(self):
        return (
            self.op in OPS
            and not self.failed
            and self.ratio is not None
            and self.mbps is not None
            and self.mbps > 0.0
        )

    def as_dict(self):
        out = {}
        for name in FIELDS:
            value = getattr(self, name)
            out[name] = "" if value is None else value
        return out


def _to_float(text):
    """Parse a float, mapping empty or unparseable cells to None."""
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def _to_int(text):
    value = _to_float(text)
    if value is None:
        return None
    return int(value)


def load_rows(csv_path):
    """Read the results CSV; a missing file or a header-only file is empty."""
    if not csv_path or not os.path.exists(csv_path):
        return []
    rows = []
    with open(csv_path, "r", newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            return []
        missing = [name for name in FIELDS if name not in reader.fieldnames]
        if missing:
            sys.stderr.write(
                "report: %s is missing column(s): %s\n"
                % (csv_path, ", ".join(missing))
            )
        for index, record in enumerate(reader):
            if record is None:
                continue
            if not any(
                isinstance(value, str) and value.strip()
                for value in record.values()
            ):
                continue  # blank trailing line (a restkey list is not text)
            rows.append(Row(record, index))
    return rows


# --------------------------------------------------------------------------
# grouping and Pareto frontier
# --------------------------------------------------------------------------


def group_rows(rows):
    """Group plottable rows by (corpus, op), corpora and ops in stable order."""
    groups = collections.OrderedDict()
    corpora = sorted({row.corpus for row in rows if row.plottable})
    for corpus in corpora:
        for op in OPS:
            member = [
                row
                for row in rows
                if row.plottable and row.corpus == corpus and row.op == op
            ]
            if member:
                groups[(corpus, op)] = member
    return groups


def pareto_frontier(rows):
    """The non-dominated rows of one group: max ratio and max MB/s at once."""
    frontier = []
    for i, row in enumerate(rows):
        dominated = False
        for j, other in enumerate(rows):
            if i == j:
                continue
            if other.ratio >= row.ratio and other.mbps >= row.mbps and (
                other.ratio > row.ratio or other.mbps > row.mbps
            ):
                dominated = True
                break
        if not dominated:
            frontier.append(row)
    frontier.sort(key=lambda r: (r.ratio, r.mbps))
    return frontier


# --------------------------------------------------------------------------
# number formatting
# --------------------------------------------------------------------------


def fmt_ratio(value, digits=2):
    if value is None:
        return ""
    return "%.*fx" % (digits, value)


def fmt_mbps(value, digits=1):
    if value is None:
        return ""
    return "%.*f MB/s" % (digits, value)


def fmt_num(value, digits=2):
    """Compact tick label: drop the decimals once the number is large."""
    if value is None:
        return ""
    if value >= 100.0:
        return "%.0f" % value
    if value >= 10.0:
        return "%.1f" % value
    return "%.*f" % (digits, value)


def fmt_fixed(value, digits):
    if value is None:
        return ""
    return "%.*f" % (digits, value)


def fmt_int(value):
    if value is None:
        return ""
    return "%d" % value


# --------------------------------------------------------------------------
# chart geometry
# --------------------------------------------------------------------------


def _ticks_linear(lo, hi, count=5):
    """count-ish evenly spaced round ticks covering [lo, hi]."""
    if hi <= lo:
        return [lo]
    span = hi - lo
    step = span / float(max(count - 1, 1))
    magnitude = 10.0 ** math.floor(math.log10(step)) if step > 0 else 1.0
    for multiple in (1.0, 2.0, 2.5, 5.0, 10.0):
        if step <= magnitude * multiple:
            step = magnitude * multiple
            break
    start = math.floor(lo / step) * step
    ticks = []
    value = start
    while value <= hi + step * 0.5 and len(ticks) < 24:
        if value >= lo - step * 0.5:
            ticks.append(value)
        value += step
    if not ticks:
        ticks = [lo, hi]
    return ticks


def _ticks_log(lo, hi):
    """1-2-5 decade ticks inside [lo, hi] (both are log10 values)."""
    ticks = []
    exponent = int(math.floor(lo)) - 1
    top = int(math.ceil(hi)) + 1
    while exponent <= top:
        base = 10.0 ** exponent
        for multiple in (1.0, 2.0, 5.0):
            value = base * multiple
            logv = math.log10(value)
            if lo <= logv <= hi:
                ticks.append(value)
        exponent += 1
    return ticks


def _marker(shape, cx, cy, radius, color, title):
    """One scatter marker, wrapped so a hover shows the whole row."""
    cx = round(cx, 1)
    cy = round(cy, 1)
    if shape == "square":
        body = '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" />' % (
            cx - radius,
            cy - radius,
            radius * 2.0,
            radius * 2.0,
        )
    elif shape == "diamond":
        body = '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f %.1f,%.1f" />' % (
            cx,
            cy - radius,
            cx + radius,
            cy,
            cx,
            cy + radius,
            cx - radius,
            cy,
        )
    elif shape == "triangle":
        body = '<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f" />' % (
            cx,
            cy - radius,
            cx + radius,
            cy + radius,
            cx - radius,
            cy + radius,
        )
    else:
        body = '<circle cx="%.1f" cy="%.1f" r="%.1f" />' % (cx, cy, radius)
    return (
        '<g fill="%s" stroke="#333333" stroke-width="0.6">'
        "<title>%s</title>%s</g>" % (color, html.escape(title), body)
    )


def svg_chart(corpus, op, rows, frontier):
    """A standalone SVG scatter with the Pareto frontier highlighted."""
    width, height = SVG_W, SVG_H
    left, right = MARGIN_L, width - MARGIN_R
    top, bottom = MARGIN_T, height - MARGIN_B
    title_text = "%s - %s" % (corpus, op)
    subtitle = "%d run(s), %d on the Pareto frontier" % (len(rows), len(frontier))

    head = (
        '<svg xmlns="http://www.w3.org/2000/svg" class="chart" '
        'viewBox="0 0 %d %d" width="%d" height="%d" '
        'role="img" aria-label="%s">'
        % (
            width,
            height,
            width,
            height,
            html.escape("%s ratio against throughput" % title_text),
        )
    )
    parts = [head]

    if not rows:
        parts.append(
            '<rect x="1" y="1" width="%d" height="%d" fill="#ffffff" '
            'stroke="%s" />' % (width - 2, height - 2, GRID_COLOR)
        )
        parts.append(
            '<text x="%d" y="%d" font-family="sans-serif" font-size="16" '
            'text-anchor="middle" fill="#666666">%s</text>'
            % (width // 2, height // 2, html.escape("no data for " + title_text))
        )
        parts.append("</svg>\n")
        return "".join(parts)

    ratios = [row.ratio for row in rows]
    logs = [math.log10(row.mbps) for row in rows]
    x_lo, x_hi = min(ratios), max(ratios)
    y_lo, y_hi = min(logs), max(logs)
    if x_hi - x_lo < 1e-9:
        pad = max(abs(x_hi) * 0.05, 0.05)
    else:
        pad = (x_hi - x_lo) * 0.06
    x_lo -= pad
    x_hi += pad
    if y_hi - y_lo < 1e-9:
        y_lo -= 0.15
        y_hi += 0.15
    else:
        y_pad = (y_hi - y_lo) * 0.08
        y_lo -= y_pad
        y_hi += y_pad

    def sx(value):
        return left + (value - x_lo) / (x_hi - x_lo) * (right - left)

    def sy(value):
        return bottom - (value - y_lo) / (y_hi - y_lo) * (bottom - top)

    # background and grid
    parts.append(
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#ffffff" '
        'stroke="%s" />' % (left, top, right - left, bottom - top, GRID_COLOR)
    )
    for tick in _ticks_linear(x_lo, x_hi):
        x = sx(tick)
        if x < left - 0.5 or x > right + 0.5:
            continue
        parts.append(
            '<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" '
            'stroke-width="0.5" />' % (x, top, x, bottom, GRID_COLOR)
        )
        parts.append(
            '<text x="%.1f" y="%d" font-family="sans-serif" font-size="10" '
            'text-anchor="middle" fill="#444444">%s</text>'
            % (x, bottom + 16, html.escape("%.2f" % tick))
        )
    for tick in _ticks_log(y_lo, y_hi):
        y = sy(math.log10(tick))
        if y < top - 0.5 or y > bottom + 0.5:
            continue
        parts.append(
            '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" '
            'stroke-width="0.5" />' % (left, y, right, y, GRID_COLOR)
        )
        parts.append(
            '<text x="%d" y="%.1f" font-family="sans-serif" font-size="10" '
            'text-anchor="end" fill="#444444">%s</text>'
            % (left - 8, y + 3.5, html.escape(fmt_num(tick)))
        )

    # axes titles
    parts.append(
        '<text x="%d" y="%d" font-family="sans-serif" font-size="12" '
        'text-anchor="middle" fill="#333333">ratio (input / output, higher is '
        "better)</text>" % ((left + right) // 2, height - 26)
    )
    parts.append(
        '<text transform="rotate(-90 %.1f %.1f)" x="%.1f" y="%.1f" '
        'font-family="sans-serif" font-size="12" text-anchor="middle" '
        'fill="#333333">MB/s (log scale)</text>'
        % (
            float(left) - 58.0,
            (top + bottom) / 2.0,
            float(left) - 58.0,
            (top + bottom) / 2.0,
        )
    )
    parts.append(
        '<text x="%d" y="26" font-family="sans-serif" font-size="15" '
        'font-weight="bold" fill="#222222">%s</text>'
        % (left, html.escape(title_text))
    )
    parts.append(
        '<text x="%d" y="42" font-family="sans-serif" font-size="11" '
        'fill="#666666">%s</text>' % (left, html.escape(subtitle))
    )

    # frontier polyline first, so markers sit on top of it
    if len(frontier) >= 2:
        vertices = " ".join(
            "%.1f,%.1f" % (sx(row.ratio), sy(math.log10(row.mbps)))
            for row in frontier
        )
        parts.append(
            '<polyline points="%s" fill="none" stroke="%s" stroke-width="2" '
            'stroke-dasharray="6 3" />' % (vertices, FRONTIER_COLOR)
        )

    frontier_ids = set(id(row) for row in frontier)
    for row in rows:
        shape, color = TOOL_STYLE.get(row.tool, FALLBACK_STYLE)
        is_front = id(row) in frontier_ids
        radius = 6.0 if is_front else 4.0
        cx = sx(row.ratio)
        cy = sy(math.log10(row.mbps))
        title = "%s  ratio %s  %s  out %s bytes" % (
            row.label,
            fmt_ratio(row.ratio, 4),
            fmt_mbps(row.mbps),
            fmt_int(row.output_bytes),
        )
        parts.append(_marker(shape, cx, cy, radius, color, title))
        parts.append(
            '<text x="%.1f" y="%.1f" font-family="sans-serif" font-size="9" '
            'fill="#333333">%s</text>'
            % (cx + radius + 3.0, cy + 3.0, html.escape(row.label))
        )

    # legend of the tools present in this group
    present = sorted({row.tool for row in rows})
    legend_x = right + 18
    legend_y = top + 8
    parts.append(
        '<text x="%d" y="%d" font-family="sans-serif" font-size="11" '
        'font-weight="bold" fill="#333333">legend</text>' % (legend_x, legend_y)
    )
    legend_y += 18
    for tool in present:
        shape, color = TOOL_STYLE.get(tool, FALLBACK_STYLE)
        parts.append(_marker(shape, legend_x + 6, legend_y - 4, 5.0, color, tool))
        parts.append(
            '<text x="%d" y="%d" font-family="sans-serif" font-size="10" '
            'fill="#333333">%s</text>'
            % (legend_x + 18, legend_y, html.escape(tool))
        )
        legend_y += 18
    parts.append(
        '<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" '
        'stroke-width="2" stroke-dasharray="6 3" />'
        % (legend_x, legend_y - 4.0, legend_x + 14, legend_y - 4.0, FRONTIER_COLOR)
    )
    parts.append(
        '<text x="%d" y="%d" font-family="sans-serif" font-size="10" '
        'fill="#333333">Pareto frontier</text>' % (legend_x + 18, legend_y)
    )

    parts.append("</svg>\n")
    return "".join(parts)


def write_png(corpus, op, rows, frontier, path):
    """Optional matplotlib copy of a chart; failures are never fatal."""
    if not HAVE_MPL:
        return False
    try:
        figure, axes = plt.subplots(figsize=(9.0, 5.6), dpi=110)
        tools = sorted({row.tool for row in rows})
        for tool in tools:
            shape, color = TOOL_STYLE.get(tool, FALLBACK_STYLE)
            group = [row for row in rows if row.tool == tool]
            axes.scatter(
                [row.ratio for row in group],
                [row.mbps for row in group],
                marker={"square": "s", "diamond": "D", "triangle": "^"}.get(
                    shape, "o"
                ),
                s=42,
                color=color,
                label=tool,
                zorder=3,
            )
        if frontier:
            axes.plot(
                [row.ratio for row in frontier],
                [row.mbps for row in frontier],
                linestyle="--",
                color=FRONTIER_COLOR,
                zorder=2,
                label="Pareto frontier",
            )
        for row in rows:
            axes.annotate(
                row.label,
                (row.ratio, row.mbps),
                fontsize=6,
                xytext=(3, 3),
                textcoords="offset points",
            )
        axes.set_xlabel("ratio (input / output)")
        axes.set_ylabel("MB/s (log)")
        axes.set_yscale("log")
        axes.set_title("%s - %s" % (corpus, op))
        axes.grid(True, alpha=0.3)
        axes.legend(fontsize=8, loc="best")
        figure.tight_layout()
        figure.savefig(path)
        plt.close(figure)
        return True
    except Exception as exc:  # pragma: no cover - depends on the machine
        sys.stderr.write("report: png chart failed (%s)\n" % exc)
        return False


# --------------------------------------------------------------------------
# outputs
# --------------------------------------------------------------------------


def environment(rows):
    """Everything the CSV says about the machine, de-duplicated."""
    cpu_models = sorted({row.cpu_model for row in rows if row.cpu_model})
    cpu_counts = sorted({row.cpu_count for row in rows if row.cpu_count is not None})
    runners = sorted({row.runner for row in rows if row.runner})
    versions = collections.OrderedDict()
    for row in rows:
        if row.tool and row.tool not in versions:
            versions[row.tool] = row.tool_version or "unknown"
    loads = [row.loadavg for row in rows if row.loadavg is not None]
    return {
        "cpu_model": cpu_models,
        "cpu_count": cpu_counts,
        "runner": runners,
        "tool_versions": dict(versions),
        "loadavg_max": max(loads) if loads else None,
        "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    }


def sort_rows(rows):
    order = {"encode": 0, "decode": 1}
    return sorted(
        rows,
        key=lambda r: (
            r.corpus,
            order.get(r.op, 2),
            -(r.ratio if r.ratio is not None else float("-inf")),
            r.tool,
            r.config,
        ),
    )


def build_html(title, rows, groups, frontiers, env):
    """The whole report as one self-contained HTML document."""
    style = (
        "<style>\n"
        "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;"
        "margin:24px;color:#222;background:#fff;line-height:1.45}\n"
        "h1{font-size:24px;margin-bottom:4px}\n"
        "h2{font-size:19px;margin-top:32px;border-bottom:1px solid #ddd;"
        "padding-bottom:4px}\n"
        "h3{font-size:15px;margin-top:22px;color:#444}\n"
        ".meta{color:#555;font-size:13px;margin:2px 0}\n"
        ".chart{max-width:900px;width:100%;height:auto;margin:8px 0;"
        "border:1px solid #eee}\n"
        "table{border-collapse:collapse;font-size:12px;margin-top:8px}\n"
        "th,td{border:1px solid #e2e2e2;padding:3px 7px;text-align:right}\n"
        "th{background:#f4f4f4;text-align:center}\n"
        "td.text,th.text{text-align:left}\n"
        "code{background:#f6f6f6;padding:1px 4px;border-radius:3px}\n"
        "ul{margin:6px 0 12px 0}\n"
        "footer{margin-top:36px;color:#777;font-size:12px}\n"
        "</style>\n"
    )
    parts = ["<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n",
             '<meta charset="utf-8">\n',
             '<meta name="viewport" content="width=device-width,initial-scale=1">\n',
             "<title>%s</title>\n" % html.escape(title),
             style,
             "</head>\n<body>\n"]
    parts.append("<h1>%s</h1>\n" % html.escape(title))
    parts.append(
        '<p class="meta">generated %s &middot; %d rows from the results CSV</p>\n'
        % (html.escape(env["generated"]), len(rows))
    )
    parts.append("<h2>Environment</h2>\n<ul>\n")
    parts.append(
        "<li>CPU: %s</li>\n"
        % html.escape(", ".join(env["cpu_model"]) if env["cpu_model"] else "unknown")
    )
    parts.append(
        "<li>logical CPUs: %s</li>\n"
        % html.escape(
            ", ".join(str(c) for c in env["cpu_count"]) if env["cpu_count"] else "unknown"
        )
    )
    parts.append(
        "<li>runner: %s</li>\n"
        % html.escape(", ".join(env["runner"]) if env["runner"] else "unknown")
    )
    parts.append(
        "<li>peak load average recorded: %s</li>\n"
        % html.escape(
            fmt_fixed(env["loadavg_max"], 2) if env["loadavg_max"] is not None else "n/a"
        )
    )
    if env["tool_versions"]:
        entries = ", ".join(
            "%s %s" % (html.escape(name), html.escape(version))
            for name, version in sorted(env["tool_versions"].items())
        )
        parts.append("<li>tools: %s</li>\n" % entries)
    parts.append("</ul>\n")

    parts.append("<h2>Charts</h2>\n")
    if not groups:
        parts.append("<p>No plottable rows in the results CSV.</p>\n")
    for (corpus, op), member in groups.items():
        parts.append(
            "<h3>%s &mdash; %s</h3>\n" % (html.escape(corpus), html.escape(op))
        )
        parts.append(svg_chart(corpus, op, member, frontiers[(corpus, op)]))
        png_name = "pareto_%s_%s.png" % (corpus.replace("/", "_"), op)
        if os.path.exists(os.path.join(_OUT_DIR[0], png_name)):
            parts.append(
                '<img class="chart" alt="%s" src="%s">\n'
                % (html.escape("%s %s" % (corpus, op)), html.escape(png_name))
            )
        parts.append("<ul>\n")
        for row in frontiers[(corpus, op)]:
            parts.append(
                "<li>%s &mdash; ratio %s, %s</li>\n"
                % (
                    html.escape(row.label),
                    html.escape(fmt_ratio(row.ratio, 4)),
                    html.escape(fmt_mbps(row.mbps)),
                )
            )
        parts.append("</ul>\n")

    parts.append("<h2>All results</h2>\n")
    parts.append("<table>\n<thead><tr>\n")
    columns = [
        ("corpus", "text"),
        ("tool", "text"),
        ("tool_version", "text"),
        ("config", "text"),
        ("threads", ""),
        ("op", "text"),
        ("input_bytes", ""),
        ("output_bytes", ""),
        ("ratio", ""),
        ("seconds", ""),
        ("mbps", ""),
        ("max_rss_kb", ""),
        ("notes", "text"),
    ]
    for name, kind in columns:
        parts.append('<th class="%s">%s</th>\n' % (kind, html.escape(name)))
    parts.append("</tr></thead>\n<tbody>\n")
    for row in sort_rows(rows):
        parts.append("<tr>\n")
        for name, kind in columns:
            if name == "ratio":
                text = fmt_fixed(row.ratio, 4)
            elif name == "seconds":
                text = fmt_fixed(row.seconds, 3)
            elif name == "mbps":
                text = fmt_fixed(row.mbps, 2)
            elif name in ("threads", "input_bytes", "output_bytes"):
                text = fmt_int(getattr(row, name))
            elif name == "max_rss_kb":
                text = fmt_fixed(row.max_rss_kb, 0)
            else:
                text = getattr(row, name) or ""
            parts.append(
                '<td class="%s">%s</td>\n' % (kind, html.escape(str(text)))
            )
        parts.append("</tr>\n")
    parts.append("</tbody>\n</table>\n")

    parts.append(
        "<footer>Generated by <code>tools/bench/report.py</code>. "
        "Charts plot ratio against throughput per corpus and operation; the "
        "dashed line joins the non-dominated rows.</footer>\n"
    )
    parts.append("</body>\n</html>\n")
    return "".join(parts)


def build_markdown(title, groups, frontiers, env):
    """Frontier-only tables, plus the environment block."""
    parts = ["# %s\n\n" % title]
    parts.append("generated %s\n\n" % env["generated"])
    parts.append("## Environment\n\n")
    if env["cpu_model"]:
        parts.append("- CPU: %s\n" % ", ".join(env["cpu_model"]))
    if env["cpu_count"]:
        parts.append("- logical CPUs: %s\n" % ", ".join(str(c) for c in env["cpu_count"]))
    if env["runner"]:
        parts.append("- runner: %s\n" % ", ".join(env["runner"]))
    for name, version in sorted(env["tool_versions"].items()):
        parts.append("- %s: %s\n" % (name, version))
    parts.append("\n")
    if not groups:
        parts.append("No plottable rows.\n")
        return "".join(parts)
    for (corpus, op), frontier in frontiers.items():
        parts.append("## %s &mdash; %s\n\n" % (corpus, op))
        parts.append("| tool | config | threads | ratio | MB/s | RSS (MB) |\n")
        parts.append("|---|---|---|---|---|---|\n")
        if not frontier:
            parts.append("| _none_ | | | | | |\n\n")
            continue
        for row in frontier:
            rss = row.max_rss_kb / 1024.0 if row.max_rss_kb is not None else None
            parts.append(
                "| %s | %s | %s | %s | %s | %s |\n"
                % (
                    row.tool,
                    row.config,
                    fmt_int(row.threads),
                    fmt_fixed(row.ratio, 4),
                    fmt_fixed(row.mbps, 2),
                    fmt_fixed(rss, 1),
                )
            )
        parts.append("\n")
    return "".join(parts)


def build_json(rows, frontiers, env):
    """Machine-readable rows plus frontier indices into that array."""
    out_rows = [row.as_dict() for row in sort_rows(rows)]
    index_of = {}
    for position, row in enumerate(sort_rows(rows)):
        index_of[id(row)] = position
    pareto = {}
    for (corpus, op), frontier in frontiers.items():
        pareto["%s/%s" % (corpus, op)] = sorted(index_of[id(row)] for row in frontier)
    return {
        "environment": env,
        "pareto_index_into": "rows",
        "pareto": pareto,
        "rows": out_rows,
    }


# The HTML builder needs the output directory to know whether PNG copies
# exist; it is set in main() rather than threaded through every call.
_OUT_DIR = [""]


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Render the zGEC benchmark CSV into charts and a report."
    )
    parser.add_argument("--csv", required=True, help="results CSV to read")
    parser.add_argument("--out", required=True, help="directory to write into")
    parser.add_argument(
        "--title", default="zGEC benchmark report", help="report title"
    )
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    out_dir = args.out
    _OUT_DIR[0] = out_dir
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    rows = load_rows(args.csv)
    groups = group_rows(rows)
    frontiers = collections.OrderedDict()
    for key, member in groups.items():
        frontiers[key] = pareto_frontier(member)
    env = environment(rows)

    written = []
    for (corpus, op), member in groups.items():
        name = "pareto_%s_%s.svg" % (corpus.replace("/", "_"), op)
        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(svg_chart(corpus, op, member, frontiers[(corpus, op)]))
        written.append(name)
        png = "pareto_%s_%s.png" % (corpus.replace("/", "_"), op)
        if HAVE_MPL and write_png(
            corpus, op, member, frontiers[(corpus, op)], os.path.join(out_dir, png)
        ):
            written.append(png)

    html_path = os.path.join(out_dir, "report.html")
    with open(html_path, "w", encoding="utf-8") as handle:
        handle.write(build_html(args.title, rows, groups, frontiers, env))
    written.append("report.html")

    md_path = os.path.join(out_dir, "summary.md")
    with open(md_path, "w", encoding="utf-8") as handle:
        handle.write(build_markdown(args.title, groups, frontiers, env))
    written.append("summary.md")

    json_path = os.path.join(out_dir, "results.json")
    with open(json_path, "w", encoding="utf-8") as handle:
        json.dump(build_json(rows, frontiers, env), handle, indent=2, sort_keys=False)
        handle.write("\n")
    written.append("results.json")

    # A copy of the input next to the report keeps the artifact self-contained.
    if args.csv and os.path.exists(args.csv):
        with open(args.csv, "r", encoding="utf-8", errors="replace") as src:
            payload = src.read()
        copy_path = os.path.join(out_dir, "results.csv")
        with open(copy_path, "w", encoding="utf-8") as dst:
            dst.write(payload)
        written.append("results.csv")

    print(
        "report: %d rows, %d corpus/op groups, wrote %s into %s"
        % (len(rows), len(groups), ", ".join(written), out_dir)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
