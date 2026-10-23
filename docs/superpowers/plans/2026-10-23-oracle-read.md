# OraDuck read path (`oraduck_scan`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a DuckDB table function `oraduck_scan` that reads Oracle tables, views and synonyms into DuckDB at the network's line rate, with projection and filter pushdown (partition pruning) and parallel Oracle sessions, then benchmark it against five equivalent solutions.

**Architecture:** Layers without DuckDB (decoders, type mapping, filter → SQL, scan SQL, rowid ranges: pure and unit-tested; source description, fetch cursor, work planning: OCI, integration-tested) under a thin DuckDB glue (`oraduck_scan` table function, expression → filter translation, vector writers). The glue's bind data can be built from an `OracleTableDescription`, so a later `ATTACH` and `oraduck_query` reuse every layer.

**Tech Stack:** C++17, DuckDB v2.0.0 extension API, OCI loaded at run time (`OciApi`), Catch2 (unit/integration), sqllogictest, pytest + python-oracledb (end-to-end), Python benchmark harness (`oraduck-bench`).

**Spec:** `docs/superpowers/specs/2026-10-23-oracle-read-design.md` (read it first; this plan argues from it).

## Global Constraints

- DuckDB **v2.0.0 only** (the `duckdb/` submodule); no 1.5 code paths.
- Extension targets C++17; no Oracle SDK at build time: every OCI function goes through `OciApi` (`src/include/oraduck/oci_api.hpp`), loaded at run time; Instant Client 19 or later; Oracle Database 19c or later.
- Platforms: Linux x86_64/arm64 (manylinux_2_28) and Windows x86_64 (MSVC): extension code uses only standard C++ and DuckDB facilities (no POSIX-only calls).
- One OCI environment per session (`CreateOciEnv` per `OciSession`, as today).
- English code, comments and docs; DuckDB code style (tabs, braces, `PascalCase` functions, short comments), matching `src/`.
- Pure and OCI layers throw `oraduck::OraduckError` / `oraduck::OracleError` (message without prefix); the DuckDB glue rethrows them as DuckDB exceptions prefixed `"OraDuck: "` (`BinderException` at bind, `IOException` during the scan, `InvalidInputException` for option values).
- Credentials come from the existing `oraduck` secret; the named parameter is `connection`.
- A filter that is not provably equivalent in Oracle stays in DuckDB (anti-goal: silently wrong results).
- Pure sources join `PURE_SRCS`, OCI sources join `OCI_SRCS` in `Makefile`, and every extension source joins `EXTENSION_SOURCES` in `CMakeLists.txt`, in the task that creates them. `make unit` and `make itest` must keep building without DuckDB headers.
- The COPY writer's behaviour and tests stay unchanged.
- Target release: v0.4.0 (the tag is only pushed after Hugues confirms).

Commands used throughout (from the repository root):

| Purpose | Command |
|---|---|
| Build extension + CLI | `GEN=ninja make release` |
| Unit tests (no Oracle) | `make unit` |
| Integration tests (Oracle) | `source env.sh && make itest` |
| sqllogictest | `make test` |
| End-to-end tests (Oracle) | `source env.sh && uv run pytest tests/e2e -q` |
| Benchmark harness tests | `uv run pytest tests/bench -q` |
| Oracle container | `./infra/oracle/start.sh` |

## Review Focus

1. **`EMPTY_CLOB()` against NULL**: an empty CLOB arrives as `''`, a NULL CLOB as NULL (Task 7 integration test, Task 9 end-to-end test).
2. **Column names that collide case-insensitively** (`"Id"` and `ID`) or are SQL keywords (`"ORDER"`): DuckDB names are deduplicated deterministically (`ID`, `Id_1`) and every column is readable (Task 5 quoting test, Task 9 end-to-end test).
3. **An Oracle error in the middle of a scan** (ORA-01476 raised by a view row): DuckDB reports the ORA code, does not hang, and releases every Oracle session at once (Task 9 end-to-end test).
4. **Two `oraduck_scan` calls in one query** (self-join): both read correctly with their own sessions (Task 9 end-to-end test).
5. **Nothing to read**: an empty table (no segments, zero extents units), a filter that matches nothing, and `count(*)` (no projected column) return correct results (Task 8 integration test, Task 9 end-to-end test).

---

### Task 1: OCI functions for fetching, test privileges, test tablespaces

**Files:**
- Modify: `src/include/oraduck/oci_api.hpp`, `src/oci_api.cpp`
- Create: `infra/oracle/setup/02_read.sh`, `infra/oracle/grants.sh`
- Modify: `infra/oracle/start.sh` (pass `-e READER_PASSWORD`), `env.sh`
- Test: `test/cpp/test_oci_api.cpp`, `test/cpp/itest_session.cpp`

**Interfaces:**
- Produces in `OciApi`: `OCIDefineByPos2(OCIStmt*, OCIDefine**, OCIError*, ub4 position, void *valuep, sb8 value_sz, ub2 dty, void *indp, ub4 *rlenp, ub2 *rcodep, ub4 mode)`, `OCIDefineDynamic(OCIDefine*, OCIError*, void *octxp, OCICallbackDefine)`, `OCIBreak(void *hndlp, OCIError*)`, `OCIReset(void *hndlp, OCIError*)`, and the callback type `typedef sb4 (*OCICallbackDefine)(void *octxp, OCIDefine *defnp, ub4 iter, void **bufpp, ub4 **alenp, ub1 *piecep, void **indp, ub2 **rcodep)`; typedef `sb8` (`long long`).
- Produces constants: `OCI_ATTR_PREFETCH_ROWS = 11`, `OCI_ATTR_PREFETCH_MEMORY = 13`, `OCI_ATTR_CHARSET_FORM = 32`, `OCI_ATTR_ROWS_FETCHED = 197`, `SQLCS_IMPLICIT = 1`, `SQLCS_NCHAR = 2`, `OCI_DYNAMIC_FETCH = 2`, `OCI_DESCRIBE_ONLY = 0x10`, `OCI_HTYPE_DEFINE = 6`, `OCI_ONE_PIECE = 0`, `OCI_FIRST_PIECE = 1`, `OCI_NEXT_PIECE = 2`, `OCI_LAST_PIECE = 3`, `SQLT_BIN = 23`, `SQLT_LVC = 94`, `SQLT_LVB = 95`, `SQLT_AFC = 96`, `SQLT_CLOB = 112`, `SQLT_BLOB = 113`, `SQLT_TIMESTAMP = 187`, `SQLT_TIMESTAMP_TZ = 188`, `kInternalTimestamp = 180` (move it from `tools/probe.cpp`).
- Produces environment: `ORADUCK_ORACLE_READER_USER` (default `reader`), `ORADUCK_ORACLE_READER_PASSWORD` (default `ReaderPwd_2026`) in `env.sh`.
- Produces in the database (idempotent, run by `infra/oracle/grants.sh` on an existing container and by `02_read.sh` on a new one): `BENCH` tablespace `MAXSIZE 80G`; smallfile tablespace `BENCH_SMALL` (256M, autoextend to 4G) with unlimited quota for `BENCH`; grants to `BENCH`: `CREATE VIEW`, `CREATE SYNONYM`, `CREATE PUBLIC SYNONYM`, `DROP PUBLIC SYNONYM`, `SELECT` on `V_$SESSION`, `V_$MYSTAT`, `V_$STATNAME`, `V_$SEGMENT_STATISTICS`, `V_$SQL`; user `READER` with only `CREATE SESSION` (no dictionary access, no `FLASHBACK`).

- [ ] **Step 1: Write the failing tests**

In `test/cpp/itest_session.cpp`, add:

```cpp
TEST_CASE("The fetch functions are loaded", "[session]") {
	CHECK(Oci().OCIDefineByPos2 != nullptr);
	CHECK(Oci().OCIDefineDynamic != nullptr);
	CHECK(Oci().OCIBreak != nullptr);
	CHECK(Oci().OCIReset != nullptr);
}

TEST_CASE("The test database has the read-path privileges", "[session]") {
	OciSession session(TestCredentials());
	CHECK(Scalar(session, "SELECT COUNT(*) FROM v$mystat WHERE ROWNUM = 1") == "1");
	CHECK(Scalar(session, "SELECT COUNT(*) FROM user_ts_quotas WHERE tablespace_name = 'BENCH_SMALL'") == "1");
	OciSession reader({std::getenv("ORADUCK_ORACLE_READER_USER"), std::getenv("ORADUCK_ORACLE_READER_PASSWORD"),
	                   TestCredentials().dsn});
	CHECK_THROWS_WITH(reader.QueryStrings("SELECT COUNT(*) FROM dba_extents WHERE ROWNUM = 1"),
	                  Catch::Contains("ORA-00942"));
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `source env.sh && make itest`
Expected: compile error (`OCIDefineByPos2` is not a member of `OciApi`).

- [ ] **Step 3: Add the functions, constants and typedefs to `oci_api.hpp`, resolve them in `Resolve()` (`oci_api.cpp`), write `02_read.sh` (heredoc SQL like `01_bench.sh`, each `CREATE` wrapped in a PL/SQL block that ignores "already exists" errors) and `grants.sh` (`docker exec -i -e READER_PASSWORD=… oraduck-ora19 bash -s < infra/oracle/setup/02_read.sh`), add the two variables to `env.sh`, pass `-e READER_PASSWORD="$ORADUCK_ORACLE_READER_PASSWORD"` in `start.sh`.**

- [ ] **Step 4: Apply the grants and run the tests**

Run: `source env.sh && ./infra/oracle/start.sh && ./infra/oracle/grants.sh && make itest && make unit`
Expected: all tests pass, including the existing ones.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/oci_api.hpp src/oci_api.cpp infra/oracle env.sh test/cpp
git commit -m "Load the OCI fetch functions and set up the read-path test database"
```

---

### Task 2: Decoders of Oracle internal formats

**Files:**
- Create: `src/include/oraduck/oracle_decoding.hpp`, `src/oracle_decoding.cpp`
- Modify: `Makefile` (`PURE_SRCS`), `CMakeLists.txt`
- Test: `test/cpp/test_oracle_decoding.cpp`

