#!/usr/bin/env bash
set -euo pipefail

ENABLE_MYSQL=OFF
ENABLE_POSTGRES=OFF
ENABLE_ODBC=OFF

for arg in "$@"; do
  case "$arg" in
    --mysql) ENABLE_MYSQL=ON ;;
    --pg|--postgres) ENABLE_POSTGRES=ON ;;
    --odbc) ENABLE_ODBC=ON ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

echo "==> Updating apt and installing the base toolchain"
sudo apt update
sudo apt install -y build-essential cmake

if [ "$ENABLE_MYSQL" = ON ]; then
  echo "==> Installing the MySQL client library"
  sudo apt install -y default-libmysqlclient-dev
fi
if [ "$ENABLE_POSTGRES" = ON ]; then
  echo "==> Installing the PostgreSQL client libraries"
  sudo apt install -y libpqxx-dev libpq-dev
fi
if [ "$ENABLE_ODBC" = ON ]; then
  echo "==> Installing unixODBC development files"
  sudo apt install -y unixodbc-dev
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
echo "==> Configuring (mysql=$ENABLE_MYSQL pg=$ENABLE_POSTGRES odbc=$ENABLE_ODBC)"
mkdir -p "$BUILD"
cmake -S "$ROOT" -B "$BUILD" \
  -DDBMW_ENABLE_MYSQL="$ENABLE_MYSQL" \
  -DDBMW_ENABLE_POSTGRES="$ENABLE_POSTGRES" \
  -DDBMW_ENABLE_ODBC="$ENABLE_ODBC"

echo "==> Building"
cmake --build "$BUILD" -j"$(nproc)"

echo "==> Done. Build output: $BUILD"
