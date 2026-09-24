Measurements excluded from the report: taken before the SGA was locked in RAM (LOCK_SGA; the SGA had
been swapped out to zram), before the Oracle files moved to btrfs without copy-on-write (erratic
fsync), and before the "one OCI environment per session" fix. Kept for traceability;
`oraduck-bench report` only reads `bench/results/*.jsonl`.