**Interfaces:**
- Consumes: `EncodeOracleNumber`, `EncodeOracleDate`, `EncodeOracleTimestamp`, `OracleUint128` (`oracle_encoding.hpp`) in tests.
- Produces (namespace `oraduck`):
  - `struct OracleInt128 { uint64_t lo; int64_t hi; };` (two's complement, the layout of `duckdb::hugeint_t`)
  - `enum class NumberDecodeStatus { kOk, kOverflow, kInexact, kInfinity };`
  - `NumberDecodeStatus DecodeOracleNumberScaled64(const uint8_t *bytes, size_t len, uint8_t scale, bool round, int64_t &out);` value × 10^scale; `kInexact` when digits remain beyond `scale` and `round` is false; with `round`, half away from zero.
  - `NumberDecodeStatus DecodeOracleNumberScaled128(const uint8_t *bytes, size_t len, uint8_t scale, bool round, OracleInt128 &out);` same, up to 38 digits.
  - `std::string DecodeOracleNumberText(const uint8_t *bytes, size_t len);` canonical decimal text, no exponent, no trailing fractional zeros, leading `0` before the point; Oracle ±infinity → `"Infinity"` / `"-Infinity"`.
  - `int64_t DaysFromCivil(int64_t year, uint32_t month, uint32_t day);` proleptic Gregorian, astronomical year numbering, days since 1970-01-01.
  - `bool DecodeOracleDate(const uint8_t *bytes7, int64_t &micros);` microseconds since 1970-01-01; fields are taken as is (no Julian conversion); an Oracle BC year Y (negative) becomes astronomical year Y + 1; false on invalid bytes.
  - `bool DecodeOracleTimestamp(const uint8_t *bytes, size_t len, int64_t &micros);` `len` 7 or 11, nanoseconds truncated to microseconds.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("NUMBER reference bytes", "[decoding]") {
	const uint8_t zero[] = {0x80}, one[] = {0xC1, 0x02}, minus_one[] = {0x3E, 0x64, 0x66};
	const uint8_t v12345[] = {0xC2, 0x02, 0x18, 0x2E};             // 123.45
	const uint8_t m12345[] = {0x3D, 0x64, 0x4E, 0x38, 0x66};       // -123.45
	const uint8_t half[] = {0xC0, 0x33};                           // 0.5
	int64_t v = 0;
	CHECK(DecodeOracleNumberScaled64(zero, 1, 0, false, v) == NumberDecodeStatus::kOk); CHECK(v == 0);
	CHECK(DecodeOracleNumberScaled64(one, 2, 0, false, v) == NumberDecodeStatus::kOk); CHECK(v == 1);
	CHECK(DecodeOracleNumberScaled64(minus_one, 3, 0, false, v) == NumberDecodeStatus::kOk); CHECK(v == -1);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 2, false, v) == NumberDecodeStatus::kOk); CHECK(v == 12345);
	CHECK(DecodeOracleNumberScaled64(m12345, 5, 2, false, v) == NumberDecodeStatus::kOk); CHECK(v == -12345);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 1, false, v) == NumberDecodeStatus::kInexact);
	CHECK(DecodeOracleNumberScaled64(v12345, 4, 1, true, v) == NumberDecodeStatus::kOk); CHECK(v == 1235);
	CHECK(DecodeOracleNumberScaled64(m12345, 5, 1, true, v) == NumberDecodeStatus::kOk); CHECK(v == -1235);
	CHECK(DecodeOracleNumberText(half, 2) == "0.5");
	CHECK(DecodeOracleNumberText(m12345, 5) == "-123.45");
	CHECK(DecodeOracleNumberText(zero, 1) == "0");
}

TEST_CASE("NUMBER round trip with the encoder", "[decoding]") {
	// For each value: EncodeOracleNumber, then decode at the same scale; covers 0, ±1, ±(2^63 - 1),
	// 38-digit magnitudes (OracleUint128), scales 0, 2, 9 and 10, values below 1 (0.0000000001).
	// Expectations: Scaled64 returns kOverflow above int64, Scaled128 returns the exact value,
	// DecodeOracleNumberText returns the decimal literal used to build the case.
}

