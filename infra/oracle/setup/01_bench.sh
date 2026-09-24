#!/bin/bash
# Run once by the Oracle image after the database is created (this file is sourced).
sqlplus -s / as sysdba <<EOF
WHENEVER SQLERROR EXIT FAILURE
ALTER SYSTEM SET cpu_count = 8 SCOPE = BOTH;
ALTER SYSTEM SET sga_max_size = 3G SCOPE = SPFILE;
ALTER SYSTEM SET sga_target = 3G SCOPE = SPFILE;
ALTER SYSTEM SET pga_aggregate_target = 1G SCOPE = BOTH;
-- SGA locked in RAM: otherwise the host (zram, high swappiness) swaps it out
ALTER SYSTEM SET lock_sga = TRUE SCOPE = SPFILE;
ALTER SESSION SET CONTAINER = ORCLPDB1;
CREATE BIGFILE TABLESPACE bench DATAFILE '/opt/oracle/oradata/ORCLCDB/ORCLPDB1/bench01.dbf' SIZE 9G AUTOEXTEND ON NEXT 1G MAXSIZE 20G;
CREATE USER bench IDENTIFIED BY "${BENCH_PASSWORD}" DEFAULT TABLESPACE bench QUOTA UNLIMITED ON bench;
GRANT CREATE SESSION, CREATE TABLE TO bench;
EXIT
EOF
