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
