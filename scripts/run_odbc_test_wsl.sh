#!/usr/bin/env bash
set -uo pipefail
PW="${DBMW_TEST_ODBC_PASSWORD:-Dbmw!Test123}"
IMAGE=mcr.microsoft.com/mssql/server:2022-latest

echo "==> ensure image $IMAGE"
for i in $(seq 1 120); do
  docker image inspect "$IMAGE" >/dev/null 2>&1 && break
  sleep 5
done
docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "IMAGE NOT READY"; exit 1; }

echo "==> start SQL Server container"
docker rm -f mssql >/dev/null 2>&1 || true
docker run -d --name mssql -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD="$PW" -p 1433:1433 "$IMAGE"

echo "==> wait for SQL Server ready"
READY=0
for i in $(seq 1 60); do
  if docker logs mssql 2>&1 | grep -qi "ready for client connections"; then READY=1; break; fi
  sleep 3
done
if [ "$READY" != "1" ]; then echo "MSSQL NOT READY"; docker logs mssql | tail -25; exit 1; fi
echo "MSSQL READY"

export DBMW_TEST_ODBC_HOST=127.0.0.1
export DBMW_TEST_ODBC_PORT=1433
export DBMW_TEST_ODBC_USER=sa
export DBMW_TEST_ODBC_PASSWORD="$PW"
export DBMW_TEST_ODBC_DATABASE=master
export DBMW_TEST_ODBC_DRIVER=FreeTDS

cd /root/dbmw/build-it/tests
echo "==> run dbmw_odbc_integration_test"
./dbmw_odbc_integration_test 2>&1
echo "ODBC_TEST_EXIT=$?"
