# OraDuck probe results

Output of `tools/probe.cpp` (`make probe`), run on 2026-09-23 against Oracle
19.3.0.0.0 with 1,000,000 rows per variant; client pinned to CPUs 0-7, Oracle on
CPUs 8-15. The TIMESTAMP internal variant was measured before the switch to the
canonical 7-byte form for values without fractional seconds (see E2 below).

## E9 — Encoders and DUMP()

Encoders matching `DUMP()`: 29 / 29 (19 NUMBER values up to 38 digits, 5 DATE
and 5 TIMESTAMP values between 0001-01-01 and 9999-12-31).

## E1/E2 — Single-column representations (1 session)

| Variant | Time (s) | Rows/s | Check |
|---|---|---|---|
| NUMBER ← int64, SQLT_INT (native) | 0.312 | 3,208,250 | OK |
| NUMBER ← int64, SQLT_NUM (encoded) | 0.331 | 3,019,118 | OK |
| NUMBER ← int64, SQLT_CHR (text) | 0.367 | 2,727,947 | OK |
| NUMBER ← int64, SQLT_INT, forced EntrySet | 0.366 | 2,733,568 | OK |
| NUMBER(15,2) ← decimal, SQLT_NUM (encoded) | 0.340 | 2,942,327 | OK |
| NUMBER(15,2) ← decimal, SQLT_CHR (text) | 0.429 | 2,330,048 | OK |
| DATE, SQLT_DAT (7 bytes) | 0.299 | 3,345,397 | OK |
| DATE, SQLT_CHR (text, 19) | 0.484 | 2,067,247 | OK |
| TIMESTAMP(6), SQLT_CHR (text, 26) | 0.616 | 1,622,229 | OK |
| TIMESTAMP(6), internal type 180 (11 bytes) | 0.389 | 2,571,171 | OK |
| BINARY_DOUBLE, SQLT_BDOUBLE (native) | 0.321 | 3,116,253 | OK |
| BINARY_DOUBLE, SQLT_FLT (native) | 0.312 | 3,209,912 | OK |
| VARCHAR2(30), SQLT_CHR | 0.347 | 2,881,015 | OK |

## E3 — Stream accumulation (5 columns)

| Setting | Time (s) | Rows/s | Column array | Check |
|---|---|---|---|---|
| accumulate_stream = false | 0.983 | 1,017,494 | rows=2048 contiguous=yes | OK |
| accumulate_stream = true | 0.823 | 1,215,020 | rows=2048 contiguous=yes | OK |

## E4 — Stream buffer size (5 columns)

| Setting | Time (s) | Rows/s | Column array | Check |
|---|---|---|---|---|
| stream 64 KiB | 1.131 | 883,932 | rows=2048 contiguous=yes | OK |
| stream 256 KiB | 1.077 | 928,331 | rows=2048 contiguous=yes | OK |
| stream 1024 KiB | 0.961 | 1,040,476 | rows=2048 contiguous=yes | OK |
| stream 4096 KiB | 0.896 | 1,115,684 | rows=2048 contiguous=yes | OK |
| stream 16384 KiB | 0.971 | 1,030,265 | rows=2048 contiguous=yes | OK |

## E5 — Rows per column array (5 columns)

| Setting | Time (s) | Rows/s | Column array | Check |
|---|---|---|---|---|
| column_array_rows = 2048 | 0.941 | 1,063,046 | rows=2048 contiguous=yes | OK |
| column_array_rows = 8192 | 0.829 | 1,206,854 | rows=8192 contiguous=yes | OK |

## E6 — Parallel sessions (2,000,000 rows, 5 columns)

| Sessions | Time (s) | Rows/s | Max Finish (s) | Check |
|---|---|---|---|---|
| 1 | 1.658 | 1,206,235 | 0.025 | OK |
| 2 | 0.941 | 2,124,284 | 0.083 | OK |
| 4 | 0.677 | 2,955,153 | 0.068 | OK |
| 8 | 0.713 | 2,806,628 | 0.221 | OK |

## E7 — Mixed-case identifiers

| Attempt | Result |
|---|---|
| raw names: ProbeMixed / MaCol | ERROR: OCIDirPathPrepare: ORA-39826: Direct path load of view or synonym (BENCH.PROBEMIXED) could not be resolved. |
| quoted names: "ProbeMixed" / "MaCol" | OK (1 row) |

## E8 — Indexed table (PARALLEL = TRUE)

ORA-26002: OCIDirPathPrepare: ORA-26002: Table BENCH.PROBE_IDX has index defined upon it.

## Decisions