TEST_CASE("DATE and TIMESTAMP", "[decoding]") {
	const uint8_t d[] = {120, 124, 5, 7, 11, 21, 31};               // 2024-05-07 10:20:30
	int64_t micros = 0;
	CHECK(DecodeOracleDate(d, micros)); CHECK(micros == 1715077230000000LL);
	const uint8_t bc[] = {100, 99, 1, 1, 1, 1, 1};                   // Oracle -0001-01-01 = astronomical 0000-01-01
	CHECK(DecodeOracleDate(bc, micros)); CHECK(micros == -719528LL * 86400 * 1000000);
	CHECK(DaysFromCivil(1970, 1, 1) == 0);
	CHECK(DaysFromCivil(2024, 5, 7) == 19850);
	// Round trip through EncodeOracleTimestamp for 0001-01-01, 1969-12-31 23:59:59.999999, 9999-12-31
	// 23:59:59.999999, with 7- and 11-byte forms; an 11-byte value with 123456789 ns decodes to ...123456 µs.
}
```

The two round-trip cases are written as loops over a table of literals; their expectations are stated in the comments.

- [ ] **Step 2: Run them to verify they fail**

Run: `make unit`
Expected: compile error (`oracle_decoding.hpp` not found).

- [ ] **Step 3: Implement the functions in `src/oracle_decoding.cpp`; add the file to `PURE_SRCS` and `EXTENSION_SOURCES`.**

NUMBER layout (mirror of `EncodeOracleNumber`): byte 0 = sign and base-100 exponent (positive: `0xC1 + e`, negative: its bitwise complement), then base-100 digits stored `+1` (positive) or `101 - d` (negative, followed by `0x66` when shorter than 21 digits); `0x80` alone is zero; `{0xFF, 0x65}` is +infinity and `{0x00}` is −infinity.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make unit`
Expected: PASS (all test cases, including the existing ones).

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/oracle_decoding.hpp src/oracle_decoding.cpp test/cpp/test_oracle_decoding.cpp Makefile CMakeLists.txt
git commit -m "Add decoders for Oracle NUMBER, DATE and TIMESTAMP"
```

---

### Task 3: Read probe and recorded decisions

**Files:**
- Create: `tools/read_probe.cpp`
- Modify: `Makefile` (target `read-probe`, like `probe`), `docs/probe-results.md` (new section "Read path")

**Interfaces:**
- Consumes: `OciSession`, `OciApi` (Task 1), decoders (Task 2).
- Produces: a "Read path decisions" table at the end of `docs/probe-results.md` with exactly these keys, read by Tasks 4, 7 and 8:

| Key | Allowed values |
|---|---|
| `TIMESTAMP_DEFINE` | `internal180` (define type 180, 7/11 bytes) or `to_char` (`TO_CHAR(col, 'YYYY-MM-DD HH24:MI:SS.FF6')`, parsed) |
| `INTEGER_DEFINE` | `sqlt_num` (internal bytes + `DecodeOracleNumberScaled64`) or `sqlt_int` (OCI conversion to 8-byte integers) |
| `LOB_TECHNIQUE` | `dynamic_define` (`OCIDefineByPos2` + `OCI_DYNAMIC_FETCH`, `SQLT_LVC`/`SQLT_LVB`), `lob_prefetch` (locators with LOB prefetch), or `substr_locator` (`DBMS_LOB.SUBSTR` inline + locator only for larger values) |
| `FETCH_BUDGET_BYTES` | define-buffer budget per session |
| `PREFETCH_MEMORY_BYTES` | `OCI_ATTR_PREFETCH_MEMORY` per statement |
| `SNAPSHOT_DEFAULT` | `auto` or `none` |

- [ ] **Step 1: Write the probe** (tables built server-side with `INSERT /*+ APPEND */ … SELECT … FROM dual CONNECT BY LEVEL <= n`, timings with `std::chrono`, round trips from `V$MYSTAT` joined to `V$STATNAME` on `'SQL*Net roundtrips to/from client'`). Measurements, one per section:
  - **R1** NUMBER(18,0), 2M rows: `SQLT_NUM` + decoder against `SQLT_INT`.
  - **R2** TIMESTAMP(6), 2M rows: define type 180 (does `OCIDefineByPos` accept it, and do the bytes decode to the values `TO_CHAR` reports?) against `SQLT_TIMESTAMP` descriptors against `TO_CHAR` text.
  - **R3** A 550-column table shaped like the spec's workload (`VARCHAR2(4000 BYTE)` with 5–30 characters, `NUMBER(38,0)`, `NUMBER(25,9)`, `DATE`, `CHAR(1)`, `TIMESTAMP(6)`, unconstrained `NUMBER`), 200k rows: rows/s and round trips per 10,000 rows for budgets {16, 64, 256} MiB × prefetch memory {0, 4, 16, 64} MiB.
  - **R4** CLOBs, 100k rows, two distributions (all ≤ 4,000 bytes; 10 % of 100 KB): each of the three techniques; rows/s, round trips per 1,000 rows, and an exact check of values above 1 MB.
  - **R5** A LIST-partitioned 2M-row table, 4 sessions: ROWID ranges (built with `DBMS_ROWID.ROWID_CREATE` from `USER_EXTENTS`) against `ORA_HASH(ROWID, 3) = k` against one session; time of the extents query; time of a unit whose ROWID range lies in a partition pruned by the filter.
  - **R6** The same scan with and without `AS OF SCN`.
  - **R7** Unconstrained `NUMBER` through `SQLT_BDOUBLE`, 2M rows.

- [ ] **Step 2: Run it**

Run: `source env.sh && taskset -c 0-7 make read-probe | tee build/read-probe.txt`
Expected: every section prints a table; R2 and R4 print `OK` for their value checks.

- [ ] **Step 3: Record the results and the decisions** in `docs/probe-results.md`: the measured tables, then the decisions table. Rules: take the faster option unless it costs per-row or per-value round trips (more than 1 round trip per 100 rows for R4's small-CLOB distribution disqualifies a technique); choose `TIMESTAMP_DEFINE = to_char` only if type 180 is refused or decodes wrongly; choose `SNAPSHOT_DEFAULT = none` only if `AS OF SCN` costs more than 5 %.

- [ ] **Step 4: Commit**

```bash
git add tools/read_probe.cpp Makefile docs/probe-results.md
git commit -m "Add the read probe and record the fetch decisions"
```

---

### Task 4: Read type mapping

**Files:**
- Modify: `src/include/oraduck/oracle_names.hpp` (two fields of `OracleColumn`)
- Create: `src/include/oraduck/read_types.hpp`, `src/read_types.cpp`
- Modify: `Makefile`, `CMakeLists.txt`
- Test: `test/cpp/test_read_types.cpp`

**Interfaces:**
- Consumes: `OracleColumn` (fields below), `ub2` and `SQLT_*` (Task 1), the decisions of Task 3.
- Produces:
  - In `OracleColumn`: `uint32_t char_length = 0;` (ALL_TAB_COLUMNS.CHAR_LENGTH) and `bool national = false;` (CHARACTER_SET_NAME = 'NCHAR_CS'). Defaults leave the writer unchanged.
  - `enum class DuckTypeId { kSmallint, kInteger, kBigint, kDecimal, kFloat, kDouble, kVarchar, kTimestamp, kTimestampTz, kBlob };`
  - `struct DuckTypeSpec { DuckTypeId id; uint8_t width = 0; uint8_t scale = 0; };`
  - `enum class FetchDecoder { kNumberInt64, kNumberDecimal64, kNumberDecimal128, kNumberText, kNativeInt64, kNativeDouble, kNativeFloat, kString, kBinary, kDate, kTimestamp, kTimestampText, kUnsupported };`
  - `struct UnconstrainedNumber { enum class Kind { kDouble, kVarchar, kDecimal } kind = Kind::kDouble; uint8_t width = 0; uint8_t scale = 0; };`
  - `UnconstrainedNumber ParseUnconstrainedNumber(const std::string &text);` accepts `DOUBLE`, `VARCHAR`, `DECIMAL(p,s)` (case-insensitive, spaces allowed, 1 ≤ p ≤ 38, 0 ≤ s ≤ p); throws `OraduckError` naming the accepted forms.
  - `bool IsUtf8Charset(const std::string &charset);` true for `AL32UTF8` and `UTF8`.
  - `struct ReadColumnPlan { std::string name; std::string select_expr; DuckTypeSpec duck_type; FetchDecoder decoder; ub2 dty = 0; uint32_t define_bytes = 0; bool national = false; bool dynamic = false; uint8_t scale = 0; bool round = false; std::string unsupported; };`
  - `ReadColumnPlan PlanReadColumn(const OracleColumn &column, const UnconstrainedNumber &unconstrained, const std::string &db_charset);`

Mapping rules (spec 5.3; `select_expr` is the double-quoted column name unless stated):

| Oracle (`data_type`, precision p, scale s) | `duck_type` | `decoder` / `dty` / `define_bytes` |
|---|---|---|
| `NUMBER`, p in 1..4, s = 0 | SMALLINT | per `INTEGER_DEFINE`: `kNumberInt64`/`SQLT_NUM`/22 or `kNativeInt64`/`SQLT_INT`/8 |
| `NUMBER`, p in 5..9, s = 0 | INTEGER | same |
| `NUMBER`, p in 10..18, s = 0 | BIGINT | same |
| `NUMBER`, p ≤ 18, s in 1..p | DECIMAL(p,s) | `kNumberDecimal64`/`SQLT_NUM`/22 |
| `NUMBER`, 19 ≤ p ≤ 38, 0 ≤ s ≤ p | DECIMAL(p,s) | `kNumberDecimal128`/`SQLT_NUM`/22 |
| `NUMBER`, s < 0 | DECIMAL(p − s, 0) if p − s ≤ 38, else DOUBLE | decimal decoders at scale 0, or `kNativeDouble` |
| `NUMBER`, s > p | DECIMAL(s, s) | decimal decoders |
| `NUMBER`, precision NULL, scale 0 (`NUMBER(*,0)`, `INTEGER`) | DECIMAL(38,0) | `kNumberDecimal128` |
| `NUMBER` with precision and scale NULL, `FLOAT(b)` | per `unconstrained`: DOUBLE / VARCHAR / DECIMAL(p,s) | `kNativeDouble`/`SQLT_BDOUBLE`/8, `kNumberText`/`SQLT_NUM`/22, or decimal decoders with `round = true` |
| `BINARY_FLOAT` / `BINARY_DOUBLE` | FLOAT / DOUBLE | `kNativeFloat`/`SQLT_BFLOAT`/4, `kNativeDouble`/`SQLT_BDOUBLE`/8 |
| `DATE` | TIMESTAMP | `kDate`/`SQLT_DAT`/7 |
| `TIMESTAMP(n)` | TIMESTAMP | per `TIMESTAMP_DEFINE`: `kTimestamp`/180/11, or `kTimestampText`/`SQLT_CHR`/26 with `select_expr` `TO_CHAR("C", 'YYYY-MM-DD HH24:MI:SS.FF6')` |
| `TIMESTAMP(n) WITH TIME ZONE` | TIMESTAMPTZ | as `TIMESTAMP`, `select_expr` `SYS_EXTRACT_UTC("C")` |
| `TIMESTAMP(n) WITH LOCAL TIME ZONE` | TIMESTAMPTZ | as `TIMESTAMP`, `select_expr` `SYS_EXTRACT_UTC(CAST("C" AS TIMESTAMP WITH TIME ZONE))` |
| `VARCHAR2`, `CHAR` | VARCHAR | `kString`/`SQLT_CHR`; `define_bytes` = `data_length` when `IsUtf8Charset(db_charset)`, else `data_length × 3` |
| `NVARCHAR2`, `NCHAR` (`national`) | VARCHAR | `kString`/`SQLT_CHR`, `national = true`, `define_bytes` = `data_length × 3 / 2` (AL16UTF16 bytes → UTF-8) rounded up |
| `CLOB`, `NCLOB` | VARCHAR | `kString`, per `LOB_TECHNIQUE` (`dynamic = true` for `dynamic_define`) |
| `RAW` | BLOB | `kBinary`/`SQLT_BIN`/`data_length` |
| `BLOB` | BLOB | `kBinary`, per `LOB_TECHNIQUE` |
| `ROWID`, `UROWID` | VARCHAR | `kString`/`SQLT_CHR`/4000, `select_expr` `ROWIDTOCHAR("C")` (UROWID: `CAST("C" AS VARCHAR2(4000))`) |
| anything else (`LONG`, `LONG RAW`, `XMLTYPE`, `JSON`, `VECTOR`, `BFILE`, `INTERVAL …`, object and collection types) | VARCHAR | `kUnsupported`, `unsupported` = `"column \"C\" has Oracle type T, which oraduck_scan does not read"` |

INTERVAL types are deferred (the spec allows it); they take the last row.

- [ ] **Step 1: Write the failing tests**: one `SECTION` per row of the table, building an `OracleColumn` and checking `duck_type`, `decoder`, `dty`, `define_bytes` and `select_expr`; plus:

```cpp
TEST_CASE("ParseUnconstrainedNumber", "[read_types]") {
	CHECK(ParseUnconstrainedNumber("double").kind == UnconstrainedNumber::Kind::kDouble);
	CHECK(ParseUnconstrainedNumber("VARCHAR").kind == UnconstrainedNumber::Kind::kVarchar);
	auto d = ParseUnconstrainedNumber(" decimal( 20 , 4 ) ");
	CHECK(d.kind == UnconstrainedNumber::Kind::kDecimal); CHECK(d.width == 20); CHECK(d.scale == 4);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("DECIMAL(39,0)"), OraduckError);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("DECIMAL(5,6)"), OraduckError);
	CHECK_THROWS_AS(ParseUnconstrainedNumber("BIGINT"), OraduckError);
}
```

- [ ] **Step 2: Run them to verify they fail**

Run: `make unit`
Expected: compile error (`read_types.hpp` not found).

- [ ] **Step 3: Implement `PlanReadColumn`, `ParseUnconstrainedNumber` and `IsUtf8Charset` in `src/read_types.cpp`** (parse `data_type` strings as `ALL_TAB_COLUMNS` reports them: `TIMESTAMP(6)`, `TIMESTAMP(6) WITH TIME ZONE`, …); add the file to `PURE_SRCS` and `EXTENSION_SOURCES`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make unit`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/oracle_names.hpp src/include/oraduck/read_types.hpp src/read_types.cpp test/cpp/test_read_types.cpp Makefile CMakeLists.txt
git commit -m "Map Oracle columns to DuckDB types and fetch plans"
```

---

### Task 5: Binds, filter translation, scan SQL and ROWID ranges

**Files:**
- Create: `src/include/oraduck/sql_binds.hpp`, `src/sql_binds.cpp`, `src/include/oraduck/filter_sql.hpp`, `src/filter_sql.cpp`, `src/include/oraduck/read_sql.hpp`, `src/read_sql.cpp`, `src/include/oraduck/rowid_ranges.hpp`, `src/rowid_ranges.cpp`
- Modify: `Makefile`, `CMakeLists.txt`
- Test: `test/cpp/test_filter_sql.cpp`, `test/cpp/test_read_sql.cpp`, `test/cpp/test_rowid_ranges.cpp`

**Interfaces:**
- Consumes: encoders (`oracle_encoding.hpp`), `OracleInt128` (Task 2), `OracleColumn`, `ReadColumnPlan`, `IsUtf8Charset` (Task 4).
- Produces in `sql_binds.hpp`:
  - `struct BindValue { ub2 dty; std::vector<uint8_t> bytes; std::string display; };` (`display`: SQL literal for EXPLAIN)
  - `struct SqlFragment { std::string sql; std::vector<BindValue> binds; std::string display; };` placeholders are written `:b1`, `:b2`, …; binds are positional, in textual order (OCI binds by position of appearance).
  - `struct SqlStatement { std::string sql; std::vector<BindValue> binds; };`
  - `BindValue BindNumber(int64_t value);`, `BindValue BindDecimal(const OracleInt128 &value, uint8_t scale);` (`SQLT_NUM`), `BindValue BindText(const std::string &value);` (`SQLT_CHR`), `BindValue BindDate(int64_t micros);` (`SQLT_DAT`, 7 bytes), `BindValue BindDouble(double value);` (`SQLT_BDOUBLE`).
- Produces in `filter_sql.hpp`:
  - `struct FilterConstant { enum class Kind { kInteger, kDecimal, kDouble, kString, kTimestamp, kDate } kind; int64_t i64 = 0; OracleInt128 i128 {}; uint8_t scale = 0; double f64 = 0; std::string text; };` (`kTimestamp`: `i64` = microseconds since epoch; `kDate`: `i64` = days since epoch)
  - `enum class FilterOp { kAnd, kOr, kEq, kNe, kLt, kLe, kGt, kGe, kIn, kIsNull, kIsNotNull, kDateEq };` (`kDateEq`: `CAST(col AS DATE) = d`)
  - `struct FilterNode { FilterOp op; size_t column = 0; std::vector<FilterConstant> constants; std::vector<FilterNode> children; };` (`column` indexes `OracleTableDescription::columns`)
  - `std::optional<SqlFragment> TranslateFilter(const FilterNode &node, const std::vector<OracleColumn> &columns, const std::string &db_charset);` `std::nullopt` when any part is not pushable; the whole node or nothing.
- Produces in `read_sql.hpp`:
  - `enum class SplitStrategy { kAuto, kExtents, kHash, kNone };` `SplitStrategy ParseSplitStrategy(const std::string &);`
  - `enum class SnapshotMode { kAuto, kScn, kNone };` `SnapshotMode ParseSnapshotMode(const std::string &);` (both case-insensitive, throw `OraduckError` listing the accepted values)
  - `enum class SliceKind { kNone, kRowidRange, kHash };`
  - `struct Slice { SliceKind kind = SliceKind::kNone; std::string rowid_lo, rowid_hi; uint32_t hash_buckets = 0, hash_bucket = 0; };` (ROWID range is half-open `[lo, hi)`)
  - `struct ScanSqlInput { std::string owner, object; std::vector<const ReadColumnPlan *> columns; std::vector<SqlFragment> filters; std::string snapshot_scn; };`
  - `std::string QuoteIdentifier(const std::string &name);` → `"NAME"` (dictionary names are used as is).
  - `SqlStatement BuildScanSql(const ScanSqlInput &input, const Slice &slice);`
- Produces in `rowid_ranges.hpp`:
  - `std::string EncodeRowid(uint32_t data_object_id, uint32_t relative_fno, uint32_t block, uint16_t row);` extended ROWID `OOOOOOFFFBBBBBBRRR` in base 64 (`A–Z a–z 0–9 + /`); when `relative_fno == 1024` (bigfile tablespace), the file field holds `block >> 22` and the block field `block & 0x3FFFFF`.
  - `struct SegmentExtent { uint32_t data_object_id; uint32_t relative_fno; uint32_t block_id; uint32_t blocks; };`
  - `uint64_t TargetBlocks(uint64_t total_blocks, uint32_t sessions);` = `max(ceil(total_blocks / (4 × sessions)), 128)`.
  - `std::vector<Slice> BuildRowidSlices(std::vector<SegmentExtent> extents, uint64_t target_blocks);`

Translation rules (spec 4.3; one unit test per bullet):
- Comparisons and `IN` bind constants typed like the Oracle column: `NUMBER` with precision → `BindNumber`/`BindDecimal`; `BINARY_FLOAT`/`BINARY_DOUBLE` → `BindDouble`; `DATE` → `BindDate` (constant with a fractional second: not pushed); `TIMESTAMP(n)` with n ≤ 6 → `TO_TIMESTAMP(:bN, 'YYYY-MM-DD HH24:MI:SS.FF6')` with a text bind (n > 6: not pushed); `VARCHAR2`, `NVARCHAR2`, `CHAR`, `NCHAR` → `BindText` (non-padded semantics).
- Not pushed: a constant whose kind does not match the column family; the empty string; `NUMBER` without precision; `TIMESTAMP WITH (LOCAL) TIME ZONE`; LOB columns except `IS NULL`/`IS NOT NULL`; string ranges (`<`, `<=`, `>`, `>=`) when `!IsUtf8Charset(db_charset)` or on national columns; `IN` lists with a constant that is not pushable; `kAnd`/`kOr` with any child not pushable; unsupported column types.
- `kDateEq` on a `DATE` column → `("C" >= :b1 AND "C" < :b2 + 1)` with `BindDate(d)` twice.
- Fragments are parenthesized; `display` inlines the literals (`DATE '2024-05-07'`, `'abc'`, `20240507`).

SQL shape of `BuildScanSql` (binds in this order: snapshot, filters, slice):

```
SELECT /* oraduck_scan */ <select_expr, …> FROM "OWNER"."OBJECT"[ AS OF SCN TO_NUMBER(:b1)][ WHERE <f1> AND <f2> … [AND <slice>]]
```

with `<select_expr, …>` = `1` when `columns` is empty; slice `ROWID >= CHARTOROWID(:bN) AND ROWID < CHARTOROWID(:bM)` or `ORA_HASH(ROWID, :bN) = :bM` (binds `hash_buckets - 1` and `hash_bucket`).

`BuildRowidSlices`: group extents by `data_object_id`; per object, sort by `(relative_fno, block_id)` and cut a new unit when the accumulated blocks reach `target_blocks`; unit boundaries are `EncodeRowid(obj, 0, 0, 0)` for the first unit of an object, `EncodeRowid(obj, fno, block_id, 0)` of the extent that opens each next unit, and `EncodeRowid(obj + 1, 0, 0, 0)` after the last; consecutive units share their boundary (`hi` of one = `lo` of the next), so the units cover every row of the object exactly once. Largest units first.

- [ ] **Step 1: Write the failing tests**, including:

```cpp
TEST_CASE("EncodeRowid", "[rowid]") {
	CHECK(EncodeRowid(0, 0, 0, 0) == "AAAAAAAAAAAAAAAAAA");
	CHECK(EncodeRowid(1, 1, 1, 1) == "AAAAABAABAAAAABAAB");
	CHECK(EncodeRowid(0xFFFFFFFFu, 1023, 0x3FFFFF, 65535) == "D/////AP/AAP///P//");
	CHECK(EncodeRowid(7, 1024, (5u << 22) | 9u, 0) == "AAAAAHAAFAAAAAJAAA"); // bigfile: block 5·2^22 + 9
}

