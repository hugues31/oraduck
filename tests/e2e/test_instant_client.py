"""Without Instant Client: the extension loads, OCI is only needed to talk to Oracle."""

import os

from oraduck_helpers import run_duckdb, secret_sql


def _without_instant_client() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "OCI_HOME")}


def test_the_extension_works_without_instant_client_until_oracle_is_needed():
    proc = run_duckdb(secret_sql() + "SELECT count(*) FROM duckdb_secrets() WHERE type = 'oraduck';\n",
                      env=_without_instant_client())
    assert proc.returncode == 0, proc.stderr
    assert "1" in proc.stdout


def test_missing_instant_client_is_reported_clearly():
    for sql in ("SELECT oraduck_oci_version();\n",
                secret_sql() + "COPY (SELECT 1 AS id) TO 'E2E_ANY' (FORMAT oraduck, CONNECTION 'ora');\n"):
        proc = run_duckdb(sql, env=_without_instant_client())
        assert proc.returncode != 0
        assert "Oracle Instant Client not found" in proc.stderr, proc.stderr
        assert "LD_LIBRARY_PATH" in proc.stderr