| Measurement | Choice | Rationale |
|---|---|---|
| E2 NUMBER ← int64 | native `SQLT_INT`, zero copy | 0.312 s vs 0.331 s for encoded `SQLT_NUM` and 0.367 s as text |
| E2 NUMBER(15,2) | encoded `SQLT_NUM` | 0.340 s vs 0.429 s as text; values match |
| E2 DATE | `SQLT_DAT` | 0.299 s vs 0.484 s as text |
| E2 TIMESTAMP | **internal type 180** instead of text | 0.389 s vs 0.616 s (37 % faster); values match (checksums and `DUMP()`). Later fix: values without fractional seconds are sent in Oracle's canonical 7-byte form, since 11 bytes with zero nanoseconds do not compare equal to SQL literals. Used for TIMESTAMP(p ≥ 6) only, so that Oracle still rounds lower precisions. |
| E2 BINARY_DOUBLE | `SQLT_BDOUBLE` | 0.321 s; `SQLT_FLT` 0.312 s, within noise |
| E3 accumulation | **`accumulate_stream = true`** | 0.823 s vs 0.983 s (16 % faster) |
| E4 stream buffer | **4 MiB** | best time (0.896 s); confirmed by `oraduck-bench tune` with 8 sessions |
| E5 8,192-row column array | left for a later version | 0.829 s vs 0.941 s (12 % faster); needs string copies across DataChunks |
| E6 parallelism | parallel Finish | 1 → 4 sessions: 1.66 s → 0.68 s; 8 sessions: 0.71 s; max Finish 0.22 s |
| E7 mixed case | **identifiers always double-quoted** | raw names: ORA-39826 (OCI upper-cases them) |
| E8 indexes | rejected at bind time | ORA-26002 confirmed |

## Read path

Output of `tools/read_probe.cpp` (`make read-probe`), run on 2026-10-23 against Oracle 19.3.0.0.0
with Instant Client 19.32; client pinned to CPUs 0-7, Oracle on CPUs 8-15. The probe builds its
tables on the server (`INSERT /*+ APPEND */ … SELECT` from a `CONNECT BY` row generator) and drops
them at the end. The laptop was shared with an unrelated build that ran in bursts (load average 2 to
4 during the recorded run, up to 24 between runs). So times are medians of interleaved runs, and the
decisions rest either on differences larger than the run-to-run spread (checked against an earlier
full run, quoted where it matters) or on counts that load does not change (round trips, value
checks). Round trips are `SQL*Net roundtrips to/from client` from `V$MYSTAT`, except for LOBs (R4),
where that statistic misses LOB reads and the client's own request count (kernel TCP counters) is
used.

### R1 — NUMBER(18,0) into BIGINT (2,000,000 rows)

2,048 rows per fetch, prefetch off; median of 3 runs. Decode = time spent turning the define buffer into int64 (per value).

| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Check |
|---|---|---|---|---|---|---|
| `SQLT_NUM` (22 bytes) + `DecodeOracleNumberScaled64` | 0.421 | 4,753,047 | 0.343 | 92.3 | 978 | OK |
| `SQLT_INT` (8 bytes, OCI conversion) | 0.231 | 8,639,775 | 0.169 | 2.3 | 978 | OK |

`SQLT_INT` is 1.8× faster (0.231 s against 0.421 s; first full run: 0.238 s against 0.450 s) for the
same round trips and exact sums. The gap is the decoder: 92 ns per value (`ParseNumber` builds a
`std::string` per value, as the Task 2 review noted), while OCI's conversion costs less than the fetch
itself. The decoder stays necessary for the columns OCI cannot convert exactly (`NUMBER(38,0)`,
`NUMBER(25,9)`: 180 of the 550 workload columns): 7M rows × 180 columns × ~90 ns is ~115 s of client
CPU, ~15 s per session with 8 sessions, so removing the string from `ParseNumber` is worth doing.

### R2 — TIMESTAMP(6) into TIMESTAMP (2,000,000 rows)

`OCIDefineByPos(…, 180, 11 bytes)` on the edge values (BC, year 1, 9999, NULL…): accepted (10 rows fetched); values of 7 bytes: 4, of 11 bytes: 5.

2,048 rows per fetch, prefetch off; median of 3 runs. Sum check: Σ microseconds since 1970 against Oracle's `TRUNC`/`SSSSS`/`FF6` sums. Row check: each value against `TO_CHAR(C, 'SYYYY-MM-DD HH24:MI:SS.FF6')` of the same row.

| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Sum check | Row check |
|---|---|---|---|---|---|---|---|
| type 180 (11 bytes) + `DecodeOracleTimestamp` | 0.259 | 7,729,574 | 0.199 | 19.8 | 978 | OK | OK (20010 rows) |
| `SQLT_TIMESTAMP` descriptors + `OCIDateTimeGetDate/Time` | 0.268 | 7,457,101 | 0.208 | 19.1 | 978 | OK | OK (20010 rows) |
| `TO_CHAR(C, 'YYYY-MM-DD HH24:MI:SS.FF6')` (`SQLT_CHR`) + parse | 0.882 | 2,267,906 | 0.158 | 22.2 | 978 | OK | — |

Type 180 is accepted by `OCIDefineByPos` (and by `OCIDefineByPos2` in the timed runs), returns
7 bytes when the fraction is zero and 11 otherwise, and decodes to exactly what `TO_CHAR` reports on
20,010 rows, including BC dates, year 1, 9999-12-31 23:59:59.999999 and the Julian side of 1582. It is
the fastest define (0.259 s); descriptors are close (0.268 s) but need one descriptor per row and two
OCI calls per value; `TO_CHAR` is 3.4× slower (formatting on the server).

