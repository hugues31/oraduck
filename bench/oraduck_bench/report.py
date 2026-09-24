"""Benchmark report: medians, markdown tables and charts."""

from __future__ import annotations

import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib
import matplotlib.ticker

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

import re  # noqa: E402

from .config import (COMPETITOR_RUNS, COMPETITOR_SERIES, COMPETITOR_TIMEOUT_S, RINIE_COMMIT, ROOT,  # noqa: E402
                     RUNS)

# Validated categorical palette (dataviz/validate_palette.js, light surface #fcfcfb: all checks PASS;
# oraduck/quack/rinie also pass all-pairs, contrast relief = the table next to the chart)
COLORS = {"oraduck": "#2a78d6", "sqlldr": "#eb6834", "quack_oracle": "#1baf7a", "rinie_oracle": "#eda100"}
SURFACE = "#fcfcfb"
INK = "#1f1f1e"  # primary text
INK_MUTED = "#6b6a63"  # axes, ticks
GRID = "#e4e3dd"


def _style(ax) -> None:
    """Recessive axes: light horizontal grid, no top/right frame, neutral text."""
    ax.set_facecolor(SURFACE)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    ax.spines[["top", "right"]].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(INK_MUTED)
    ax.tick_params(colors=INK_MUTED, labelcolor=INK)
LABELS = {"oraduck": "OraDuck", "sqlldr": "CSV + SQL*Loader", "quack_oracle": "quack-oracle",
          "rinie_oracle": "duckdb-oracle"}


def load(files: list[Path]) -> tuple[list[dict], list[dict], list[dict]]:
    """(environments, measures, events): events are the timeouts and skipped runs of the competitor suite."""
    envs, measures, events = [], [], []
    for path in files:
        for line in Path(path).read_text().splitlines():
            record = json.loads(line)
            {"env": envs, "measure": measures}.get(record["type"], events).append(record)
    return envs, measures, events


def medians(measures: list[dict]) -> dict[tuple, dict]:
    groups: dict[tuple, list[dict]] = defaultdict(list)
    for m in measures:
        if m["warmup"]:
            continue
        key = (m["suite"], m["nrows"], m["ncols"], m["threads"], m["method"], m["stream_size"])
        groups[key].append(m)
    out = {}
    for key, items in groups.items():
        totals = [i["total_s"] for i in items]
        med = {"total_s": statistics.median(totals), "min_s": min(totals), "max_s": max(totals)}
        for field in ("csv_s", "load_s", "csv_bytes"):
            values = [i[field] for i in items if i.get(field) is not None]
            med[field] = statistics.median(values) if values else None
        out[key] = med
    return out


def _rows_label(n: int) -> str:
    return f"{n // 1_000_000}M" if n >= 1_000_000 else f"{n // 1000}k"


def _throughput(csv_bytes: float | None, seconds: float) -> str:
    """Throughput in MB/s of CSV equivalent: the same reference volume for both methods."""
    return f"{csv_bytes / 1e6 / seconds:.1f}" if csv_bytes else "—"


def _pick(meds: dict, suite: str, nrows: int, ncols: int, threads: int, method: str) -> dict | None:
    for key, value in meds.items():
        if key[:5] == (suite, nrows, ncols, threads, method):
            return value
    return None


