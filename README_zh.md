# SQLConduit — C++ 数据库连接中间件

> English: [README.md](README.md) · [详细指南](docs/guide.md)

SQLConduit 为 C++ 应用提供统一的数据库访问层。应用通过同一套 API 使用 MySQL、PostgreSQL、Oracle
和 ODBC 数据库，并由中间件集中处理连接池、参数绑定、事务、超时、路由和运行指标。

它适合需要以下能力的服务：

- 管理一个或多个数据库连接，避免业务代码直接维护连接生命周期；
- 使用参数化 SQL、事务、批量执行、流式查询和异步调用；
- 统一配置重试、熔断、读写路由、限流、SQL 审计和查询缓存；
- 查看完整 SQL、慢 SQL 与连接池统计数据。

项目统一使用 C++17。数据库驱动按需编译，默认全部关闭。

## 快速使用

### 1. 构建

先安装 CMake、C++ 编译器和目标数据库的客户端开发库，然后启用需要的驱动：

```bash
cmake -S . -B build \
  -DSQLCONDUIT_ENABLE_POSTGRES=ON
cmake --build build -j
```

可用开关：

- `SQLCONDUIT_ENABLE_MYSQL=ON`：MySQL，需要 libmysqlclient；
- `SQLCONDUIT_ENABLE_POSTGRES=ON`：PostgreSQL，需要 libpqxx 和 libpq；
- `SQLCONDUIT_ENABLE_ORACLE=ON`：Oracle，需要 OCI（Oracle Instant Client，Basic + SDK）；
- `SQLCONDUIT_ENABLE_ODBC=ON`：SQL Server 等 ODBC 数据库，需要 unixODBC。

Linux、macOS 的依赖安装方式见[详细构建说明](docs/guide.md#构建wsl--linux)。

### 2. 配置数据源

以 PostgreSQL 为例，新建 `config/datasources.json`：

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

配置文件同时支持 JSON（`.json`）和 YAML（`.yaml` / `.yml`），字段与校验规则完全一致。
完整模板见 [JSON](config/datasources.json.example) 和
[YAML](config/datasource.yaml.example)。正式契约见
[JSON Schema](config/sqlconduit.schema.json)；JSON 示例已通过 `$schema` 自动关联，常用 IDE
可直接提示字段、类型、枚举、范围与默认值；YAML 示例通过 `yaml-language-server` 声明复用
同一份契约。生产环境建议使用 `password_env`，不要把密码写入配置文件。

配置采用严格契约：未知字段、错误类型和越界值会直接失败，不再静默忽略或自动修正。文件加载及
跨字段、数据源引用等语义错误返回 `ConfigError`；未向 `Client` 注册所需驱动时返回
`UnknownDriver`，连接建立失败则返回 `ConnectionFailed`。

### 3. 查询和执行

SQL 使用 `?` 占位，参数由驱动原生绑定：

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

常见 CRUD 可以使用结构化构造器，让标识符和绑定参数始终分离：

```cpp
auto built = sqlconduit::sql::Builder::select("users")
    .columns({"id", "name"})
    .where(sqlconduit::sql::eq("status", std::string("active")))
    .where(sqlconduit::sql::ge("age", std::int64_t{18}))
    .orderBy("id")
    .build();

if (built.ok())
    status = client.query(built.statement.sql, built.statement.params, rows);
```

构造器只覆盖可移植的 `SELECT`、`INSERT`、`UPDATE` 和 `DELETE`：值始终留在 `Params`
中，字段加入顺序就是参数顺序；全表更新或删除必须显式调用 `.allowAllRows()`。MySQL 需要
反引号时传入 `common::util::Dialect::MySQL`，默认使用标准双引号。分页、Upsert、生成键、
锁、JOIN、表达式和任意 SQL 方言翻译仍由业务 SQL 负责。

指定数据源时，把名称作为第一个参数：

```cpp
client.query("analytics", "SELECT count(*) FROM events", rows);
```

`Client` 是唯一的高层运行时入口，也是 move-only 类型；每个实例拥有独立的连接拓扑，
析构时自动关闭自己管理的连接池。初始化成功后再次调用 `init()` 会返回
`AlreadyInitialized`，配置更新应使用 `reload()`：

```cpp
sqlconduit::Client client;
auto status = client.addDriver(sqlconduit::drivers::postgres());
if (status.ok()) status = client.init("config/datasources.json");
if (!status.ok()) return 1;

sqlconduit::common::ResultSet rows;
status = client.query("SELECT id, name FROM users", rows);

client.shutdown();
```

每个 `Client` 独立持有查询缓存、SQL 审计、拦截器与观测状态；可通过
`setObserver()` 注册实例级回调，并用 `slowSqlStats()` / `recentSlowSql()` 读取该实例的
慢 SQL 数据。库不再提供隐式的进程级默认客户端。

实例客户端同时提供 future 风格异步接口，任务由该实例自己的执行器和连接拓扑处理：

```cpp
auto pending = client.queryAsync("SELECT id, name FROM users WHERE id = ?",
                                 {std::int64_t(42)});
auto result = pending.get();
if (!result.status.ok()) return 1;
```

`queryAsync()`、`queryAllAsync()`、`executeAsync()`、`executeKeysAsync()`、
`queryEachAsync()`、`executeBatchAsync()` 和 `transactionAsync()` 均不会访问其他
`Client` 的状态。`async.enabled: false` 时会返回一个已经就绪且状态为
`ConfigError` 的 future。

公共头文件的稳定性分层、0.x 兼容规则和当前异步边界见
[公共 API 稳定性约定](docs/api_stability.md)。

### 4. 事务

多条语句必须通过 `transaction()` 固定在同一连接上。回调成功时提交，返回失败或抛出异常时自动回滚：

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

### 5. 实体映射（可选）

`sqlconduit/mapping.h` 是 header-only 适配层，按业务手写的字段声明在 `ResultSet` 与业务结构体之间搬运数据。它不是 ORM——SQL 仍由业务书写，核心引擎零改动：

```cpp
#include "sqlconduit/mapping.h"

struct User {
    std::int64_t id;
    std::string  name;
    std::optional<std::string> email;   // 自动接 SQL NULL
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

类型不符、NULL 落到非 `optional` 成员返回 `MappingError`（不填默认值）；缺列默认跳过、多余列默认忽略，可分别用 `.missingColumns(...)` / `.extraColumns(...)` 收紧。写方向有 `paramsOf` / `insertSql` / `updateSql` / `insertAs` / `updateAs` / `insertBatchAs`，并支持生成键回填。异步查询可先调用 `client.queryAsync()`，再用 `mapping::fromRows<T>()` 映射结果。

### 5.x 例程与索引（`sqlconduit/util.h`）

`common::util` 管的是**调用协议与生命周期**，不做 SQL 方言翻译——例程体由业务按目标方言书写。

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

结构化调用可以一次拿回**全部结果集**，并支持 OUT / INOUT 参数：

```cpp
util::RoutineRef proc{"p", util::RoutineKind::Procedure, "my"};

// 纯 IN：池路径即可，sets 里是这次调用产生的每个结果集
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));
util::CallResult r;
util::call(client, proc, params, r);   // r.sets / r.rowCount() / r.affected