### R3 — 550-column table (200,000 rows): define budget × prefetch memory

Columns: 200 `VARCHAR2(4000 BYTE)` (5–30 characters, 10 % NULL), 100 `NUMBER(38,0)`, 80 `NUMBER(25,9)`, 60 `DATE`, 50 `CHAR(1 BYTE)`, 30 `TIMESTAMP(6)`, 30 `NUMBER`, fetched as raw bytes (`SQLT_CHR`, `SQLT_NUM`, `SQLT_DAT`, type 180: decode costs are in R1, R2 and R7); define buffer 809,820 bytes per row (define size + 8). Rows per fetch = budget / bytes per row (at most 2,048). `OCI_ATTR_PREFETCH_ROWS = 0`. The sink reads every indicator and length.

Median of 3 interleaved runs (after one warm-up scan).

| Budget (MiB) | Prefetch memory (MiB) | Rows/fetch | Wall (s) | Rows/s | Client CPU (s) | Round trips / 10,000 rows | KB / round trip | Wire (MB/s) | Check |
|---|---|---|---|---|---|---|---|---|---|
| 16 | 0 | 20 | 8.23 | 24,302 | 6.62 | 500.1 | 127 | 155 | OK |
| 16 | 4 | 20 | 8.61 | 23,242 | 6.91 | 500.1 | 127 | 148 | OK |
| 16 | 16 | 20 | 8.87 | 22,555 | 7.26 | 250.1 | 255 | 144 | OK |
| 16 | 64 | 20 | 9.01 | 22,194 | 7.92 | 100.0 | 636 | 141 | OK |
| 64 | 0 | 82 | 7.77 | 25,753 | 6.82 | 122.0 | 521 | 164 | OK |
| 64 | 4 | 82 | 8.01 | 24,975 | 7.31 | 122.0 | 522 | 159 | OK |
| 64 | 16 | 82 | 8.24 | 24,281 | 7.53 | 122.0 | 522 | 155 | OK |
| 64 | 64 | 82 | 8.86 | 22,578 | 8.36 | 61.0 | 1,042 | 144 | OK |
| 256 | 0 | 331 | 8.03 | 24,901 | 7.76 | 30.3 | 2,100 | 158 | OK |
| 256 | 4 | 331 | 7.95 | 25,159 | 7.67 | 30.3 | 2,100 | 160 | OK |
| 256 | 16 | 331 | 7.97 | 25,104 | 7.68 | 30.3 | 2,100 | 160 | OK |
| 256 | 64 | 331 | 8.16 | 24,511 | 7.87 | 30.2 | 2,103 | 156 | OK |

Bytes sent by Oracle per row: 6,364 (SQL*Net bytes / rows; at 115 MB/s of payload that is 18,070 rows/s on a 1 Gbit/s link). Fetches are synchronous: with a 0.5 ms round-trip time (spec 10.3), 90 % of 125 MB/s needs at least 563 KB per round trip.

Narrow table: RP_PART of R5 (6 columns: `NUMBER(12)` and `NUMBER(4)` as `SQLT_INT`, `NUMBER(15,2)`, `VARCHAR2(30)`, `DATE`, `TIMESTAMP(6)`; 2,000,000 rows; define buffer 134 bytes per row), median of 3 interleaved runs.

| Rows/fetch | Prefetch memory (MiB) | Wall (s) | Rows/s | Client CPU (s) | Round trips / 10,000 rows | KB / round trip |
|---|---|---|---|---|---|---|
| 2,048 | 0 | 0.746 | 2,682,080 | 0.591 | 4.89 | 100 |
| 2,048 | 4 | 1.169 | 1,710,516 | 0.771 | 0.28 | 1,782 |
| 2,048 | 16 | 1.307 | 1,530,535 | 0.965 | 0.07 | 7,000 |
| 2,048 | 64 | 1.348 | 1,484,188 | 1.014 | 0.02 | 24,498 |
| 8,192 | 0 | 0.738 | 2,708,419 | 0.671 | 1.23 | 399 |
| 32,768 | 0 | 0.713 | 2,806,310 | 0.639 | 0.32 | 1,556 |

- **Prefetch memory is sized from the declared column widths** (809,820 bytes per row here), not
  from the data: 4 and 16 MiB add nothing at a 64 MiB budget, and 64 MiB adds 82 rows per round
  trip, exactly what a 64 MiB define array already holds. At the 16 and 64 MiB budgets it always
  costs time (an extra copy): +3 to +14 % wall and up to +23 % client CPU. At 256 MiB it changes
  nothing (same round trips, times within 2 %).
- **64 MiB without prefetch is the fastest** (7.77 s, 25,753 rows/s; the first full run gave 8.31 s,
  also the fastest). 256 MiB is 3 % slower and uses 14 % more client CPU (7.76 s against 6.82 s: the
  larger buffers cost cache and TLB misses); 16 MiB is 6 % slower.
