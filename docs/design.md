# OraDuck — DuckDB extension for direct path loads into Oracle

- **Date**: 2026-09-23
- **Status**: implemented (v1)
- **Author**: Hugues Gaillard

## 1. Context and goal

Move the result of a DuckDB query into an existing Oracle table as fast as
possible, with no intermediate CSV and no Python.

The usual method exports the DuckDB table to CSV files on disk and loads them
with SQL\*Loader in direct path mode. OraDuck replaces that pipeline with a
DuckDB extension that feeds the OCI Direct Path API (the engine behind
`sqlldr direct=true`) straight from DuckDB vectors, in parallel.

The project ships:

1. the `oraduck` DuckDB extension (C++);
2. a reproducible benchmark setup (Oracle 19c in Docker, Fakelake data);
3. a report comparing OraDuck with the CSV + SQL\*Loader method.

## 2. Scope

### In scope (v1)

- `COPY (<query>) TO '<SCHEMA.TABLE>' (FORMAT ORADUCK, CONNECTION '<secret>')`.
- An `oraduck` DuckDB secret type holding the Oracle credentials.
- Parallel load: one OCI Direct Path session per DuckDB thread.
- The scalar types listed in section 5.
- DuckDB pinned to a stable release, Oracle Instant Client 19 or later. Prebuilt for Linux
  x86_64 and arm64 and Windows x86_64 (CI: build and tests without Oracle);
  tested against Oracle and benchmarked on Linux x86_64 only.

### Out of scope (v1)

- Reading from Oracle (scanner), `ATTACH`, `INSERT INTO` an Oracle database.
- Creating or altering tables (DDL): the target table must exist.
- UPDATE / MERGE / upsert.
- LOBs (CLOB, BLOB), NVARCHAR2 / NCHAR, nested types (LIST, STRUCT, MAP),
  TIMESTAMP WITH TIME ZONE, INTERVAL, UUID, HUGEINT / UHUGEINT.
- Target tables with indexes (see 6.4).
- A reject file like sqlldr's `badfile`: one invalid row fails the whole COPY.
- Signed distribution (community extensions), macOS (no Instant Client 19 for
  Apple Silicon).
- Benchmarking in ARCHIVELOG mode.

## 3. Usage

```sql
SET allow_unsigned_extensions = true;   -- or duckdb -unsigned
LOAD 'oraduck';

CREATE SECRET oracle_test (
    TYPE oraduck,
    USER 'bench',
    PASSWORD '...',
    DSN 'dbhost:1521/ORCLPDB1'           -- EZConnect host:port/service
);

COPY (
    SELECT id, amount, operation_date, label
    FROM source
)
TO 'MY_SCHEMA.MY_TABLE'
(
    FORMAT ORADUCK,
    CONNECTION 'oracle_test'
);
```

Like any DuckDB `COPY`, it returns the number of rows loaded.

### 3.1 `oraduck` secret

| Key        | Required | Description                                   |
|------------|----------|-----------------------------------------------|
| `USER`     | yes      | Oracle user                                   |
| `PASSWORD` | yes      | Password (redacted in `duckdb_secrets()`)     |
| `DSN`      | yes      | EZConnect string `host:port/service`          |

Secrets can be temporary or `PERSISTENT` (standard DuckDB mechanism).

### 3.2 Target

The COPY "file path" is read as an Oracle table name:

- `'TABLE'`: a table in the connected user's schema;
- `'SCHEMA.TABLE'`: a table in another schema;
- unquoted identifiers are upper-cased (Oracle semantics); double-quoted
  identifiers are kept as is (`'"MySchema"."MyTable"'`).

### 3.3 COPY options

| Option        | Default          | Purpose |
|---------------|------------------|---------|
| `CONNECTION`  | — (required)     | Name of an `oraduck` secret |
| `STREAM_SIZE` | 4 MiB            | Size in bytes of the direct path stream buffer per session (64 KiB to 256 MiB) |
| `SKIP_INDEX_MAINTENANCE` | false  | Accept an indexed table and leave its indexes `UNUSABLE` (6.4) |

