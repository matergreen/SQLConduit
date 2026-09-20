#!/usr/bin/env bash
# 在 WSL 里跑 dbmw 集成测试。
#
# 流程：同步最新源码到 ext4 -> 启动 MySQL/PG -> 供给测试账号 -> 全量编译（含 ODBC 测试）
#       -> 跑 mysql/postgres 集成测试
#
# 用法（在 WSL 里，建议 root：wsl -u root）：
#   bash scripts/run-integration-wsl.sh
#   MYSQL_PW=xxx PG_PW=yyy bash scripts/run-integration-wsl.sh   # 自定义密码
#   MYSQL_USER=root PG_USER=postgres bash scripts/run-integration-wsl.sh  # 用已有账号
#
# 说明：
#   - 源码必须落在 ext4（/root/dbmw），直接在 /mnt/d 编译会踩 DrvFS 的坑，故脚本会先同步。
#   - 以 root 运行时会自动 CREATE 专用测试账号 dbmw（MySQL）与 role dbmw（PG），
#     **不改动** 已有的 root / postgres 密码；想用已有账号就用上面的环境变量覆盖。
#   - ODBC 集成测试只做「编译验证」：WSL 里没有 SQL Server 服务端，跑不了，故 ctest 用 -E odbc 排除。
#   - 依赖缺失时先跑 scripts/setup-wsl.sh --mysql --pg --odbc 装依赖。
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
sleep 3

echo "==> 1.5/5 供给测试账号（root 时自动建；非 root 时跳过，需账号已存在）"
if [ "$IS_ROOT" = "1" ]; then
  # MySQL：走 unix socket + auth_socket 免密，新建账号不影响 root@127.0.0.1 已有密码
  mysql -uroot <<SQL || echo "    警告：MySQL 账号供给失败（可能缺少 socket 权限）"
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
  # MySQL 8 默认禁止创建不确定/修改数据的函数，开一下避免 CREATE FUNCTION 被拒
  mysql -uroot -e "SET GLOBAL log_bin_trust_function_creators=1;" 2>/dev/null || true
  echo "    MySQL 账号 ready: $MYSQL_USER / dbmw_test"

  su postgres -c "psql -v ON_ERROR_STOP=0 -q" <<SQL || echo "    警告：PG 账号供给失败"
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
  echo "    PG 账号 ready: $PG_USER / $PG_DB"
else
  echo "    非 root，跳过账号供给（依赖已存在的 $MYSQL_USER / $PG_USER）"
fi

for i in $(seq 1 60); do
  mysqladmin -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 && break
  sleep 1
done
mysqladmin -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 ping >/dev/null 2>&1 \
  && echo "    MySQL 就绪" || { echo "错误：MySQL 未就绪，检查 MYSQL_USER/MYSQL_PW" >&2; exit 1; }

for i in $(seq 1 60); do pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 && break; sleep 1; done
pg_isready -h127.0.0.1 -p5432 >/dev/null 2>&1 \
  && echo "    PostgreSQL 就绪" || { echo "错误：PostgreSQL 未就绪" >&2; exit 1; }
PGPASSWORD="$PG_PW" psql -h127.0.0.1 -U"$PG_USER" -d"$PG_DB" -tAc "select 1" >/dev/null 2>&1 \
  && echo "    PG 登录 OK" || { echo "错误：PG 用 $PG_USER 登录失败，检查 PG_USER/PG_DB/PG_PW" >&2; exit 1; }

echo "==> 2/5 确保 MySQL 测试库 $MYSQL_DB 存在"
mysql -u"$MYSQL_USER" -p"$MYSQL_PW" -h127.0.0.1 -e "CREATE DATABASE IF NOT EXISTS $MYSQL_DB;" 2>/dev/null

echo "==> 3/5 配置（MySQL/PG 必开；ODBC 若装了 unixODBC 也开，用于验证测试代码能编过）"
# 注意：Ubuntu 26.04 的 unixodbc-dev 不再提供 odbc_config，改探测 odbcinst / sql.h 头文件
ODBC_OPT=OFF
if command -v odbc_config >/dev/null 2>&1 || command -v odbcinst >/dev/null 2>&1 ||
   [ -f /usr/include/sql.h ] || [ -f /usr/local/include/sql.h ]; then
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