- At 64 MiB a round trip carries 521 KB (82 rows of 6,364 bytes on the wire). With a 0.5 ms round
  trip that is 89 % of a 1 Gbit/s link for one session, and two or more sessions overlap their
  round trips.
- **One session is client-bound**: client CPU is 88 % of the wall time, about 60 ns per value inside
  OCI for raw fetches with no decoding. 25,753 rows/s × 6,364 bytes is 164 MB/s, only 1.4× the
  115 MB/s of a 1 Gbit/s link (7M rows at line rate take about 6.5 minutes): the scan must use
  several sessions by default to keep headroom once decoding is added.
- **Narrow rows**: at 2,048 rows per fetch (one DuckDB vector) a round trip carries only 100 KB.
  Prefetch memory raises that to 1.8–24 MB but costs 57–81 % more time, while 8,192- or 32,768-row
  define arrays reach 399 KB and 1.6 MB per round trip at no cost (0.738 s and 0.713 s against
  0.746 s).

### R4 — CLOB into VARCHAR (100,000 rows per distribution)

Small: `RPAD('x', 1 + MOD(id * 7919, 3600), 'abcdefghé')`, 1 to 3,999 bytes (mean ~2,000), ASCII and `é`. Mixed: the same with every 10th row 99,975 bytes (90,000 characters). 1,024 rows per fetch, prefetch off (`OCI_ATTR_PREFETCH_ROWS = 0`, `OCI_ATTR_PREFETCH_MEMORY = 0`); median of 3 runs. Requests: data segments the client sent to Oracle (kernel TCP counters, one per OCI call); `V$MYSTAT` misses LOB reads served after a LOB prefetch, so both are shown. Check: every value compared byte for byte with the generator.

| Technique | Distribution | Wall (s) | Rows/s | Payload (MB/s) | Client CPU (s) | Requests / 1,000 rows | `V$MYSTAT` round trips / 1,000 rows | Check |
|---|---|---|---|---|---|---|---|---|
| `dynamic_define` as specified (`SQLT_LVC`, `OCI_DYNAMIC_FETCH`) | small | — | — | — | — | — | — | ERROR: OCIStmtFetch2: ORA-24370: illegal piecewise operation attempted |
| `dynamic_define` as specified (`SQLT_LVC`, `OCI_DYNAMIC_FETCH`) | mixed | — | — | — | — | — | — | ERROR: OCIStmtFetch2: ORA-24370: illegal piecewise operation attempted |
| `dynamic_define` with `SQLT_CHR` (`OCI_DYNAMIC_FETCH`, 64 KiB pieces) | small | 1.76 | 56,944 | 114 | 0.19 | 1.0 | 1.0 | OK |
| `dynamic_define` with `SQLT_CHR` (`OCI_DYNAMIC_FETCH`, 64 KiB pieces) | mixed | 5.88 | 17,017 | 201 | 0.78 | 1.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 4,000 characters) | small | 1.89 | 52,989 | 106 | 0.49 | 1.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 4,000 characters) | mixed | 7.53 | 13,278 | 157 | 2.12 | 101.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 16,384 characters) | small | 2.08 | 48,167 | 96 | 0.53 | 1.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 16,384 characters) | mixed | 8.15 | 12,272 | 145 | 2.32 | 101.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 32,768 characters) | small | 2.17 | 46,189 | 92 | 0.48 | 1.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 32,768 characters) | mixed | 8.54 | 11,712 | 138 | 2.35 | 101.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 131,072 characters) | small | 3.04 | 32,863 | 66 | 0.51 | 1.0 | 1.0 | OK |
| `lob_prefetch` (locators, LOB prefetch 131,072 characters) | mixed | 7.76 | 12,881 | 152 | 2.36 | 1.0 | 1.0 | OK |
| `substr_locator` (`DBMS_LOB.SUBSTR` ≤ 1,333 characters inline, else locator) | small | 6.30 | 15,861 | 32 | 1.04 | 630.7 | 1.0 | OK |
| `substr_locator` (`DBMS_LOB.SUBSTR` ≤ 1,333 characters inline, else locator) | mixed | 11.78 | 8,490 | 100 | 2.65 | 667.5 | 1.0 | OK |

Exact check of large and edge values (1.4 MB, 3.0 MB, NULL, EMPTY_CLOB(), é漢😀 (9 bytes); the large values repeat `<tag><k>é漢😀;`, so multi-byte characters straddle every piece and chunk boundary):