The OCI column array holds 2,048 rows (the DuckDB vector size): one DuckDB
DataChunk fills it exactly, without copying strings. The probe (section 11)
measured 8,192 rows 12 % faster on a 5-column table; a `COLUMN_ARRAY_ROWS`
option is left for a later version.

The number of Oracle sessions equals the number of DuckDB threads that receive
data (`SET threads = N`).

DuckDB's generic file options (`PER_THREAD_OUTPUT`, `PARTITION_BY`,
`FILE_SIZE_BYTES`, `USE_TMP_FILE`) are rejected with `FORMAT ORADUCK`. If a
local file in the current directory has the same name as the target, the COPY
is rejected: DuckDB would otherwise replace it with a temporary file.

## 4. Extension architecture

### 4.1 Components

| Component           | Responsibility |
|---------------------|----------------|
| `OraduckExtension`  | Entry point: registers the secret type and the `oraduck` copy function. |
| Secret type         | Declares the `oraduck` type, validates its keys, redacts the password. |
| `OciApi`            | The OCI functions OraDuck calls, loaded at run time from Instant Client (`dlopen` of `libclntsh.so`, `LoadLibrary` of `oci.dll`) on first use, with a clear error when it is missing. OraDuck declares the few OCI types, constants and signatures it needs: building needs no Oracle SDK. |
| `CreateOciEnv`      | Creates an OCI environment (`OCIEnvNlsCreate`, threaded mode, AL32UTF8 charset id 873). **One environment per session**: concurrent handle allocations on a shared environment corrupt its heap (ORA-21500 KGHALO4, SIGSEGV), even with `OCI_THREADED`. |
| `OciSession`        | OCI connection and session (environment, error handle, service context). RAII. |
| `DescribeTarget`    | Resolves `SCHEMA.TABLE` and describes its columns (name, type, size, precision, scale, nullability, index count). |
| `ColumnPlan`        | For each query column: the matching Oracle column, the chosen OCI external type, the encoder. Built at bind time, immutable afterwards. |
| Encoders            | Convert a DuckDB vector into OCI column array entries (see 4.3). Pure functions, testable without Oracle. |
| `DirectPathLoader`  | Direct path context of one session: prepare, column array, stream, `ColArrayToStream` / `LoadStream` loop, finish, abort. RAII. |
| Copy function       | DuckDB copy function callbacks (bind, global init, local init, sink, combine, finalize). |
| `OracleError`       | Wraps `OCIErrorGet` output: ORA code and text, plus the column and row when available. |

### 4.2 How a COPY runs

1. **Bind** (single thread)
   - reads the `CONNECTION` option and fetches the secret; error if missing;
   - opens a session and describes the target table;
   - maps query columns to Oracle columns **by name**: exact match first,
     otherwise case-insensitively; error if a query column does not exist in
     the table, if the match is ambiguous, or if two query columns target the
     same Oracle column;
   - checks every type (section 5) and builds the `ColumnPlan`;
   - error if the table has an index (6.4) or a `NOT NULL` column that the
     query does not provide;
   - schema errors are raised here, before any data is sent.
2. **Global init**: shared state (loaders waiting for commit, mutex).
3. **Local init** (per thread): empty local state. The session and the direct
   path context are opened **lazily** on the thread's first `sink`, so small
   tables do not open useless sessions.
4. **Sink** (per thread, per DataChunk of at most 2,048 rows)
   - fills the OCI column array from the vectors (encoders);
   - calls `OCIDirPathColArrayToStream`; when the stream is full
     (`OCI_CONTINUE`), calls `OCIDirPathLoadStream`, resets the stream and
     resumes at the first unconverted row (`OCI_ATTR_ROW_COUNT` is relative
     to the last call, as documented by Oracle);
   - `OCI_NEED_DATA` from `OCIDirPathLoadStream` means the stream ended with a
     partial row (a row wider than the buffer); its remainder goes into the
     next stream;
   - the stream copies the data: the DataChunk can be released when the sink
     returns.
5. **Combine** (per thread): sends the pending stream, then hands the loader
   over to the global state **without committing**.
