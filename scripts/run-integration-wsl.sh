#!/usr/bin/env bash
# Run dbmw integration tests under WSL.
#
# Flow: sync sources to ext4 -> start MySQL/PG -> provision test accounts -> build
#       everything (including ODBC tests) -> run MySQL/PostgreSQL integration tests.
#
# Usage (inside WSL; root via `wsl -u root` is recommended):
#   bash scripts/run-integration-wsl.sh
#   MYSQL_PW=xxx PG_PW=yyy bash scripts/run-integration-wsl.sh   # custom passwords
#   MYSQL_USER=root PG_USER=postgres bash scripts/run-integration-wsl.sh  # existing accounts
#
# Notes:
#   - Sources must be built on ext4 (/root/dbmw). Building directly under /mnt/d is unreliable
#     with DrvFS, so this script synchronizes the tree first.
#   - When run as root, it creates dedicated `dbmw` MySQL and PostgreSQL accounts without
#     changing existing root/postgres passwords. Override the variables above to reuse accounts.
#   - ODBC integration tests are compile-only because WSL has no SQL Server instance; ctest
#     excludes them with `-E odbc`.
#   - Install missing dependencies with scripts/setup-wsl.sh --mysql --pg --odbc.
set -euo pipefail

MYSQL_PW="${MYSQL_PW:-dbmwtest}"
PG_PW="${PG_PW:-dbmwtest}"
MYSQL_USER="${MYSQL_USER:-dbmw}"
MYSQL_DB="${MYSQL_DB:-dbmw_test}"
PG_USER="${PG_USER:-dbmw}"
PG_DB="${PG_DB:-dbmw}"
WORK=/root/dbmw
BUILD="$WORK/build-it"

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SELF_DIR/.." && pwd)"
JOBS="$(nproc)"

SUDO=""
if [ "$(id -u)" != "0" ] && command -v sudo >/dev/null 2>&1; then SUDO="sudo"; fi
IS_ROOT=0; [ "$(id -u)" = "0" ] && IS_ROOT=1

echo "==> 0/5 Syncing sources $SRC -> $WORK (excluding build*/.git/.workbuddy)"
if [ "$SRC" != "$WORK" ]; then
  rm -rf "$WORK"
  mkdir -p "$WORK"
  ( cd "$SRC" && tar --exclude='./build*' --exclude='./.git' --exclude='./.workbuddy' -cf - . ) \
    | tar -xf - -C "$WORK"
else
  echo "    Already in $WORK; skipping synchronization"
fi

echo "==> 1/5 Starting MySQL / PostgreSQL"
if ! command -v mysql >/dev/null 2>&1; then
  echo "Error: mysql client not found; run scripts/setup-wsl.sh --mysql --pg first" >&2
  exit 1
fi
$SUDO service mysql start || true
$SUDO service postgresql start || true
sleep 3

echo "==> 1.5/5 Provisioning test accounts (automatic as root; skipped otherwise)"
if [ "$IS_ROOT" = "1" ]; then
  # Use the local Unix socket/auth_socket path; creating test users does not alter root credentials.
  mysql -uroot <<SQL || echo "    Warning: MySQL account provisioning failed (check socket permissions)"
CREATE USER IF NOT EXISTS 'dbmw'@'%' IDENTIFIED BY '$MYSQL_PW';
CREATE USER IF NOT EXISTS 'dbmw'@'localhost' IDENTIFIED BY '$MYSQL_PW';
ALTER USER 'dbmw'@'%' IDENTIFIED BY '$MYSQL_PW';
ALTER USER 'dbmw'@'localhost' IDENTIFIED BY '$MYSQL_PW';
GRANT ALL PRIVILEGES ON *.* TO 'dbmw'@'%' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO 'dbmw'@'localhost' WITH GRANT OPTION;
CREATE DATABASE IF NOT EXISTS dbmw_test;
GRANT ALL PRIVILEGES ON dbmw_test.* TO 'dbmw'@'%';
GRANT ALL PRIVILEGES ON dbmw_test.* TO 'dbmw'@'localhost';
FLUSH PRIVILEGES;
SQL
  # MySQL 8 blocks non-deterministic/data-modifying functions by default.
  mysql -uroot -e "SET GLOBAL log_bin_trust_function_creators=1;" 2>/dev/null || true
  echo "    MySQL account ready: $MYSQL_USER / dbmw_test"

  su postgres -c "psql -v ON_ERROR_STOP=0 -q" <<SQL || echo "    Warning: PostgreSQL account provisioning failed"
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = 'dbmw') THEN
    CREATE ROLE dbmw LOGIN SUPERUSER CREATEDB CREATEROLE PASSWORD '$PG_PW';
  ELSE
    ALTER ROLE dbmw LOGIN SUPERUSER CREATEDB CREATEROLE PASSWORD '$PG_PW';
  END IF;
