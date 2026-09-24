# OraDuck environment — source it from bash or zsh: `source env.sh`
if [ -n "${BASH_SOURCE:-}" ]; then _oraduck_env="${BASH_SOURCE[0]}"; else _oraduck_env="$0"; fi
export ORADUCK_ROOT="$(cd "$(dirname "$_oraduck_env")" && pwd)"
unset _oraduck_env

export OCI_HOME="$ORADUCK_ROOT/.deps/instantclient_19_32"
case ":${LD_LIBRARY_PATH:-}:" in
	*":$OCI_HOME:"*) ;;
	*) export LD_LIBRARY_PATH="$OCI_HOME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ;;
esac
case ":$PATH:" in
	*":$OCI_HOME:"*) ;;
	*) export PATH="$OCI_HOME:$PATH" ;;
esac
export NLS_LANG=AMERICAN_AMERICA.AL32UTF8

# CMake 4 rejects third-party projects declaring cmake_minimum_required < 3.5
export CMAKE_POLICY_VERSION_MINIMUM=3.5

# Disposable local Oracle database (container oraduck-ora19); override before sourcing if needed
export ORADUCK_ORACLE_DSN="${ORADUCK_ORACLE_DSN:-10.210.19.10:1521/ORCLPDB1}"
export ORADUCK_ORACLE_USER="${ORADUCK_ORACLE_USER:-bench}"
export ORADUCK_ORACLE_PASSWORD="${ORADUCK_ORACLE_PASSWORD:-BenchPwd_2026}"
export ORADUCK_ORACLE_SYS_PASSWORD="${ORADUCK_ORACLE_SYS_PASSWORD:-SysPwd_2026}"