6. **Finalize** (single thread): calls `OCIDirPathFinish` on every loader, in
   parallel threads (each commits its session), then closes the sessions.

The copy function declares the parallel execution mode
(`PARALLEL_COPY_TO_FILE`): row order is not preserved, which does not matter
for a heap table.

### 4.3 Encoders

Principle: avoid any client-side text conversion when OCI accepts a native
format, and never copy strings.

| DuckDB data | Representation passed to OCI |
|-------------|------------------------------|
| VARCHAR | pointer + length into the `string_t` data (zero copy), `SQLT_CHR` |
| integers, BOOLEAN | native value passed as is (`SQLT_INT` / `SQLT_UIN`, zero copy) |
| DECIMAL | Oracle internal NUMBER format (`SQLT_NUM`), encoded by OraDuck into a local buffer |
| DOUBLE / FLOAT to BINARY_DOUBLE / BINARY_FLOAT | native IEEE (`SQLT_BDOUBLE` / `SQLT_BFLOAT`) |
| DOUBLE / FLOAT to NUMBER or to the other binary type | floating-point external type (`SQLT_FLT`), converted by OCI; NaN and infinities are rejected for NUMBER |
| DATE / TIMESTAMP to DATE | 7-byte internal DATE (`SQLT_DAT`), seconds truncated |
| DATE / TIMESTAMP to TIMESTAMP(p ≥ 6) | internal TIMESTAMP (type 180), in Oracle's canonical form: 7 bytes without fractional seconds, 11 bytes otherwise. 37 % faster than text (probe). The canonical form matters: 11 bytes with zero nanoseconds do not compare equal to SQL literals. |
| DATE / TIMESTAMP to TIMESTAMP(p < 6) | text `YYYY-MM-DD HH24:MI:SS.FF6` with `OCI_ATTR_DATEFORMAT`, so that Oracle rounds to the column precision |
| NULL | `OCI_DIRPATH_COL_NULL` flag from the validity mask |

Encoding buffers (NUMBER ≤ 22 bytes, DATE 7, TIMESTAMP 11) are allocated once
per thread and reused. Several column arrays are converted into the same stream
before it is sent (probe: 16 % faster). Table, schema and column names are
passed to OCI double-quoted, otherwise OCI upper-cases them (ORA-39826).

Zero-copy pointers target the DuckDB vector memory, including short strings
(≤ 12 bytes) stored inside the `string_t` itself: they stay valid until the
conversion into the stream, which happens within the same sink.

## 5. Type mapping

| DuckDB type | Accepted Oracle target types | Notes |
|-------------|------------------------------|-------|
| BOOLEAN | NUMBER | `true` → 1, `false` → 0 |
| TINYINT … BIGINT, UTINYINT … UBIGINT | NUMBER | a value outside the column precision/scale → Oracle error |
| DECIMAL(p, s) (all widths) | NUMBER | |
| FLOAT, DOUBLE | BINARY_FLOAT, BINARY_DOUBLE, NUMBER | NaN/±infinity → error for NUMBER |
| VARCHAR | VARCHAR2, CHAR | UTF-8 passed as is (AL32UTF8 client); too long → ORA-12899; `''` becomes NULL (Oracle semantics) |
| DATE | DATE, TIMESTAMP | outside 0001-01-01 … 9999-12-31 (including ±infinity) → error |
| TIMESTAMP | TIMESTAMP, DATE | to DATE: truncated to the second |
| other | — | explicit error at bind time |

Oracle columns missing from the query receive NULL; a missing `NOT NULL`
column is an error at bind time.

## 6. Transactions and errors

### 6.1 Atomicity

- No data is visible before `finalize`.
- Any error before `finalize` (conversion, `LoadStream`, user interrupt)
  triggers `OCIDirPathAbort` on every open session through RAII destructors:
  the table is left unchanged.
- The only non-atomic window: when an `OCIDirPathFinish` fails after other
  sessions have committed. OraDuck then reports a **partial load** error.

### 6.2 Error messages