| Technique | Check | Requests (5 rows) | `V$MYSTAT` round trips |
|---|---|---|---|
| `dynamic_define` as specified (`SQLT_LVC`, `OCI_DYNAMIC_FETCH`) | ERROR: OCIStmtFetch2: ORA-24370: illegal piecewise operation attempted | — | — |
| `dynamic_define` with `SQLT_CHR` (`OCI_DYNAMIC_FETCH`, 64 KiB pieces) | FAIL (2/5); 1.4 MB: 1428936 bytes instead of 1428894, first difference at byte 6327 (`ef bf bd ef bf bd`); 3.0 MB: 2948993 bytes instead of 2948895, first difference at byte 6097 (`ef bf bd ef bf bd`); EMPTY_CLOB(): NULL | 2 | 2 |
| `lob_prefetch` (locators, LOB prefetch 4,000 characters) | OK (5/5) | 4 | 2 |
| `lob_prefetch` (locators, LOB prefetch 16,384 characters) | OK (5/5) | 4 | 2 |
| `lob_prefetch` (locators, LOB prefetch 32,768 characters) | OK (5/5) | 4 | 2 |
| `lob_prefetch` (locators, LOB prefetch 131,072 characters) | OK (5/5) | 4 | 2 |
| `substr_locator` (`DBMS_LOB.SUBSTR` ≤ 1,333 characters inline, else locator) | OK (5/5) | 4 | 2 |

- **`dynamic_define` as specified does not work**: `SQLT_LVC` with `OCI_DYNAMIC_FETCH` raises
  ORA-24370 at the first fetch (`SQLT_LVB` on a CLOB raises ORA-00932). Defined as `SQLT_CHR` (or
  `SQLT_LNG`, static or dynamic, with any piece size: the same bytes come back), the LOB data
  interface runs and is the fastest (56,944 rows/s on small values, all inline), **but it is not
  correct**: every supplementary character (4-byte UTF-8, here 😀) that falls on an internal
  boundary of the server's LOB-to-LONG conversion comes back as two U+FFFD (`ef bf bd ef bf bd`),
  first at byte 6,327 of the 1.4 MB value whatever the client piece size (64 KiB, 1 MiB and 4 MiB
  were tried); and `EMPTY_CLOB()` comes back as NULL. The damage is data loss, so it cannot be
  repaired after the fetch: rejected on correctness (anti-goal: silently wrong results). The small
  and mixed distributions pass only because they hold no supplementary character.
- **`substr_locator` is correct but costs 631 requests per 1,000 small rows**: a locator read for
  every value longer than 1,333 characters, the longest prefix that always fits `VARCHAR2(4000 BYTE)`
  in AL32UTF8 under `MAX_STRING_SIZE = STANDARD` (the test database's setting). Disqualified (the
  limit is 10 per 1,000).
- **`lob_prefetch` is exact on every value** (1.4 MB and 3.0 MB with multi-byte characters across
  every boundary, NULL, `EMPTY_CLOB()` as an empty string) and costs no request beyond the array
  fetches on the small distribution (1.0 per 1,000 rows). A value longer than the prefetch size costs
  one request (101 per 1,000 rows on the mixed distribution for sizes up to 32,768 characters).
- **The prefetch size has a server cost even for small values**: from 4,000 to 131,072 characters,
  small-value throughput drops from 52,989 to 32,863 rows/s with the same client CPU. A separate
  20,000-row check measured server CPU (`CPU used by this session`) of 0.20, 0.24 and 0.39 s for
  4,000, 32,768 and 131,072 characters. A value longer than the prefetch size is read again from its
  start (32,768 characters: +115 MB on the wire for 2,000 values of 90,000 characters).
- **`V$MYSTAT` misses LOB reads**: `SQL*Net roundtrips to/from client` reports 1.0 per 1,000 rows
  where the client sent 101 or 631 requests. `user calls` counts them (2,034 for 2,000 reads in the
  same 20,000-row check).

### R5 — Parallel split of a LIST-partitioned table (2,000,000 rows, 8 partitions, 4 sessions)

Table in the bigfile `BENCH` tablespace. `USER_EXTENTS` has no block address (`RELATIVE_FNO`, `BLOCK_ID`), so ROWID ranges cannot come from it. Extent map: `DBA_EXTENTS`: ORA-942, so `DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID` (chunk size = target blocks, then `ROWID BETWEEN start_rowid AND end_rowid`; create task + chunks + read + drop task) — 32.1 ms for 16 extents/chunks → 16 ROWID units (target = max(⌈16,384 blocks / 16⌉, 128) = 1,024 blocks). Units come from a shared queue; sessions are opened before the clock starts. 2,048 rows per fetch, prefetch off; median of 3 runs.

| Scan | Strategy | Sessions | Units | Wall (s) | Rows/s | Planning / wall | Check |
|---|---|---|---|---|---|---|---|
| full table | one session | 1 | 1 | 0.863 | 2,316,896 | — | OK |
| full table | extents (ROWID ranges) | 4 | 16 | 0.292 | 6,846,520 | 11.0 % | OK |
| full table | hash (`ORA_HASH(ROWID, 3) = k`) | 4 | 4 | 0.429 | 4,665,737 | — | OK |
| PART_KEY = 5 | one session | 1 | 1 | 0.118 | 2,118,335 | — | OK |
| PART_KEY = 5 | extents (ROWID ranges) | 4 | 16 | 0.071 | 3,510,275 | 45.1 % | OK |
| PART_KEY = 5 | hash (`ORA_HASH(ROWID, 3) = k`) | 4 | 4 | 0.052 | 4,804,781 | — | OK |

