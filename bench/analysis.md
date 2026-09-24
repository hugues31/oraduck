<!-- Hand-written from the 2026-09-24 campaign: rewrite it after any new measurement campaign. -->
- **OraDuck is faster in all 9 cases**, by ×1.2 to ×3.2 (medians). On the largest case (10M × 50,
  6.4 GB of CSV) it loads in 31.7 s versus 70.7 s, about 200 MB/s of CSV equivalent versus 90 MB/s.
- **Row width**: for small tables the speedup grows with the number of columns (100k rows: ×1.2 with
  5 columns, ×3.0 with 50). Formatting and parsing CSV costs more as rows get wider, whereas OraDuck
  passes values in native or Oracle internal formats.
- **100k rows**: the table fits in a single DuckDB row group (122,880 rows), so there is only one
  thread, one OraDuck session and one CSV file for sqlldr: no parallelism at all. OraDuck is then
  dominated by a fixed cost of about 0.3 s (connection, prepare, commit), which is why 100k and 1M rows
  with 5 columns take about the same time.
- **CSV share**: writing the CSV files accounts for 10 % (100k × 5) to 36 % (10M × 20 and 10M × 50) of
  the SQL\*Loader time. Even against the sqlldr phase alone, OraDuck is faster in 8 cases out of 9;
  the exception is 1M × 5 (0.354 s vs 0.340 s).
- **Scaling** (1M × 20): OraDuck goes from 1.60 s to 0.93 s between N = 1 and N = 8, flattening from
  N = 4 (Oracle on 8 E-cores, synchronous commits, `log file sync`). SQL\*Loader gains more from
  parallelism (7.0 s to 2.0 s) because it parses the CSV on the client side, so the speedup drops from
  ×4.4 to ×2.1.
- **Spread**: it remains large on the big cases, e.g. OraDuck from 2.0 s to 6.6 s on 1M × 50. Client and
  server share a 15 GB laptop under memory pressure (zram swap), despite the cpusets, the locked SGA
  and copy-on-write-free storage. Medians of 5 runs are reported, with the min–max range.
- **Asymmetric tuning**: only OraDuck's `STREAM_SIZE` was tuned (on 1M × 20). SQL\*Loader runs with the
  parameters mandated by the reference method, untuned.
- **Limits**: Oracle 19.3 instead of 19.30, NOARCHIVELOG database, client and server on the same
  machine, a single set of column types (see `bench/oraduck_bench/schema.py`), timestamps without
  fractional seconds (Fakelake writes epoch seconds).
