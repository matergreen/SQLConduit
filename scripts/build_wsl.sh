#!/usr/bin/env bash
set -euo pipefail
SRC=/mnt/d/chiang/dbmw
WORK=/root/sqlconduit
BUILD=$WORK/build-it
ENABLE_ODBC="${ENABLE_ODBC:-ON}"
ENABLE_ORACLE="${ENABLE_ORACLE:-OFF}"
OCI_INCLUDE_DIR="${OCI_INCLUDE_DIR:-}"
OCI_LIBRARY="${OCI_LIBRARY:-}"

echo "==> sync $SRC -> $WORK"
rm -rf "$WORK"; mkdir -p "$WORK"
( cd "$SRC" && tar --exclude='./build*' --exclude='./.git' --exclude='./.workbuddy' -cf - . ) | tar -xf - -C "$WORK"

cd "$WORK"
ARGS=(-S . -B "$BUILD" -G "Unix Makefiles"
  -DSQLCONDUIT_ENABLE_MYSQL=OFF -DSQLCONDUIT_ENABLE_POSTGRES=OFF
  -DSQLCONDUIT_ENABLE_ODBC="$ENABLE_ODBC" -DSQLCONDUIT_ENABLE_ORACLE="$ENABLE_ORACLE"
  -DSQLCONDUIT_BUILD_TESTS=ON -DSQLCONDUIT_BUILD_INTEGRATION_TESTS=ON)
if [ -n "$OCI_INCLUDE_DIR" ]; then ARGS+=(-DOCI_INCLUDE_DIR="$OCI_INCLUDE_DIR"); fi
if [ -n "$OCI_LIBRARY" ]; then ARGS+=(-DOCI_LIBRARY="$OCI_LIBRARY"); fi

echo "==> cmake configure (ODBC=$ENABLE_ODBC ORACLE=$ENABLE_ORACLE)"
cmake "${ARGS[@]}" >/tmp/cmake.log 2>&1 && echo CMAKE_OK || { echo CMAKE_FAIL; tail -25 /tmp/cmake.log; exit 1; }

echo "==> build"
cmake --build "$BUILD" -j"$(nproc)" >/tmp/build.log 2>&1 && echo BUILD_OK || { echo BUILD_FAIL; tail -40 /tmp/build.log; exit 1; }

echo "==> integration test binaries =="
ls -la "$BUILD"/tests/sqlconduit_*_integration_test 2>/dev/null || echo "(none built)"
