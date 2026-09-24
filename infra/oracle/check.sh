#!/usr/bin/env bash
# Prints the effective configuration of the benchmark Oracle container
set -euo pipefail
NAME=oraduck-ora19
echo "HOST_CPUSET=$(docker inspect -f '{{.HostConfig.CpusetCpus}}' "$NAME")"
echo "HOST_MEMORY=$(docker inspect -f '{{.HostConfig.Memory}}' "$NAME")"
docker exec -i "$NAME" sqlplus -s / as sysdba <<'SQL'
SET HEADING OFF FEEDBACK OFF PAGESIZE 0 LINESIZE 200
SELECT 'VERSION=' || version_full FROM v$instance;
SELECT 'LOG_MODE=' || log_mode FROM v$database;
SELECT 'SGA_TARGET_MB=' || value / 1024 / 1024 FROM v$parameter WHERE name = 'sga_target';
SELECT 'PGA_TARGET_MB=' || value / 1024 / 1024 FROM v$parameter WHERE name = 'pga_aggregate_target';
SELECT 'CPU_COUNT=' || value FROM v$parameter WHERE name = 'cpu_count';
SELECT 'LOCK_SGA=' || value FROM v$parameter WHERE name = 'lock_sga';
SELECT 'CHARSET=' || value FROM nls_database_parameters WHERE parameter = 'NLS_CHARACTERSET';
ALTER SESSION SET CONTAINER = ORCLPDB1;
SELECT 'BENCH_TS_GB=' || ROUND(bytes / 1024 / 1024 / 1024) FROM dba_data_files WHERE tablespace_name = 'BENCH';
SELECT 'BENCH_USER=' || username FROM dba_users WHERE username = 'BENCH';
SQL