def _matrix_section(meds: dict, out_dir: Path) -> list[str]:
    cases = sorted({(k[1], k[2], k[3]) for k in meds if k[0] == "matrix"})
    if not cases:
        return []
    lines = ["## Matrix (N = 8)", "",
             "| Rows | Columns | CSV volume (MB) | OraDuck (s) | SQL*Loader total (s) | of which CSV (s) "
             "| of which sqlldr (s) | Speedup | OraDuck (rows/s) | OraDuck (MB/s) | SQL*Loader (MB/s) "
             "| OraDuck min–max (s) | SQL*Loader min–max (s) |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    labels, oraduck_times, ldr_times = [], [], []
    for nrows, ncols, threads in cases:
        ora = _pick(meds, "matrix", nrows, ncols, threads, "oraduck")
        ldr = _pick(meds, "matrix", nrows, ncols, threads, "sqlldr")
        if not ora or not ldr:
            continue
        volume = f"{ldr['csv_bytes'] / 1e6:.1f}" if ldr["csv_bytes"] else "—"
        lines.append(
            f"| {_rows_label(nrows)} | {ncols} | {volume} | {ora['total_s']:.3f} | {ldr['total_s']:.3f} "
            f"| {ldr['csv_s']:.3f} | {ldr['load_s']:.3f} | ×{ldr['total_s'] / ora['total_s']:.1f} "
            f"| {nrows / ora['total_s']:,.0f} "
            + f"| {_throughput(ldr['csv_bytes'], ora['total_s'])} | {_throughput(ldr['csv_bytes'], ldr['total_s'])} "
            + f"| {ora['min_s']:.3f}–{ora['max_s']:.3f} | {ldr['min_s']:.3f}–{ldr['max_s']:.3f} |"
        )
        labels.append(f"{_rows_label(nrows)}×{ncols}")
        oraduck_times.append(ora["total_s"])
        ldr_times.append(ldr["total_s"])
    # Small multiples (one panel per row count), linear scale: bars on a log scale cannot
    # be compared by their length.
    by_rows: dict[int, list[tuple[int, float, float]]] = defaultdict(list)
    for label, b, l in zip(labels, oraduck_times, ldr_times):
        rows_label, ncols = label.split("×")
        by_rows[next(n for n, c, _ in cases if _rows_label(n) == rows_label)].append((int(ncols), b, l))
    panels = sorted(by_rows)
    fig, axes = plt.subplots(1, len(panels), figsize=(4 * len(panels), 4.2), facecolor=SURFACE, squeeze=False)
    width = 0.38
    for ax, nrows in zip(axes[0], panels):
        _style(ax)
        items = sorted(by_rows[nrows])
        x = list(range(len(items)))
        ora = [b for _, b, _ in items]
        ldr = [l for _, _, l in items]
        ax.bar([i - width / 2 for i in x], ora, width, color=COLORS["oraduck"], edgecolor=SURFACE, linewidth=2,
               label=LABELS["oraduck"])
        ax.bar([i + width / 2 for i in x], ldr, width, color=COLORS["sqlldr"], edgecolor=SURFACE, linewidth=2,
               label=LABELS["sqlldr"])
        top = max(ldr + ora)
        for i, (b, l) in enumerate(zip(ora, ldr)):
            ax.annotate(f"×{l / b:.1f}", (i, max(b, l)), xytext=(0, 4), textcoords="offset points", ha="center",
                        va="bottom", fontsize=9, color=INK, fontweight="bold")
        ax.set_ylim(0, top * 1.18)
        ax.set_xticks(x, [f"{c} cols" for c, _, _ in items], color=INK)
        ax.set_title(f"{_rows_label(nrows)} rows", color=INK, fontsize=11)
        ax.set_ylabel("Median export time (s)", color=INK_MUTED)
    axes[0][0].legend(frameon=False, loc="upper left", labelcolor=INK)
    fig.suptitle("DuckDB → Oracle 19c: OraDuck vs CSV + SQL*Loader (N = 8) — speedup above the bars",
                 color=INK, fontsize=12)
    fig.tight_layout()
    fig.savefig(out_dir / "report_matrix.png", dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return lines + ["", "![Matrix](report_matrix.png)", ""]


def _scaling_section(meds: dict, out_dir: Path) -> list[str]:
    keys = sorted({(k[1], k[2], k[3]) for k in meds if k[0] == "scaling"})
    if not keys:
        return []
    lines = ["## Scaling", "", "| N | OraDuck (s) | SQL*Loader (s) | Speedup |", "|---|---|---|---|"]
    series = {"oraduck": [], "sqlldr": []}
    threads_list = []
    for nrows, ncols, threads in keys:
        ora = _pick(meds, "scaling", nrows, ncols, threads, "oraduck")
        ldr = _pick(meds, "scaling", nrows, ncols, threads, "sqlldr")
        if not ora or not ldr:
            continue
        lines.append(f"| {threads} | {ora['total_s']:.3f} | {ldr['total_s']:.3f} "
                     f"| ×{ldr['total_s'] / ora['total_s']:.1f} |")
        threads_list.append(threads)
        series["oraduck"].append(ora["total_s"])
        series["sqlldr"].append(ldr["total_s"])
    fig, ax = plt.subplots(figsize=(6.5, 4), facecolor=SURFACE)
    _style(ax)
    for method, values in series.items():
        ax.plot(threads_list, values, marker="o", markersize=8, linewidth=2, label=LABELS[method],
                color=COLORS[method], markeredgecolor=SURFACE, markeredgewidth=2)
        ax.annotate(f"{values[-1]:.2f} s", (threads_list[-1], values[-1]), xytext=(8, 0), textcoords="offset points",
                    va="center", fontsize=9, color=INK)
    ax.set_ylim(0, max(max(v) for v in series.values()) * 1.12)
    ax.set_xticks(threads_list)
    ax.set_xlabel("N (DuckDB threads / sqlldr files and processes)", color=INK_MUTED)
    ax.set_ylabel("Median time (s)", color=INK_MUTED)
    ax.set_title(f"Scaling ({_rows_label(keys[0][0])} × {keys[0][1]})", color=INK)
    ax.legend(frameon=False, labelcolor=INK)
    fig.tight_layout()
    fig.savefig(out_dir / "report_scaling.png", dpi=150, facecolor=SURFACE)
    plt.close(fig)
    return lines + ["", "![Scaling](report_scaling.png)", ""]


def _tune_section(meds: dict) -> list[str]:
    rows = sorted((k[5], v["total_s"]) for k, v in meds.items() if k[0] == "tune")
    if not rows:
        return []
    lines = ["## STREAM_SIZE tuning (OraDuck, 1M × 20, N = 8)", "", "| STREAM_SIZE | Time (s) |", "|---|---|"]
    lines += [f"| {size // 1024} KiB | {total:.3f} |" for size, total in rows]
    lines += ["", "Sizes were measured one after the other, not interleaved: the shape of the curve mostly reflects "
              "the machine's drift during the campaign. 4 MiB is kept (best median, small spread between runs, "
              "consistent with the single-session probe, E4). The SQL*Loader parameters are the ones mandated by the "
              "reference method and were not tuned."]
    return lines + [""]


def _times(value: float) -> str:
    return f"{value:.0f}" if value >= 10 else f"{value:.1f}"


def _ratio(value: float) -> str:
    return "×" + _times(value)


def _competitors_section(meds: dict, events: list[dict], versions: dict, out_dir: Path) -> list[str]:
    events = [e for e in events if e.get("suite") == "competitors"]
    cases = sorted({(k[1], k[2]) for k in meds if k[0] == "competitors"} | {(e["nrows"], e["ncols"]) for e in events})
    if not cases:
        return []
    # keyed by (method, threads, nrows, ncols)
    timeouts = {(e["method"], e["threads"], e["nrows"], e["ncols"]): e["limit_s"]
                for e in events if e["type"] == "timeout"}
    skipped = {(e["method"], e["threads"], e["nrows"], e["ncols"]) for e in events if e["type"] == "skipped"}

    def cell(method: str, threads: int, nrows: int, ncols: int) -> str:
        med = _pick(meds, "competitors", nrows, ncols, threads, method)
        if med:
            return f"{med['total_s']:.3f}"
        key = (method, threads, nrows, ncols)
        if key in timeouts:
            return f"> {timeouts[key] / 60:.0f} min"
        return "not run" if key in skipped else "—"

    def speedup(method: str, nrows: int, ncols: int) -> str:
        ora = _pick(meds, "competitors", nrows, ncols, 8, "oraduck")
        other = _pick(meds, "competitors", nrows, ncols, 8, method)
        if ora and other:
            return _ratio(other["total_s"] / ora["total_s"])
        if ora and (method, 8, nrows, ncols) in timeouts:
            return "> " + _ratio(timeouts[method, 8, nrows, ncols] / ora["total_s"])
        return "—"

    limit_min = COMPETITOR_TIMEOUT_S // 60
    lines = [
        "## Comparison with other DuckDB → Oracle extensions", "",
        f"Source: a DuckDB in-memory table, copied from the benchmark database before the timer starts (the "
        f"10M × 50 case, about 9 GB, reads the attached database file instead). Timed: the single statement that "
        f"writes to Oracle, `COPY … TO` for OraDuck and `INSERT INTO <attached table> SELECT * FROM source` for the "
        f"others. Median of {COMPETITOR_RUNS} runs after one warm-up run, methods interleaved, checksums checked "
        f"after every run. A run is stopped after {limit_min} min (whole DuckDB process); a method stopped on a "
        f"case is not run on the cases with at least as many rows and columns (\"not run\").", "",
        f"- [quack-oracle](https://github.com/krokozyab/quack-oracle): community extension `oracle_scanner` "
        f"{versions.get('quack_oracle', '?')}, pure C++ implementation of the Oracle wire protocol; one session, "
        "array inserts of up to 1,024 rows per round trip.",
        f"- [duckdb-oracle](https://github.com/rinie/duckdb-oracle) (rinie): commit "
        f"{versions.get('rinie_oracle', RINIE_COMMIT)}, built from source against DuckDB v1.5.5 (one missing "
        "`#include` added to build with GCC 16); ODPI-C, one session, one `INSERT … VALUES` per row.",
        "- OraDuck: OCI direct path, one session per DuckDB thread (N = 8, and N = 1 for a single-session "
        "comparison).",
        "- All three load identical data (same checksums, `MINUS` = 0 between the tables). One difference: "
        "OraDuck refuses DECIMAL into BINARY_DOUBLE (and other conversions outside its type table), the other "
        "two let Oracle convert.", "",
        "| Rows | Columns | OraDuck N=8 (s) | OraDuck N=1 (s) | quack-oracle (s) | duckdb-oracle (s) "
        "| OraDuck speedup vs quack-oracle | OraDuck speedup vs duckdb-oracle |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for nrows, ncols in cases:
        cells = " | ".join(cell(m, t, nrows, ncols) for m, t in COMPETITOR_SERIES)
        lines.append(f"| {_rows_label(nrows)} | {ncols} | {cells} "
                     f"| {speedup('quack_oracle', nrows, ncols)} | {speedup('rinie_oracle', nrows, ncols)} |")
    single = [other["total_s"] / ora["total_s"] for nrows, ncols in cases
              if (ora := _pick(meds, "competitors", nrows, ncols, 1, "oraduck"))
              and (other := _pick(meds, "competitors", nrows, ncols, 8, "quack_oracle"))]
    if single:
        lo, hi = (_times(v) for v in (min(single), max(single)))
        span = lo if lo == hi else f"{lo} to {hi}"
        lines += ["", f"With a single session (N = 1), OraDuck is {span} times faster than quack-oracle."]
    _competitors_chart(meds, cases, timeouts, out_dir)
    return lines + ["", "![Comparison with other extensions](report_competitors.png)", ""]


def _competitors_chart(meds: dict, cases: list[tuple[int, int]], timeouts: dict, out_dir: Path) -> None:
    """Dot plot of rows/s on a log axis (dots, not bars: bar lengths on a log scale are meaningless)."""
    styles = {("oraduck", 8): dict(marker="o", label="OraDuck (N = 8)"),
              ("oraduck", 1): dict(marker="o", label="OraDuck (N = 1)", hollow=True),
              ("quack_oracle", 8): dict(marker="s", label="quack-oracle"),
              ("rinie_oracle", 8): dict(marker="D", label="duckdb-oracle")}
    fig, ax = plt.subplots(figsize=(8, 0.45 * len(cases) + 1.8), facecolor=SURFACE)
    _style(ax)
    ax.grid(axis="y", visible=False)
    ax.grid(axis="x", which="major", color=GRID, linewidth=0.8)
    y = {case: i for i, case in enumerate(reversed(cases))}
    # small vertical dodge per series, so that close values (OraDuck N = 8 and N = 1) do not hide each other
    dodge = dict(zip(styles, (0.17, -0.17, 0.0, 0.0)))
    for (method, threads), style in styles.items():
        color = COLORS[method]
        xs, ys, bound_x, bound_y = [], [], [], []
        for nrows, ncols in cases:
            med = _pick(meds, "competitors", nrows, ncols, threads, method)
            if med:
                xs.append(nrows / med["total_s"])
                ys.append(y[nrows, ncols] + dodge[method, threads])
            elif (method, threads, nrows, ncols) in timeouts:
                bound_x.append(nrows / timeouts[method, threads, nrows, ncols])
                bound_y.append(y[nrows, ncols] + dodge[method, threads])
        face = SURFACE if style.get("hollow") else color
        edge = color if style.get("hollow") else SURFACE
        ax.plot(xs, ys, linestyle="none", marker=style["marker"], markersize=9, markerfacecolor=face,
                markeredgecolor=edge, markeredgewidth=2 if style.get("hollow") else 1.5, label=style["label"])
        ax.plot(bound_x, bound_y, linestyle="none", marker="<", markersize=11, markerfacecolor=color,
                markeredgecolor=SURFACE, markeredgewidth=1.5)
    if timeouts:
        ax.plot([], [], linestyle="none", marker="<", markersize=11, markerfacecolor=INK_MUTED,
                markeredgecolor=SURFACE, label=f"stopped after {COMPETITOR_TIMEOUT_S // 60} min (slower than shown)")
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(
        lambda v, _: f"{v / 1e6:g}M" if v >= 1e6 else (f"{v / 1e3:g}k" if v >= 1e3 else f"{v:g}")))
    ax.set_yticks(list(y.values()), [f"{_rows_label(r)} × {c}" for r, c in y], color=INK)
    ax.set_ylim(-0.6, len(cases) - 0.4)
    ax.set_xlabel("Rows per second (median, log scale)", color=INK_MUTED)
    ax.set_title("DuckDB in-memory table → Oracle 19c: DuckDB extensions", color=INK, fontsize=12)
    ax.legend(frameon=False, labelcolor=INK, loc="upper center", bbox_to_anchor=(0.5, -0.1),
              ncol=3, fontsize=9)
    fig.tight_layout()
    fig.savefig(out_dir / "report_competitors.png", dpi=150, facecolor=SURFACE, bbox_inches="tight")
    plt.close(fig)


def _sqlldr_banner(banner: str) -> str:
    # "SQL*Loader: Release 19.0.0.0.0 - Production on <date> / Version 19.32.0.0.0" -> "Release ... / Version ..."
    banner = banner.replace("SQL*Loader: ", "")
    return re.sub(r" - Production on [^/]*", " ", banner).replace("  ", " ").strip()


def write_report(files: list[Path], out: Path = ROOT / "bench" / "REPORT.md",
                 analysis: Path | None = ROOT / "bench" / "analysis.md") -> Path:
    envs, measures, events = load(files)
    meds = medians(measures)
    env = envs[-1] if envs else {}
    lines = [
        "# OraDuck benchmark: DuckDB → Oracle 19c",
        "",
        "Only the export is timed: from the loaded DuckDB table to the rows committed in Oracle. "
        "SQL*Loader = CSV export (N files) + N concurrent sqlldr processes. "
        f"Median of {RUNS} runs, after one warm-up run, methods interleaved.",
        "",
        "## Environment",
        "",
        f"- DuckDB {env.get('duckdb', '?')}, OCI client {env.get('oci_client', '?')}, "
        f"Oracle {env.get('oracle', '?')}",
        "- Oracle 19c 19.3.0.0 (free registry image). The intended target, 19c 19.30 (Release Update), could not "
        "be tested: its images and patches require an Oracle support contract.",
        f"- SQL*Loader: {_sqlldr_banner(env.get('sqlldr', '?'))} — parameters "
        f"`{' '.join(env.get('sqlldr_params', []))}`",
        f"- CPU {env.get('cpu', '?')}; client on CPUs {env.get('client_affinity', '?')}, "
        f"Oracle on CPUs {env.get('oracle_cpuset', '?')}",
        f"- {env.get('fakelake', '?')}; "
        f"available memory {env.get('mem_available_gb', '?')} GB",
        "",
    ]
    out_dir = out.parent
    lines += _matrix_section(meds, out_dir)
    if analysis is not None and Path(analysis).exists():
        lines += ["## Analysis", "", Path(analysis).read_text().strip(), ""]
    lines += _scaling_section(meds, out_dir)
    versions = next((e["competitors"] for e in reversed(envs) if "competitors" in e), {})
    lines += _competitors_section(meds, events, versions, out_dir)
    lines += _tune_section(meds)
    out.write_text("\n".join(lines) + "\n")
    return out