Units run alone with `PART_KEY = 5` (one session, query included): 14 units in pruned partitions: mean 0.66 ms, max 1.28 ms, 0 rows; 2 units in partition P5: mean 49.2 ms.

- **`USER_EXTENTS` has no block address** (no `RELATIVE_FNO`, no `BLOCK_ID`), so ROWID ranges
  cannot come from it, as the brief and the design assumed. BENCH cannot read `DBA_EXTENTS`
  (ORA-00942). `DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID` builds the same extent-based ranges
  with definer's rights: it works for the owner and, checked by hand, for the READER user with only
  SELECT on the table (no `CREATE JOB` needed; it writes a task in the dictionary, dropped
  afterwards). The probe used it; the ranges cover every row exactly once in the bigfile tablespace
  (count and `SUM(ID)` checks).
- Extents with 4 sessions read the table 3.0× faster than one session (0.292 s against 0.863 s).
  Hash reaches 2.0× (0.429 s), since every session scans the whole table.
- **A unit in a pruned partition costs 0.66 ms** on average (max 1.28 ms) including its query, and
  returns no row.
- The target unit size comes from all segments, so the filtered read (one partition of 8) gets 2 of
  the 16 units with data and runs on 2 sessions: extents 0.071 s against hash 0.052 s.
- Planning costs 32 ms: 11 % of this small table's 0.29 s parallel read, far below 2 % of a read of
  minutes.

### R6 — `AS OF SCN`

SCN 4007289 read with TIMESTAMP_TO_SCN(SYSTIMESTAMP) (DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER: ORA-904; V$DATABASE.CURRENT_SCN: ORA-942); queries use `AS OF SCN TO_NUMBER(:1)`. Each scan without `AS OF` is followed by the same scan with it; cost = median of the per-pair time ratios (robust to load changes on the machine). Noise floor: the same pairs with `AS OF` in neither scan. Server CPU (`CPU used by this session`, median) and consistent gets from `V$MYSTAT`.

| Scan | Pairs | Without: wall (s) | With `AS OF SCN`: wall (s) | Cost | Server CPU without / with (s) | Consistent gets without / with | Rows without / with |
|---|---|---|---|---|---|---|---|
| RP_WIDE (550 columns, 200,000 rows) | 3 | 7.7654 | 7.8422 | +0.6 % | 1.77 / 1.83 | 602,445 / 602,447 | 200,000 / 200,000 |
| RP_PART full table | 9 | 0.7438 | 0.7363 | +0.2 % | 0.53 / 0.58 | 15,410 / 15,412 | 2,000,000 / 2,000,000 |
| RP_PART full table — noise floor | 9 | 0.6913 | 0.6854 | -0.1 % | 0.53 / 0.54 | 15,410 / 15,410 | 2,000,000 / 2,000,000 |
| RP_PART PART_KEY = 5 | 9 | 0.0835 | 0.0880 | +5.1 % | 0.07 / 0.06 | 1,924 / 1,926 | 250,000 / 250,000 |
| RP_PART PART_KEY = 5 — noise floor | 9 | 0.0849 | 0.0876 | +3.4 % | 0.07 / 0.07 | 1,924 / 1,924 | 250,000 / 250,000 |
| RP_PART full table, extents, 4 sessions (16 units) | 9 | 0.2669 | 0.2717 | +2.7 % | — | — | 2,000,000 / 2,000,000 |
| RP_PART full table, extents, 4 sessions (16 units) — noise floor | 9 | 0.2687 | 0.2719 | +1.1 % | — | — | 2,000,000 / 2,000,000 |
| RP_PART ROWID unit of a pruned partition, `PART_KEY = 5` (one pair per unit) | 14 | 0.0004 | 0.0019 | +363.2 % | — | — | 0 / 0 |

The throughput cost of `AS OF SCN` is within noise and below 5 %: +0.6 % on the workload-shaped
scan, +0.2 % on the full scan (noise floor −0.1 %), +2.7 % with 4 sessions (noise floor +1.1 %). The
84 ms filtered scan shows +5.1 % against a noise floor of +3.4 %. Consistent gets are the same, so
pruning holds (1,924 against 1,926), and server CPU differs by 0.06 s at most (centisecond
resolution). The one systematic cost is
about 1.5 ms per query (pruned unit: 0.4 ms → 1.9 ms), negligible for units of seconds. (The first
full run, with 5 pairs and no noise floor, measured −4.6 %, +5.3 %, −1.8 % and +5.1 %: the same
conclusion at a higher noise level.)

BENCH can only read the SCN with `TIMESTAMP_TO_SCN(SYSTIMESTAMP)` (`DBMS_FLASHBACK`: ORA-00904,
no `EXECUTE`; `V$DATABASE`: ORA-00942). Checked as SYS, it maps to an SCN about 2 s in the past
(1 to 2 SCNs behind `CURRENT_SCN` on this idle database).

### R7 — Unconstrained NUMBER into DOUBLE (2,000,000 rows)

