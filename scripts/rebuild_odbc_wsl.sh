#!/usr/bin/env bash
set -uo pipefail
SRC=/mnt/d/chiang/dbmw
WORK=/root/dbmw
BUILD=$WORK/build-it
echo "==> sync patched odbc_driver.cpp"
mkdir -p "$WORK"
tar -cf - -C "$SRC" src/driver/odbc_driver.cpp include/dbmw/mapping.h include/dbmw/util.h tests/dbmw_odbc_integration_test.cpp | tar -xf - -C "$WORK"
echo "==> incremental rebuild (ODBC test binary)"
cmake --build "$BUILD" -j$(nproc) >/tmp/rebuild.log 2>&1 && echo BUILD_OK || { echo BUILD_FAIL; tail -25 /tmp/rebuild.log; }
ls -la "$BUILD/tests/dbmw_odbc_integration_test" 2>/dev/null && echo BIN_OK
