#!/usr/bin/env bash
# Run the SQL Server (ODBC) and Oracle integration tests against containers that
# are already up. Unlike run_odbc_test_wsl.sh / run_oracle_test_wsl.sh, this script
# never creates, removes or restarts containers - start them yourself first.
#
# Usage (inside WSL, as root):
#   bash scripts/run_mssql_oracle_test_wsl.sh
#
# Credentials are read from the running containers themselves (MSSQL_SA_PASSWORD /
# ORACLE_PWD), so nothing is hard-coded here. Override by exporting
# SQLCONDUIT_TEST_ODBC_PASSWORD / SQLCONDUIT_TEST_ORACLE_PASSWORD.
#
# The build directory must already contain both integration test binaries built with
# -DSQLCONDUIT_ENABLE_ODBC=ON -DSQLCONDUIT_ENABLE_ORACLE=ON (see rebuild_oracle_wsl.sh).
set -uo pipefail

CLNT=/opt/oracle_client
BUILD=/root/sqlconduit/build-it

for n in mssql oracle; do
  docker ps --format '{{.Names}}' | grep -qx "$n" || { echo "ERROR: container '$n' is not running"; exit 1; }
done

container_env() {
  docker inspect "$1" --format '{{range .Config.Env}}{{println .}}{{end}}' \
    | sed -n "s/^$2=//p" | head -1
}

MSSQL_PW="${SQLCONDUIT_TEST_ODBC_PASSWORD:-$(container_env mssql MSSQL_SA_PASSWORD)}"
ORA_PW="${SQLCONDUIT_TEST_ORACLE_PASSWORD:-$(container_env oracle ORACLE_PWD)}"
[ -n "$MSSQL_PW" ] || { echo "ERROR: SQL Server SA password not found"; exit 1; }
[ -n "$ORA_PW" ] || { echo "ERROR: Oracle password not found"; exit 1; }

[ -x "$BUILD/tests/sqlconduit_odbc_integration_test" ] \
  || { echo "ERROR: ODBC integration test binary missing"; exit 1; }
[ -x "$BUILD/tests/sqlconduit_oracle_integration_test" ] \
  || { echo "ERROR: Oracle integration test binary missing"; exit 1; }

RC=0

echo "################ SQL Server / ODBC integration test ################"
export SQLCONDUIT_TEST_ODBC_HOST=127.0.0.1
export SQLCONDUIT_TEST_ODBC_PORT=1433
export SQLCONDUIT_TEST_ODBC_USER=sa
export SQLCONDUIT_TEST_ODBC_PASSWORD="$MSSQL_PW"
export SQLCONDUIT_TEST_ODBC_DATABASE=master
export SQLCONDUIT_TEST_ODBC_DRIVER=FreeTDS
( cd "$BUILD/tests" && ./sqlconduit_odbc_integration_test )
[ $? -eq 0 ] || { echo "ODBC_TEST_FAILED"; RC=1; }

echo
echo "################ Oracle integration test ################"
export SQLCONDUIT_TEST_ORACLE_HOST=127.0.0.1
export SQLCONDUIT_TEST_ORACLE_PORT=1521
export SQLCONDUIT_TEST_ORACLE_USER=system
export SQLCONDUIT_TEST_ORACLE_SERVICE=FREEPDB1
export SQLCONDUIT_TEST_ORACLE_PASSWORD="$ORA_PW"
export ORACLE_HOME="$CLNT/home"
export LD_LIBRARY_PATH="$CLNT/home/lib:${LD_LIBRARY_PATH:-}"
( cd "$BUILD/tests" && ./sqlconduit_oracle_integration_test )
[ $? -eq 0 ] || { echo "ORACLE_TEST_FAILED"; RC=1; }

echo
[ "$RC" -eq 0 ] && echo "ALL INTEGRATION TESTS PASSED" || echo "INTEGRATION TESTS FAILED"
exit "$RC"
