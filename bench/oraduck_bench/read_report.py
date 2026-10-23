"""Read benchmark report (Oracle -> DuckDB): tables, charts and the success criteria of spec 10.5."""

from __future__ import annotations

import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402

from .config import ROOT  # noqa: E402
from .netlink import LINE_RATE_BPS, LINE_RATE_BYTES_S  # noqa: E402
from .read_config import READ_COMPETITOR_RUNS, READ_RUNS, READ_TIMEOUT_S  # noqa: E402
from .read_methods import TIMED_SCOPE  # noqa: E402
from .report import COLORS, GRID, INK, INK_MUTED, SURFACE, _style  # noqa: E402

LABELS = {"oraduck": "OraDuck", "oraduck_scan": "OraDuck (oraduck_scan)", "quack_oracle": "quack-oracle",
          "rinie_oracle": "duckdb-oracle", "oracledb_thin": "python-oracledb (thin)",
          "oracledb_thick": "python-oracledb (thick)", "connectorx": "ConnectorX", "sqlplus_csv": "SQL*Plus → CSV"}
LINE_RATE_TARGET = 0.90
SHAPED_CASE = (1_000_000, 550)
PLACEHOLDER = ("<!-- Hand-written after each measurement campaign: replace this placeholder with the "
               "analysis of the results above (bench/read_analysis.md). -->\n_Not written yet._")
EVENT_TYPES = ("mismatch", "skipped", "timeout", "error")
Group = tuple  # (nrows, ncols, method, threads)


def label(method: str) -> str:
    return LABELS.get(method, method)


def load(files: list[Path]) -> dict:
    out = {"env": [], "measure": [], "planning": [], "events": []}
    for path in files:
        for line in Path(path).read_text().splitlines():
            record = json.loads(line)
            kind = record["type"]
            if kind in ("env", "measure", "planning"):
                out[kind].append(record)
            elif kind in EVENT_TYPES:
                out["events"].append(record)
    out["measure"] = [m for m in out["measure"] if m.get("suite", "read") == "read"]
    return out


def network(record: dict) -> str:
    """local, shaped (1 Gbit/s), compression (a variant), or other (another shaping, not reported)."""
    if record.get("variant"):
        return record["variant"]
    rate = (record.get("shaping") or {}).get("rate_bps")
    if rate is None:
        return "local"
    return "shaped" if rate == LINE_RATE_BPS else "other"


def group_runs(measures: list[dict], net: str) -> dict[Group, list[dict]]:
    groups: dict[Group, list[dict]] = defaultdict(list)
    for m in measures:
        if not m.get("warmup") and network(m) == net:
            groups[(m["nrows"], m["ncols"], m["method"], m["threads"])].append(m)
    return groups


def utilisation(link_bytes: int | None, total_s: float) -> float | None:
    """Fraction of the 125 MB/s link used."""
    if not link_bytes or total_s <= 0:
        return None
    return link_bytes / total_s / LINE_RATE_BYTES_S


def _median(runs: list[dict], key: str = "total_s") -> float:
    return statistics.median(r[key] for r in runs)


def _span(runs: list[dict]) -> str:
    times = [r["total_s"] for r in runs]
    return f"{min(times):.3f}–{max(times):.3f}"


def _pct(value: float | None) -> str:
    return "—" if value is None else f"{value * 100:.1f} %"


def _median_utilisation(runs: list[dict]) -> float | None:
    values = [u for r in runs if (u := utilisation(r.get("link_bytes"), r["total_s"])) is not None]
    return statistics.median(values) if values else None


def _extra(runs: list[dict], key: str, default="?"):
    return runs[-1].get("extra", {}).get(key, default)


def _case(nrows: int, ncols: int) -> str:
    return f"{nrows:,} × {ncols}"


def _speed(nrows: int, runs: list[dict]) -> tuple[str, str]:
    median = _median(runs)
    db_bytes = _extra(runs, "db_bytes", None)
    rate = f"{nrows / median:,.0f}"
    return rate, (f"{db_bytes / median / 1e6:,.1f}" if db_bytes else "—")


