#!/usr/bin/env bash
set -euo pipefail

# 9P (/mnt/d) caches aggressively; tar can capture stale Windows-side content and
# break the build (e.g. picks up a pre-Oracle datasource_config.h). Mount D: with
# cache=none to force fresh reads for the sync step.
NC=/mnt/d_nc
if ! [ -f "$NC/chiang/dbmw/CMakeLists.txt" ]; then
  mkdir -p "$NC"
  mount -t drvfs D: "$NC" -o cache=none,rw 2>/dev/null || true
fi
if [ -f "$NC/chiang/dbmw/CMakeLists.txt" ]; then
  SRC="$NC/chiang/dbmw"
else
  SRC=/mnt/d/chiang/dbmw
fi

BUILD=/root/sqlconduit/build-it
CLNT=/opt/oracle_client

docker ps --format "{{.Names}}" 2>/dev/null | grep -q "^oracle$" || { echo "ORACLE CONTAINER NOT RUNNING"; exit 1; }

echo "==> CLEAN source dir then re-sync (avoids stale pre-Oracle source leftovers)"
rm -rf /root/sqlconduit
mkdir -p /root/sqlconduit
tar -cf - -C "$SRC" CMakeLists.txt src include tests cmake third_party config | tar -xf - -C /root/sqlconduit
echo "SYNC_DONE: lob_max_bytes present? $(grep -c lob_max_bytes /root/sqlconduit/include/sqlconduit/config/datasource_config.h)"

echo "==> CLEAN build dir (stale .o from prior ODBC build causes inconsistent compile)"
rm -rf "$BUILD"

echo "==> cmake configure (Oracle=ON, ODBC=ON)"
cmake -S /root/sqlconduit -B "$BUILD" -G "Unix Makefiles" \
  -DSQLCONDUIT_ENABLE_MYSQL=OFF -DSQLCONDUIT_ENABLE_POSTGRES=OFF \
  -DSQLCONDUIT_ENABLE_ODBC=ON -DSQLCONDUIT_ENABLE_ORACLE=ON \
  -DSQLCONDUIT_BUILD_TESTS=ON -DSQLCONDUIT_BUILD_INTEGRATION_TESTS=ON \
  -DOCI_INCLUDE_DIR="$CLNT/home/rdbms/public" \
  -DOCI_LIBRARY="$CLNT/home/lib/libclntsh.so" >/tmp/cmake_ora.log 2>&1 \
  && echo CMAKE_OK || { echo CMAKE_FAIL; tail -30 /tmp/cmake_ora.log; exit 1; }

echo "==> build"
cmake --build "$BUILD" -j"$(nproc)" >/tmp/build_ora.log 2>&1 \
  && echo BUILD_OK || { echo BUILD_FAIL; tail -50 /tmp/build_ora.log; exit 1; }

echo "==> run sqlconduit_oracle_integration_test"
export SQLCONDUIT_TEST_ORACLE_HOST=127.0.0.1
export SQLCONDUIT_TEST_ORACLE_PORT=1521
export SQLCONDUIT_TEST_ORACLE_USER=system
export SQLCONDUIT_TEST_ORACLE_SERVICE=FREEPDB1
export SQLCONDUIT_TEST_ORACLE_PASSWORD="SqlConduit!Test123"
export ORACLE_HOME="$CLNT/home"
export LD_LIBRARY_PATH="$CLNT/home/lib:${LD_LIBRARY_PATH:-}"
cd "$BUILD/tests"
./sqlconduit_oracle_integration_test 2>&1
echo "ORACLE_TEST_EXIT=$?"