END
\$\$;
SELECT 'dbmw role ready';
SQL
  su postgres -c "psql -tAc \"SELECT 1 FROM pg_database WHERE datname='dbmw'\"" | grep -q 1 \
    || su postgres -c "createdb -O dbmw dbmw"
  echo "    PostgreSQL account ready: $PG_USER / $PG_DB"
else
  echo "    Not running as root; using existing $MYSQL_USER / $PG_USER accounts"
fi

for i in $(seq 1 60); do
  mysqladmin -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 && break
  sleep 1
done
mysqladmin -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 \
  && echo "    MySQL is ready" || { echo "Error: MySQL is not ready; check MYSQL_USER/MYSQL_PW" >&2; exit 1; }

for i in $(seq 1 60); do pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 && break; sleep 1; done
pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 \
  && echo "    PostgreSQL is ready" || { echo "Error: PostgreSQL is not ready" >&2; exit 1; }
PGPASSWORD="$PG_PW" psql -h127.0.0.1 -U"$PG_USER" -d"$PG_DB" -tAc "select 1" >/dev/null 2>&1 \
  && echo "    PostgreSQL login succeeded" || { echo "Error: PostgreSQL login as $PG_USER failed; check PG_USER/PG_DB/PG_PW" >&2; exit 1; }

echo "==> 2/5 Ensuring MySQL test database $MYSQL_DB exists"
mysql -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 -e "CREATE DATABASE IF NOT EXISTS $MYSQL_DB;" 2>/dev/null

echo "==> 3/5 Configuring (MySQL/PG required; ODBC enabled when unixODBC is installed)"
# Ubuntu 26.04 unixodbc-dev no longer provides odbc_config; also probe odbcinst/sql.h.
ODBC_OPT=OFF
if command -v odbc_config >/dev/null 2>&1 || command -v odbcinst >/dev/null 2>&1 ||
   [ -f /usr/include/sql.h ] || [ -f /usr/local/include/sql.h ]; then
  ODBC_OPT=ON
else
  echo "    unixODBC not found -> ODBC=OFF (run setup-wsl.sh --odbc to compile-check ODBC tests)"
fi
cmake -S "$WORK" -B "$BUILD" -G "Unix Makefiles" \
  -DDBMW_ENABLE_MYSQL=ON \
  -DDBMW_ENABLE_POSTGRES=ON \
  -DDBMW_ENABLE_ODBC="$ODBC_OPT" \
  -DDBMW_BUILD_TESTS=ON \
  -DDBMW_BUILD_INTEGRATION_TESTS=ON

echo "==> 4/5 Building ($JOBS jobs)"
cmake --build "$BUILD" -j"$JOBS"

echo "==> 4.5/5 Integration test binaries"
find "$BUILD" -name 'dbmw_*_integration_test' -type f | sort

echo "==> 5/5 Running MySQL / PostgreSQL integration tests (excluding ODBC)"
export DBMW_TEST_MYSQL_HOST=127.0.0.1
export DBMW_TEST_MYSQL_PORT=3306
export DBMW_TEST_MYSQL_USER="$MYSQL_USER"
export DBMW_TEST_MYSQL_DATABASE="$MYSQL_DB"
export DBMW_TEST_MYSQL_PASSWORD="$MYSQL_PW"
export DBMW_TEST_PG_HOST=127.0.0.1
export DBMW_TEST_PG_PORT=5432
export DBMW_TEST_PG_USER="$PG_USER"
export DBMW_TEST_PG_DATABASE="$PG_DB"
export DBMW_TEST_PG_PASSWORD="$PG_PW"

cd "$BUILD"
ctest -N | grep -i integration || true
ctest -R integration -E odbc --output-on-failure