def _environment(env: dict, shaped: list[dict]) -> list[str]:
    competitors = env.get("competitors", {})
    lines = [
        "## Environment", "",
        f"- DuckDB {env.get('duckdb', '?')}, OCI client {env.get('oci_client', '?')}, Oracle {env.get('oracle', '?')} "
        "(free 19.3 registry image: the intended 19.30 needs a support contract)",
        f"- CPU {env.get('cpu', '?')}; client on CPUs {env.get('client_affinity', '?')}, "
        f"Oracle on CPUs {env.get('oracle_cpuset', '?')}; available memory {env.get('mem_available_gb', '?')} GB",
    ]
    for name, version in competitors.items():
        lines.append(f"- {LABELS.get(name, name)}: {version}")
    if shaped:
        s = shaped[-1].get("shaping", {})
        lines.append(f"- Shaped runs: tbf {s.get('rate_bps', 0) / 1e9:g} Gbit/s on the container side, netem delay "
                     f"{s.get('container_delay_us', '?')} µs (Oracle → client) and {s.get('bridge_delay_us', '?')} µs "
                     "(client → Oracle) on the Docker bridge")
    return lines + [""]


def _scope_section(local: dict[Group, list[dict]]) -> list[str]:
    lines = [f"Timed scope, the same for every method: **{TIMED_SCOPE}**. Untimed: extension installation and "
             "loading, secret creation, and the checks done afterwards (row count, file size, column types, "
             "checksums). The threads setting is not the parallelism: the sessions column is what each method "
             "really opens.", ""]
    seen = {}
    for (_, _, method, threads), runs in sorted(local.items(), key=lambda kv: kv[0][2:]):
        seen.setdefault((method, threads), (_extra(runs, "sessions"), _extra(runs, "timed")))
    if seen:
        lines += ["| Method | Threads | Sessions | What the timer covers |", "|---|---|---|---|"]
        lines += [f"| {label(m)} | {t} | {s} | {d.split(': ', 1)[-1]} |" for (m, t), (s, d) in seen.items()]
        lines.append("")
    return lines


def _matrix_section(local: dict[Group, list[dict]]) -> list[str]:
    rows = [(k, v) for k, v in sorted(local.items()) if k[2].startswith("oraduck")]
    lines = ["## Matrix (OraDuck, unconstrained local link)", "",
             f"Median of {READ_RUNS} runs after one warm-up run; MB/s is DuckDB storage (database file) per second.",
             ""]
    if not rows:
        return lines + ["No OraDuck measurement.", ""]
    lines += ["| Method | Threads | Rows | Columns | Median (s) | Min–max (s) | Rows/s | MB/s |",
              "|---|---|---|---|---|---|---|---|"]
    for (nrows, ncols, method, threads), runs in rows:
        rate, mbs = _speed(nrows, runs)
        lines.append(f"| {label(method)} | {threads} | {nrows:,} | {ncols} | {_median(runs):.3f} | {_span(runs)} "
                     f"| {rate} | {mbs} |")
    return lines + [""]


def _ratio(value: float) -> str:
    return f"×{value:.0f}" if value >= 10 else f"×{value:.1f}"


def _event_lines(events: list[dict]) -> list[str]:
    seen, lines = set(), []
    for e in events:
        key = (e["type"], e["nrows"], e["ncols"], e["method"], e["threads"], network(e))
        if key in seen:
            continue
        seen.add(key)
        who = f"{label(e['method'])} (N = {e['threads']}) on {_case(e['nrows'], e['ncols'])}"
        if e["type"] == "mismatch":
            lines.append(f"- {who}: **incorrect result** (not ranked): {e.get('message', '')[:300]}")
        elif e["type"] == "timeout":
            lines.append(f"- {who}: stopped after {e.get('limit_s', READ_TIMEOUT_S):g} s")
        elif e["type"] == "skipped":
            lines.append(f"- {who}: not run ({e.get('reason', 'a method stopped on a smaller case')})")
        else:
            lines.append(f"- {who}: error: {e.get('message', '')[:300]}")
    return lines


