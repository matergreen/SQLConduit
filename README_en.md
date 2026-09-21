# dbmw — C++ Database Connection Middleware

> [中文](README.md) · [Detailed guide](docs/guide_en.md)

dbmw gives C++ applications a unified database access layer. The same API works with MySQL,
PostgreSQL, Oracle, and ODBC databases while the middleware centrally manages connection pooling,
parameter binding, transactions, timeouts, routing, and runtime metrics.

It is intended for services that need to:

- manage one or more databases without handling connection lifecycles in business code;
- use parameterized SQL, transactions, batches, streaming reads, and asynchronous calls;
- configure retries, circuit breaking, read/write routing, rate limits, SQL auditing, and caching;
- inspect rendered SQL, slow-query statistics, and connection-pool metrics.

The core uses C++17. The optional coroutine API uses C++20. Database drivers are opt-in and
disabled by default.

## Quick start

### 1. Build

Install CMake, a C++ compiler, and the client development library for your database, then enable
the required driver:

```bash
cmake -S . -B build \
  -DDBMW_ENABLE_POSTGRES=ON
cmake --build build -j
```

Available switches:

- `DBMW_ENABLE_MYSQL=ON`: MySQL; requires libmysqlclient;
- `DBMW_ENABLE_POSTGRES=ON`: PostgreSQL; requires libpqxx and libpq;
- `DBMW_ENABLE_ORACLE=ON`: Oracle; requires OCI (Oracle Instant Client, Basic + SDK);
- `DBMW_ENABLE_ODBC=ON`: ODBC databases such as SQL Server; requires unixODBC;
- `DBMW_ENABLE_ASYNC_CORO=ON`: enable the C++20 coroutine API.

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
#include "dbmw/dbmw.h"

#include <cstdint>
#include <string>

