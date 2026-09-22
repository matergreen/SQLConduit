#!/usr/bin/env bash
# Run the MySQL and PostgreSQL integration tests against the packages installed
# inside WSL (no Docker). Sources sync into $WORK without wiping existing build
# directories, and this uses its own build dir so the Oracle/ODBC build stays intact.
#
# Usage (inside WSL as root):
#   bash scripts/run_mysql_pg_test_wsl.sh
#
# Override the test accounts with MYSQL_USER / MYSQL_PW / MYSQL_DB and
# PG_USER / PG_PW / PG_DB (defaults: sqlconduit / sqlconduittest).
set -uo pipefail

NC=/mnt/d_nc/chiang/dbmw
SRC=/mnt/d/chiang/dbmw
[ -f "$NC/CMakeLists.txt" ] && SRC="$NC"
WORK=/root/sqlconduit
BUILD=$WORK/build-mypg

MYSQL_USER="${MYSQL_USER:-sqlconduit}"
MYSQL_PW="${MYSQL_PW:-sqlconduittest}"
MYSQL_DB="${MYSQL_DB:-sqlconduit_test}"
PG_USER="${PG_USER:-sqlconduit}"
PG_PW="${PG_PW:-sqlconduittest}"
PG_DB="${PG_DB:-sqlconduit}"

echo "==> [1/6] sync sources from $SRC (existing build dirs preserved)"
mkdir -p "$WORK"
tar -cf - -C "$SRC" CMakeLists.txt src include tests cmake third_party config 2>/dev/null | tar -xf - -C "$WORK"
echo "    SYNC_DONE"

echo "==> [2/6] start services"
service mysql start >/dev/null 2>&1 || true
service postgresql start >/dev/null 2>&1 || true
for i in $(seq 1 30); do mysqladmin -uroot ping >/dev/null 2>&1 && break; sleep 1; done
for i in $(seq 1 30); do pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 && break; sleep 1; done
mysqladmin -uroot ping >/dev/null 2>&1 && echo "    MySQL alive" || { echo "    ERROR: MySQL not reachable"; exit 1; }
pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 && echo "    PostgreSQL ready" || { echo "    ERROR: PostgreSQL not reachable"; exit 1; }
echo "    versions: $(mysql -uroot -N -e 'select version()' 2>/dev/null) | $(su postgres -c 'psql -tAc "select version()"' 2>/dev/null | head -1)"

echo "==> [3/6] provision test accounts"
mysql -uroot <<SQL || echo "    WARN: MySQL provisioning failed"
CREATE USER IF NOT EXISTS '$MYSQL_USER'@'%' IDENTIFIED BY '$MYSQL_PW';
CREATE USER IF NOT EXISTS '$MYSQL_USER'@'localhost' IDENTIFIED BY '$MYSQL_PW';
ALTER USER '$MYSQL_USER'@'%' IDENTIFIED BY '$MYSQL_PW';
ALTER USER '$MYSQL_USER'@'localhost' IDENTIFIED BY '$MYSQL_PW';
GRANT ALL PRIVILEGES ON *.* TO '$MYSQL_USER'@'%' WITH GRANT OPTION;
GRANT ALL PRIVILEGES ON *.* TO '$MYSQL_USER'@'localhost' WITH GRANT OPTION;
CREATE DATABASE IF NOT EXISTS $MYSQL_DB;
GRANT ALL PRIVILEGES ON $MYSQL_DB.* TO '$MYSQL_USER'@'%';
FLUSH PRIVILEGES;
SQL
mysql -uroot -e "SET GLOBAL log_bin_trust_function_creators=1;" 2>/dev/null || true
mysqladmin -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 \
  && echo "    MySQL account ready ($MYSQL_USER / $MYSQL_DB)" \
  || { echo "    ERROR: MySQL login as $MYSQL_USER failed"; exit 1; }

su postgres -c "psql -v ON_ERROR_STOP=0 -q" <<SQL || echo "    WARN: PostgreSQL provisioning failed"
DO \$\$
BEGIN
  IF NOT EXISTS (SELECT FROM pg_roles WHERE rolname = '$PG_USER') THEN
    CREATE ROLE $PG_USER LOGIN SUPERUSER CREATEDB CREATEROLE PASSWORD '$PG_PW';
  ELSE
    ALTER ROLE $PG_USER LOGIN SUPERUSER CREATEDB CREATEROLE PASSWORD '$PG_PW';
  END IF;
END
\$\$;
SQL
su postgres -c "psql -tAc \"SELECT 1 FROM pg_database WHERE datname='$PG_DB'\"" | grep -q 1 \
  || su postgres -c "createdb -O $PG_USER $PG_DB"
PGPASSWORD="$PG_PW" psql -h127.0.0.1 -U"$PG_USER" -d"$PG_DB" -tAc "select 1" >/dev/null 2>&1 \
  && echo "    PostgreSQL account ready ($PG_USER / $PG_DB)" \
  || { echo "    ERROR: PostgreSQL login as $PG_USER failed"; exit 1; }

echo "==> [4/6] cmake configure ($BUILD: MYSQL=ON POSTGRES=ON, ODBC/ORACLE=OFF)"
cmake -S "$WORK" -B "$BUILD" -G "Unix Makefiles" \
  -DSQLCONDUIT_ENABLE_MYSQL=ON -DSQLCONDUIT_ENABLE_POSTGRES=ON \
  -DSQLCONDUIT_ENABLE_ODBC=OFF -DSQLCONDUIT_ENABLE_ORACLE=OFF \
  -DSQLCONDUIT_BUILD_TESTS=ON -DSQLCONDUIT_BUILD_INTEGRATION_TESTS=ON \
  >/tmp/cmake_mypg.log 2>&1 && echo "    CMAKE_OK" \
  || { echo "    CMAKE_FAIL"; tail -30 /tmp/cmake_mypg.log; exit 1; }

echo "==> [5/6] build"
cmake --build "$BUILD" -j"$(nproc)" >/tmp/build_mypg.log 2>&1 && echo "    BUILD_OK" \
  || { echo "    BUILD_FAIL"; tail -40 /tmp/build_mypg.log; exit 1; }

echo "==> [6/6] run integration tests"
export SQLCONDUIT_TEST_MYSQL_HOST=127.0.0.1
export SQLCONDUIT_TEST_MYSQL_PORT=3306
export SQLCONDUIT_TEST_MYSQL_USER="$MYSQL_USER"
export SQLCONDUIT_TEST_MYSQL_DATABASE="$MYSQL_DB"
export SQLCONDUIT_TEST_MYSQL_PASSWORD="$MYSQL_PW"
export SQLCONDUIT_TEST_PG_HOST=127.0.0.1
export SQLCONDUIT_TEST_PG_PORT=5432
export SQLCONDUIT_TEST_PG_USER="$PG_USER"
export SQLCONDUIT_TEST_PG_DATABASE="$PG_DB"
export SQLCONDUIT_TEST_PG_PASSWORD="$PG_PW"

RC=0
echo "################ MySQL integration test ################"
( cd "$BUILD/tests" && ./sqlconduit_mysql_integration_test )
[ $? -eq 0 ] || { echo "MYSQL_TEST_FAILED"; RC=1; }

echo
echo "################ PostgreSQL integration test ################"
( cd "$BUILD/tests" && ./sqlconduit_postgres_integration_test )
[ $? -eq 0 ] || { echo "PG_TEST_FAILED"; RC=1; }

echo
[ "$RC" -eq 0 ] && echo "ALL INTEGRATION TESTS PASSED" || echo "INTEGRATION TESTS FAILED"
exit "$RC"