Values: integers, `ROUND(r / 7, 12)`, `ROUND(r * 1.0001, 4)`, `-r * 1000000.5`. 2,048 rows per fetch, prefetch off; median of 3 runs. Checks: Σ against Oracle's `SUM(C)` (relative 1e-9), then every double against `strtod` of the exact decimal, bit for bit.

| Define | Wall (s) | Rows/s | Client CPU (s) | Decode (ns/value) | Round trips | Check |
|---|---|---|---|---|---|---|
| `SQLT_BDOUBLE` (8 bytes, OCI conversion) | 0.896 | 2,231,535 | 0.746 | 2.6 | 978 | OK; correctly rounded (all 2,000,000) |
| `SQLT_FLT` (8 bytes, OCI conversion) | 0.198 | 10,097,978 | 0.147 | 2.4 | 978 | OK; FAIL: 338,196 / 2,000,000 not correctly rounded (first: 0.142857142857 → 0.14285714285700002) |
| `SQLT_NUM` + `DecodeOracleNumberText` + `strtod` | 0.517 | 3,865,139 | 0.407 | 131.4 | 978 | OK; correctly rounded by construction |
| `SQLT_NUM` + `DecodeOracleNumberText` (the `VARCHAR` option) | 0.368 | 5,440,900 | 0.299 | 75.4 | 978 | OK (20000 rows equal to `TO_CHAR`) |

`SQLT_BDOUBLE` is correctly rounded but costs about 300 ns per value inside OCI on these 12- to
18-digit values (client CPU 0.746 s against 0.147 s for `SQLT_FLT`). `SQLT_FLT` is 4.5× faster
but not correctly rounded on 17 % of the values (0.142857142857 gives 0.14285714285700002 instead
of 0.14285714285699999), so it must not be used. Decoding the `SQLT_NUM` bytes to
text and `strtod` is exact and costs 45 % less client CPU than `SQLT_BDOUBLE` (0.407 s): Task 4's
`kNativeDouble`/`SQLT_BDOUBLE` mapping is correct, and an exact decoder over `SQLT_NUM` would be
cheaper. The text decoder (the `VARCHAR` option) matches `TO_CHAR` on 20,000 rows.

### R8 — `AS OF SCN` with one parse per session (final fix wave, 2026-10-24)

R6 timed one statement per scan, so it missed the parse cost of a parallel read. Oracle never shares a flashback
cursor between parse calls: when each work unit prepared its statement, every unit was a hard parse and a new child
cursor. Since then each session prepares the unit statement once and runs it again for each unit with new binds
(`FetchCursor::Execute`). Table: 550 columns of the read benchmark (`read_oracle_ddl(…, 550)`), LIST/LIST, 12
subpartitions of one row each (8 MB initial extents); `CREATE TABLE t AS SELECT * FROM oraduck_scan(…, sessions :=
8, snapshot := …)`, one CLI per run, the two snapshots alternating; hard parses from `V$SYSSTAT` (all sessions, as
SYS) around each run. Median of 5 to 9 runs; the machine was shared (load average 3 to 6).

| Code | Units | `snapshot := 'none'` (s) | `'auto'`, `AS OF SCN` (s) | Cost | Hard parses per scan, none / auto |
|---|---|---|---|---|---|
| One prepare per unit (before), unit floor 128 blocks | 96 | 0.346 | 1.343 | +288 % | 0 / 97–100 (V$SQL: one child cursor per execution) |
| One prepare per session, unit floor 128 blocks | 96 | 0.179 | 0.327 | +83 % | 0 / 9 |
| Final: unit floor 1,024 blocks, no snapshot for one unit | 12 | 0.199 | 0.322 | +62 % | 0 / 9 |

The 9 hard parses are one per session (8) and the `AS OF` check of the planning. The remaining cost is fixed per scan
and grows with the sessions (final code, same table, median of 5): 4 sessions +0.078 s (+38 %), 8 sessions +0.145 s
(+51 %), 16 sessions +0.232 s (+88 %, floor 128); one session no longer uses a snapshot (one statement). Measured
alone (8 python-oracledb sessions parsing the 550-column `AS OF` query at the same moment): one hard parse costs
13 ms; 8 sessions parsing the same text take 117–126 ms, as the child cursors of one parent are created one after
another, against 30–35 ms with one text per session. On a large read the cost disappears in the noise:
`RD_1000000_50` (50 columns), target partition (1,000,000 rows), 8 sessions, `SELECT *` discarded: 2.03 s with
`AS OF` against 2.29 s without, median of the per-pair ratios +0.3 % (5 pairs, runs from 1.8 to 2.8 s).

Decision: the rule (keep `auto` only if `AS OF SCN` costs at most 5 % on this table) sets `SNAPSHOT_DEFAULT = none`:
the snapshot adds about 0.12 s to an 8-session scan (13 ms per session for 550 columns), +62 % here, and
more with more sessions; it stays available with `snapshot := 'auto'` or `'scn'`. One text per session (a session
number in the comment) would cut the 8-session cost to about 30 ms, still above 5 % of this scan.

### Findings for the next tasks