Every OCI error is raised as a DuckDB exception carrying the ORA code and text,
the OCI operation, and, when available, the column and the row within the
batch.

### 6.3 Interrupts

A DuckDB interrupt (Ctrl-C) aborts the sessions as in 6.1.

### 6.4 Indexes

Parallel direct path loads do not maintain indexes (ORA-26002, the same
restriction as `sqlldr parallel=true`). By default OraDuck rejects an indexed
target table at bind time, with a message that names the option below.

`SKIP_INDEX_MAINTENANCE true` is the equivalent of `sqlldr skip_index_maintenance=true`:
every session sets `OCI_ATTR_DIRPATH_SKIPINDEX_METHOD` to
`OCI_DIRPATH_INDEX_MAINT_SKIP_ALL`, the parallel load proceeds and Oracle marks
the table's indexes `UNUSABLE`; the user rebuilds them (`ALTER INDEX … REBUILD`).
Measured on Oracle 19c, identical for sqlldr and OCI: the rows are loaded, the
indexes are `UNUSABLE`, a rebuild makes them `VALID`; with duplicate keys, the
rebuild of a unique index fails (ORA-01452) and the rows stay loaded.

A serial direct path load (`OCI_ATTR_DIRPATH_PARALLEL = FALSE`, like
`sqlldr direct=true` without `parallel`) maintains the indexes itself; it is
not offered: it gives up the parallel sessions, and a unique index still ends
`UNUSABLE` without any error when the rows contain duplicate keys.

## 7. Build and dependencies

- Based on the official `duckdb/extension-template` (CMake); DuckDB is a
  submodule pinned to v1.5.5. DuckDB forces C++11 through the CMake cache; the
  extension targets are set to C++17.
