# OraDuck

A DuckDB extension that reads Oracle tables (`ATTACH` and `oraduck_scan`, in
parallel, with filter pushdown) and loads the result of a query into an Oracle
table through the OCI Direct Path API — the engine behind `sqlldr direct=true` —
in parallel (one Oracle session per DuckDB thread) and without any intermediate
file.

```sql
INSTALL oraduck FROM community;
LOAD oraduck;
CREATE SECRET oracle_test (TYPE oraduck, USER 'bench', PASSWORD '...', DSN 'dbhost:1521/ORCLPDB1');
-- read
ATTACH '' AS ora (TYPE oraduck, SECRET oracle_test);
SELECT * FROM ora.MY_SCHEMA.MY_TABLE WHERE operation_date = DATE '2024-05-07';
-- write
COPY (SELECT id, amount, operation_date, label FROM source)
TO 'MY_SCHEMA.MY_TABLE' (FORMAT ORADUCK, CONNECTION 'oracle_test');
```

The reader is described in [Read from Oracle](#read-from-oracle).

Write benchmark: on Oracle 19c, over 9 table shapes from 100k × 5 to 10M × 50, OraDuck is:

- 1.2 to 3.2 times faster than exporting the table to CSV files and loading
  them with parallel direct path `sqlldr` (median of 5 runs);
- 6 to 48 times faster than [quack-oracle](https://github.com/krokozyab/quack-oracle)
  and 240 to 500 times faster than [duckdb-oracle](https://github.com/rinie/duckdb-oracle),
  the other DuckDB extensions that write to Oracle (from an in-memory table,
  median of 3 runs; duckdb-oracle takes more than 15 minutes from 1M rows).

See [`bench/REPORT.md`](bench/REPORT.md).

## Install

Prebuilt for **DuckDB v2.0.0** on Linux x86_64 and arm64 (glibc 2.28 or later)
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
   - for the CLI: `duckdb-cli-linux-amd64.tar.gz` (or `-linux-arm64`,
     `-windows-amd64`) from the [DuckDB v2.0.0 release](https://github.com/duckdb/duckdb/releases/tag/v2.0.0);
   - for Python: `pip download duckdb==2.0.0 -d wheels`, on a machine with the
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
   `~/.duckdb/extensions/v2.0.0/<platform>/` once; later sessions only need
   `LOAD oraduck`.

   **CLI**

   ```sh
   tar -xzf duckdb-cli-linux-amd64.tar.gz
   ./duckdb -unsigned -c "INSTALL oraduck FROM '/opt/oraduck'; LOAD oraduck; SELECT oraduck_oci_version();"
   ```

   **Python**

   ```sh
   pip install --no-index --find-links wheels duckdb==2.0.0
   ```

   ```python
   import duckdb

   con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
   con.execute("INSTALL oraduck FROM '/opt/oraduck'")
   con.load_extension("oraduck")
   print(con.execute("SELECT oraduck_oci_version()").fetchone())
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

## Read from Oracle

OraDuck reads Oracle tables, views and synonyms through a read-only `ATTACH`.
It uses the same `oraduck` secret as the writer (see [Secret](#secret)).

```sql
CREATE SECRET oracle_prod (TYPE oraduck, USER '...', PASSWORD '...', DSN 'dbhost:1521/SERVICE');
ATTACH '' AS ora (TYPE oraduck, SECRET oracle_prod);
SELECT * FROM ora.MY_SCHEMA.TOTO WHERE part_date = DATE '2024-05-07';
```

### ATTACH

| Option | Default | Purpose |
|---|---|---|
| `SECRET` | required | name of an `oraduck` secret |
| `SESSIONS` | `SET threads` | Oracle sessions per scan (1 to 1024) |
| `SPLIT` | `auto` | how a scan is divided between sessions: `auto`, `extents`, `hash`, `none` |
| `SNAPSHOT` | `auto` | read consistency across sessions: `auto`, `scn`, `none` (see [How a read runs](#how-a-read-runs)) |
| `UNCONSTRAINED_NUMBER` | `DOUBLE` | DuckDB type of a `NUMBER` without precision: `DOUBLE`, `VARCHAR`, `DECIMAL(p,s)` |

The options after `SECRET` have the same meaning as the `oraduck_scan`
parameters (below). The path must be empty: the connection comes from the
secret.

- Schemas are Oracle users. The default schema is the connected user's current
  schema (`ora.TOTO`). Names are matched exactly, then upper-cased (Oracle's
  unquoted names): `ora.toto` finds `TOTO`.
- Tables, views and synonyms are readable. Public synonyms are reachable from
  the default schema (`ora.DUAL`), not from another schema.
- `SHOW TABLES FROM ora` lists the connected user's schema, and the other
  schemas once a query has named them: listing never walks every Oracle user.
- A table's description is cached until `DETACH` and a new `ATTACH`: a column
  added in Oracle is not seen before that. A name found missing stays missing
  the same way: a table created in Oracle after a query failed to find it is
  seen after `DETACH` and `ATTACH`.
- The database is read-only. `INSERT`, `UPDATE`, `DELETE`, DDL and the other
  writes fail with `OraDuck: attached Oracle databases are read-only; load data
  with COPY … TO '<SCHEMA.TABLE>' (FORMAT ORADUCK, CONNECTION '<secret>')`.
  `ANALYZE` and `VACUUM` on an attached table are not supported (they scan the
  table before failing).

### oraduck_scan

```sql
SELECT * FROM oraduck_scan('MY_SCHEMA.TOTO', connection := 'oracle_prod');
SELECT * FROM oraduck_scan('TOTO', connection := 'oracle_prod',
                           sessions := 8, split := 'extents', snapshot := 'scn',
                           unconstrained_number := 'DECIMAL(20,4)');
```

`oraduck_scan('<[SCHEMA.]TABLE>', connection := '<secret>' [, sessions := n]
[, split := 'auto'|'extents'|'hash'|'none'] [, snapshot := 'auto'|'scn'|'none']
[, unconstrained_number := 'DOUBLE'|'VARCHAR'|'DECIMAL(p,s)'])`

`connection` is required. The name is read like the COPY target (unquoted
names are upper-cased, double-quoted names are kept as is).

Wrapped in a view, the function keeps the filter pushdown:

```sql
CREATE VIEW toto AS FROM oraduck_scan('MY_SCHEMA.TOTO', connection := 'oracle_prod');
SELECT count(*) FROM toto WHERE part_date = DATE '2024-05-07';
```

### How a read runs

**Projection.** Only the selected columns are fetched (`count(*)` fetches
none).

**Filters.** The filters of the `WHERE` clause that Oracle evaluates exactly as
DuckDB would go to the Oracle `WHERE` clause, which lets Oracle prune
partitions and use its indexes. Pushed: comparisons (`=`, `<>`, `<`, `<=`, `>`,
`>=`), `BETWEEN`, `IN` (up to 1,000 values), `IS [NOT] NULL`, `AND`/`OR`
combinations of these, and `CAST(ts AS DATE) = DATE '...'` on a `DATE` column.
Everything else stays in DuckDB and is applied to the rows received.

- A filter is pushed only when it compares the bare column with constants of
  the column's DuckDB type. `dec IN (0, 0.25, -250)` against a `DECIMAL(10,2)`
  column stays in DuckDB: because of `-250`, DuckDB casts the column (here to
  `DECIMAL(12,2)`), so the filter is no longer on the bare column; `dec IN
  (0.00, 0.25, -250.00)` is pushed. Likewise, a `VARCHAR2` partition key
  compared with a number (`key = 20240507`) is not pushed and the scan reads
  every partition: write `key = '20240507'`.
- String comparisons are pushed only for non-empty constants and a column
  without column-level collation; string ranges (`<`, `<=`, `>`, `>=`,
  `BETWEEN`) only on AL32UTF8 databases, for non-national columns. On another
  character set only ASCII constants are pushed.
- Stays in DuckDB: `LIKE`, `NOT`, functions on a column, empty strings
  (`''` is NULL in Oracle), comparisons on unconstrained `NUMBER` columns (read
  as `DOUBLE` by default) and on LOB columns (`IS [NOT] NULL` on them is
  pushed), time zone types, `TIMESTAMP(n)` with n above 6.

`EXPLAIN` shows what was pushed (`Oracle WHERE`) and the requested options:

```sql
EXPLAIN SELECT * FROM ora.MY_SCHEMA.TOTO WHERE part_date = DATE '2024-05-07';
-- Table: MY_SCHEMA.TOTO
-- Oracle WHERE: ("PART_DATE" = DATE '2024-05-07')
-- Split: auto
-- Snapshot: auto
```

**Parallel sessions.** A scan opens up to `SESSIONS` Oracle sessions (default:
`SET threads = N`), never more than it has work units. The units come from the
split strategy:

| `SPLIT` | Units | Needs |
|---|---|---|
| `auto` | `extents` for a table, `hash` for a view or when `extents` is not possible | |
| `extents` | ranges of ROWIDs, from the table's extents | a heap table (not a view; not index-organized, clustered, external or hybrid partitioned) |
| `hash` | `ORA_HASH(ROWID, n-1) = k`: one full scan per session | a source that has a ROWID (not a join or aggregate view) |
| `none` | one session, one query | |

`extents` reads `DBA_EXTENTS` when the user can. Otherwise it creates a
temporary `DBMS_PARALLEL_EXECUTE` task, reads its chunks and drops it:
OraDuck then writes a task row in Oracle's dictionary for a moment (the user
needs access to `DBMS_PARALLEL_EXECUTE`). With `auto`, an `extents`
failure falls back to `hash`, and `hash` to one session, with a note; with an
explicit `SPLIT`, it is an error.

**Consistency.** Each work unit is one Oracle statement, consistent by itself.
With `SNAPSHOT 'auto'` (the default), every unit reads `AS OF SCN` of one SCN,
read at planning time, when the user has `EXECUTE ON DBMS_FLASHBACK` or
`SELECT ON V_$DATABASE`: the sessions see the same data even when Oracle is
being written. Without that privilege, or when a flashback query is refused,
each session reads at its own moment, and the scan says so in its `Snapshot`
and `Notes` in `EXPLAIN ANALYZE` and as a warning in the DuckDB log.
`snapshot := 'scn'` turns that situation into an error; `'none'` never uses an
SCN. Flashback query needs `FLASHBACK` on another user's table. A scan of one
unit (one session, `SPLIT 'none'`, a small table) never uses an SCN.

Without an SCN the units run at different moments: while Oracle is being
written, a change committed during the scan is seen by the units that start
after it and not by the others, and with row movement enabled a row that moves
during the scan (partition key update, shrink) can be missed or read twice.

The snapshot has a fixed cost per scan, not per row: Oracle never shares a
flashback (`AS OF`) cursor between parse calls, so each session hard-parses
the unit statement, and the sessions parse in turn: about 13 ms per session
for 550 columns (0.12 s for 8 sessions, more with more sessions). It is within
noise on a 1M-row partition; on a tiny read (a few rows over many partitions)
it can be most of the time, and `snapshot := 'none'` avoids it
([`docs/probe-results.md`](docs/probe-results.md), R8).

`EXPLAIN ANALYZE` adds what the run did:

| Key | Content |
|---|---|
| `Strategy` | the split strategy used |
| `Units` | number of work units |
| `Sessions` | Oracle sessions used |
| `Snapshot` | the SCN, `per session`, or `single statement` (one unit) |
| `Notes` | why a fallback happened (`—` when none) |
| `Planning (ms)` | time spent planning (snapshot, extents) |
| `Oracle SQL` | the statement of the first unit, binds inlined |

### Oracle privileges

For the reading user:

| Privilege | Effect |
|---|---|
| `SELECT` on the tables | required |
| `SELECT ON SYS.DBA_EXTENTS` | best splitting (without it, `DBMS_PARALLEL_EXECUTE` is tried) |
| `EXECUTE ON SYS.DBMS_FLASHBACK` | consistent parallel reads (`SELECT ON V_$DATABASE` also works) |

### Read types

| Oracle | DuckDB |
|---|---|
| `NUMBER(p,0)`, p ≤ 18 | `SMALLINT` (p ≤ 4), `INTEGER` (p ≤ 9), `BIGINT` |
| other `NUMBER(p,s)` | `DECIMAL(p,s)`; `DECIMAL(s,s)` when s > p |
| `NUMBER(*,0)` | `DECIMAL(38,0)` |
| `NUMBER(p,s)`, negative scale | exact `DECIMAL(p-s,0)` |
| `p-s` > 38 or `s` > 38 | `DOUBLE` (digits may be lost) |
| `NUMBER` without precision, `FLOAT`, `FLOAT(b)` (any precision) | `unconstrained_number`: `DOUBLE` by default (digits beyond about 15 are lost), `VARCHAR`, or `DECIMAL(p,s)` |
| `BINARY_FLOAT`, `BINARY_DOUBLE` | `FLOAT`, `DOUBLE` |
| `DATE` | `TIMESTAMP` |
| `TIMESTAMP(n)` | `TIMESTAMP` (nanoseconds truncated to microseconds) |
| `TIMESTAMP WITH [LOCAL] TIME ZONE` | `TIMESTAMPTZ` |
| `VARCHAR2`, `CHAR`, `NVARCHAR2`, `NCHAR`, `CLOB`, `NCLOB` | `VARCHAR` (UTF-8, validated; `CHAR` padding is kept; `EMPTY_CLOB()` reads as `''`) |
| `RAW`, `BLOB` | `BLOB` |
| `ROWID`, `UROWID` | `VARCHAR` |
| `LONG`, `LONG RAW`, `XMLTYPE`, `JSON`, `VECTOR`, `BFILE`, `INTERVAL`, object types | error, only when the column is selected |

An unsupported column is part of the table: `SELECT other_columns` works, and
`SELECT *` fails with `column "X" has Oracle type XMLTYPE, which oraduck_scan
does not read`.

### Network

On a production link, the network is the bottleneck. SQL\*Net compression and
the session data unit (SDU) go in the DSN as a connect descriptor:

```sql
CREATE SECRET oracle_prod (TYPE oraduck, USER '...', PASSWORD '...',
  DSN '(DESCRIPTION=(COMPRESSION=on)(SDU=2097152)(ADDRESS=(PROTOCOL=TCP)(HOST=dbhost)(PORT=1521))(CONNECT_DATA=(SERVICE_NAME=SERVICE)))');
```

SQL\*Net compression requires the Oracle Advanced Compression option licence.

### Read limits

- Ctrl-C and `max_execution_time` are honoured between two fetches: a single
  slow fetch (a heavy view) finishes first.
- Temporary LOBs created by views that compute CLOBs are released at the end
  of the query.
- Every scan has its own snapshot: two scans of the same table in one query
  (a self join, the table named twice) read at two moments, even with
  `SNAPSHOT 'auto'` and every privilege (each reads its own SCN). A CTE
  referenced twice is materialized and reads Oracle once.
- Each Oracle session holds up to 64 MiB of fetch buffers, plus the LOB values
  of its current batch (kept up to twice that size): this memory is outside
  DuckDB's `memory_limit`.
- Rows come in no particular order: units are read in parallel.
- Each DuckDB release needs a matching OraDuck build: C++ extensions are tied
  to the exact DuckDB version, and the stable C API cannot push filters.

### Read benchmark

<!-- READ BENCHMARK SUMMARY: filled after the campaign (bench/REPORT_READ.md) -->

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
   DuckDB v2.0.0 (the version of `build/release/duckdb`) into
   `.deps/competitors/rinie/oracle.duckdb_extension`.
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