int main() {
    auto status = dbmw::DBMW::init("config/datasources.json");
    if (!status.ok()) return 1;

    dbmw::common::ResultSet rows;
    dbmw::common::Params params{std::int64_t(42)};
    status = dbmw::DBMW::query(
        "SELECT id, name FROM users WHERE id = ?", params, rows);

    std::int64_t affected = 0;
    if (status.ok()) {
        status = dbmw::DBMW::execute(
            "UPDATE users SET last_seen = now() WHERE id = ?", params, affected);
    }

    dbmw::DBMW::shutdown();
    return status.ok() ? 0 : 1;
}
```

Pass a data-source name as the first argument to target a specific source:

```cpp
dbmw::DBMW::query("analytics", "SELECT count(*) FROM events", rows);
```

### 4. Transactions

Use `transaction()` to keep multiple statements on one connection. A successful callback is
committed; a returned error or exception is rolled back automatically:

```cpp
auto status = dbmw::DBMW::transaction([](dbmw::core::Session &session) {
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

`dbmw/mapping.h` is a header-only adapter layer that moves data between a `ResultSet` and your
structs, following a field declaration you write by hand. It is not an ORM — SQL stays in your
code and the engine core is untouched:

```cpp
#include "dbmw/mapping.h"

struct User {
    std::int64_t id;
    std::string  name;
    std::optional<std::string> email;   // receives SQL NULL
};

template <> struct dbmw::mapping::RowMapper<User> {
    static auto describe() {
        return dbmw::mapping::Mapping<User>()
            .field(&User::id,    "id", dbmw::mapping::FieldFlags::PrimaryKey)
            .field(&User::name,  "name")
            .field(&User::email, "email");
    }
};

auto r = dbmw::queryAs<User>("SELECT id, name, email FROM users WHERE id = ?",
                             {std::int64_t(42)});
if (r.status.ok() && !r.items.empty()) use(r.items[0]);
```

Type mismatches and NULL landing in a non-`optional` member yield `MappingError` (no silent default
values). Missing columns are skipped by default and extra columns ignored; each can be tightened via
`.missingColumns(...)` / `.extraColumns(...)`. The write direction offers `paramsOf` / `insertSql` /
`updateSql` / `insertAs` / `updateAs` / `insertBatchAs`, including generated-key back-fill. On the
async side, `dbmw::async::queryAs<T>` comes in callback / future / coroutine form.

### 5.x Routines and indexes (`dbmw/util.h`)

`common::util` owns the **call protocol and the lifecycle** — it does not translate SQL dialects, so
routine bodies are written by the application in the target dialect.

```cpp
#include "dbmw/util.h"
namespace util = dbmw::common::util;

util::CreateRoutineOptions o;
o.dataSource = "main";
auto st = util::createRoutine(R"(CREATE PROCEDURE p(IN x INT) BEGIN UPDATE t SET a = x; END)", o);

util::RoutineRef fn{"public.f", util::RoutineKind::Function, "pg"};
std::string sql;
util::makeCallSql(fn, 2, util::Dialect::Postgres, true, sql);   // SELECT * FROM "public"."f"(?, ?)

common::ResultSet rs;
util::callQuery(sql, {common::Value(std::int64_t(1))}, rs);

util::IndexSpec idx{"t", "idx_t_a", {"a"}, false, false, false, "BTREE"};
util::createIndex(idx);
```

A structured call returns **every result set** the routine produced and can carry OUT / INOUT
parameters:

```cpp
util::RoutineRef proc{"p", util::RoutineKind::Procedure, "my"};

// IN only: the pool path is enough, sets holds every result set from this call
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));
util::CallResult r;
util::call(proc, params, r);   // r.sets / r.rowCount() / r.affected

// OUT / INOUT: the Session overload is required (the session variable must be read
// back on the same connection)
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});
DBMW::transaction("my", [&](core::Session &s) { return util::call(s, proc, params, r); });
// r.outParams[0] is the OUT value
```

OUT / INOUT support: MySQL (requires `Session`) and postgres functions (pool path is enough, the
values are the leading columns of the result row). postgres procedures and SQL Server return
`NotSupported`, and so does the async path (no connection affinity). Use
`async::util::callAll()` to collect multiple result sets asynchronously.

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

util::runScriptsInDir("./migrations", o);     // recursively run every .sql under the directory
util::runScripts({"./a.sql", "./b.sql"}, o);  // explicit file list
util::runScriptText("CREATE TABLE t(id INT); INSERT INTO t VALUES (1);", o);  // in-memory script
```

On failure a missing file/directory yields `IoError`; a statement error stops at the first one when
`stopOnError` (default) is true, or runs everything and lets the last error win when false. Per-file
results land in `perFile`. Async: `async::util::runScriptText` / `runScripts` / `runScriptsInDir`
(callback / future / coroutine). Statements are scheduled strictly in sequence; callback forms
return a `Handle` that can report state or cancel the remaining script.

### 6. Run tests

```bash
cmake -S . -B build -DDBMW_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Live integration tests support PostgreSQL, MySQL, Oracle, and SQL Server (ODBC). Provide
connection details through the `DBMW_TEST_PG_*`, `DBMW_TEST_MYSQL_*`, `DBMW_TEST_ORACLE_*`, and
`DBMW_TEST_ODBC_*` environment variables:

```bash
cmake -S . -B build \
  -DDBMW_ENABLE_POSTGRES=ON \
  -DDBMW_ENABLE_MYSQL=ON \
  -DDBMW_ENABLE_ODBC=ON \
  -DDBMW_ENABLE_ORACLE=ON \
  -DDBMW_BUILD_TESTS=ON \
  -DDBMW_BUILD_INTEGRATION_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Detailed documentation

See the [dbmw detailed guide](docs/guide_en.md) for connection pooling, asynchronous APIs,
entity mapping, routines and scripts, PostgreSQL types, cursors, failover, observability,
configuration, and driver extensions.
See the [changelog](CHANGELOG.md) for release highlights.
