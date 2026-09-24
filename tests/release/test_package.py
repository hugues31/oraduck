import gzip
import os
import subprocess
import tarfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "infra" / "release" / "package.sh"


def _package(tmp_path, platforms):
    dist = tmp_path / "dist"
    for platform in platforms:
        (dist / f"oraduck-{platform}").mkdir(parents=True)
        (dist / f"oraduck-{platform}" / "oraduck.duckdb_extension").write_bytes(platform.encode())
    return subprocess.run([str(SCRIPT), str(dist), str(tmp_path / "site"), str(tmp_path / "release")],
                          env={**os.environ, "DUCKDB_VERSION": "v1.5.5"}, capture_output=True, text=True)


def test_artifacts_become_an_extension_repository_and_release_files(tmp_path):
    proc = _package(tmp_path, ["linux_amd64", "windows_amd64"])
    assert proc.returncode == 0, proc.stderr
    # layout read by INSTALL ... FROM '<repository>': <repository>/<version>/<platform>/<name>.duckdb_extension.gz
    repo = tmp_path / "site" / "v1.5.5"
    assert gzip.decompress((repo / "linux_amd64" / "oraduck.duckdb_extension.gz").read_bytes()) == b"linux_amd64"
    assert gzip.decompress((repo / "windows_amd64" / "oraduck.duckdb_extension.gz").read_bytes()) == b"windows_amd64"
    # DuckDB takes the extension name from the file name: the release ships the repository tree
    # (for offline installs), where every file is named oraduck.duckdb_extension.gz
    release = tmp_path / "release"
    assert [p.name for p in release.iterdir()] == ["oraduck-repository.tar.gz"]
    with tarfile.open(release / "oraduck-repository.tar.gz") as archive:
        assert sorted(archive.getnames()) == [
            "v1.5.5", "v1.5.5/linux_amd64", "v1.5.5/linux_amd64/oraduck.duckdb_extension.gz",
            "v1.5.5/windows_amd64", "v1.5.5/windows_amd64/oraduck.duckdb_extension.gz"]
    assert "INSTALL oraduck FROM 'https://hugues31.github.io/oraduck';" in (tmp_path / "site" / "index.html").read_text()


def test_no_artifact_is_an_error(tmp_path):
    proc = _package(tmp_path, [])
    assert proc.returncode != 0
    assert "no artifact" in proc.stderr
