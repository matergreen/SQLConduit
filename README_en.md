# SQLConduit — C++ Database Connection Middleware

> [中文](README.md) · [Detailed guide](docs/guide_en.md)

SQLConduit gives C++ applications a unified database access layer. The same API works with MySQL,
PostgreSQL, Oracle, and ODBC databases while the middleware centrally manages connection pooling,
parameter binding, transactions, timeouts, routing, and runtime metrics.

It is intended for services that need to:

- manage one or more databases without handling connection lifecycles in business code;
- use parameterized SQL, transactions, batches, streaming reads, and asynchronous calls;
- configure retries, circuit breaking, read/write routing, rate limits, SQL auditing, and caching;
- inspect rendered SQL, slow-query statistics, and connection-pool metrics.

The project uses C++17. Database drivers are opt-in and disabled by default.

## Quick start

### 1. Build

Install CMake, a C++ compiler, and the client development library for your database, then enable
the required driver:

```bash
cmake -S . -B build \
  -DSQLCONDUIT_ENABLE_POSTGRES=ON
cmake --build build -j
```

Available switches:

- `SQLCONDUIT_ENABLE_MYSQL=ON`: MySQL; requires libmysqlclient;
- `SQLCONDUIT_ENABLE_POSTGRES=ON`: PostgreSQL; requires libpqxx and libpq;
- `SQLCONDUIT_ENABLE_ORACLE=ON`: Oracle; requires OCI (Oracle Instant Client, Basic + SDK);
- `SQLCONDUIT_ENABLE_ODBC=ON`: ODBC databases such as SQL Server; requires unixODBC.

