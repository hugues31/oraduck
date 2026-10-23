"""Runs the DuckDB CLI built with the OraDuck extension."""

from __future__ import annotations

import csv
import io
import re
import subprocess
from pathlib import Path

from .checks import DUCK_CHECKSUM_SQL, Checksums
from .config import DUCKDB_CLI, WORK

_TIMER = re.compile(r"Run Time \(s\): real (\d+(?:\.\d+)?)")


# The CLI prints WARNING log entries on stdout (colored), in the middle of a result
_WARNING = re.compile(r"(?:\x1b\[[0-9;]*m)*WARNING:\n(?:\x1b\[[0-9;]*m)*(.*?)\n\n(?:\x1b\[[0-9;]*m)*", re.DOTALL)


def strip_warnings(stdout: str) -> str:
    return _WARNING.sub("", stdout)


_PASSWORD = re.compile(r"(PASSWORD\s+')[^']*(')", re.IGNORECASE)
_CONNECT_STRING = re.compile(r"([\w$#.-]+/)[^\s@'\"/]+(@)")


_URL_CREDENTIALS = re.compile(r"([a-z][a-z0-9+.-]*://[^\s:/@'\"]+:)[^\s@'\"]+(@)", re.IGNORECASE)


_known_secrets: set[str] = set()


def add_secret(secret: str | None) -> None:
    """Registers a password: every error message built by this module masks it."""
    if secret:
        _known_secrets.add(secret)


def forget_secrets() -> None:
    _known_secrets.clear()


def redact(text: str, secrets: tuple[str, ...] = ()) -> str:
    """Hides every literal value of `secrets` and of the registered secrets (also when URL-encoded), first, so a
    password containing `@` or `/` is not cut by a pattern; then the passwords of CREATE SECRET statements, of
    user/password@dsn and of scheme://user:password@host strings."""
    from urllib.parse import quote

    forms = set()
    for secret in (*secrets, *_known_secrets):
        if secret:
            forms.update((secret, quote(secret, safe="")))
    for form in sorted(forms, key=len, reverse=True):
        text = text.replace(form, "***")
    return _URL_CREDENTIALS.sub(r"\1***\2", _CONNECT_STRING.sub(r"\1***\2", _PASSWORD.sub(r"\1***\2", text)))


class DuckDBError(RuntimeError):
    pass


class MethodTimeout(DuckDBError):
    pass


def run(sql: str, db: Path | str = ":memory:", csv_output: bool = False, timeout_s: float | None = None,
        unsigned: bool = False, cli: Path | None = None, secrets: tuple[str, ...] = ()) -> str:
    """`cli` selects another DuckDB CLI (the v1.5.5 one for extensions not built for 2.0)."""
    cli = cli or DUCKDB_CLI
    args = [str(cli), "-batch", "-bail"]
    if cli == DUCKDB_CLI:  # v2.0 prints agent-mode output when AI_AGENT is set; v1.5 has no such option
        args.append("-no-agent")
    if csv_output:
        args += ["-csv", "-noheader"]
    if unsigned:
        args.append("-unsigned")
    args.append(str(db))
    WORK.mkdir(parents=True, exist_ok=True)
    try:
        proc = subprocess.run(args, input=sql, text=True, capture_output=True, cwd=WORK, timeout=timeout_s)
    except subprocess.TimeoutExpired as exc:  # the CLI was killed
        raise MethodTimeout(f"duckdb killed after {timeout_s} s") from exc
    if proc.returncode != 0:
        raise DuckDBError(redact(
            f"duckdb failed ({proc.returncode}):\n{proc.stderr.strip()}\n--- script ---\n{sql[:2000]}", secrets))
    return strip_warnings(proc.stdout)


def query(sql: str, db: Path | str, unsigned: bool = False, cli: Path | None = None) -> list[list[str]]:
    return [row for row in csv.reader(io.StringIO(run(sql, db, csv_output=True, unsigned=unsigned, cli=cli)))
            if row]


def timed(setup: str, statement: str, db: Path | str, timeout_s: float | None = None,
          unsigned: bool = False, cli: Path | None = None) -> float:
    """Time in seconds of `statement` alone, measured by `.timer on`."""
    out = run(f"{setup}\n.timer on\n{statement}\n.timer off\n", db, timeout_s=timeout_s, unsigned=unsigned,
              cli=cli)
    times = _TIMER.findall(out)
    if len(times) != 1:
        raise DuckDBError(f"missing or ambiguous timing in the output:\n{out[-2000:]}")
    return float(times[0])


def timed_sum(setup: str, statements: list[str], db: Path | str, timeout_s: float | None = None,
              unsigned: bool = False, cli: Path | None = None) -> float:
    """Sum of the CLI timers of `statements` (each one timed on its own), `setup` untimed."""
    body = "\n".join(f".timer on\n{st}\n.timer off" for st in statements)
    out = run(f"{setup}\n{body}\n", db, timeout_s=timeout_s, unsigned=unsigned, cli=cli)
    times = _TIMER.findall(out)
    if len(times) != len(statements):
        raise DuckDBError(f"expected {len(statements)} timings, got {len(times)}:\n{out[-2000:]}")
    return sum(float(t) for t in times)


def checksums(table: str, db: Path | str) -> Checksums:
    return Checksums(*(int(v) for v in query(DUCK_CHECKSUM_SQL.format(table=table), db)[0]))


def version(cli: Path | None = None) -> str:
    return query("SELECT version();", ":memory:", cli=cli)[0][0]
