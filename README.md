# OraDuck

A DuckDB extension that loads the result of a query into an Oracle table through
the OCI Direct Path API — the engine behind `sqlldr direct=true` — in parallel
(one Oracle session per DuckDB thread) and without any intermediate file.

```sql
INSTALL oraduck FROM community;
LOAD oraduck;
CREATE SECRET oracle_test (TYPE oraduck, USER 'bench', PASSWORD '...', DSN 'dbhost:1521/ORCLPDB1');
COPY (SELECT id, amount, operation_date, label FROM source)
TO 'MY_SCHEMA.MY_TABLE' (FORMAT ORADUCK, CONNECTION 'oracle_test');
```

On Oracle 19c, over 9 table shapes from 100k × 5 to 10M × 50, OraDuck is:

- 1.2 to 3.2 times faster than exporting the table to CSV files and loading
  them with parallel direct path `sqlldr` (median of 5 runs);
- 6 to 48 times faster than [quack-oracle](https://github.com/krokozyab/quack-oracle)
  and 240 to 500 times faster than [duckdb-oracle](https://github.com/rinie/duckdb-oracle),
  the other DuckDB extensions that write to Oracle (from an in-memory table,
  median of 3 runs; duckdb-oracle takes more than 15 minutes from 1M rows).

See [`bench/REPORT.md`](bench/REPORT.md).

## Install

Prebuilt for **DuckDB v1.5.5** on Linux x86_64 and arm64 (glibc 2.28 or later)
and Windows x86_64. No compiler is needed.

OraDuck loads [Oracle Instant Client](https://www.oracle.com/database/technologies/instant-client.html)
19 or later (Basic or Basic Light; tested with 19.32) the first time it talks to
Oracle: put its directory in `LD_LIBRARY_PATH` (Linux) or `PATH` (Windows)
**before** starting DuckDB or Python. Without it, the extension still loads and
reports what is missing. On Linux, Instant Client needs `libaio` and `libnsl`;
on Ubuntu 24.04, install `libaio1t64` and link `libaio.so.1` to `libaio.so.1t64`.

### Online (community)

Install the signed extension from the DuckDB community repository. No
`-unsigned` flag or `allow_unsigned_extensions` setting is needed.

```sql
INSTALL oraduck FROM community;
LOAD oraduck;
```

In Python:

```python
import duckdb

con = duckdb.connect()
con.execute("INSTALL oraduck FROM community")
con.load_extension("oraduck")
```

To replace a previously installed GitHub Pages or release build, use
`FORCE INSTALL oraduck FROM community` in a new session before `LOAD oraduck`.

### Alternative: GitHub Pages

The project's own builds are also available from GitHub Pages. These builds
are unsigned and require the settings shown below.

```sql
-- duckdb -unsigned
-- Python: con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
INSTALL oraduck FROM 'https://hugues31.github.io/oraduck';
LOAD oraduck;
```

### Offline (GitHub release)

The GitHub release files are unsigned, so the offline examples below also
enable unsigned extensions.

1. On a connected machine, download:
   - `oraduck-repository.tar.gz` from the [latest release](https://github.com/hugues31/oraduck/releases/latest)
     (all platforms);
   - the Instant Client 19 Basic zip for the target platform, for example
     `instantclient-basic-linux.x64-19.32.0.0.0dbru.zip`;
   - for the CLI: `duckdb_cli-linux-amd64.zip` (or `-linux-arm64`,
     `-windows-amd64`) from the [DuckDB v1.5.5 release](https://github.com/duckdb/duckdb/releases/tag/v1.5.5);
   - for Python: `pip download duckdb==1.5.5 -d wheels`, on a machine with the
     same OS, architecture and Python version as the target.
2. Copy the files to the target machine and extract them (Linux shown; on
   Windows, extract the same archives and add `instantclient_19_32` to `PATH`):

   ```sh
   mkdir -p /opt/oraduck /opt/oracle
   tar -xzf oraduck-repository.tar.gz -C /opt/oraduck
   unzip instantclient-basic-linux.x64-19.32.0.0.0dbru.zip -d /opt/oracle
   export LD_LIBRARY_PATH=/opt/oracle/instantclient_19_32:$LD_LIBRARY_PATH
   ```

3. Install the extension from the extracted directory. `INSTALL` copies it into
   `~/.duckdb/extensions/v1.5.5/<platform>/` once; later sessions only need
   `LOAD oraduck`.

   **CLI**

   ```sh
   unzip duckdb_cli-linux-amd64.zip
   ./duckdb -unsigned -c "INSTALL oraduck FROM '/opt/oraduck'; LOAD oraduck; SELECT oraduck_oci_version();"
   ```

   **Python**

   ```sh
   pip install --no-index --find-links wheels duckdb==1.5.5
   ```

   ```python
   import duckdb

   con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
   con.execute("INSTALL oraduck FROM '/opt/oraduck'")
   con.load_extension("oraduck")
   print(con.sql("SELECT oraduck_oci_version()").fetchone())
   ```

The extracted directory can also be served by an internal web server:
`INSTALL oraduck FROM 'https://mirror.example.com/oraduck'`.

## Build

Requirements: a C++17 compiler, CMake and Ninja. Building needs no Oracle
software; the tests that talk to OCI need Instant Client
(`./infra/instantclient/install.sh` downloads 19.32 into `.deps/`,
`source env.sh` sets `OCI_HOME` and `LD_LIBRARY_PATH`).

```sh
git clone --recurse-submodules https://github.com/hugues31/oraduck && cd oraduck
GEN=ninja make release
# DuckDB CLI with the extension built in:  build/release/duckdb
# loadable extension:                      build/release/extension/oraduck/oraduck.duckdb_extension
```

The [Build workflow](.github/workflows/build.yml) builds and tests the three
platforms for code changes on `main` and in pull requests; a `v*` tag publishes
the extension repository (GitHub Pages) and the release.

## Usage

### Secret

| Key        | Required | Description |
|------------|----------|-------------|
| `USER`     | yes      | Oracle user |
| `PASSWORD` | yes      | Password (redacted in `duckdb_secrets()`) |
| `DSN`      | yes      | EZConnect string `host:port/service` |

### Target

The COPY target is an Oracle table name: `'TABLE'` (current schema) or
`'SCHEMA.TABLE'`. Unquoted names are upper-cased; double-quoted names are kept
as is (`'"MySchema"."MyTable"'`). Query columns are matched to table columns by
name (exact match first, then case-insensitive).

### COPY options

| Option | Default | Purpose |
|---|---|---|
| `CONNECTION` | required | name of an `oraduck` secret |
| `STREAM_SIZE` | 4 MiB | direct path stream buffer per session (65536 to 268435456 bytes) |
| `SKIP_INDEX_MAINTENANCE` | false | load a table that has indexes and leave them `UNUSABLE`, like `sqlldr skip_index_maintenance=true` |

The number of Oracle sessions follows `SET threads = N`.

### Tables with indexes

Parallel direct path loads cannot maintain indexes (ORA-26002, as with
`sqlldr parallel=true`): an indexed table is refused unless
`SKIP_INDEX_MAINTENANCE true` is set. The load then runs at full speed and
leaves every index of the table `UNUSABLE`; rebuild them afterwards:

```sql
COPY (SELECT ...) TO 'MY_SCHEMA.MY_TABLE'
(FORMAT oraduck, CONNECTION 'oracle_test', SKIP_INDEX_MAINTENANCE true);
-- in Oracle:
ALTER INDEX MY_SCHEMA.MY_TABLE_IX REBUILD PARALLEL 8;
```

Until it is rebuilt, an unusable index is ignored by queries, but a unique
index (or a primary key) blocks inserts into the table (ORA-01502). If the
loaded rows contain duplicate keys, rebuilding a unique index fails
(ORA-01452): the rows are loaded and must be fixed first.

### Types

| DuckDB | Oracle |
|---|---|
| integers, BOOLEAN (0/1), DECIMAL | NUMBER |
| FLOAT, DOUBLE | BINARY_FLOAT, BINARY_DOUBLE, NUMBER (NaN/infinity rejected for NUMBER) |
| VARCHAR | VARCHAR2, CHAR |
| DATE, TIMESTAMP | DATE (seconds truncated), TIMESTAMP |

Any other type is rejected at bind time: convert it with `CAST`.

## Behavior and limitations

- The target table must exist; OraDuck does not run DDL. A table with indexes
  needs `SKIP_INDEX_MAINTENANCE true` (see above).
- Oracle columns missing from the query receive NULL; a missing `NOT NULL`
  column is an error.
- Nothing is visible before the end of the COPY and any error aborts every
  session. The one exception: if the final commit fails for some sessions after
  others committed, the error reports a **partial** load.
- One invalid row fails the whole COPY (there is no reject file).
- Empty strings become NULL (Oracle semantics); dates are limited to
  0001-01-01 … 9999-12-31.
- If a local file in the current directory is named like the target, the COPY
  is refused (DuckDB would replace it with a temporary file). Do not create such
  a file while a load is running: DuckDB deletes it if the COPY fails.
- `PER_THREAD_OUTPUT` and `PARTITION_BY` are refused, but DuckDB creates an
  empty directory named after the target before OraDuck can refuse them.
- A view passes the bind checks and fails on the first rows sent (ORA-39826);
  nothing is loaded.

## Tests

```sh
./infra/instantclient/install.sh && source env.sh
make unit          # encoders, name parsing, OCI loader (no Oracle)
make itest         # OCI layer and direct path loader (Oracle required)
make test          # sqllogictest (Instant Client tests skipped without OCI_HOME)
uv run pytest      # end-to-end and benchmark harness tests (Oracle required)
```

The Oracle-dependent tests use the benchmark container (see below) through the
`ORADUCK_ORACLE_*` variables set by `env.sh`.

## Benchmark

The benchmark compares OraDuck with "DuckDB → N CSV files → N concurrent
`sqlldr` direct path loads" (`multithreading=true direct=true parallel=true
bindsize=134217728 readsize=134217728 streamsize=1000000 columnarrayrows=1000`),
and with the quack-oracle and duckdb-oracle extensions. Only the export is
timed. Protocol and environment: [`docs/design.md`](docs/design.md) sections 8
and 9; results: [`bench/REPORT.md`](bench/REPORT.md).

To reproduce:

1. Accept the license of `database/enterprise` on
   [container-registry.oracle.com](https://container-registry.oracle.com) and run
   `docker login container-registry.oracle.com` (user: your Oracle account e-mail,
   password: an auth token from the registry profile).
2. `./infra/oracle/start.sh` creates the Oracle 19c container (20 to 45 minutes
   the first time); `./infra/oracle/check.sh` prints its configuration.
3. `./infra/fakelake/install.sh` builds [Fakelake](https://github.com/soma-smart/Fakelake).
   For the comparison: `INSTALL oracle_scanner FROM community` in the DuckDB CLI,
   and build [duckdb-oracle](https://github.com/rinie/duckdb-oracle) against
   DuckDB v1.5.5 into `.deps/competitors/rinie/oracle.duckdb_extension`.
4. Run the campaigns pinned to the client CPUs:

   ```sh
   source env.sh
   taskset -c 0-7 uv run oraduck-bench tune      # STREAM_SIZE
   taskset -c 0-7 uv run oraduck-bench scaling   # N = 1, 2, 4, 8
   taskset -c 0-7 uv run oraduck-bench matrix    # 9 table shapes
   taskset -c 0-7 uv run oraduck-bench competitors  # quack-oracle, duckdb-oracle
   uv run oraduck-bench report                   # bench/REPORT.md
   ```

The scripts assume the benchmark machine's layout (Oracle on CPUs 8-15, client on
CPUs 0-7, 15 GB of RAM); adjust `infra/oracle/start.sh` and
`bench/oraduck_bench/cli.py` for another machine.

## Design notes

- [`docs/design.md`](docs/design.md): architecture, type mapping, transactions,
  benchmark protocol.
- [`docs/probe-results.md`](docs/probe-results.md): OCI measurements behind the
  encoding choices (`tools/probe.cpp`).

## Ideas

- 8,192-row column arrays instead of 2,048: 12 % faster in the probe (5 columns),
  but it requires copying strings across DataChunks.

## License

MIT, see [`LICENSE`](LICENSE).
