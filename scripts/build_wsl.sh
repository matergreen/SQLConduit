#!/usr/bin/env bash
set -euo pipefail
SRC=/mnt/d/chiang/dbmw
WORK=/root/dbmw
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
  -DDBMW_ENABLE_MYSQL=OFF -DDBMW_ENABLE_POSTGRES=OFF
  -DDBMW_ENABLE_ODBC="$ENABLE_ODBC" -DDBMW_ENABLE_ORACLE="$ENABLE_ORACLE"
  -DDBMW_BUILD_TESTS=ON -DDBMW_BUILD_INTEGRATION_TESTS=ON)
if [ -n "$OCI_INCLUDE_DIR" ]; then ARGS+=(-DOCI_INCLUDE_DIR="$OCI_INCLUDE_DIR"); fi
if [ -n "$OCI_LIBRARY" ]; then ARGS+=(-DOCI_LIBRARY="$OCI_LIBRARY"); fi

echo "==> cmake configure (ODBC=$ENABLE_ODBC ORACLE=$ENABLE_ORACLE)"
cmake "${ARGS[@]}" >/tmp/cmake.log 2>&1 && echo CMAKE_OK || { echo CMAKE_FAIL; tail -25 /tmp/cmake.log; exit 1; }

echo "==> build"
cmake --build "$BUILD" -j"$(nproc)" >/tmp/build.log 2>&1 && echo BUILD_OK || { echo BUILD_FAIL; tail -40 /tmp/build.log; exit 1; }

echo "==> integration test binaries =="
ls -la "$BUILD"/tests/dbmw_*_integration_test 2>/dev/null || echo "(none built)"