// OUT / INOUT：必须用 Session 重载（需要在同一条连接上回读会话变量）
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});
client.transaction("my", [&](core::Session &s) { return util::call(client, s, proc, params, r); });
// r.outParams[0] 即 OUT 值
```

OUT / INOUT 的方言支持范围：MySQL（需 `Session`）、postgres 函数（池路径即可，值来自结果行前 N 列）；
Oracle 可用 `CallParam::out(common::ValueType::String)` 和 `CallParam::refCursor()` 直接走池路径；
postgres 存储过程与 SQL Server 返回 `NotSupported`。

治理行为：DDL 强制走主库（清除 shadow 标记）、默认 `NonIdempotent`（不重试）、
结构变更后失效该数据源查询缓存；方言不支持的组合（`CREATE OR REPLACE`、
`IF NOT EXISTS`、`CASCADE`、`CONCURRENTLY`、`USING`）一律返回 `NotSupported`，不静默降级。
#### 5.x.1 脚本执行（目录 / 文件列表 / 内存）

`util` 还能批量跑 SQL 脚本：按目录递归收集 `.sql`、给定文件列表、或直接执行内存里的脚本字符串。
每条语句都走与 `createRoutine` / `createIndex` 相同的治理链路（强制主库、不重试、失效缓存），
`;` 会正确规避字符串字面量、注释与 `BEGIN..END` 复合块。

```cpp
util::ScriptOptions o;
o.dataSource = "main";

