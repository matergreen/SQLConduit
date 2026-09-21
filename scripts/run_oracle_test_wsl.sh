#!/usr/bin/env bash
set -uo pipefail
SRC=/mnt/d/chiang/sqlconduit
PW="${SQLCONDUIT_TEST_ORACLE_PASSWORD:-SqlConduit!Test123}"
IMAGE=container-registry.oracle.com/database/free:latest
BUILD=/root/sqlconduit/build-it
CLNT=/opt/oracle_client

echo "==> sync source from $SRC (excluding build dir)"
mkdir -p /root/sqlconduit
tar -cf - -C "$SRC" CMakeLists.txt src include tests cmake | tar -xf - -C /root/sqlconduit

echo "==> ensure image $IMAGE"
docker image inspect "$IMAGE" >/dev/null 2>&1 || { echo "==> pulling $IMAGE"; docker pull "$IMAGE"; }

echo "==> start Oracle Free container"
docker rm -f oracle >/dev/null 2>&1 || true
docker run -d --name oracle -e ORACLE_PWD="$PW" -p 1521:1521 "$IMAGE"

echo "==> wait for Oracle ready (DATABASE IS READY TO USE)"
READY=0
for i in $(seq 1 120); do
  if docker logs oracle 2>&1 | grep -q "DATABASE IS READY TO USE"; then READY=1; break; fi
  sleep 5
done
[ "$READY" = 1 ] || { echo "ORACLE NOT READY"; docker logs oracle | tail -30; exit 1; }
echo "ORACLE READY"

OH=$(docker exec oracle bash -c 'echo $ORACLE_HOME')
echo "ORACLE_HOME=$OH"

echo "==> copy full ORACLE_HOME from container (lib+headers+mesg+tz)"
rm -rf "$CLNT"; mkdir -p "$CLNT"
docker cp "oracle:$OH" "$CLNT/home" >/dev/null
echo "$CLNT/home/lib" > /etc/ld.so.conf.d/oracle.conf
ldconfig
ls "$CLNT/home/lib/libclntsh.so"* || { echo "libclntsh.so MISSING"; exit 1; }
ls "$CLNT/home/rdbms/public/oci.h" || { echo "oci.h MISSING"; exit 1; }

echo "==> reconfigure + build with Oracle=ON"
cd /root/sqlconduit
cmake -S . -B "$BUILD" -G "Unix Makefiles" \
  -DSQLCONDUIT_ENABLE_MYSQL=OFF -DSQLCONDUIT_ENABLE_POSTGRES=OFF \
  -DSQLCONDUIT_ENABLE_ODBC=ON -DSQLCONDUIT_ENABLE_ORACLE=ON \
  -DSQLCONDUIT_BUILD_TESTS=ON -DSQLCONDUIT_BUILD_INTEGRATION_TESTS=ON \
  -DOCI_INCLUDE_DIR="$CLNT/home/rdbms/public" \
  -DOCI_LIBRARY="$CLNT/home/lib/libclntsh.so" >/tmp/cmake_ora.log 2>&1 && echo CMAKE_OK || { echo CMAKE_FAIL; tail -25 /tmp/cmake_ora.log; exit 1; }
cmake --build "$BUILD" -j"$(nproc)" >/tmp/build_ora.log 2>&1 && echo BUILD_OK || { echo BUILD_FAIL; tail -40 /tmp/build_ora.log; exit 1; }

export SQLCONDUIT_TEST_ORACLE_HOST=127.0.0.1
export SQLCONDUIT_TEST_ORACLE_PORT=1521
export SQLCONDUIT_TEST_ORACLE_USER=system
export SQLCONDUIT_TEST_ORACLE_SERVICE=FREEPDB1
export SQLCONDUIT_TEST_ORACLE_PASSWORD="$PW"
export ORACLE_HOME="$CLNT/home"
export LD_LIBRARY_PATH="$CLNT/home/lib:$LD_LIBRARY_PATH"
cd "$BUILD/tests"
echo "==> run sqlconduit_oracle_integration_test"
./sqlconduit_oracle_integration_test 2>&1
echo "ORACLE_TEST_EXIT=$?"