def _competitors_section(local: dict[Group, list[dict]], events: list[dict], out_dir: Path) -> list[str]:
    cases = sorted({k[:2] for k in local})
    lines = ["## Competitors (unconstrained local link)", "",
             f"Median of {READ_COMPETITOR_RUNS} runs after one warm-up run, methods interleaved, checksums compared "
             "with Oracle's after every run: only methods that load identical data are ranked. Speedup = method "
             "median / OraDuck (N = 8) median. Each method ran in the DuckDB version of its column. Runs of the same "
             "method and case from several result files (read-matrix and read-competitors) are merged into one "
             "median.", ""]
    others = [e for e in events if network(e) == "local"]
    if not any(k[2] != "oraduck" or k[3] != 8 for k in local) and not others:
        return lines + ["No competitor measurement.", ""]
    lines += ["| Rows | Columns | Method | N | Sessions | DuckDB | Median (s) | Min–max (s) | Rows/s | Speedup of OraDuck |",
              "|---|---|---|---|---|---|---|---|---|---|"]
    for nrows, ncols in cases:
        base = local.get((nrows, ncols, "oraduck", 8))
        for (r, c, method, threads), runs in sorted(local.items(), key=lambda kv: kv[0][2:]):
            if (r, c) != (nrows, ncols):
                continue
            speedup = "—"
            if base and (method, threads) != ("oraduck", 8):
                speedup = _ratio(_median(runs) / _median(base))
            lines.append(f"| {nrows:,} | {ncols} | {label(method)} | {threads} | {_extra(runs, 'sessions')} | "
                         f"{_extra(runs, 'duckdb')} | {_median(runs):.3f} | {_span(runs)} | "
                         f"{nrows / _median(runs):,.0f} | {speedup} |")
    lines.append("")
    if others:
        lines += ["Not ranked:", ""] + _event_lines(others) + [""]
    if _competitors_chart(local, cases, out_dir):
        lines += ["![Read: competitors](report_read_competitors.png)", ""]
    return lines