See the [detailed build instructions](docs/guide_en.md#building-wsl--linux) for Linux and macOS.

### 2. Configure a data source

For PostgreSQL, create `config/datasources.json`:

```json
{
  "default_datasource": "main",
  "pool": {
    "min": 1,
    "max": 8,
    "borrow_timeout_ms": 3000
  },
  "datasources": [
    {
      "name": "main",
      "type": "postgres",
      "host": "127.0.0.1",
      "port": 5432,
      "user": "app",
      "password_env": "APP_DB_PASSWORD",
      "database": "app"
    }
  ]
}
```

```bash
export APP_DB_PASSWORD='your-password'
```

Configuration files support both JSON (`.json`) and YAML (`.yaml` / `.yml`) with identical fields
and validation. Complete templates are available for
[JSON](config/datasources.json.example) and [YAML](config/datasource.yaml.example). Prefer
`password_env` in production instead of storing a password in the file.

### 3. Query and execute

Use `?` placeholders. Values are bound natively by the driver:

```cpp
#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/drivers/postgres.h"

#include <cstdint>
#include <string>

int main() {
    sqlconduit::Client client;
    auto status = client.addDriver(sqlconduit::drivers::postgres());
    if (status.ok()) status = client.init("config/datasources.json");
    if (!status.ok()) return 1;

    sqlconduit::common::ResultSet rows;
    sqlconduit::common::Params params{std::int64_t(42)};
    status = client.query(
        "SELECT id, name FROM users WHERE id = ?", params, rows);

    std::int64_t affected = 0;
    if (status.ok()) {
        status = client.execute(
            "UPDATE users SET last_seen = now() WHERE id = ?", params, affected);
    }

    client.shutdown();
    return status.ok() ? 0 : 1;
}
```

Pass a data-source name as the first argument to target a specific source:

```cpp
client.query("analytics", "SELECT count(*) FROM events", rows);
```

`Client` is the sole high-level runtime entry. It is move-only, owns an independent connection
topology, and closes its pools on destruction. After a successful
`init()`, use `reload()` for configuration changes; another `init()` returns
`AlreadyInitialized`.

```cpp
sqlconduit::Client client;
auto status = client.addDriver(sqlconduit::drivers::postgres());
if (status.ok()) status = client.init("config/datasources.json");
if (!status.ok()) return 1;

sqlconduit::common::ResultSet rows;
status = client.query("SELECT id, name FROM users", rows);

client.shutdown();
```

Each `Client` owns its query cache, SQL audit policy, interceptors, and observability state. Use
`setObserver()` for an instance callback and `slowSqlStats()` / `recentSlowSql()` for that
instance's slow-query data. The library no longer provides an implicit process-wide default client.

Instance clients also provide future-based asynchronous operations backed by that client's own
executor and connection topology:

```cpp
auto pending = client.queryAsync("SELECT id, name FROM users WHERE id = ?",
                                 {std::int64_t(42)});
auto result = pending.get();
if (!result.status.ok()) return 1;
```

`queryAsync()`, `queryAllAsync()`, `executeAsync()`, `executeKeysAsync()`, `queryEachAsync()`,
`executeBatchAsync()`, and `transactionAsync()` never resolve through another `Client`. With
`async.enabled: false`, they return a ready future containing a
`ConfigError` status.

See the [public API stability policy](docs/api_stability.md) for compatibility levels, the 0.x
versioning rules, and the current asynchronous boundary.

### 4. Transactions

Use `transaction()` to keep multiple statements on one connection. A successful callback is
committed; a returned error or exception is rolled back automatically:

```cpp
auto status = client.transaction([](sqlconduit::core::Session &session) {
    std::int64_t affected = 0;
    auto result = session.execute(
        "UPDATE accounts SET balance = balance - ? WHERE id = ?",
        {std::int64_t(100), std::int64_t(1)}, affected);
    if (!result.ok()) return result;

    return session.execute(
        "UPDATE accounts SET balance = balance + ? WHERE id = ?",
        {std::int64_t(100), std::int64_t(2)}, affected);
});
```

### 5. Entity mapping (optional)

`sqlconduit/mapping.h` is a header-only adapter layer that moves data between a `ResultSet` and your
structs, following a field declaration you write by hand. It is not an ORM — SQL stays in your
code and the engine core is untouched:

```cpp
#include "sqlconduit/mapping.h"

struct User {
    std::int64_t id;
    std::string  name;
    std::optional<std::string> email;   // receives SQL NULL
};

template <> struct sqlconduit::mapping::RowMapper<User> {
    static auto describe() {
        return sqlconduit::mapping::Mapping<User>()
            .field(&User::id,    "id", sqlconduit::mapping::FieldFlags::PrimaryKey)
            .field(&User::name,  "name")
            .field(&User::email, "email");
    }
};

auto r = sqlconduit::queryAs<User>(client, "SELECT id, name, email FROM users WHERE id = ?",
                             {std::int64_t(42)});
if (r.status.ok() && !r.items.empty()) use(r.items[0]);
```

Type mismatches and NULL landing in a non-`optional` member yield `MappingError` (no silent default
values). Missing columns are skipped by default and extra columns ignored; each can be tightened via
`.missingColumns(...)` / `.extraColumns(...)`. The write direction offers `paramsOf` / `insertSql` /
`updateSql` / `insertAs` / `updateAs` / `insertBatchAs`, including generated-key back-fill. For an
asynchronous query, call `client.queryAsync()` and then map the rows with `mapping::fromRows<T>()`.

### 5.x Routines and indexes (`sqlconduit/util.h`)

`common::util` owns the **call protocol and the lifecycle** — it does not translate SQL dialects, so
routine bodies are written by the application in the target dialect.

```cpp
#include "sqlconduit/util.h"
namespace util = sqlconduit::common::util;

util::CreateRoutineOptions o;
o.dataSource = "main";
auto st = util::createRoutine(client, R"(CREATE PROCEDURE p(IN x INT) BEGIN UPDATE t SET a = x; END)", o);

util::RoutineRef fn{"public.f", util::RoutineKind::Function, "pg"};
std::string sql;
util::makeCallSql(fn, 2, util::Dialect::Postgres, true, sql);   // SELECT * FROM "public"."f"(?, ?)

common::ResultSet rs;
util::callQuery(client, sql, {common::Value(std::int64_t(1))}, rs);

util::IndexSpec idx{"t", "idx_t_a", {"a"}, false, false, false, "BTREE"};
util::createIndex(client, idx);
```

A structured call returns **every result set** the routine produced and can carry OUT / INOUT
parameters:

```cpp
util::RoutineRef proc{"p", util::RoutineKind::Procedure, "my"};

// IN only: the pool path is enough, sets holds every result set from this call
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));
util::CallResult r;
util::call(client, proc, params, r);   // r.sets / r.rowCount() / r.affected

// OUT / INOUT: the Session overload is required (the session variable must be read
// back on the same connection)
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});
client.transaction("my", [&](core::Session &s) { return util::call(client, s, proc, params, r); });
// r.outParams[0] is the OUT value
```

OUT / INOUT support: MySQL (requires `Session`) and postgres functions (pool path is enough, the
values are the leading columns of the result row). Oracle uses
`CallParam::out(common::ValueType::String)` and `CallParam::refCursor()` directly through the pool.
postgres procedures and SQL Server return `NotSupported`.

Governance: DDL is pinned to the primary (the `shadow` flag is cleared), defaults to
`NonIdempotent` (no retries), and invalidates that data source's query cache after a structural
change. Dialect combinations that do not exist (`CREATE OR REPLACE`, `IF NOT EXISTS`, `CASCADE`,
`CONCURRENTLY`, `USING`) return `NotSupported` instead of silently degrading.

#### 5.x.1 Script execution (directory / file list / in-memory)

`util` can also run SQL scripts in bulk: recursively collect `.sql` files under a directory, take an
explicit file list, or run an in-memory script string. Every statement goes through the same
governance as `createRoutine` / `createIndex` (pinned to primary, no retry, cache invalidated), and
`;` correctly skips string literals, comments and `BEGIN..END` compound blocks.

```cpp
util::ScriptOptions o;
o.dataSource = "main";

util::runScriptsInDir(client, "./migrations", o);     // recursively run every .sql under the directory
util::runScripts(client, {"./a.sql", "./b.sql"}, o);  // explicit file list
util::runScriptText(client, "CREATE TABLE t(id INT); INSERT INTO t VALUES (1);", o);  // in-memory script
```

On failure a missing file/directory yields `IoError`; a statement error stops at the first one when
`stopOnError` (default) is true, or runs everything and lets the last error win when false. Per-file
results land in `perFile`.

### 6. Run tests

```bash
cmake -S . -B build -DSQLCONDUIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Live integration tests support PostgreSQL, MySQL, Oracle, and SQL Server (ODBC). Provide
connection details through the `SQLCONDUIT_TEST_PG_*`, `SQLCONDUIT_TEST_MYSQL_*`, `SQLCONDUIT_TEST_ORACLE_*`, and
`SQLCONDUIT_TEST_ODBC_*` environment variables:

```bash
cmake -S . -B build \
  -DSQLCONDUIT_ENABLE_POSTGRES=ON \
  -DSQLCONDUIT_ENABLE_MYSQL=ON \
  -DSQLCONDUIT_ENABLE_ODBC=ON \
  -DSQLCONDUIT_ENABLE_ORACLE=ON \
  -DSQLCONDUIT_BUILD_TESTS=ON \
  -DSQLCONDUIT_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Integrating into your project

SQLConduit ships as a core static library plus four optional driver archives. A consumer linking
one driver does not need the client SDKs for the other databases.

### Option 1: find_package (recommended)

```bash
cmake -S . -B build -DSQLCONDUIT_ENABLE_POSTGRES=ON ...
cmake --build build -j
cmake --install build --prefix /your/prefix
```

```cmake
find_package(sqlconduit REQUIRED COMPONENTS Postgres)
target_link_libraries(your_target PRIVATE sqlconduit::postgres)
```

Register the selected driver before initialization:

```cpp
#include <sqlconduit/client.h>
#include <sqlconduit/drivers/postgres.h>

sqlconduit::Client client;
auto status = client.addDriver(sqlconduit::drivers::postgres());
if (status.ok()) status = client.init("database.json");
```

The Oracle client is usually not on the default search path. Point at it from **your own project**
(the package records no client path when it is installed):

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/your/prefix \
  -DSQLCONDUIT_OCI_LIBRARY_DIR=/path/to/instantclient/lib
```

`ORACLE_HOME` and `LD_LIBRARY_PATH` are honoured as well.

### Option 2: pkg-config

```bash
g++ -std=c++17 app.cpp $(pkg-config --cflags sqlconduit-postgres) \
    $(pkg-config --libs --static sqlconduit-postgres) -o app
```

The core package is `sqlconduit`; driver packages are `sqlconduit-mysql`,
`sqlconduit-postgres`, `sqlconduit-odbc`, and `sqlconduit-oracle`. For complex static-link
environments, prefer `find_package`.

### Option 3: FetchContent / add_subdirectory

```cmake
include(FetchContent)
FetchContent_Declare(sqlconduit
    GIT_REPOSITORY https://github.com/matergreen/SQLConduit.git
    GIT_TAG        v0.7.0)
FetchContent_MakeAvailable(sqlconduit)

target_link_libraries(your_target PRIVATE sqlconduit::postgres)
```

As a subproject the driver switches (`SQLCONDUIT_ENABLE_*`) are ordinary CMake options in your
build; pass `-DSQLCONDUIT_INSTALL=OFF` if you do not want SQLConduit to contribute install rules to
the parent project.

### Option 4: pre-built archives

[Releases](https://github.com/matergreen/SQLConduit/releases) publish an archive per platform
(Linux x86_64/aarch64 for gcc and clang, macOS arm64, Windows MSVC). Unpacking gives you `include/`,
`lib/`, `lib/cmake/sqlconduit/`, and `lib/pkgconfig/`.

## Detailed documentation

See the [SQLConduit detailed guide](docs/guide_en.md) for connection pooling, asynchronous APIs,
entity mapping, routines and scripts, PostgreSQL types, cursors, failover, observability, error
codes, configuration, and driver extensions.
See the [changelog](CHANGELOG.md) for release highlights.