TEST_CASE("BuildRowidSlices covers each object once", "[rowid]") {
	// Two objects (7: three extents in files 4 and 5; 9: one extent), target 100 blocks:
	// expect object 7 split where its accumulated blocks reach 100, units of one object chained
	// (hi of unit k == lo of unit k+1), first lo == EncodeRowid(7, 0, 0, 0), last hi == EncodeRowid(8, 0, 0, 0),
	// object 9 in a single unit [EncodeRowid(9,0,0,0), EncodeRowid(10,0,0,0)); no extents -> no slices.
	CHECK(BuildRowidSlices({}, 128).empty());
	CHECK(TargetBlocks(100000, 8) == 3125);
	CHECK(TargetBlocks(100, 8) == 128);
}

TEST_CASE("Partition key filters are pushed", "[filter_sql]") {
	// columns: 0 PART_DATE DATE, 1 SUB_KEY VARCHAR2(8), 2 ID NUMBER(38,0), 3 AMOUNT NUMBER (no precision),
	//          4 FLAG CHAR(1), 5 NOTE CLOB, 6 NAME NVARCHAR2(20)
	auto f = TranslateFilter({FilterOp::kEq, 0, {Timestamp(2024, 5, 7)}}, Columns(), "AL32UTF8");
	REQUIRE(f); CHECK(f->sql == "(\"PART_DATE\" = :b1)"); CHECK(f->binds[0].dty == SQLT_DAT);
	CHECK(f->display == "(\"PART_DATE\" = DATE '2024-05-07')");
	f = TranslateFilter({FilterOp::kDateEq, 0, {Date(2024, 5, 7)}}, Columns(), "AL32UTF8");
	REQUIRE(f); CHECK(f->sql == "(\"PART_DATE\" >= :b1 AND \"PART_DATE\" < :b2 + 1)");
	f = TranslateFilter({FilterOp::kIn, 1, {Text("A"), Text("B")}}, Columns(), "AL32UTF8");
	REQUIRE(f); CHECK(f->sql == "(\"SUB_KEY\" IN (:b1, :b2))");
	CHECK(TranslateFilter({FilterOp::kGt, 1, {Text("A")}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kGt, 1, {Text("A")}}, Columns(), "WE8MSWIN1252"));
	CHECK_FALSE(TranslateFilter({FilterOp::kNe, 1, {Text("")}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kEq, 3, {Integer(1)}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kEq, 0, {TimestampMicros(1715077230000001)}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kEq, 5, {Text("x")}}, Columns(), "AL32UTF8"));
	CHECK(TranslateFilter({FilterOp::kIsNull, 5, {}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kGt, 6, {Text("a")}}, Columns(), "AL32UTF8"));
	CHECK(TranslateFilter({FilterOp::kEq, 4, {Text("Y")}}, Columns(), "AL32UTF8"));
	CHECK_FALSE(TranslateFilter({FilterOp::kEq, 1, {Integer(1)}}, Columns(), "AL32UTF8"));
	// kOr with one child not pushable -> nullopt; kAnd of two pushable children -> "(… AND …)" with :b1, :b2
}

TEST_CASE("BuildScanSql", "[read_sql]") {
	// Columns "ID" (NUMBER(38,0)) and "Order" (VARCHAR2), one filter fragment, snapshot "123", hash slice 2 of 4:
	CHECK(sql.sql == "SELECT /* oraduck_scan */ \"ID\", \"Order\" FROM \"BENCH\".\"T\" AS OF SCN TO_NUMBER(:b1) "
	                 "WHERE (\"ID\" = :b2) AND ORA_HASH(ROWID, :b3) = :b4");
	CHECK(sql.binds.size() == 4);
	// No column: "SELECT /* oraduck_scan */ 1 FROM …"; no filter and no slice: no WHERE; ROWID slice:
	// "ROWID >= CHARTOROWID(:bN) AND ROWID < CHARTOROWID(:bM)".
}
```

(`Columns()`, `Timestamp()`, `Date()`, `Text()`, `Integer()`, `TimestampMicros()` are small helpers in the test file.)

- [ ] **Step 2: Run them to verify they fail**

Run: `make unit`
Expected: compile errors (headers not found).

- [ ] **Step 3: Implement the four files; add them to `PURE_SRCS` and `EXTENSION_SOURCES`.**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `make unit`
Expected: PASS.

- [ ] **Step 5: Cross-check `EncodeRowid` against Oracle**: add to `test/cpp/itest_session.cpp` a test that compares `EncodeRowid(o, f, b, r)` with `DBMS_ROWID.ROWID_CREATE(1, o, f, b, r)` for smallfile values, and, on a row of a table in the bigfile `BENCH` tablespace, rebuilds its ROWID from `DBMS_ROWID.ROWID_OBJECT`, `ROWID_BLOCK_NUMBER(rowid, 'BIGFILE')` and `ROWID_ROW_NUMBER` with `relative_fno = 1024`.

Run: `source env.sh && make itest`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/include/oraduck/sql_binds.hpp src/sql_binds.cpp src/include/oraduck/filter_sql.hpp src/filter_sql.cpp src/include/oraduck/read_sql.hpp src/read_sql.cpp src/include/oraduck/rowid_ranges.hpp src/rowid_ranges.cpp test/cpp Makefile CMakeLists.txt
git commit -m "Translate filters to Oracle SQL and build scan statements and ROWID ranges"
```

---

### Task 6: Source description and read session