util::runScriptsInDir(client, "./migrations", o);                                  // 递归执行目录下所有 .sql
util::runScripts(client, {"./a.sql", "./b.sql"}, o);                               // 显式文件列表
util::runScriptText(client, "CREATE TABLE t(id INT); INSERT INTO t VALUES (1);", o); // 内存脚本
```

失败时：文件 / 目录问题返回 `IoError`；语句错误按 `stopOnError`（默认 `true`）首错即停，
`stopOnError=false` 跑完全部、最后一条错误胜出。逐文件结果落在 `perFile`。

### 6. 运行测试

```bash
cmake -S . -B build -DSQLCONDUIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

真实数据库集成测试支持 PostgreSQL、MySQL、Oracle 和 SQL Server（ODBC）。分别通过
`SQLCONDUIT_TEST_PG_*`、`SQLCONDUIT_TEST_MYSQL_*`、`SQLCONDUIT_TEST_ORACLE_*`、`SQLCONDUIT_TEST_ODBC_*`
环境变量提供连接信息：

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

## 集成到你的工程

SQLConduit 以一个核心静态库和四个可选驱动静态库发布。只使用某个驱动时，不需要安装
其他数据库的客户端开发包。

### 方式一：find_package（推荐）

```bash
cmake -S . -B build -DSQLCONDUIT_ENABLE_POSTGRES=ON ...
cmake --build build -j
cmake --install build --prefix /your/prefix
```

```cmake
find_package(sqlconduit REQUIRED COMPONENTS Postgres)
target_link_libraries(your_target PRIVATE sqlconduit::postgres)
```

```cpp
#include <sqlconduit/client.h>
#include <sqlconduit/drivers/postgres.h>

sqlconduit::Client client;
auto status = client.addDriver(sqlconduit::drivers::postgres());
if (status.ok()) status = client.init("database.json");
```

只需要不依赖数据库 SDK 的公共类型和基础能力时，使用 `COMPONENTS Core` 与
`sqlconduit::core`。多个数据库可同时列出组件并链接对应目标。

Oracle 客户端通常不在默认搜索路径。这个路径要在**你自己的工程**上指定（包在安装时不记录任何
客户端路径）：

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/your/prefix \
  -DSQLCONDUIT_OCI_LIBRARY_DIR=/path/to/instantclient/lib
```

`ORACLE_HOME` 与 `LD_LIBRARY_PATH` 也会被自动采纳。

### 方式二：pkg-config

```bash
g++ -std=c++17 app.cpp $(pkg-config --cflags sqlconduit-postgres) \
    $(pkg-config --libs --static sqlconduit-postgres) -o app
```

核心包名为 `sqlconduit`，驱动包名为 `sqlconduit-mysql`、`sqlconduit-postgres`、
`sqlconduit-odbc`、`sqlconduit-oracle`。静态链接驱动时仍需确保对应客户端库位于链接路径；
复杂环境推荐使用 `find_package`。

### 方式三：FetchContent / add_subdirectory

```cmake
include(FetchContent)
FetchContent_Declare(sqlconduit
    GIT_REPOSITORY https://github.com/matergreen/SQLConduit.git
    GIT_TAG        v0.7.0)
FetchContent_MakeAvailable(sqlconduit)

target_link_libraries(your_target PRIVATE sqlconduit::postgres)
```

作为子项目时，驱动开关（`SQLCONDUIT_ENABLE_*`）在你的工程里同样是普通 CMake 选项；
若不想让上层工程产生安装规则，传 `-DSQLCONDUIT_INSTALL=OFF`。

### 方式四：预编译包

[Releases](https://github.com/matergreen/SQLConduit/releases) 提供各平台压缩包
（Linux x86_64/aarch64 的 gcc 与 clang、macOS arm64、Windows MSVC），解压后：
`include/`、`lib/`、`lib/cmake/sqlconduit/`、`lib/pkgconfig/`。正式 Release 同时提供
`SHA256SUMS`、SPDX JSON SBOM 和可用 `gh attestation verify` 验证的构建来源证明。

## 详细文档

连接池、异步 API、实体映射、例程与脚本、PostgreSQL 类型、游标、故障转移、可观测性、
错误码、配置项和驱动扩展等内容见 [SQLConduit 详细指南](docs/guide.md)。
指标兼容性与标签基数约束见 [指标契约](docs/metrics.md)，性能测试方法见
[基准说明](benchmarks/README.md)。版本功能摘要见 [CHANGELOG](CHANGELOG.md)。参与贡献、
安全报告和版本支持范围分别见 [CONTRIBUTING](CONTRIBUTING.md)、[SECURITY](SECURITY.md)
和 [SUPPORT](SUPPORT.md)。
