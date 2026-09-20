#!/usr/bin/env bash
# 在 WSL 里跑 dbmw 集成测试。
#
# 流程：同步最新源码到 ext4 -> 启动 MySQL/PG -> 全量编译（含 ODBC 测试）-> 跑 mysql/postgres 集成测试
#
# 用法（在 WSL 里）：
#   MYSQL_PW=<你的mysql root密码> PG_PW=<你的postgres密码> bash scripts/run-integration-wsl.sh
#
# 说明：
#   - 源码必须落在 ext4（/root/dbmw），直接在 /mnt/d 编译会踩 DrvFS 的坑，故脚本会先同步。
#   - ODBC 集成测试只做「编译验证」：WSL 里没有 SQL Server 服务端，跑不了，故 ctest 用 -E odbc 排除。
#   - 依赖缺失时先跑 scripts/setup-wsl.sh --mysql --pg --odbc 装依赖。
set -euo pipefail

MYSQL_PW="${MYSQL_PW:-dbmwtest}"
PG_PW="${PG_PW:-dbmwtest}"
MYSQL_DB="${MYSQL_DB:-dbmw_test}"
WORK=/root/dbmw
BUILD="$WORK/build-it"

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$SELF_DIR/.." && pwd)"
JOBS="$(nproc)"

SUDO=""
if command -v sudo >/dev/null 2>&1; then SUDO="sudo"; fi

echo "==> 0/5 同步源码 $SRC -> $WORK（排除 build*/.git/.workbuddy）"
if [ "$SRC" != "$WORK" ]; then
  rm -rf "$WORK"
  mkdir -p "$WORK"
  ( cd "$SRC" && tar --exclude='./build*' --exclude='./.git' --exclude='./.workbuddy' -cf - . ) \
    | tar -xf - -C "$WORK"
else
  echo "    已在 $WORK，跳过同步"
fi

echo "==> 1/5 启动 MySQL / PostgreSQL"
if ! command -v mysql >/dev/null 2>&1; then
  echo "错误：未找到 mysql 客户端，请先跑 scripts/setup-wsl.sh --mysql --pg" >&2
  exit 1
fi
$SUDO service mysql start || true
$SUDO service postgresql start || true

for i in $(seq 1 60); do
  mysqladmin -uroot -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 && break
  sleep 1
done
mysqladmin -uroot -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 \
  && echo "    MySQL 就绪" || { echo "错误：MySQL 未就绪，检查 MYSQL_PW 是否正确" >&2; exit 1; }

for i in $(seq 1 60); do pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 && break; sleep 1; done
pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 \
  && echo "    PostgreSQL 就绪" || { echo "错误：PostgreSQL 未就绪" >&2; exit 1; }

echo "==> 2/5 确保 MySQL 测试库 $MYSQL_DB 存在"
mysql -uroot -p"$MYSQL_PW" -h127.0.0.1 -e "CREATE DATABASE IF NOT EXISTS $MYSQL_DB;" 2>/dev/null

echo "==> 3/5 配置（MySQL/PG 必开；ODBC 若装了 unixODBC 也开，用于验证测试代码能编过）"
ODBC_OPT=OFF
if command -v odbc_config >/dev/null 2>&1; then
  ODBC_OPT=ON
else
  echo "    未检测到 unixODBC -> ODBC=OFF（想验证 ODBC 测试代码请先跑 setup-wsl.sh --odbc）"
fi
cmake -S "$WORK" -B "$BUILD" -G "Unix Makefiles" \
  -DDBMW_ENABLE_MYSQL=ON \
  -DDBMW_ENABLE_POSTGRES=ON \
  -DDBMW_ENABLE_ODBC="$ODBC_OPT" \
  -DDBMW_BUILD_TESTS=ON \
  -DDBMW_BUILD_INTEGRATION_TESTS=ON

echo "==> 4/5 编译（$JOBS jobs）"
cmake --build "$BUILD" -j"$JOBS"

echo "==> 4.5/5 集成测试二进制"
find "$BUILD" -name 'dbmw_*_integration_test' -type f | sort

echo "==> 5/5 跑 MySQL / PostgreSQL 集成测试（排除 odbc：WSL 无 SQL Server 服务端）"
export DBMW_TEST_MYSQL_HOST=127.0.0.1
export DBMW_TEST_MYSQL_PORT=3306
export DBMW_TEST_MYSQL_USER=root
export DBMW_TEST_MYSQL_DATABASE="$MYSQL_DB"
export DBMW_TEST_MYSQL_PASSWORD="$MYSQL_PW"
export DBMW_TEST_PG_HOST=127.0.0.1
export DBMW_TEST_PG_PORT=5432
export DBMW_TEST_PG_USER=postgres
export DBMW_TEST_PG_DATABASE=postgres
export DBMW_TEST_PG_PASSWORD="$PG_PW"

cd "$BUILD"
ctest -N | grep -i integration || true
ctest -R integration -E odbc --output-on-failure