**Files:**
- Create: `src/include/oraduck/describe_source.hpp`, `src/describe_source.cpp`
- Modify: `Makefile` (`OCI_SRCS`), `CMakeLists.txt`
- Test: `test/cpp/itest_describe_source.cpp`

**Interfaces:**
- Consumes: `OciSession::QueryStrings`, `OciSession::Execute`, `TargetName`, `OracleColumn` (with Task 4's fields).
- Produces:
  - `enum class SourceKind { kTable, kView };`
  - `struct OracleTableDescription { std::string owner, name; SourceKind kind = SourceKind::kTable; bool partitioned = false; int64_t num_rows = -1; bool owned_by_session_user = false; std::string db_charset, nchar_charset; std::vector<OracleColumn> columns; };`
  - `OracleTableDescription DescribeSource(OciSession &session, const TargetName &name);`
  - `void PrepareReadSession(OciSession &session);` runs `ALTER SESSION SET NLS_COMP = 'BINARY' NLS_SORT = 'BINARY'`.

Behaviour: empty owner → `SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA')`; object looked up in `ALL_OBJECTS` among `TABLE`, `VIEW`, `MATERIALIZED VIEW` (treated as a table), `SYNONYM`; a synonym (private, then `PUBLIC` when the owner was implicit) is followed through `ALL_SYNONYMS` up to 8 levels (a loop or a `DB_LINK` is an error); columns from `ALL_TAB_COLUMNS` (`column_name, data_type, data_length, NVL(data_precision,-1), NVL(data_scale,-1), nullable, char_length, character_set_name`, ordered by `column_id`); tables: `partitioned` and `num_rows` from `ALL_TABLES`; charsets from `NLS_DATABASE_PARAMETERS`; `owned_by_session_user` = owner equals `USER`. Missing object → `OraduckError("Oracle table or view not found or not accessible: OWNER.NAME")`.

- [ ] **Step 1: Write the failing integration tests**: a table with every type of spec 5.3 (check `data_type`, `precision`/`scale` sentinels for `NUMBER`, `NUMBER(*,0)`, `NUMBER(25,9)`, `char_length`, `national` for `NVARCHAR2`); a LIST/LIST partitioned table (`partitioned == true`, `num_rows` after `DBMS_STATS.GATHER_TABLE_STATS`); a view (`kind == kView`); a private synonym and a public synonym resolving to the table; a synonym loop (error); a missing table (error text above); `db_charset == "AL32UTF8"`, `nchar_charset == "AL16UTF16"`; `PrepareReadSession` then `SELECT value FROM nls_session_parameters WHERE parameter = 'NLS_COMP'` returns `BINARY`.

- [ ] **Step 2: Run them to verify they fail**

Run: `source env.sh && make itest`
Expected: compile error (`describe_source.hpp` not found).

- [ ] **Step 3: Implement `DescribeSource` and `PrepareReadSession`; add the file to `OCI_SRCS` and `EXTENSION_SOURCES`.**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `source env.sh && make itest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/describe_source.hpp src/describe_source.cpp test/cpp/itest_describe_source.cpp Makefile CMakeLists.txt
git commit -m "Describe Oracle tables, views and synonyms for reading"
```

---

### Task 7: Fetch cursor

**Files:**
- Create: `src/include/oraduck/fetch_cursor.hpp`, `src/fetch_cursor.cpp`
- Modify: `Makefile` (`OCI_SRCS`), `CMakeLists.txt`
- Test: `test/cpp/itest_fetch_cursor.cpp`

**Interfaces:**
- Consumes: `OciSession`, `SqlStatement`, `BindValue` (Task 5), `ReadColumnPlan` (Task 4), the decisions of Task 3, decoders (Task 2) in tests.
- Produces:
  - `struct FetchOptions { uint64_t buffer_budget_bytes; uint32_t prefetch_memory_bytes; uint32_t max_rows = 2048; };` defaults from `FETCH_BUDGET_BYTES` and `PREFETCH_MEMORY_BYTES`.
  - `class FetchCursor` (non-copyable, RAII on the statement):
    - `FetchCursor(OciSession &session, const SqlStatement &statement, std::vector<const ReadColumnPlan *> columns, const FetchOptions &options);` prepares, binds, defines and executes.
    - `uint32_t Fetch();` next batch; returns its row count, 0 at the end.
    - `uint32_t rows_per_fetch() const;` = clamp(budget / Σ(define_bytes + 8), 1, max_rows).
    - `const uint8_t *Value(size_t column, uint32_t row, uint32_t &length) const;` bytes of a value in the last batch, `nullptr` for NULL; LOB values are contiguous whatever `LOB_TECHNIQUE` does internally.
  - Columns with `national` set `OCI_ATTR_CHARSET_FORM = SQLCS_NCHAR` on their define. Unsupported columns never reach the cursor (the glue rejects them first).

- [ ] **Step 1: Write the failing integration tests**:
  - every type of spec 5.3 read back and decoded with the Task 2 decoders, compared with `TO_CHAR`/`DUMP` values from Oracle: NULLs in every column, multi-byte UTF-8 (`é`, `漢字`, emoji), `NVARCHAR2`, `CHAR(3)` value `'A  '` kept padded;
  - CLOBs: NULL, `EMPTY_CLOB()` (length 0, not NULL), 10 bytes, 5,000 bytes, 2 MB with multi-byte characters at the piece boundaries, compared byte for byte;
  - a 550-column table (200 `VARCHAR2(4000)` columns) with a 16 MiB budget: `rows_per_fetch()` ≥ 16 and all rows read;
  - round trips: reading 100,000 rows of a table with 5 small CLOB columns costs at most 1,000 `SQL*Net roundtrips to/from client` (`V$MYSTAT` delta);
  - an ORA error during the fetch (view `SELECT id, 1 / (id - 50000) AS x FROM t`, 100,000 rows) throws `OracleError` with `ora_code == 1476`.

- [ ] **Step 2: Run them to verify they fail**

Run: `source env.sh && make itest`
Expected: compile error (`fetch_cursor.hpp` not found).

- [ ] **Step 3: Implement `FetchCursor`**: `OCIStmtPrepare2`, binds with `OCIBindByPos` in order, `OCI_ATTR_PREFETCH_ROWS = 0` and `OCI_ATTR_PREFETCH_MEMORY`, one define per column (array of `rows_per_fetch` values, indicators `sb2`, lengths `ub4` through `OCIDefineByPos2`), LOBs per `LOB_TECHNIQUE`, `OCIStmtExecute(iters = 0)`, then `OCIStmtFetch2(rows_per_fetch)` with the row count from `OCI_ATTR_ROWS_FETCHED` (rows may come with `OCI_NO_DATA`). Add the file to `OCI_SRCS` and `EXTENSION_SOURCES`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `source env.sh && make itest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/fetch_cursor.hpp src/fetch_cursor.cpp test/cpp/itest_fetch_cursor.cpp Makefile CMakeLists.txt
git commit -m "Add the array fetch cursor with byte-sized prefetching and inline LOBs"
```

---

### Task 8: Work planning: snapshot and split strategies

**Files:**
- Create: `src/include/oraduck/work_units.hpp`, `src/work_units.cpp`
- Modify: `Makefile` (`OCI_SRCS`), `CMakeLists.txt`
- Test: `test/cpp/itest_work_units.cpp`

**Interfaces:**
- Consumes: `OciSession`, `OracleTableDescription` (Task 6), `SplitStrategy`, `SnapshotMode`, `Slice`, `BuildScanSql` (Task 5), `SegmentExtent`, `BuildRowidSlices`, `TargetBlocks` (Task 5), `FetchCursor` (Task 7) in tests.
- Produces:
  - `struct WorkPlan { SplitStrategy strategy; std::vector<Slice> slices; std::string scn; std::string notes; };` (`strategy` is never `kAuto`; `scn` empty = no `AS OF`; `notes` explains fallbacks, e.g. `"extents: ORA-00942, using hash"`)
  - `std::string ReadCurrentScn(OciSession &session);` tries `DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER`, then `V$DATABASE.CURRENT_SCN`, then `TIMESTAMP_TO_SCN(SYSTIMESTAMP)`; returns `""` when all fail.
  - `WorkPlan PlanWork(OciSession &session, const OracleTableDescription &source, SplitStrategy split, SnapshotMode snapshot, uint32_t sessions);`

Behaviour:
- Snapshot: `kNone` → no SCN. Otherwise read the SCN and check it with `SELECT 1 FROM <object> AS OF SCN TO_NUMBER(:b1) WHERE ROWNUM < 1`; on failure `kScn` throws `OracleError` with Oracle's message, `kAuto` leaves `scn` empty and adds a note.
- `sessions == 1` → `kNone`, one slice, no dictionary query.
- `kExtents` (or first step of `kAuto` for tables): segments and extents from `DBA_EXTENTS` joined to `ALL_OBJECTS` (`data_object_id`, matching `subobject_name` to `partition_name`, segment types `TABLE`, `TABLE PARTITION`, `TABLE SUBPARTITION`), or from `USER_EXTENTS`/`USER_OBJECTS` when `owned_by_session_user`; slices from `BuildRowidSlices(extents, TargetBlocks(total, sessions))`. No extents → zero slices (empty table). Failure: `kExtents` throws `OraduckError("split 'extents' needs read access to DBA_EXTENTS or ownership of the table: …")`; `kAuto` notes and goes on.
- `kHash` (second step of `kAuto`, first step for views): check `SELECT 1 FROM <object> WHERE ORA_HASH(ROWID, 1) = 0 AND ROWNUM < 1`; slices: `sessions` buckets. Failure (ORA-01445 on a join view): `kHash` throws, `kAuto` goes to `kNone`.
- `kNone`: one slice.
- The plan is computed once per query execution (the glue calls it from its global init), never cached in bind data: ROWID ranges go stale after `TRUNCATE` or new partitions.

- [ ] **Step 1: Write the failing integration tests**, on a LIST-partitioned (`PART_DATE` DATE) and LIST-subpartitioned (`SUB_KEY` VARCHAR2(8)) table of 300,000 rows with a unique `ID`, created once in the bigfile `BENCH` tablespace and once in `BENCH_SMALL`:
  - for `kExtents`, `kHash` and `kNone` with 4 sessions: reading every slice with a `FetchCursor` on `SELECT ID` gives 300,000 rows, 300,000 distinct IDs and the same `SUM(ID)` as Oracle;
  - `kAuto` for `BENCH` (owner) → `kExtents`; for `READER` (granted `SELECT` only) → `kHash` with a note naming the extents error; forced `kExtents` for `READER` → throws;
  - a simple view → `kHash`; a join view → `kNone` under `kAuto`, throws under `kHash`;
  - an empty table (deferred segment) under `kExtents` → zero slices;
  - snapshot: `BENCH` gets a non-empty SCN; `READER` with `kAuto` gets `""` and a note, with `kScn` throws;
  - `sessions == 1` → one `kNone` slice.

- [ ] **Step 2: Run them to verify they fail**

Run: `source env.sh && make itest`
Expected: compile error (`work_units.hpp` not found).

- [ ] **Step 3: Implement `ReadCurrentScn` and `PlanWork`; add the file to `OCI_SRCS` and `EXTENSION_SOURCES`.**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `source env.sh && make itest`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck/work_units.hpp src/work_units.cpp test/cpp/itest_work_units.cpp Makefile CMakeLists.txt
git commit -m "Plan parallel reads: ROWID extents, ROWID hash or a single session, with an SCN snapshot"
```

---

### Task 9: `oraduck_scan` table function (projection pushdown, parallel scan)

**Files:**
- Create: `src/include/oraduck_scan.hpp`, `src/oraduck_scan.cpp`, `src/include/scan_vectors.hpp`, `src/scan_vectors.cpp`
- Modify: `src/oraduck_extension.cpp` (register), `CMakeLists.txt`
- Test: `test/sql/oraduck_scan.test`, `tests/e2e/oraduck_helpers.py`, `tests/e2e/test_scan_types.py`, `tests/e2e/test_scan_parallel.py`, `tests/e2e/test_scan_errors.py`

**Interfaces:**
- Consumes: everything from Tasks 4–8; `GetOraduckCredentials` (`oraduck_secret.hpp`).
- Produces (namespace `duckdb`):
  - `struct ScanOptions { uint32_t sessions = 0; oraduck::SplitStrategy split = oraduck::SplitStrategy::kAuto; oraduck::UnconstrainedNumber unconstrained; oraduck::SnapshotMode snapshot = oraduck::SnapshotMode::kAuto; };` (`sessions == 0` → the number of DuckDB threads)
  - `void RegisterOraduckScan(ExtensionLoader &loader);`
  - `TableFunction GetOraduckScanFunction();`
  - `unique_ptr<FunctionData> BindOraduckScan(ClientContext &context, oraduck::OracleCredentials credentials, oraduck::OracleTableDescription source, const ScanOptions &options, vector<LogicalType> &return_types, vector<Identifier> &names);` the seam a future `ATTACH` uses.
  - `struct OraduckScanBindData : public TableFunctionData` with `credentials`, `source`, `std::vector<oraduck::ReadColumnPlan> columns` (one per Oracle column), `options`, `std::vector<oraduck::SqlFragment> filters` (filled by Task 10).
  - In `scan_vectors.hpp`: `LogicalType ToLogicalType(const oraduck::DuckTypeSpec &spec);` and `void WriteColumn(const oraduck::FetchCursor &cursor, size_t cursor_column, const oraduck::ReadColumnPlan &plan, uint32_t first_row, uint32_t count, Vector &out, idx_t out_offset);` (decodes `count` rows of the batch into `out` at `out_offset`; decoder errors → `IOException` naming the column).

Behaviour:
- Signature `oraduck_scan(VARCHAR)`, named parameters `connection` (VARCHAR, required), `sessions` (INTEGER, 1..1024), `split`, `unconstrained_number`, `snapshot` (VARCHAR). Missing `connection` → `BinderException("OraDuck: the connection parameter is required (name of an oraduck secret)")`.
- Bind: parse the name (`ParseTargetName`), open one session, `PrepareReadSession`, `DescribeSource`, plan every column; names are the Oracle names, deduplicated case-insensitively in column order by appending `_1`, `_2`, … (`ID`, `Id_1`).
- `projection_pushdown = true`, `filter_pushdown = false`, `filter_prune = false`; `cardinality` from `num_rows` when known.
- Global init (once per execution): projected columns from `input.column_indexes` (ids outside the Oracle columns, such as the empty-column id of `count(*)`, mean no column); a projected unsupported column → `BinderException` with the plan's `unsupported` text; one planning session runs `PrepareReadSession` and `PlanWork(…, sessions)`; `MaxThreads()` = max(1, min(sessions, slices)).
- Scan (per thread): opens its own session lazily (so sessions open in parallel), runs `PrepareReadSession`, takes slices from an atomic index, builds the statement with `BuildScanSql`, fetches with `FetchCursor` and fills the output chunk up to `STANDARD_VECTOR_SIZE` rows across fetches and slices; with no projected column, it only counts rows (`output.SetCardinality`).
- `to_string` (EXPLAIN): `Table` (`OWNER.NAME`), `Columns` (projected names), `Oracle WHERE` (filter `display` texts, or `—`), `Split`, `Snapshot` (requested values).
- `get_metrics` (EXPLAIN ANALYZE extra info, *Claude's call*: the work plan exists only at execution): `Strategy`, `Units`, `Sessions`, `Snapshot` (SCN or `per session`), `Notes`, `Planning (ms)` (time of `PlanWork`, for the spec's under-2 % target), `Oracle SQL` (statement of the first unit, binds as their `display`).
- A snapshot fallback (`SnapshotMode::kAuto` without `AS OF`) also writes a DuckDB log entry at level WARNING with the note.
- Errors during the scan → `IOException("OraDuck: " + what)`; sessions are owned by the local states and closed when the query ends or fails.

- [ ] **Step 1: Write the failing tests**

`test/sql/oraduck_scan.test` (no Oracle):

```
require oraduck

statement error
SELECT * FROM oraduck_scan('BENCH.T');
----
connection parameter is required

statement error
SELECT * FROM oraduck_scan('BENCH.T', connection := 'absent');
----
not found

statement error
SELECT * FROM oraduck_scan('BENCH.T', connection := 'absent', split := 'random');
----
<REGEX>:.*split.*auto.*extents.*hash.*none.*
```

(The option values are checked before the secret is fetched.)

In `tests/e2e/oraduck_helpers.py`, add `scan_json(sql: str, setup: str = "") -> list[dict]` (runs the CLI with `-json` after `secret_sql()`, returns the last JSON array of stdout, `parse_float=Decimal`; raises with stderr on failure) and `scan_error(sql: str) -> str`.

End-to-end tests (all values compared as `CAST(col AS VARCHAR)` or `strftime`):
- `test_scan_types.py::test_every_type_round_trip` — one row per case of spec 5.3 inserted with python-oracledb, read with `oraduck_scan`, expected DuckDB text per case (e.g. `NUMBER(25,9)` `1234567890123456.123456789`, `NUMBER(38,0)` `12345678901234567890123456789012345678`, `DATE` `2024-05-07 10:20:30`, `TIMESTAMP(9)` `…123456`, `TIMESTAMP WITH TIME ZONE` `+02:00` → the UTC instant, `CHAR(3)` `'A  '`, `RAW` → `\xDE\xAD`); dates 0001-01-01, 1582-10-04, 1582-10-15, 1969-12-31 23:59:59, 9999-12-31 23:59:59 match Oracle's `TO_CHAR(col, 'YYYY-MM-DD HH24:MI:SS')`; `typeof(col)` matches spec 5.3; `unconstrained_number := 'VARCHAR'` and `'DECIMAL(20,4)'` on an unconstrained `NUMBER`.
- `test_scan_types.py::test_empty_clob_is_not_null` — `EMPTY_CLOB()` → `''`, NULL → NULL (Review Focus 1).
- `test_scan_types.py::test_colliding_and_keyword_names` — columns `"Id"`, `ID`, `"ORDER"`: DuckDB names `Id`, `ID_1` (order of `column_id`), `ORDER`; all readable (Review Focus 2).
- `test_scan_types.py::test_unsupported_column_only_when_selected` — table with an `XMLTYPE` column: `SELECT id …` works, `SELECT *` fails naming the column and `XMLTYPE`.
- `test_scan_types.py::test_projection_pushdown` — `EXPLAIN ANALYZE SELECT a FROM oraduck_scan(…)` shows `Oracle SQL` selecting only `"A"`.
- `test_scan_parallel.py::test_split_strategies_agree` — 1,000,000 rows, `SET threads = 8`: `count(*)`, `sum(id)`, `count(DISTINCT id)` identical for `split := 'extents'`, `'hash'`, `'none'`, and equal to Oracle's; EXPLAIN ANALYZE shows the forced strategy and, for extents, `Units` ≥ 8.
- `test_scan_parallel.py::test_reader_falls_back_to_hash` — secret with `READER`: `Strategy` = `hash` and a note; `Snapshot` = `per session`.
- `test_scan_parallel.py::test_two_scans_in_one_query` — self-join on `id` of the same table through two `oraduck_scan` calls returns 1,000,000 rows (Review Focus 4).
- `test_scan_parallel.py::test_nothing_to_read` — empty table (`count(*)` = 0, `SELECT *` returns no rows with the right column names), `count(*)` on 1,000,000 rows = 1,000,000 (Review Focus 5).
- `test_scan_parallel.py::test_prepared_statement_after_truncate` — `PREPARE q AS SELECT count(*) FROM oraduck_scan(…)`; `EXECUTE q` → n; Oracle `TRUNCATE` + insert m rows; `EXECUTE q` → m.
- `test_scan_errors.py::test_error_mid_scan_releases_sessions` — view raising ORA-01476 at row 50,000 of 1,000,000: the CLI (without `-bail`) prints `ORA-01476`, then runs `.shell sleep 3`; meanwhile `V$SESSION` has no session whose `PROCESS` is the CLI's PID (Review Focus 3).
- `test_scan_errors.py::test_interrupt_releases_sessions` — `SET max_execution_time = 1000` on a 7,000,000-row scan: interrupted; same `V$SESSION` check.
- `test_scan_errors.py::test_bind_errors` — missing table (message with `not found or not accessible`), wrong password (`ORA-01017`), `sessions := 0`, `unconstrained_number := 'BIGINT'`, `snapshot := 'scn'` as `READER` (`ORA-01031`).

- [ ] **Step 2: Run them to verify they fail**

Run: `GEN=ninja make release && make test && source env.sh && uv run pytest tests/e2e -q -k scan`
Expected: `make test` fails on `oraduck_scan.test` (function does not exist); the e2e tests fail the same way.

- [ ] **Step 3: Implement `scan_vectors.cpp` (one writer per `FetchDecoder`, through `FlatVector` writers at an offset as DuckDB's `AGENTS.md` describes; `hugeint_t` from `OracleInt128`; strings with `StringVector::AddString`/`AddStringOrBlob`) and `oraduck_scan.cpp`; register the function in `LoadInternal`; add both files to `EXTENSION_SOURCES`.**

- [ ] **Step 4: Run the tests to verify they pass**

Run: `GEN=ninja make release && make test && source env.sh && uv run pytest tests/e2e -q`
Expected: PASS, writer tests included.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck_scan.hpp src/oraduck_scan.cpp src/include/scan_vectors.hpp src/scan_vectors.cpp src/oraduck_extension.cpp CMakeLists.txt test/sql/oraduck_scan.test tests/e2e
git commit -m "Add oraduck_scan: parallel Oracle reads with projection pushdown"
```

---

### Task 10: Filter pushdown

**Files:**
- Create: `src/include/oraduck_scan_filters.hpp`, `src/oraduck_scan_filters.cpp`
- Modify: `src/oraduck_scan.cpp`, `CMakeLists.txt`
- Test: `tests/e2e/test_scan_filters.py`

**Interfaces:**
- Consumes: `FilterNode`, `FilterConstant`, `TranslateFilter` (Task 5), `OraduckScanBindData::filters` (Task 9).
- Produces: `std::optional<oraduck::FilterNode> ExpressionToFilterNode(const Expression &expr, const LogicalGet &get);` and the `pushdown_complex_filter` callback of `oraduck_scan`.

Behaviour:
- Recognized DuckDB 2.0 expressions: comparisons (`BoundComparisonExpression::IsComparison`, `Left`/`Right`; constant on the left is flipped), `BETWEEN` (`BoundBetweenExpression::Input`/`LowerBound`/`UpperBound` and inclusivity → two comparisons under `kAnd`), `IN` (`BoundOperatorExpression` of type `COMPARE_IN`), `IS NULL`/`IS NOT NULL` (`OPERATOR_IS_NULL`/`OPERATOR_IS_NOT_NULL`), conjunctions (`BoundConjunctionExpression`), and `CAST(col AS DATE) = DATE constant` (`BoundCastExpression::IsCast`, child a column of type TIMESTAMP, target DATE) → `kDateEq`. Anything else → `std::nullopt`.
- A column reference maps to the Oracle column through `get.GetColumnIds()[binding.column_index].GetPrimaryIndex()`; constants (`BoundConstantExpression::GetValue`) convert to `FilterConstant` by DuckDB type (integers → `kInteger`, DECIMAL → `kDecimal` with its scale, FLOAT/DOUBLE → `kDouble`, VARCHAR → `kString`, TIMESTAMP → `kTimestamp`, DATE → `kDate`); NULL constants → `std::nullopt`.
- `pushdown_complex_filter`: for each expression, `ExpressionToFilterNode` then `TranslateFilter(node, source.columns, source.db_charset)`; when both succeed, append the fragment to `bind_data.filters` and erase the expression from the vector (DuckDB no longer applies it); otherwise leave it.
- `BuildScanSql` receives `bind_data.filters` (Task 9 passes an empty vector until now); EXPLAIN's `Oracle WHERE` shows their `display`.

- [ ] **Step 1: Write the failing tests** (`tests/e2e/test_scan_filters.py`), on a LIST-partitioned table (`PART_DATE` DATE: 2024-05-06, 2024-05-07, 2024-05-08, DEFAULT; LIST subpartitions on `SUB_KEY` VARCHAR2(8): `'A'`, `'B'`, DEFAULT) with 30,000 rows, columns of every pushable family, NULLs, empty-string-as-NULL, `CHAR(3)` values `'A  '` and `'AB '`:
  - `test_filter_equivalence` — parametrized over at least 40 predicates (each operator per column family, `BETWEEN`, `IN`, `OR` across columns, `IS [NOT] NULL`, `col <> ''`, `flag = 'A  '`, `flag = 'A'`, `CAST(part_date AS DATE) = DATE '2024-05-07'`, unconstrained `NUMBER`, string ranges, a predicate DuckDB cannot push such as `length(name) > 3`): rows of `SELECT id FROM oraduck_scan(…) WHERE <p>` equal rows of `SELECT id FROM (SELECT * FROM oraduck_scan(…)) WHERE <p>` evaluated on a DuckDB copy (`CREATE TABLE copy AS SELECT * FROM oraduck_scan(…)` first, then the predicate on `copy`).
  - `test_partition_pruning` — `V$SEGMENT_STATISTICS` `logical reads` deltas per subobject around `SELECT count(*) FROM oraduck_scan(…) WHERE part_date = DATE '2024-05-07' AND sub_key = 'A'`: > 0 for the target subpartition, 0 for every other subpartition; same with `part_date = DATE '2024-05-07'` alone (only that partition's subpartitions read) and with a LIST table without subpartitions on a `VARCHAR2` key (`WHERE region = 'EU'`).
  - `test_view_approach` — `CREATE VIEW toto AS FROM oraduck_scan(…)`; `EXPLAIN SELECT * FROM toto WHERE part_date = DATE '2024-05-07'` shows `Oracle WHERE` `("PART_DATE" = DATE '2024-05-07')` and no DuckDB `FILTER` operator; results equal the direct call.
  - `test_unpushable_filter_stays_in_duckdb` — `WHERE amount = 1.5` (unconstrained `NUMBER`): EXPLAIN shows a DuckDB `FILTER` and `Oracle WHERE` `—`; results correct.

- [ ] **Step 2: Run them to verify they fail**

Run: `GEN=ninja make release && source env.sh && uv run pytest tests/e2e/test_scan_filters.py -q`
Expected: `test_partition_pruning` and `test_view_approach` fail (all partitions read, no `Oracle WHERE`); equivalence passes (DuckDB filters everything).

- [ ] **Step 3: Implement `oraduck_scan_filters.cpp` and the callback; add the file to `EXTENSION_SOURCES`.**

- [ ] **Step 4: Run all tests**

Run: `GEN=ninja make release && make test && make unit && source env.sh && make itest && uv run pytest tests/e2e -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/include/oraduck_scan_filters.hpp src/oraduck_scan_filters.cpp src/oraduck_scan.cpp CMakeLists.txt tests/e2e/test_scan_filters.py
git commit -m "Push DuckDB filters down to Oracle for partition pruning"
```

---

### Task 11: Read benchmark harness: data, OraDuck method, protocol, read-only mode

**Files:**
- Create: `bench/oraduck_bench/read_config.py`, `bench/oraduck_bench/read_schema.py`, `bench/oraduck_bench/read_data.py`, `bench/oraduck_bench/read_methods.py`, `bench/oraduck_bench/read_runner.py`
- Modify: `bench/oraduck_bench/cli.py`, `pyproject.toml` (dependencies `duckdb==2.0.0`, `pyarrow`, `connectorx`)
- Test: `tests/bench/test_read_schema.py`, `tests/bench/test_read_methods.py`, `tests/bench/test_read_runner.py`

**Interfaces:**
- Consumes: `duck.run`, `duck.timed`, `oracle.connect`, `OracleConfig` (existing), the OraDuck COPY writer and `oraduck_scan`.
- Produces:
  - `read_config.py`: `READ_CASES = ((1_000_000, 50), (1_000_000, 550), (7_000_000, 50), (7_000_000, 550))`, `TARGET_DATE = "2024-05-07"`, `READ_RUNS = 5`, `READ_COMPETITOR_RUNS = 3`, `READ_TIMEOUT_S = 15 * 60`, `READ_OUT_DB = WORK / "read_out.duckdb"`.
  - `read_schema.py`: `read_table_name(nrows: int, ncols: int) -> str` (`RD_<rows>_<cols>`); `read_columns(ncols: int) -> list[ColumnSpec]`: first `PART_DATE DATE`, `SUB_KEY VARCHAR2(8)`, `ID NUMBER(38,0)`, then a cycle of `VARCHAR2(4000 BYTE)` ×3 (5–30 characters), `NUMBER(38,0)` ×2, `NUMBER(25,9)` ×2, `DATE`, `CHAR(1 BYTE)`, `TIMESTAMP(6)`, unconstrained `NUMBER`; one `CLOB` column per 250 columns (at least 1); `read_oracle_ddl(nrows, ncols) -> str` (LIST partitions 2024-05-06, 2024-05-07, 2024-05-08, DEFAULT; subpartition template LIST on `SUB_KEY` `'A'`, `'B'`, DEFAULT); `READ_CHECKSUM_DUCK` / `READ_CHECKSUM_ORACLE` (rows, `SUM(ID)`, `SUM(LENGTH(V1))`, NULL count of `D1` (first `NUMBER(25,9)`), `SUM(LENGTH(CLOB1))`, all over the target partition).
  - `read_data.py`: `ensure_read_table(cfg, nrows, ncols) -> str` (idempotent: keeps a table whose target-partition row count is right): Fakelake → Parquet → OraDuck `COPY` into the partitioned table (`nrows` rows in the target partition, `nrows // 20` in each other dated partition), CLOBs filled server-side for 2 % of the rows (*Claude's call*: values of 100–3,000 characters, one row in five of them 20,000 characters), `DBMS_STATS.GATHER_TABLE_STATS`.
  - `read_methods.py`: `Measurement` fields `method, table, nrows, ncols, threads, total_s, link_bytes: int | None, extra: dict`; `oraduck_sql(cfg, table, threads, where) -> tuple[str, str]` (setup, timed `CREATE TABLE t AS SELECT * FROM oraduck_scan('BENCH.<table>', connection := 'bench') WHERE <where>`); `run_oraduck(cfg, table, threads, where) -> Measurement` into `READ_OUT_DB` (deleted before each run).
  - `read_runner.py`: `run_read_case(cfg, nrows, ncols, methods, runs, out, where=None)` (warm-up, interleaved runs, checksums after every run against Oracle's on the same predicate, mismatch → `ChecksumMismatch`, timeout skip rule of `competitors.should_skip`, JSONL records with suite name).
  - CLI subcommands: `read-smoke` (1M × 50, OraDuck, 1 run), `read-matrix` (all `READ_CASES`, OraDuck N = 8), `read-external --dsn DSN --user USER --table OWNER.TABLE --where SQL [--runs N]` (no data generation, password from `ORADUCK_EXTERNAL_PASSWORD`, OraDuck only, prints the median and rows/s and MB/s of DuckDB storage).

- [ ] **Step 1: Write the failing tests** (no Oracle): `read_columns(550)` has 550 columns, starts with `PART_DATE`, `SUB_KEY`, `ID`, has 2 CLOBs; `read_columns(50)` has 1 CLOB; `read_oracle_ddl` contains `PARTITION BY LIST ("PART_DATE")` and `SUBPARTITION TEMPLATE`; `oraduck_sql` contains `oraduck_scan('BENCH.RD_1000000_50', connection := 'bench')` and `SET threads = 8;`; the runner interleaves methods (`["oraduck", "x", "oraduck", "x"]` after the warm-up) and raises `ChecksumMismatch` on differing checksums (fakes for the method and checksum functions, as in `tests/bench/test_runner.py`).

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run pytest tests/bench -q`
Expected: import errors for the new modules.

- [ ] **Step 3: Implement the modules and subcommands.**

- [ ] **Step 4: Run the tests and a smoke run**

Run: `uv run pytest tests/bench -q && source env.sh && taskset -c 0-7 uv run oraduck-bench read-smoke`
Expected: tests PASS; the smoke run prints one measurement and `checksums OK`.

- [ ] **Step 5: Commit**

```bash
git add bench/oraduck_bench pyproject.toml uv.lock tests/bench
git commit -m "Add the read benchmark harness: partitioned wide tables, OraDuck method, read-only mode"
```

---

### Task 12: Read benchmark competitors

**Files:**
- Modify: `bench/oraduck_bench/read_methods.py`, `bench/oraduck_bench/read_config.py`, `bench/oraduck_bench/cli.py`, `infra/instantclient/install.sh` (package `sqlplus`)
- Create: `bench/oraduck_bench/read_python_methods.py` (scripts run as subprocesses: `python -I -m oraduck_bench.read_python_methods <method> …`)
- Create: `infra/duckdb15/install.sh` (DuckDB v1.5.5 CLI into `.deps/duckdb-1.5.5/`)
- Test: `tests/bench/test_read_methods.py`

**Interfaces:**
- Consumes: Task 11's `Measurement`, `run_read_case`, checksums; `RINIE_EXTENSION` (existing config).
- Produces: methods `quack_oracle`, `rinie_oracle`, `oracledb_thin`, `oracledb_thick`, `connectorx`, `sqlplus_csv`, each a `run_<method>(cfg, table, threads, where) -> Measurement` writing the same DuckDB table into `READ_OUT_DB`; `READ_COMPETITOR_SERIES` = OraDuck N = 8 and N = 1 plus the six methods; CLI subcommand `read-competitors`.

Behaviour:
- `quack_oracle`: the DuckDB 2.0 CLI when `INSTALL oracle_scanner FROM community` works there, otherwise the 1.5.5 CLI; `ATTACH 'ora' AS o (TYPE oracle_scanner)`; timed `CREATE TABLE t AS SELECT * FROM o.<table> WHERE <where>`. The record says which DuckDB version ran.
- `rinie_oracle`: 1.5.5 CLI, `-unsigned`, `LOAD '<RINIE_EXTENSION>'`, `ATTACH '<user>/<password>@<dsn>' AS r (TYPE oracle)`, same statement on `r.BENCH.<table>`.
- `oracledb_thin` / `oracledb_thick` (thick: `oracledb.init_oracle_client(lib_dir=OCI_HOME)`): `fetch_df_batches(sql, size=100_000)`, each batch inserted into DuckDB through `pyarrow.table(batch)`; timed inside the script from connect to the last insert.
- `connectorx`: `cx.read_sql("oracle://…", sql, return_type="arrow_record_batches", partition_on="ID", partition_num=threads)` when the installed version supports streaming partitioned reads, otherwise `return_type="arrow"` and the case is skipped (`extra["skipped"] = "result does not fit in memory"`) when its estimated size exceeds half the available RAM.
- `sqlplus_csv`: `sqlplus -F -S` with `SET MARKUP CSV ON QUOTE ON`, `SET ARRAYSIZE 5000`, `SET LONG 1000000 LONGCHUNKSIZE 1000000`, `SET FEEDBACK OFF`, NLS date/timestamp formats and `NLS_NUMERIC_CHARACTERS = '.,'`, spooled under `bench/work/csv` (never `/tmp`), then DuckDB `read_csv` with explicit column types; `extra` records `spool_s`, `load_s`, `csv_bytes`; skipped when free disk is below 1.5 × the estimated CSV size.
- Every method's DuckDB result passes the same checksums.

- [ ] **Step 1: Write the failing tests** (no Oracle): generated setup and timed SQL for `quack_oracle` and `rinie_oracle` (`ATTACH` strings, `-unsigned` flag), the sqlplus script (contains `SET MARKUP CSV ON QUOTE ON` and `SPOOL`), the connectorx connection string `oracle://bench:…@10.210.19.10:1521/ORCLPDB1`, and the disk/RAM skip decisions with fake sizes.

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run pytest tests/bench -q`
Expected: FAIL (methods missing).

- [ ] **Step 3: Implement the methods, the installers and `read-competitors`.**

- [ ] **Step 4: Run the tests and one competitor case**

Run: `uv run pytest tests/bench -q && ./infra/duckdb15/install.sh && ./infra/instantclient/install.sh linux-x64 basic tools sqlplus && source env.sh && uv sync && taskset -c 0-7 uv run oraduck-bench read-competitors --only 1000000x50 --runs 1`
Expected: tests PASS; every method prints a time (or a recorded skip reason) and `checksums OK`.

- [ ] **Step 5: Commit**

```bash
git add bench/oraduck_bench infra/duckdb15 infra/instantclient/install.sh tests/bench
git commit -m "Add the read benchmark competitors"
```

---

### Task 13: Network shaping, link utilisation and read report

**Files:**
- Create: `infra/net/shape.sh`, `bench/oraduck_bench/netlink.py`, `bench/oraduck_bench/read_report.py`
- Modify: `bench/oraduck_bench/read_runner.py`, `bench/oraduck_bench/cli.py`
- Test: `tests/bench/test_read_report.py`, `tests/bench/test_netlink.py`

**Interfaces:**
- Consumes: Task 11–12 measurements.
- Produces:
  - `infra/net/shape.sh on [RATE] [RTT_MS] | off | status` (defaults `1gbit`, `0.5`): run with `sudo`; finds the `oraduck-net` bridge (`br-<network id prefix>`) and the container's network namespace (`docker inspect -f '{{.State.Pid}}'`); `on` adds, inside the container namespace on `eth0`, `netem delay <RTT/2>ms` with a `tbf rate <RATE>` child (Oracle → client direction) and `netem delay <RTT/2>ms` on the bridge (client → Oracle direction); `status` prints both qdiscs.
  - `netlink.py`: `bridge_rx_bytes() -> int` (from `/sys/class/net/<bridge>/statistics/rx_bytes`), `shaping_status() -> dict` (parsed from `tc qdisc show`), used by the runner to fill `Measurement.link_bytes` and to record the shaping in each JSONL record.
  - CLI: `read-shaped` (refuses to run unless `shaping_status()` shows the 1 Gbit/s qdisc; runs 1M × 550 for all methods and 7M × 550 for OraDuck, `oracledb_thin` and `connectorx`), `read-compression` (one OraDuck 7M × 550 run with `(COMPRESSION=on)` in a connect descriptor DSN), `read-report`.
  - `read_report.py`: `write_read_report(files: list[Path]) -> Path` writes `bench/REPORT_READ.md` and PNG charts: environment; matrix table (median s, min–max, rows/s, MB/s of DuckDB storage); competitor table and speedups; shaped table with link utilisation `link_bytes / total_s / 125e6`; OraDuck's planning share (`Planning (ms)` from an `EXPLAIN ANALYZE` run per case, against the median time); the success criteria of spec 10.5 marked met or not met; a hand-written "Analysis" placeholder like `bench/REPORT.md`'s.

- [ ] **Step 1: Write the failing tests**: `write_read_report` on a fixture JSONL (two methods, two cases, one shaped record) produces the four tables, a utilisation of `100.0 %` for `link_bytes = 125_000_000, total_s = 1.0`, and marks "fastest on every case" met when OraDuck's medians are lowest; `shaping_status` parses a captured `tc qdisc show` output with `tbf rate 1Gbit` and `netem delay 250us`.

- [ ] **Step 2: Run them to verify they fail**

Run: `uv run pytest tests/bench -q`
Expected: FAIL (modules missing).

- [ ] **Step 3: Implement the script, `netlink.py`, the runner changes and the report.**

- [ ] **Step 4: Run the tests and check the shaping**

Run: `uv run pytest tests/bench -q`, then ask Hugues to run `sudo ./infra/net/shape.sh on` (it needs root), then `./infra/net/shape.sh status && taskset -c 0-7 uv run oraduck-bench read-smoke`, then `sudo ./infra/net/shape.sh off`.
Expected: tests PASS; `status` shows `tbf rate 1Gbit` and the two `netem` delays; the shaped smoke run is slower than the unshaped one and its `link_bytes` is non-zero.

- [ ] **Step 5: Commit**

```bash
git add infra/net bench/oraduck_bench tests/bench
git commit -m "Add network shaping, link utilisation and the read benchmark report"
```

---

### Task 14: Benchmark campaign, documentation, release preparation

**Files:**
- Create: `bench/REPORT_READ.md`, charts, `bench/results/*-read-*.jsonl`
- Modify: `README.md`, `docs/design.md`

**Interfaces:**
- Consumes: everything above.

- [ ] **Step 1: Run the campaign** (client under `taskset -c 0-7`, Oracle on CPUs 8–15, nothing else running): `read-matrix`, `read-competitors`, then with shaping on `read-shaped`, then `read-compression`, shaping off, `read-report`.
Expected: all checksums OK; `bench/REPORT_READ.md` generated.

- [ ] **Step 2: Write the analysis** in `bench/REPORT_READ.md` (as measured, including where OraDuck does not win; state the DuckDB version each competitor ran in, the Oracle 19.3 / local-container limits, and whether the spec 10.5 criteria are met).

- [ ] **Step 3: Update the docs**: README section "Read from Oracle" (view approach, `oraduck_scan` parameters, type mapping table, write constants in the column's type, network options in the DSN with the Advanced Compression licence note, `EXPLAIN` / `EXPLAIN ANALYZE` output, benchmark summary linking `bench/REPORT_READ.md`, the note that each DuckDB release needs a matching OraDuck build); `docs/design.md`: scope (reading is now in scope), components table and a "Read path" section linking the spec.

- [ ] **Step 4: Full verification**

Run: `GEN=ninja make release && make unit && make test && source env.sh && make itest && uv run pytest -q`
Expected: everything passes.

- [ ] **Step 5: Commit, then ask Hugues before tagging `v0.4.0`** (the tag publishes the release and the extension repository).

```bash
git add bench README.md docs/design.md
git commit -m "Benchmark the read path and document oraduck_scan"
```
