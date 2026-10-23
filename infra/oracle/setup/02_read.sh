#!/bin/bash
# Read-path test database: run once by the Oracle image after 01_bench.sh, and re-run on an existing
# container by infra/oracle/grants.sh. Idempotent: each CREATE ignores "already exists".
sqlplus -s / as sysdba <<SQL
WHENEVER SQLERROR EXIT FAILURE
ALTER SESSION SET CONTAINER = ORCLPDB1;
-- ORA-01543: tablespace exists, ORA-01920: user name conflicts with another user or role
BEGIN
	EXECUTE IMMEDIATE q'[CREATE SMALLFILE TABLESPACE bench_small DATAFILE '/opt/oracle/oradata/ORCLCDB/ORCLPDB1/bench_small01.dbf' SIZE 256M AUTOEXTEND ON NEXT 64M MAXSIZE 4G]';
EXCEPTION WHEN OTHERS THEN IF SQLCODE != -1543 THEN RAISE; END IF;
END;
/
BEGIN
	EXECUTE IMMEDIATE 'CREATE USER reader IDENTIFIED BY "${READER_PASSWORD}"';
EXCEPTION WHEN OTHERS THEN IF SQLCODE != -1920 THEN RAISE; END IF;
END;
/
ALTER DATABASE DATAFILE '/opt/oracle/oradata/ORCLCDB/ORCLPDB1/bench01.dbf' AUTOEXTEND ON NEXT 1G MAXSIZE 80G;
ALTER USER bench QUOTA UNLIMITED ON bench_small;
GRANT CREATE VIEW, CREATE SYNONYM, CREATE PUBLIC SYNONYM, DROP PUBLIC SYNONYM, CREATE TYPE TO bench;
GRANT SELECT ON sys.v_\$session TO bench;
GRANT SELECT ON sys.v_\$mystat TO bench;
GRANT SELECT ON sys.v_\$statname TO bench;
GRANT SELECT ON sys.v_\$segment_statistics TO bench;
GRANT SELECT ON sys.v_\$sql TO bench;
GRANT SELECT ON sys.dba_extents TO bench;
GRANT EXECUTE ON sys.dbms_flashback TO bench;
GRANT CREATE SESSION TO reader;
ALTER USER reader IDENTIFIED BY "${READER_PASSWORD}";
EXIT
SQL