- The benchmark harness and the end-to-end tests drive the **DuckDB CLI built
  with the extension** (`build/release/duckdb`). The Python `duckdb` package is
  not used: loading a locally built C++ extension into a Python wheel risks C++
  ABI mismatches. The extension built by the CI (manylinux toolchain, like
  DuckDB's) loads in the Python package (checked with duckdb 1.5.5 from PyPI).
- OCI is loaded at run time (`OciApi`): the build needs no Oracle software, and
  the extension loads without Instant Client; the first function that talks to
  Oracle loads `libclntsh.so` (Linux: `LD_LIBRARY_PATH`), `oci.dll` (Windows:
  `PATH`) or `libclntsh.dylib`, Instant Client 19 or later.
- Oracle Instant Client 19.32 for the tests and the benchmark: Basic, Tools
  (`sqlldr`), downloaded from oracle.com into `.deps/` (ignored by git).
- Run-time dependencies of Instant Client on Linux: `libaio`, `libnsl`.
- Distribution (`.github/workflows/build.yml`): Linux builds run in the
  `manylinux_2_28` containers (glibc 2.28, like DuckDB's own builds), Windows
  builds use MSVC. The CI runs the unit tests and the sqllogictest, first
  without Instant Client (as DuckDB's community CI does), then with it; none
  needs an Oracle database. A `v*` tag publishes a DuckDB
  extension repository on GitHub Pages
  (`v1.5.5/<platform>/oraduck.duckdb_extension.gz`) and a release holding the
  same tree as `oraduck-repository.tar.gz`, for offline installs.

## 8. Benchmark infrastructure

### 8.1 Oracle

- Official image `container-registry.oracle.com/database/enterprise:19.3.0.0`
  (OTN developer license, free for development and testing). Manual
  prerequisite: accept the license on the registry and
  `docker login container-registry.oracle.com`.
- The intended target is 19c **19.30** (a Release Update). Its images
  (`database/enterprise_ru`) and patches require an Oracle support contract, so
  the benchmark runs on 19.3.0.0 and the report says so. The OCI Direct Path API
  and SQL\*Loader behave the same across Release Updates, but the numbers are
  not guaranteed to match on 19.30.
- Dedicated container `oraduck-ora19` on a dedicated Docker network with a
  fixed IP (`10.210.19.10:1521`), reached directly from the host: no port
  publishing (the host kernel on the benchmark machine could not load the DNAT
  module).
- PDB `ORCLPDB1`, user `BENCH`, tablespace `BENCH` pre-sized for the largest
  case (no autoextend during measurements).
- SGA 3 GB, PGA 1 GB, container memory limit 6 GB; the SGA is locked in RAM
  (`LOCK_SGA = TRUE`, `--ulimit memlock=-1`), otherwise the host (zram,
  `swappiness` 150) swaps it out.
- `cpu_count = 8` set explicitly: Oracle otherwise counts the host's 16 CPUs
  despite the cpuset.
- Database files in `.deps/oradata`, a btrfs directory without copy-on-write
  (`chattr +C`): with copy-on-write and zstd compression, redo log `fsync`
  (`log file sync`) varied from 0.1 s to 4.6 s per load.
- **NOARCHIVELOG** mode, tables in LOGGING (default).
- Setup scripts live in `infra/`.

### 8.2 CPU isolation

Machine: Intel i5-1340P, CPUs 0-7 = 4 P-cores with HT, CPUs 8-15 = 8 E-cores.

- Oracle container: `--cpuset-cpus=8-15`.
- Client (harness, DuckDB, sqlldr): run under `taskset -c 0-7`; child
  processes inherit the affinity.

### 8.3 Data

- Generated by Fakelake from YAML files written by the harness, with a fixed
  seed, as Parquet, then loaded into a persistent `bench.duckdb` file with
  explicit casts to the target types. Not timed.
- Matrix: rows ∈ {100,000; 1,000,000; 10,000,000} × columns ∈ {5; 20; 50}.
- The first 5 columns are always:

  | Column | Fakelake | DuckDB | Oracle |
  |--------|----------|--------|--------|
  | `id` | `Increment.integer` | INTEGER | NUMBER(10) |
  | `amount` | `Random.Number.f64` (0 … 100,000) | DECIMAL(15,2) | NUMBER(15,2) |
  | `label` | `Random.String.alphanumeric` 5..30 | VARCHAR | VARCHAR2(30) |
  | `operation_date` | `Random.Date.date` | DATE | DATE |
  | `event_time` | `Random.Date.datetime` | TIMESTAMP | TIMESTAMP(6) |

- Further columns cycle through 10 types:

  | # | Fakelake | DuckDB | Oracle |
  |---|----------|--------|--------|
  | 1 | `Random.Number.i32` | INTEGER | NUMBER(10) |
  | 2 | `Random.Number.f64` | DOUBLE | BINARY_DOUBLE |
  | 3 | `Random.Number.f64` | DECIMAL(15,2) | NUMBER(15,2) |
  | 4 | `Random.String.alphanumeric` 5..30 | VARCHAR | VARCHAR2(30) |
  | 5 | `Person.email` | VARCHAR | VARCHAR2(64) |
  | 6 | `Person.fname` | VARCHAR | VARCHAR2(64) |
  | 7 | `Random.Date.date` | DATE | DATE |
  | 8 | `Random.Date.datetime` | TIMESTAMP | TIMESTAMP(6) |
  | 9 | `Random.bool` | BOOLEAN | NUMBER(1) |
  | 10 | `Constant.string` (weighted list) | VARCHAR | VARCHAR2(16) |

- About 5 % NULLs in every column except `id` (Fakelake `presence` option).
  Fakelake writes datetimes as epoch seconds, so benchmark timestamps have no
  fractional part.
- Identical Oracle tables for both methods, without indexes or constraints.

## 9. Measurement protocol

### 9.1 What is timed

Only the export is timed, from an already loaded DuckDB table to the rows
committed in Oracle.

- **OraDuck**: execution time of
  `COPY (SELECT * FROM t) TO 'BENCH.T' (FORMAT ORADUCK, CONNECTION 'oracle_test')`,
  measured with `.timer on` in the DuckDB CLI built with the extension, with
  `SET threads = N`. The CSV export of the SQL\*Loader method is timed the same
  way.
- **SQL\*Loader (N CSV files + N sqlldr)**: the sum of
  1. `COPY (SELECT …) TO '<dir>' (FORMAT CSV, PER_THREAD_OUTPUT true, …)` with
     `SET threads = N`, which writes up to N files on the NVMe drive (a
     directory under `/home`, never `/tmp`, which is in RAM); the query turns
     BOOLEAN into 0/1 and sets the date and timestamp formats;
  2. N `sqlldr` processes started at once, one per non-empty file, until the
     last one exits, with exactly:
     `multithreading=true direct=true parallel=true bindsize=134217728 readsize=134217728 streamsize=1000000 columnarrayrows=1000`,
     an `APPEND` control file with the date and timestamp masks and explicit
     `CHAR(n)` lengths.
  The report gives the total and the CSV / sqlldr split.

### 9.2 What is not timed

Oracle start-up, Fakelake generation, DuckDB loading, Oracle table creation and
`TRUNCATE`, checks, CSV deletion.

### 9.3 Procedure

- N = 8 (the client cpuset's threads) for both methods over the whole matrix.
- Scaling test: N ∈ {1, 2, 4, 8} on the 1M × 20 case, for both methods.
- Per case: 1 warm-up run (discarded), then 5 measured runs **interleaving**
  the methods (A, B, A, B, …); the median is reported.
- Before every run: `TRUNCATE TABLE … DROP STORAGE`.
- After every run (not timed): row count and checksums (sum of `id`, sum of the
  lengths of `label`, NULL count of `amount`) compared between DuckDB and
  Oracle. Any mismatch invalidates the run and stops the benchmark.
- OraDuck's `STREAM_SIZE` is set once by a preliminary tuning run on 1M × 20,
  then kept for the whole matrix. The SQL\*Loader parameters are those of the
  reference method and are not tuned.

### 9.4 Results

- Raw results as JSON lines per run (times, rows, bytes, parameters, versions,
  timestamps) in `bench/results/`.
- `bench/REPORT.md`: environment, method, tables (median time, rows/s, MB/s,
  OraDuck / SQL\*Loader speedup, min–max spread), PNG charts.
- Results are reported as measured, including cases where SQL\*Loader wins.

### 9.5 Comparison with other DuckDB extensions

`oraduck-bench competitors` compares OraDuck with the two other extensions that
write from DuckDB to Oracle, on the same 9 table shapes and the same Oracle
instance:

- [quack-oracle](https://github.com/krokozyab/quack-oracle), community extension
  `oracle_scanner` (`INSTALL oracle_scanner FROM community`);
- [duckdb-oracle](https://github.com/rinie/duckdb-oracle), built from source
  against DuckDB v1.5.5 and loaded with `-unsigned`.

The source is a DuckDB in-memory table, created before the timer starts by
`CREATE TABLE … AS SELECT` from the attached benchmark database (10M × 50, about
9 GB in memory, reads the attached file instead). The timed statement is
`COPY … TO` for OraDuck (N = 8, and N = 1 since the other two use one session)
and `INSERT INTO <attached Oracle table> SELECT * FROM source` for the others.
One warm-up run, then 3 interleaved runs; median; checksums after every run.
A run is killed after 15 minutes (the whole DuckDB process); the method is then
not run on the cases with at least as many rows and columns. The harness's
sessions set `DDL_LOCK_TIMEOUT` so that the next `TRUNCATE` waits for Oracle to
roll back a killed session.

## 10. Tests

### 10.1 Unit tests (no Oracle)

A separate C++ executable based on Catch2 (shipped in the DuckDB submodule).

- Encoders: NUMBER (0, negatives, ±(2^63 − 1), DECIMAL with various scales,
  38-digit values), DATE (before 1970, leap years, bounds), TIMESTAMP
  (fractional seconds, canonical 7-byte form). Byte-for-byte comparison with
  reference values from Oracle's documented format; the probe (section 11)
  cross-checks them with `DUMP()` on Oracle 19c.
- Parsing of the `SCHEMA.TABLE` target (quotes, case).

### 10.2 Integration tests (C++, Oracle required)

Session, queries and table description; direct path loader: small stream
buffers (`OCI_CONTINUE`), stream accumulation on and off, the `EntrySet`
fallback, explicit and implicit abort, parallel sessions on one table,
concurrent loader creation, values too long, rows wider than the stream buffer,
mixed-case identifiers, an indexed table (ORA-26002, then loaded with index
maintenance skipped).

### 10.3 sqllogictest (no Oracle)

- `LOAD` of the extension, secret creation and redaction, without Instant Client.
- Errors: missing `CONNECTION`, unknown secret, unknown option, `COPY FROM`.
- Instant Client version, only when `OCI_HOME` is set (`require-env`).

### 10.4 End-to-end tests (pytest, Oracle required)

- Exact round trip for every type in section 5: written by OraDuck, read back
  with `python-oracledb` (thin mode), compared value by value.
- NULLs, multi-byte Unicode (é, 漢字, emoji), empty strings, short inlined
  strings, constant and dictionary vectors, empty source.
- Parallel load (`threads = 8`, several million rows): row count and checksums.
- Without Instant Client: the extension loads and creates secrets;
  `oraduck_oci_version()` and COPY report that Instant Client is missing.
- Indexed table: refused (the message names `SKIP_INDEX_MAINTENANCE`); with
  `SKIP_INDEX_MAINTENANCE true` and 8 threads, all rows loaded, a plain and a
  unique index left `UNUSABLE`, then `VALID` after `ALTER INDEX … REBUILD`.
- Errors: missing table or column, incompatible or unsupported type, wrong password, value too long, NULL in a NOT NULL column, dates out of
  range, NaN into NUMBER, a local file named like the target, file options,
  invalid `STREAM_SIZE`. For load errors, the table must be empty afterwards.
- Benchmark harness: both methods must load identical data (row by row
  `MINUS` in both directions), and so must quack-oracle and duckdb-oracle; a
  run killed on timeout must leave a table that can be truncated.

## 11. OCI probe

`tools/probe.cpp` reuses the extension's OCI layer (session, direct path
loader, encoders) to measure, on Oracle 19c, which external types direct path
accepts and what they cost. Results and decisions are in
`docs/probe-results.md`:

- NUMBER from integers: native `SQLT_INT` (fastest); DECIMAL: encoded
  `SQLT_NUM`; DATE: `SQLT_DAT`; BINARY_DOUBLE: `SQLT_BDOUBLE`;
- TIMESTAMP: internal type 180, 37 % faster than text;
- stream accumulation: 16 % faster; stream buffer: 4 MiB;
- parallel sessions: 1 → 4 sessions divides the time by 2.4, flat beyond;
- identifiers must be double-quoted (ORA-39826 otherwise);
- indexed tables fail with ORA-26002 in parallel mode (see 6.4 for
  `SKIP_INDEX_MAINTENANCE`);
- all 29 encoder reference values match `DUMP()`.

## 12. Repository layout

```
oraduck/
├── CMakeLists.txt, Makefile, extension_config.cmake   # DuckDB extension template
├── duckdb/                  # pinned DuckDB submodule
├── extension-ci-tools/      # pinned build tooling submodule
├── src/                     # C++ extension (section 4)
├── test/sql/                # sqllogictest
├── test/cpp/                # C++ unit and integration tests (Catch2)
├── tools/                   # OCI probe (tools/probe.cpp)
├── infra/                   # Oracle 19c in Docker, Instant Client, Fakelake
├── bench/                   # Python harness (uv), results, REPORT.md
├── tests/                   # pytest: end-to-end and harness tests
└── docs/                    # this design document and the probe results
```

## 13. Success criteria

1. `COPY … (FORMAT ORADUCK, CONNECTION …)` works with the syntax of section 3
   on Oracle 19c.
2. All tests in section 10 pass.
3. Every benchmark run has identical row counts and checksums between DuckDB
   and Oracle.
4. The report covers the whole matrix and the scaling test, for both methods.
5. Performance goal: OraDuck faster than N CSV files + N sqlldr over the whole
   matrix. This was a hypothesis to check, not a guaranteed outcome; the report
   shows the numbers obtained (OraDuck is faster in all 9 cases).
