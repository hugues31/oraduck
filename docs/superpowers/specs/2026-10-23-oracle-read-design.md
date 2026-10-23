# OraDuck read path: `oraduck_scan` (design)

- **Date**: 2026-10-23
- **Status**: approved 2026-10-23; amended the same day (read-only `ATTACH`
  added at Hugues's request; extent planning revised from the probe results)
- **Author**: Hugues Gaillard (with Claude)
- **Target release**: v0.4.0 (*Claude's call*)

Decisions that came from Claude rather than from Hugues are marked *Claude's call*;
everything else was decided in the brainstorming conversation.

## 1. Intent

OraDuck loads DuckDB data into Oracle (`COPY … TO … (FORMAT ORADUCK)`, OCI direct
path, parallel). It must now also **read** Oracle tables into DuckDB, with the
same focus on raw performance.

The reference workload is a partitioned Oracle table of about 100M rows from
which one partition is read, about **7M rows × 550 columns**, then used in
DuckDB for transformations (joins with other tables, filters, aggregations).
The user writes plain DuckDB SQL:

```sql
SELECT * FROM toto WHERE id_partition = 20240507;
```

Production environment:

- Oracle on **Exadata**, character sets **AL32UTF8** (`NLS_CHARACTERSET`) and
  **AL16UTF16** (`NLS_NCHAR_CHARACTERSET`), the same as the test container;
- a **1 Gbit/s** link between Oracle and the DuckDB machine;
- DuckDB machine with **128 GB** of RAM, **DuckDB 2.0** only;
- read privileges unknown: the reading user may or may not see `DBA_EXTENTS`.

Production tables:

- mostly **LIST** partitioned, with **LIST** subpartitions or none;
- partition keys of various types: **`DATE`**, **`VARCHAR2(n BYTE)`**, and
  others;
- read directly as tables; reading views as well is welcome.

Consequence: in production the link is the bottleneck (about 115 MB/s of
payload, so 7M × 550 is minutes of transfer whatever the code does). OraDuck
must keep the link saturated, and it must never send bytes that are not needed:
it reads only the requested columns and lets Oracle apply the filter (partition
pruning). The Exadata side is not expected to be the bottleneck.

## 2. Goals, non-goals, anti-goals

### Goals

1. A table function `oraduck_scan` that reads one Oracle table, view or
   synonym into DuckDB, with:
   - **projection pushdown**: only the columns the query uses are fetched;
   - **filter pushdown**: the `WHERE` conditions DuckDB can hand over are
     translated into the Oracle `WHERE` clause, so that Oracle prunes
     partitions;
   - **parallel reads**: several Oracle sessions, one per DuckDB thread, each
     reading a different slice;
   - direct conversion of Oracle values into DuckDB vectors (no text, no
     intermediate file).
2. The **view approach** works end to end:
   ```sql
   CREATE VIEW toto AS FROM oraduck_scan('MY_SCHEMA.TOTO', connection := 'oracle_test');
   SELECT * FROM toto WHERE id_partition = 20240507;   -- filter executed by Oracle
   ```
3. **Read-only `ATTACH`** (section 4.7, added at Hugues's request): an Oracle
   database attached as a DuckDB catalog, read through the same engine as
   `oraduck_scan`:
   ```sql
   ATTACH '' AS ora (TYPE oraduck, SECRET oracle_test);
   SELECT * FROM ora.MY_SCHEMA.TOTO WHERE id_partition = DATE '2024-05-07';
   ```
4. **DuckDB 2.0 only** (see section 8). DuckDB 1.5.x support is dropped.
5. The code is layered so that `oraduck_query` (raw SQL passthrough), the last
   member of the DuckDB "trio" with `ATTACH` and `oraduck_scan`, can be added
   later with little work. See section 6.
6. A benchmark against the equivalent solutions (section 10), with a report in
   the style of `bench/REPORT.md`.

### Non-goals (this version)

- `oraduck_query`. The architecture prepares it, but it is not delivered.
- Writing through `ATTACH` (`INSERT`, `UPDATE`, `DELETE`, DDL on an attached
  Oracle database): the attached database is read-only; the COPY writer stays
  the write path and keeps its behaviour.
- Pushing down joins, aggregates, `ORDER BY` or `LIMIT` to Oracle.
- Join-derived (dynamic, run-time) filters from DuckDB.
- Caching Oracle data between queries: every query on the view reads Oracle
  again. A user who reuses the data runs `CREATE TABLE … AS SELECT …` once.
- Oracle types outside section 5.3 (they raise an error when selected).
- macOS (no Instant Client 19 for Apple Silicon, as before).

### Anti-goals (failure even if it "works")

- **Silently wrong results.** A filter pushed to Oracle must return exactly the
  rows DuckDB would have kept. When the semantics may differ, the filter stays
  in DuckDB (slower, correct). The only accepted precision loss is the
  documented default for `NUMBER` without precision (section 5.3) and the
  documented truncation of nanoseconds.
- **Reading the whole table to filter it locally** when the filter is on the
  partition key: partition pruning must happen in Oracle.
- **One network round trip per value or per row**, especially for CLOBs.
- **Memory blow-up**: fetch buffers sized from declared lengths
  (`VARCHAR2(4000)` × hundreds of columns × thousands of rows) or a full
  materialization of the Oracle result outside DuckDB.
- **Regressions of the writer.**

## 3. Constraints

- OCI loaded at run time (`OciApi`, `dlopen`/`LoadLibrary`): the build still
  needs no Oracle software, and the extension still loads without Instant
  Client. New OCI functions are added to `OciApi`.
- Oracle Instant Client 19 or later; Oracle Database 19c or later as source.
- Platforms: Linux x86_64 and arm64 (manylinux_2_28), Windows x86_64.
- C++17 for the extension targets; repository conventions unchanged (English
  code, comments and docs; Catch2 unit tests; sqllogictest; pytest end-to-end
  tests against the `oraduck-ora19` container; Python benchmark harness).
- Credentials come from the existing `oraduck` secret type
  (`USER`, `PASSWORD`, `DSN`). `DSN` is passed as is to OCI: it can be
  EZConnect, a full connect descriptor or a TNS alias.
- One OCI environment per session (existing finding: a shared environment
  corrupts its heap under concurrent allocations, even with `OCI_THREADED`).

## 4. User-facing design

### 4.1 `oraduck_scan`

```sql
SELECT … FROM oraduck_scan('<[SCHEMA.]TABLE>', connection := '<secret>' [, option := value …]);
```

- First argument: an Oracle table, view or synonym, parsed like the COPY
  target (`TABLE`, `SCHEMA.TABLE`, unquoted identifiers upper-cased,
  double-quoted ones kept as is). The existing `oracle_names` parser is
  reused. A private or public synonym is resolved to the object it names
  (*Claude's call*; the current `DescribeTarget` does not resolve synonyms).
  Views are supported: they are read with the `hash` or `none` strategy
  (4.4).
- `connection` (required): name of an `oraduck` secret. The name matches the
  COPY option `CONNECTION` (*Claude's call*: consistency inside OraDuck rather
  than DuckDB's `SECRET` naming).

Options (named parameters):

| Option | Default | Purpose |
|---|---|---|
| `connection` | (required) | `oraduck` secret |
| `sessions` | DuckDB `threads` | Maximum number of Oracle sessions |
| `split` | `'auto'` | Parallel strategy: `'auto'`, `'extents'`, `'hash'`, `'none'` (4.4) |
| `unconstrained_number` | `'DOUBLE'` | DuckDB type for `NUMBER` without precision and `FLOAT`: `'DOUBLE'`, `'VARCHAR'` or `'DECIMAL(p,s)'` (5.3) |
| `snapshot` | `'auto'` | Read consistency across sessions: `'auto'`, `'scn'`, `'none'` (4.5) |

Tuning parameters (fetch buffer budget, prefetch size, rows per fetch) are left
to the builder; expose them only if the benchmark shows a need, with defaults
that need no tuning.

Network options (SQL\*Net compression, SDU size) need no OraDuck option: they
go into `DSN` as a connect descriptor, for example
`(DESCRIPTION=(COMPRESSION=on)(SDU=2097152)(ADDRESS=…)(CONNECT_DATA=…))`. The
README documents this, including that SQL\*Net compression requires the Oracle
Advanced Compression licence (*Claude's call*).

### 4.2 The view approach

A DuckDB view over `oraduck_scan` is the supported way to write
`SELECT … FROM toto WHERE …`. Filters and projections written against the view
must reach `oraduck_scan` exactly as if the query called the function
directly; a test proves it (section 9).

### 4.3 Filter pushdown

DuckDB hands the scan the filters it can (for example
`pushdown_complex_filter`, at bind time; the builder picks the DuckDB 2.0 hook
that sees the filters before the Oracle SQL is fixed). OraDuck translates what
it can into Oracle SQL and leaves the rest to DuckDB.

Translated (minimum): comparisons `=`, `<>`, `<`, `<=`, `>`, `>=` between a
column and a constant, `BETWEEN`, `IN (constants)`, `IS NULL`,
`IS NOT NULL`, and `AND` / `OR` combinations of translated terms.

Rules:

- Constants are **bind variables** typed like the Oracle column (NUMBER binds
  for NUMBER columns, DATE/TIMESTAMP binds for date columns, character binds
  for character columns). A bind whose type would make Oracle convert the
  column (for example a number compared with a `VARCHAR2` column, which Oracle
  turns into `TO_NUMBER(col)`) is forbidden: it disables pruning and can raise
  conversion errors. Partition pruning must survive (Oracle `KEY` pruning with
  binds is fine).
- **Partition keys must always be pushed.** This is the case that motivates
  the project. Equality, `IN` lists and ranges on the key types of the
  production tables must reach Oracle and prune partitions and
  subpartitions:
  - `DATE` keys: the DuckDB column is a `TIMESTAMP` (5.3); DuckDB casts the
    constant (`DATE '2024-05-07'` or a timestamp literal) to the column type,
    and OraDuck binds it as an Oracle `DATE`. The common form
    `CAST(col AS DATE) = d` is translated into the sargable range
    `col >= :d AND col < :d + 1` (*Claude's call*), so that it prunes too;
  - `VARCHAR2(n BYTE)` keys: string equality, `IN` and ranges with character
    binds;
  - `NUMBER(p,0)` keys: integer binds.
  A constant of another type than the column (for example an integer compared
  with a `VARCHAR2` key) makes DuckDB cast the column; such a filter cannot be
  pushed, and the README tells users to write the constant in the column's
  type.
- The databases are AL32UTF8 (production and tests): every UTF-8 constant is
  representable, and with binary comparison (`NLS_COMP=BINARY`,
  `NLS_SORT=BINARY`, set by OraDuck at logon) Oracle orders strings like
  DuckDB. String equality and ranges are therefore pushable. OraDuck checks
  the database character set at bind time; on another character set it does
  not push string ranges, nor string constants that the character set cannot
  represent.
- Never pushed (kept in DuckDB) unless the builder proves equal semantics:
  - comparisons with the empty string (`''` is NULL in Oracle: `col <> ''`
    returns no rows in Oracle, all non-NULL rows in DuckDB);
  - `CHAR(n)` comparisons unless the bind uses non-padded semantics, so that
    the result matches DuckDB comparing the padded value it receives;
  - comparisons on `NUMBER` without precision mapped to `DOUBLE` (Oracle
    compares exact decimals, DuckDB doubles);
  - anything involving time zones, NaN or infinities, or a type outside 5.3.
- A pushed filter is removed from DuckDB's plan; a filter that is not
  translated stays in DuckDB. Whether DuckDB re-checks pushed filters as a
  safety net is left to the builder (cost against risk).

### 4.4 Parallel reads

The number of sessions is `min(sessions, work units)`. Work units go into a
shared queue; each DuckDB thread owns one Oracle session (opened lazily) and
takes units until the queue is empty. Every unit query is the same `SELECT`
(projection + translated filter) with an extra predicate that selects its
slice.

`split = 'auto'` tries, in order (*Claude's call*, approved in conversation):

1. **`extents`**: ROWID ranges, so that each session reads distinct blocks.
   They come from `DBA_EXTENTS` when the reading user can read it (OraDuck
   builds the ranges), otherwise from
   `DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID`, which needs only `SELECT`
   on the table (the probe confirmed it; it writes a temporary task in
   Oracle's dictionary, dropped right after, which the README states).
   `USER_EXTENTS` cannot be used: it has no block addresses. Units are sized
   from the largest segment, not from the whole table, so that a filtered
   partition of a large table is still cut into enough units for every
   session. With LIST subpartitions, the segments are the subpartitions.
   Units must
   cover every row that matches the filter; units that fall in partitions
   eliminated by the filter must cost almost nothing; the planning queries
   must stay a small fraction of the read (target under 2 %, measured). How
   ranges are built and sized (per segment, merged extents, more units than
   sessions for load balancing, how pruned partitions are skipped) is left to
   the builder.
2. **`hash`**: predicate `ORA_HASH(ROWID, N - 1) = k`. Every session scans all
   the matching partitions in Oracle (N times the server I/O), which Exadata
   absorbs, but the network carries each row once.
3. **`none`**: one session.

A forced strategy that cannot run (for example `split = 'extents'` on a
view) is an error that says why. `EXPLAIN` shows the pushed `WHERE` and the
requested options; the chosen strategy, the number of units and the generated
Oracle SQL are visible in `EXPLAIN ANALYZE` (the work plan is computed at each
execution: ROWID ranges go stale after a `TRUNCATE`).

A view, or a table for which neither extent source works, falls back to
`hash` automatically. A view without identifiable base-table rows (a join or
aggregate view: `ORA_HASH(ROWID)` raises ORA-01445) falls back to `none`.

### 4.5 Read consistency

N sessions reading at different moments do not see one snapshot. With
`snapshot = 'auto'` (*Claude's call*), OraDuck reads the current SCN once at
bind time and every unit query uses `AS OF SCN :scn`; if Oracle refuses
(flashback query on another user's table needs the `FLASHBACK` privilege) it
falls back to per-session reads and says so in `EXPLAIN` and in a DuckDB
warning/log line. `'scn'` makes the refusal an error; `'none'` never uses
`AS OF`. The builder checks that `AS OF SCN` does not cost throughput or
pruning; if it does, the default becomes `'none'` and the report says why.

### 4.6 Errors and interrupts

- ORA errors become DuckDB exceptions with the ORA code and text, the operation
  and, for conversions, the column.
- An unsupported column type raises an error naming the column and its Oracle
  type, **only if the query selects that column** (projection pushdown makes
  `SELECT a, b` work on a table with an `XMLTYPE` column).
- A DuckDB interrupt (Ctrl-C) stops the fetches promptly (`OCIBreak` /
  `OCIReset` or closing the sessions) and releases every session.

### 4.7 Read-only `ATTACH`

```sql
ATTACH '' AS ora (TYPE oraduck, SECRET oracle_test
                  [, SESSIONS n] [, SPLIT '…'] [, SNAPSHOT '…'] [, UNCONSTRAINED_NUMBER '…']);
SELECT * FROM ora.MY_SCHEMA.TOTO WHERE id_partition = DATE '2024-05-07';
SELECT * FROM ora.TOTO;              -- the connected user's schema
SHOW TABLES FROM ora.MY_SCHEMA;
DETACH ora;
```

- `SECRET` (required) names an `oraduck` secret; the path must be empty.
- DuckDB schemas are Oracle users. The default schema of the attached
  database is the session's current schema, so `ora.TOTO` works.
- Lookups are lazy: `ATTACH` reads no dictionary beyond what it needs to
  connect; a table is described when a query names it; `SHOW TABLES` on one
  schema describes that schema's tables and views in bulk (one dictionary
  query per schema, not one per table). Listing every schema
  (`SHOW ALL TABLES`) is allowed but can be slow on a large database.
- Identifiers are matched like Oracle's unquoted names when no exact match
  exists (`ora.my_schema.toto` finds `MY_SCHEMA.TOTO`); tables, views and
  synonyms are readable, tables and views are listed.
- Every table of the attached database is read by `oraduck_scan`'s engine:
  same projection and filter pushdown, parallelism, snapshot and type
  mapping. The `ATTACH` options are the defaults of every scan of that
  database, with the same names and values as `oraduck_scan`'s parameters.
- Descriptions are cached for the life of the attachment; `DETACH` then
  `ATTACH` refreshes them.
- The database is read-only: any write or DDL statement on it fails with an
  error that points to `COPY … (FORMAT ORADUCK)`.
- `oraduck_scan` stays available; the view approach keeps working.

## 5. Performance design

### 5.1 Principles

- The network is the production bottleneck; the client must never be. On the
  local benchmark (no network limit) OraDuck is CPU-bound and must still beat
  every competitor (section 10).
- Fetch with large OCI array fetches and **prefetching sized in bytes**
  (`OCI_ATTR_PREFETCH_MEMORY` or equivalent), so that the number of round trips
  depends on the data volume, not on the number of rows or columns.
- Define buffers are sized from a **memory budget** per session, not from the
  declared column lengths: rows per fetch = budget / bytes per row, bounded.
  With 550 columns, many of them `VARCHAR2(4000)`, a fixed array of thousands
  of rows would need gigabytes. The define size of a character column must
  account for the client character set (AL32UTF8): a single-byte database
  charset can expand up to 3× in UTF-8, `CHAR` semantics up to 4× per
  character.
- Decode Oracle internal formats directly into DuckDB vectors: NUMBER, DATE and
  TIMESTAMP are decoded by OraDuck from their internal bytes when this is
  faster than letting OCI convert (the builder measures, as the writer's probe
  did; the writer already has the encoders, the decoders are their mirror and
  are unit-tested against `DUMP()` values). Strings are copied once, from the
  define buffer into the DuckDB string heap.
- **CLOBs come inline**: no `OCILobRead` round trip per value for ordinary
  sizes. Candidate techniques: defining the CLOB column as a long character
  type (the OCI "data interface for LOBs"), or LOB prefetching
  (`OCI_ATTR_DEFAULT_LOBPREFETCH_SIZE`). Values larger than the inline limit
  may cost extra round trips but must be read correctly; the limit and its
  cost are measured and documented.
- Sessions are opened in parallel (logon latency over the network is paid
  once, not N times in sequence).
- DuckDB gets a cardinality estimate (Oracle table statistics) so that its
  optimizer orders the joins that follow sensibly.

### 5.2 Probe first

Before the scan is written, extend `tools/probe.cpp` (or add a read probe) to
measure on Oracle 19c, as was done for the writer, and record the results in
`docs/probe-results.md`:

- NUMBER(p,0) p ≤ 18: OCI integer conversion against OraDuck's NUMBER decoder;
- NUMBER(p,s) with p > 18: decoder into DuckDB `DECIMAL` (hugeint);
- DATE / TIMESTAMP: internal bytes against OCI conversions;
- prefetch memory and rows per fetch on a wide table (550 columns);
- CLOB: inline define against LOB prefetch against locator reads;
- `extents` against `hash` splitting, and the planning cost;
- `AS OF SCN` cost.

### 5.3 Type mapping

| Oracle | DuckDB | Notes |
|---|---|---|
| `NUMBER(p,0)`, p ≤ 18 | smallest of `SMALLINT`/`INTEGER`/`BIGINT` that holds p digits (or `BIGINT` always; builder's call) | exact |
| `NUMBER(p,s)`, all other p ≤ 38 (e.g. `NUMBER(38,0)`, `NUMBER(25,9)`) | `DECIMAL(p,s)` | exact. Negative scale or s > p: mapped to an exact `DECIMAL` (builder's call) |
| `NUMBER` without precision, `FLOAT(b)` | `unconstrained_number`: `DOUBLE` (default), `VARCHAR` (exact text) or a user-given `DECIMAL(p,s)` (error on overflow) | *Claude's call*: `DOUBLE` loses digits beyond about 15, documented |
| `BINARY_FLOAT`, `BINARY_DOUBLE` | `FLOAT`, `DOUBLE` | |
| `DATE` | `TIMESTAMP` | an Oracle `DATE` carries the time of day |
| `TIMESTAMP(p)` | `TIMESTAMP` | nanoseconds truncated to microseconds (*Claude's call*: `TIMESTAMP_NS` only covers 1677–2262) |
| `TIMESTAMP WITH TIME ZONE`, `WITH LOCAL TIME ZONE` | `TIMESTAMPTZ` | the instant is preserved |
| `VARCHAR2`, `NVARCHAR2`, `CHAR`, `NCHAR` | `VARCHAR` (UTF-8) | `CHAR` values keep Oracle's blank padding; national types (AL16UTF16) are converted to UTF-8 by OCI (national charset form) |
| `CLOB`, `NCLOB` | `VARCHAR` | inline fetch (5.1) |
| `RAW`, `BLOB` | `BLOB` | |
| `ROWID`, `UROWID` | `VARCHAR` | |
| `INTERVAL DAY TO SECOND`, `INTERVAL YEAR TO MONTH` | `INTERVAL` | builder may defer if costly; then they join the next row |
| `LONG`, `LONG RAW`, `XMLTYPE`, `JSON`, `VECTOR`, `BFILE`, object and collection types | error when selected (4.6) | |

The target workload uses `VARCHAR2(4000 BYTE)`, `NUMBER(38,0)`, `DATE`,
`NUMBER(25,9)`, `CHAR(1 BYTE)` and `CLOB` (and more): those are the types the
tests and the benchmark must cover first.

## 6. Architecture (ready for the trio)

The scan is built in layers, so that `ATTACH` and `oraduck_query` reuse
everything below the DuckDB glue. Names are indicative; the boundaries are
the requirement.

| Layer | Responsibility | Depends on DuckDB? |
|---|---|---|
| `OracleTableDescription` | Resolves `SCHEMA.TABLE` and describes it: columns (name, Oracle type, size, precision, scale, charset form, nullability), statistics (row count), partitioning summary. Built from a session; plain data afterwards, cacheable. | no |
| `OracleQueryDescription` | Same column description, but for the result of an arbitrary SQL text (OCI describe-only execute). Needed by `oraduck_query`; the builder may implement it now or only define the seam. | no |
| Type mapping | Oracle column → DuckDB type + fetch plan (OCI external type, define size, decoder). Pure, unit-tested. | types only |
| Decoders | Oracle internal bytes → DuckDB values (NUMBER → integers / hugeint / double, DATE/TIMESTAMP → microseconds). Pure, unit-tested without Oracle, mirror of the encoders. | no |
| Filter translator | DuckDB filter expressions → Oracle SQL fragment + typed binds, plus the list of filters it could not translate. Pure, unit-tested. | expressions only |
| Read plan | Projection + translated filter + slice predicate + snapshot → SQL text and binds. Pure, unit-tested. | no |
| Splitter | Builds the work units for a read plan (extents, hash, none). Needs a session for the dictionary queries. | no |
| Fetch cursor | One statement on one session: prepare, bind, define, execute, array fetch, LOB handling, decoding into a DuckDB `DataChunk`. | vectors only |
| `oraduck_scan` glue | DuckDB table function: bind, filter pushdown, global/local init, scan, `to_string`, cardinality. The only layer, with the filter translator's expression input and the catalog, that depends on DuckDB's planner APIs, so that a DuckDB upgrade touches few files. | yes |
| Catalog (`ATTACH`) | Storage extension `oraduck`, catalog, schema and table entries, a no-op transaction manager. Table entries hold an `OracleTableDescription` and return `oraduck_scan`'s table function with bind data built from it. Write and DDL paths throw. | yes |

Seams that must exist so that the later work is small:

- The scan's bind data can be built **from an `OracleTableDescription`**
  directly, not only from the function's string argument:
  `OracleTableEntry::GetScanFunction` (ATTACH) returns the same table
  function with bind data built from its cached description.
- The fetch cursor and the type mapping work on a **column description**, not
  on a table: `oraduck_query` = query description + read plan without filter
  pushdown or splitting (one unit) + the same fetch cursor.
- Session handling (secret → credentials → `OciSession`) is shared by the
  writer, the scan and the catalog.

## 7. Behaviour details left open on purpose

- `SELECT count(*) FROM toto WHERE …` (no column needed): the builder picks
  what to fetch per row (a constant, `ROWID`) and makes it cheap.
- Row order is not preserved (parallel units); the scan does not claim an
  order.

## 8. DuckDB version

- **DuckDB v2.0.0 only**, as the repository already is (submodule, CI,
  release layout unchanged). DuckDB 1.5.x support is dropped (decided by
  Hugues).
- OraDuck stays a C++ extension. DuckDB 2.0 loads a C++ extension only into
  the exact DuckDB version it was built for (checked in
  `src/main/extension.cpp`: "built specifically for DuckDB version … can only
  be loaded with that version"), so every DuckDB 2.0.x release needs a
  rebuild. A version-independent binary would need DuckDB's stable C API,
  which offers projection pushdown but no filter pushdown (checked in the
  v2.0.0 `duckdb.h`), so partition pruning would be impossible: not an option.
  The README states this.
- DuckDB-API-dependent code stays concentrated in the glue layer (section 6),
  so that following DuckDB releases is cheap.

## 9. Tests

Same structure as the writer (`docs/design.md` section 10), plus:

- **Unit (no Oracle)**: decoders (NUMBER edge values: 0, negatives, 38 digits,
  scales, values from `DUMP()`; DATE before 1970, BC years, bounds; TIMESTAMP
  fractions and time zones), filter translator (every operator, NULL handling,
  the never-pushed cases of 4.3, bind types), read plan SQL text, extent range
  building.
- **Integration (C++, Oracle)**: fetch cursor per type; a 550-column table
  with many `VARCHAR2(4000)` columns within the memory budget; CLOBs empty,
  small, above 4,000 bytes, above the inline limit, multi-megabyte.
- **sqllogictest (no Oracle)**: `oraduck_scan` registered, argument and option
  errors, missing secret.
- **End-to-end (pytest, Oracle)**:
  - exact round trip of every type in 5.3, including multi-byte UTF-8, NULLs,
    `CHAR` padding;
  - **filter equivalence**: for a battery of predicates (each operator, NULLs,
    empty strings, `CHAR`, dates, `IN`, `OR`), the result through
    `oraduck_scan` equals the result of reading all rows and filtering in
    DuckDB;
  - **partition pruning proven**: on a partitioned table, the Oracle plan of
    the generated statement shows pruning (for example `V$SQL_PLAN` /
    `DBMS_XPLAN.DISPLAY_CURSOR` on its `SQL_ID`, or segment statistics);
  - projection pushdown proven (only the selected columns in the generated SQL);
  - the view approach (4.2) pushes filters and projections like a direct call;
  - each split strategy, forced, gives identical results, with both extent
    sources (`DBA_EXTENTS` for a user granted it, `DBMS_PARALLEL_EXECUTE` for
    a dedicated test user with only `SELECT` on the tables); `auto` falls
    back to `hash` on a view;
  - snapshot modes, including the fallback when `FLASHBACK` is missing;
  - unsupported column selected → error; not selected → works;
  - interrupt during a large read releases all sessions;
  - errors: missing table, wrong password, invalid options;
  - `ATTACH`: default schema, case-insensitive names, `SHOW TABLES`,
    `information_schema.columns`, tables, views and synonyms, filter and
    projection pushdown and partition pruning through the catalog, `ATTACH`
    options applied, two attached databases in one query, refresh by
    `DETACH`/`ATTACH`, read-only errors, missing secret or table.
- The writer's existing tests keep passing.
- Partitioned test tables cover the production shapes: LIST partitioning on a
  `DATE` key and on a `VARCHAR2` key, with and without LIST subpartitions;
  national character columns (`NVARCHAR2`, `NCLOB`) are covered by the round
  trip.

## 10. Benchmark

### 10.1 What is compared

All methods produce the same DuckDB table from the same Oracle partition; the
timer covers everything from the start of the read to the rows stored in
DuckDB.

| Method | How |
|---|---|
| OraDuck | `ATTACH` then `CREATE TABLE t AS SELECT * FROM ora.BENCH.<table> WHERE …` (the syntax the other extensions use), N = 8 and N = 1; one case also through `oraduck_scan` to show both paths match |
| quack-oracle | community extension `oracle_scanner`, `ATTACH` then `CREATE TABLE … AS SELECT … WHERE …` |
| duckdb-oracle (rinie) | built from source as today, `ATTACH` then the same statement |
| python-oracledb + Arrow | `fetch_df_batches` (Arrow) streamed into DuckDB, thin mode (and thick if it differs notably) |
| ConnectorX | `read_sql(…, return_type="arrow")` with its partitioning on a numeric column, streamed or batched into DuckDB |
| SQL\*Plus CSV + `read_csv` | `sqlplus -F` with `SET MARKUP CSV ON`, large `ARRAYSIZE`, spooled to the NVMe, then DuckDB `read_csv`; total and split reported |

Python methods must not hold the whole result in memory (7M × 550 would not fit
on the benchmark machine): they stream batches into a DuckDB file. The other
extensions may only exist for DuckDB 1.5.5 (the builder checks whether
quack-oracle has a 2.0 community build); in that case they run in a DuckDB
1.5.5 CLI while OraDuck runs in 2.0.0, and the report says so. The DuckDB
version matters little here: the time is spent in the Oracle read path.

### 10.2 Data

- An Oracle table LIST-partitioned like production (*Claude's call*: a `DATE`
  key, LIST subpartitions on a `VARCHAR2` column), with one large target
  partition and several others, so that reading the target partition proves
  pruning. The full 100M-row table does not fit on the benchmark laptop and is
  not needed for that.
- Column mix shaped like the target workload: `VARCHAR2(4000 BYTE)` (with
  realistic actual lengths, mostly short), `NUMBER(38,0)`, `NUMBER(25,9)`,
  `DATE`, `CHAR(1 BYTE)`, a few `CLOB`s (mostly small, some above 4,000
  bytes), some `NUMBER` without precision and `TIMESTAMP`. Exact proportions
  are the builder's call (the real distribution is unknown); NULLs about 5 %.
- Matrix (target partition): rows ∈ {1M, 7M} × columns ∈ {50, 550}, with
  7M × 550 as the headline case. The builder may add smaller cases for fast
  iterations.
- Generated with Fakelake and loaded with OraDuck's writer where possible
  (CLOB columns, which the writer does not support, through Oracle-side SQL).
  Not timed.

### 10.3 Network conditions

1. **Local, unconstrained** (client and container on the laptop, cpusets as
   today): measures CPU cost and protocol efficiency.
2. **Shaped to 1 Gbit/s** on the Docker network (`tc`, needs root: a script in
   `infra/` that Hugues runs with `sudo`), with a small added round-trip time
   (0.5 ms, *Claude's guess* for a data-centre LAN). Reports the link
   utilisation: bytes received on the interface / elapsed time, against
   125 MB/s.
3. One OraDuck run with SQL\*Net compression in the DSN, to quantify it.

### 10.4 Protocol

As for the writer: one warm-up run, then interleaved runs (5 for OraDuck
cases, 3 for the competitors), median and min–max reported, 15-minute timeout
with the same skip rule, and checksums after every run (row count, sums of
numeric columns, sums of string lengths, NULL counts) compared with aggregates
computed by Oracle. Any mismatch invalidates the run.

The harness can also run read-only against an existing table in another
Oracle database (DSN and table given on the command line, no data generation),
so that Hugues can run the same measurement on the Exadata (*Claude's call*).

### 10.5 Success criteria (*Claude's call*)

- Local: OraDuck is the fastest method on every case of the matrix.
- Shaped to 1 Gbit/s: on 7M × 550, OraDuck reaches at least 90 % of line rate.
- Correctness: identical checksums for every method that finishes.

Results are reported as measured, including cases where OraDuck does not win.

## 11. Deliverables

- `oraduck_scan` and read-only `ATTACH` in the extension, for DuckDB v2.0.0.
- Tests of section 9, in the existing CI.
- Probe results for the read path in `docs/probe-results.md`.
- `docs/design.md` updated with the read path (or a sibling document it links).
- README: "Read from Oracle" section (`ATTACH` first, then `oraduck_scan` and
  the view approach, options, network options in the DSN, type mapping,
  constants written in the column's type, the temporary
  `DBMS_PARALLEL_EXECUTE` task), and the note that each DuckDB release needs
  a matching OraDuck build.
- `bench/REPORT_READ.md` (or a new section of `bench/REPORT.md`) with charts.
- Release v0.4.0.

## 12. Left to the builder

- Exact OCI techniques (external types, prefetch, define sizing, LOB method),
  driven by the probe.
- Extent range construction and pruned-partition handling (within the
  requirements of 4.4).
- Internal names and file layout, within the layer boundaries of section 6.
- Whether pushed filters are re-checked in DuckDB.
- Benchmark table proportions and any extra small cases.
- Tuning parameters, exposed only if needed.
