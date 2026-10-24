- **1M × 50, the case where competitors were measured**: OraDuck with 8 sessions has a median of
  6.193 s (161,473 rows/s). Against it, the medians are: python-oracledb thin 18.459 s (×3.0),
  ConnectorX 19.194 s (×3.1), quack-oracle 30.722 s (×5.0), SQL*Plus → CSV 106.924 s (×17) and
  python-oracledb thick 112.579 s (×18). Every OraDuck run matched Oracle's checksums.
- **Session for session**: OraDuck with one session (15.241 s, 65,612 rows/s) is also ahead of the
  methods that use one session: python-oracledb thin is 1.21 times slower (18.459 s), quack-oracle
  2.0 times (30.722 s), SQL*Plus 7.0 times (106.924 s) and python-oracledb thick 7.4 times
  (112.579 s) (ratios computed from the medians of the table). Going from 1 to 8 sessions brings
  OraDuck from 15.241 s to 6.193 s (×2.5). ConnectorX, with 8 partitions (8 sessions), is the only
  other parallel method, and it is 3.1 times slower than OraDuck at 8 sessions: it takes about as
  long as python-oracledb thin with a single session.
- **The order-of-magnitude gap**: python-oracledb thick (112.579 s) and SQL*Plus (106.924 s) are
  about 17 to 18 times slower. Thick goes through a slow fetch path for the CLOB column in this
  test; SQL*Plus timing includes the process start-up and the parsing of the CSV by DuckDB
  (start-up cannot be separated).
- **duckdb-oracle is not ranked**: its result is incorrect (the CLOB column arrives empty), so the
  checksums differ from Oracle's on 1M × 50 and on 1M × 550. A fast wrong result is not a result.
- **1M × 550: only OraDuck was measured.** With ATTACH the median is 140.704 s (7,107 rows/s,
  min–max 116.147–195.792 s) and with `oraduck_scan` 170.917 s (5,851 rows/s, min–max
  101.079–302.720 s); the two are within each other's spread, so the difference between them (×1.2
  in the table) says nothing. The competitor runs were stopped at the project owner's request
  because they took too long. The only competitor data on this case are single warm-up runs:
  quack-oracle 320 s and python-oracledb thin 477 s, against 217 s for OraDuck's warm-up. They are
  indicative only, and the protocol excludes warm-up runs from the medians.
- **The wide spread and the low rows/s point to the test environment, not to OraDuck** (likely cause,
  not investigated further): the 1M × 550 table is about 10 GB, while the Oracle container has a
  3 GB SGA and a 6 GB memory limit. Client and server share one 15 GB laptop, which was using swap
  (8 GB of swap in use during the campaign). Reading is probably bound by Oracle's disk reads and by
  memory pressure.
- **Planning share**: planning takes 14.9 ms, which is 0.2 % of the 6.193 s median of 1M × 50
  (0.0 % of 1M × 550).
- **Not measured in this campaign (owner's decision)**: the 1 Gbit/s shaped series, so the line-rate
  criterion is not evaluated; SQL*Net compression; the pruned-unit overhead (a pruned unit cost
  about 0.24 ms on 304 one-row subpartitions, measured during development, not part of this
  campaign).
- **Success criteria**: OraDuck is the fastest on the only case where competitors were measured
  (1M × 50): met. Line rate on 1M × 550 at 1 Gbit/s: not evaluated. Correctness: every OraDuck run
  matched Oracle's checksums; the "not met" in the generated criteria comes from duckdb-oracle's
  incorrect result, not from OraDuck.
- **Limits**: Oracle 19.3 in a container on the same laptop, so no network: reads over a 1 Gbit/s
  link will be bounded by the link (125 MB/s at most). Medians of 3 runs for the competitors and
  for OraDuck with 1 session, of 8 runs for OraDuck with 8 sessions on 1M × 50 (the 5 runs of the
  matrix and the 3 of the competitor series, merged) and of 5 runs for OraDuck on 1M × 550, with a
  high variance on 1M × 550. The 1M × 550 case does not compare OraDuck
  with anyone.