def _competitors_chart(local: dict[Group, list[dict]], cases: list[tuple[int, int]], out_dir: Path) -> bool:
    series = sorted({(k[2], k[3]) for k in local}, key=lambda s: (s[0] != "oraduck", s))
    if not cases or not series:
        return False
    fig, ax = plt.subplots(figsize=(8, 0.5 * len(cases) * max(1, len(series) // 2) + 1.8), facecolor=SURFACE)
    _style(ax)
    ax.grid(axis="y", visible=False)
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    height = 0.8 / len(series)
    for i, (method, threads) in enumerate(series):
        xs, ys = [], []
        for j, (nrows, ncols) in enumerate(reversed(cases)):
            runs = local.get((nrows, ncols, method, threads))
            if runs:
                xs.append(nrows / _median(runs))
                ys.append(j - 0.4 + height * (i + 0.5))
        ax.plot(xs, ys, linestyle="none", marker="o", markersize=8,
                markerfacecolor=COLORS.get(method, INK_MUTED) if threads != 1 else SURFACE,
                markeredgecolor=COLORS.get(method, INK_MUTED), label=f"{label(method)} (N = {threads})")
    ax.set_xscale("log")
    ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(
        lambda v, _: f"{v / 1e6:g}M" if v >= 1e6 else (f"{v / 1e3:g}k" if v >= 1e3 else f"{v:g}")))
    ax.set_yticks(range(len(cases)), [_case(r, c) for r, c in reversed(cases)], color=INK)
    ax.set_xlabel("Rows per second (median, log scale)", color=INK_MUTED)
    ax.set_title("Oracle 19c → DuckDB table: read methods", color=INK, fontsize=12)
    ax.legend(frameon=False, labelcolor=INK, loc="upper center", bbox_to_anchor=(0.5, -0.12), ncol=3, fontsize=8)
    fig.tight_layout()
    fig.savefig(out_dir / "report_read_competitors.png", dpi=150, facecolor=SURFACE, bbox_inches="tight")
    plt.close(fig)
    return True


def _shaped_section(shaped: dict[Group, list[dict]], local: dict[Group, list[dict]], events: list[dict],
                    out_dir: Path) -> list[str]:
    lines = ["## Shaped to 1 Gbit/s", "",
             "Utilisation = bytes received on the Oracle bridge during a run / total time / 125 MB/s (the link "
             "carries Oracle's traffic only; the total includes connection and planning time). The byte window brackets the "
             "whole method call, untimed set-up and checks included, and counts all traffic received on the bridge: "
             "the utilisation is an upper bound. Median over the runs.",
             ""]
    if not shaped:
        return lines + ["No shaped run (`sudo ./infra/net/shape.sh on`, then `oraduck-bench read-shaped`).", ""]
    lines += ["| Method | N | Rows | Columns | Median (s) | Min–max (s) | Link MB | Utilisation | Slowdown vs local |",
              "|---|---|---|---|---|---|---|---|---|"]
    for (nrows, ncols, method, threads), runs in sorted(shaped.items()):
        linked = [r["link_bytes"] for r in runs if r.get("link_bytes")]
        base = local.get((nrows, ncols, method, threads))
        slowdown = _ratio(_median(runs) / _median(base)) if base else "—"
        mb = f"{statistics.median(linked) / 1e6:,.0f}" if linked else "—"
        lines.append(f"| {label(method)} | {threads} | {nrows:,} | {ncols} | {_median(runs):.3f} | {_span(runs)} | "
                     f"{mb} | {_pct(_median_utilisation(runs))} | {slowdown} |")
    lines.append("")
    others = [e for e in events if network(e) == "shaped"]
    if others:
        lines += ["Not ranked:", ""] + _event_lines(others) + [""]
    if _shaped_chart(shaped, out_dir):
        lines += ["![Read: link utilisation](report_read_shaped.png)", ""]
    return lines


def _shaped_chart(shaped: dict[Group, list[dict]], out_dir: Path) -> bool:
    bars = [(f"{label(k[2])} (N = {k[3]}), {k[1]} cols", u) for k, runs in sorted(shaped.items())
            if (u := _median_utilisation(runs)) is not None]
    if not bars:
        return False
    fig, ax = plt.subplots(figsize=(8, 0.45 * len(bars) + 1.6), facecolor=SURFACE)
    _style(ax)
    ax.grid(axis="y", visible=False)
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    names = [b[0] for b in reversed(bars)]
    values = [b[1] * 100 for b in reversed(bars)]
    ax.barh(names, values, color=COLORS["oraduck"], height=0.6)
    ax.axvline(LINE_RATE_TARGET * 100, color=INK_MUTED, linestyle="--", linewidth=1)
    ax.text(LINE_RATE_TARGET * 100, len(bars) - 0.4, " 90 % target", color=INK_MUTED, fontsize=8, va="bottom")
    ax.set_xlabel("Link utilisation (% of 125 MB/s)", color=INK_MUTED)
    ax.set_title("Link utilisation, Oracle shaped to 1 Gbit/s", color=INK, fontsize=12)
    fig.tight_layout()
    fig.savefig(out_dir / "report_read_shaped.png", dpi=150, facecolor=SURFACE, bbox_inches="tight")
    plt.close(fig)
    return True


def _compression_section(compressed: dict[Group, list[dict]], local: dict[Group, list[dict]]) -> list[str]:
    lines = ["## SQL*Net compression", ""]
    if not compressed:
        return lines + ["No compression run (`oraduck-bench read-compression`).", ""]
    lines += ["`(COMPRESSION=on)` in the connect descriptor of the DSN, otherwise the same run as the matrix.", "",
              "| Rows | Columns | Median (s) | Link MB | Time vs uncompressed |", "|---|---|---|---|---|"]
    for (nrows, ncols, method, threads), runs in sorted(compressed.items()):
        base = local.get((nrows, ncols, method, threads))
        linked = [r["link_bytes"] for r in runs if r.get("link_bytes")]
        lines.append(f"| {nrows:,} | {ncols} | {_median(runs):.3f} | "
                     f"{statistics.median(linked) / 1e6:,.0f}" if linked else
                     f"| {nrows:,} | {ncols} | {_median(runs):.3f} | —")
        lines[-1] += f" | {_ratio(_median(runs) / _median(base)) if base else '—'} |"
    return lines + [""]


def _planning_section(planning: list[dict], local: dict[Group, list[dict]]) -> list[str]:
    lines = ["## Planning share (OraDuck)", "",
             "`Planning (ms)` of the OraDuck scan in one `EXPLAIN ANALYZE` per case, against the median time of the "
             "run (N = 8).", ""]
    rows = []
    for rec in planning:
        if rec.get("variant") or rec.get("shaping"):
            continue
        runs = local.get((rec["nrows"], rec["ncols"], "oraduck", 8))
        if runs:
            rows.append((rec["nrows"], rec["ncols"], rec["planning_ms"], _median(runs)))
    if not rows:
        return lines + ["No planning measurement.", ""]
    lines += ["| Rows | Columns | Planning (ms) | Median (s) | Share |", "|---|---|---|---|---|"]
    for nrows, ncols, ms, median in sorted(set(rows)):
        lines.append(f"| {nrows:,} | {ncols} | {ms:.1f} | {median:.3f} | {_pct(ms / 1000 / median)} |")
    return lines + [""]


def _criteria(local: dict[Group, list[dict]], shaped: dict[Group, list[dict]], mismatches: list[dict],
              any_measure: bool) -> list[str]:
    def verdict(ok: bool | None, detail: str = "") -> str:
        return ("**not evaluated** (no data)" if ok is None else f"**{'met' if ok else 'not met'}**") + detail

    lines = ["## Success criteria", ""]
    losses, evaluable = [], 0
    for nrows, ncols in sorted({k[:2] for k in local}):
        ora = local.get((nrows, ncols, "oraduck", 8))
        rivals = [_median(r) for (r_, c_, m, _), r in local.items() if (r_, c_) == (nrows, ncols)
                  and not m.startswith("oraduck")]
        if ora and rivals:
            evaluable += 1
            if _median(ora) > min(rivals):
                losses.append(_case(nrows, ncols))
    fastest = None if not evaluable else not losses
    lines.append(f"- Local: OraDuck is fastest on every case of the matrix: "
                 f"{verdict(fastest, f' (slower on {', '.join(losses)})' if losses else '')}")
    ora = shaped.get((*SHAPED_CASE, "oraduck", 8))
    util = _median_utilisation(ora) if ora else None
    lines.append(f"- Shaped to 1 Gbit/s: on 1M × 550, OraDuck reaches at least {LINE_RATE_TARGET * 100:.0f} % of "
                 f"line rate: {verdict(None if util is None else util >= LINE_RATE_TARGET, f' ({_pct(util)})' if util is not None else '')}")
    names = sorted({f"{label(e['method'])} on {_case(e['nrows'], e['ncols'])}" for e in mismatches})
    lines.append(f"- Correctness: identical checksums for every method that finishes: "
                 f"{verdict(None if not any_measure else not names, f' (incorrect result: {', '.join(names)})' if names else '')}")
    return lines + [""]


def write_read_report(files: list[Path], out: Path = ROOT / "bench" / "REPORT_READ.md",
                      analysis: Path | None = ROOT / "bench" / "read_analysis.md") -> Path:
    data = load(files)
    measures = data["measure"]
    local = group_runs(measures, "local")
    shaped = group_runs(measures, "shaped")
    compressed = group_runs(measures, "compression")
    env = data["env"][-1] if data["env"] else {}
    env = {**env, "competitors": next((e["competitors"] for e in reversed(data["env"]) if "competitors" in e), {})}
    lines = ["# OraDuck read benchmark: Oracle 19c → DuckDB", "",
             "A partition of 1M rows (1M × 50 and 1M × 550 columns, wide tables with CLOBs) is read from Oracle "
             "into a new DuckDB table. Every number is a median of measured runs; methods are interleaved and "
             "checked against Oracle's checksums.", ""]
    lines += _environment(env, [m for m in measures if network(m) == "shaped"])
    lines += _scope_section({**local, **{k: v for k, v in shaped.items() if k not in local}})
    out_dir = out.parent
    lines += _matrix_section(local)
    lines += _competitors_section(local, data["events"], out_dir)
    lines += _shaped_section(shaped, local, data["events"], out_dir)
    lines += _compression_section(compressed, local)
    lines += _planning_section(data["planning"], local)
    lines += _criteria(local, shaped, [e for e in data["events"] if e["type"] == "mismatch"], bool(measures))
    text = Path(analysis).read_text().strip() if analysis is not None and Path(analysis).exists() else PLACEHOLDER
    lines += ["## Analysis", "", text, ""]
    out.write_text("\n".join(lines) + "\n")
    return out