- **Task 8, extents**: build ROWID ranges from `DBA_EXTENTS`, or else from
  `DBMS_PARALLEL_EXECUTE.CREATE_CHUNKS_BY_ROWID`, which needs only SELECT on the table. Never from
  `USER_EXTENTS`, which has no block address (R5).
- **Task 8, unit size**: size units from the segments the filter keeps; otherwise a one-partition
  read gets few units (2 of 16 in R5).
- **Task 8, SCN**: in this database only `TIMESTAMP_TO_SCN(SYSTIMESTAMP)` works for a plain user.
  It lags about 2 s, so a snapshot can miss the last seconds of commits, and a table altered (DDL)
  within that window raises ORA-01466 under `AS OF SCN`.
- **Task 7, LOB prefetch size**: the inline limit of `lob_prefetch`. 32,768 characters is a
  reasonable default: values up to 32K characters come inline, and small values lose 13 % of local
  throughput against 4,000 (46,189 against 52,989 rows/s), where 131,072 loses 38 % and doubles the
  server CPU. Count it in the define budget (up to 3 bytes per character in AL32UTF8 for each
  prefetched value).
- **Task 7, round-trip test**: `SQL*Net roundtrips to/from client` misses LOB reads made after a LOB
  prefetch, so a CLOB test based on it can pass while every value costs a round trip. Use `user calls`
  from `V$MYSTAT`, which counts them (R4).
- **Task 7, rows per fetch**: let narrow rows fetch more than 2,048 rows per call (bounded by the
  budget, not by the vector size). 32,768-row arrays cost nothing locally and carry 1.6 MB per round
  trip (R3).
- **Task 4, unconstrained `NUMBER` → `DOUBLE`**: `SQLT_BDOUBLE` is exact but costs about 300 ns per
  value in OCI; never use `SQLT_FLT` (inexact). An exact decoder over `SQLT_NUM` is cheaper (R7).
- **Task 2 follow-up**: `DecodeOracleNumberScaled64` costs 92 ns and `DecodeOracleNumberText` 75 ns
  per value (R1, R7). Removing the `std::string` from `ParseNumber` matters for the 180 decoder-only
  columns of the workload.
- **Task 9, sessions**: one session reads the workload shape at only 1.4× the link rate before any
  decoding (R3), so the default must be several sessions.
- **Task 7, LOBs**: never use the LOB data interface (CLOB defined as a character type) for CLOB or
  NCLOB: it corrupts supplementary characters and reads `EMPTY_CLOB()` as NULL (R4). BLOBs were not
  measured; `lob_prefetch` applies to them in bytes.

### Read path decisions

| Key | Value | Evidence |
|---|---|---|
| `TIMESTAMP_DEFINE` | `internal180` | R2: type 180 accepted by `OCIDefineByPos`/`OCIDefineByPos2`; 20,010 values (BC, year 1, 9999, 7- and 11-byte forms) equal to `TO_CHAR`; 0.259 s against 0.268 s (descriptors) and 0.882 s (`TO_CHAR`) for 2,000,000 rows |
| `INTEGER_DEFINE` | `sqlt_int` | R1: 0.231 s against 0.421 s for 2,000,000 `NUMBER(18,0)` values (first run 0.238 s against 0.450 s), same 978 round trips, exact sums; the decoder costs 92 ns per value |
| `LOB_TECHNIQUE` | `lob_prefetch` | R4: the only correct technique without per-value round trips: 1.0 request per 1,000 small rows, every value exact including 1.4/3.0 MB multi-byte values and `EMPTY_CLOB()`; `dynamic_define` fails (`SQLT_LVC`: ORA-24370; `SQLT_CHR`: U+FFFD for supplementary characters, `EMPTY_CLOB()` read as NULL); `substr_locator`: 631 requests per 1,000 small rows |
| `FETCH_BUDGET_BYTES` | `67108864` | R3: 64 MiB is the fastest budget on the 550-column table (7.77 s against 8.03 s at 256 MiB and 8.23 s at 16 MiB; first run 8.31/8.44/8.88 s), 82 rows and 521 KB per round trip, 12 % less client CPU than 256 MiB |
| `PREFETCH_MEMORY_BYTES` | `0` | R3: no non-zero value is faster beyond noise (best: −1 % at 256 MiB, where it changes no round trip). At 16 and 64 MiB it adds +3 to +14 % (wide rows); on narrow rows it adds +57 to +81 %, while larger define arrays cut round trips at no cost. Prefetch memory is sized from declared widths, so it only adds what a larger define array adds, plus a copy |
| `SNAPSHOT_DEFAULT` | `none` | R8 (supersedes R6's `auto`): each session hard-parses the `AS OF SCN` statement, one session after another (Oracle never shares a flashback cursor between parse calls): +62 % on the 12-subpartition 550-column table with 8 sessions (0.322 s against 0.199 s, 13 ms per session), above the 5 % rule; within noise on a 1M-row partition (+0.3 %). R6: throughput cost +0.6 % (workload scan), +0.2 % (full scan), +2.7 % (4 sessions), same consistent gets |
