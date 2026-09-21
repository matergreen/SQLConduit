# dbmw — C++ 数据库连接中间件

> English: [README_en.md](README_en.md) · [详细指南](docs/guide.md)

dbmw 为 C++ 应用提供统一的数据库访问层。应用通过同一套 API 使用 MySQL、PostgreSQL、Oracle
和 ODBC 数据库，并由中间件集中处理连接池、参数绑定、事务、超时、路由和运行指标。

它适合需要以下能力的服务：

- 管理一个或多个数据库连接，避免业务代码直接维护连接生命周期；
- 使用参数化 SQL、事务、批量执行、流式查询和异步调用；
- 统一配置重试、熔断、读写路由、限流、SQL 审计和查询缓存；
- 查看完整 SQL、慢 SQL 与连接池统计数据。

项目使用 C++17；可选协程接口使用 C++20。数据库驱动按需编译，默认全部关闭。

## 快速使用

### 1. 构建

先安装 CMake、C++ 编译器和目标数据库的客户端开发库，然后启用需要的驱动：

```bash
cmake -S . -B build \
  -DDBMW_ENABLE_POSTGRES=ON
cmake --build build -j
```

可用开关：

- `DBMW_ENABLE_MYSQL=ON`：MySQL，需要 libmysqlclient；
- `DBMW_ENABLE_POSTGRES=ON`：PostgreSQL，需要 libpqxx 和 libpq；
- `DBMW_ENABLE_ORACLE=ON`：Oracle，需要 OCI（Oracle Instant Client，Basic + SDK）；
- `DBMW_ENABLE_ODBC=ON`：SQL Server 等 ODBC 数据库，需要 unixODBC；
- `DBMW_ENABLE_ASYNC_CORO=ON`：启用 C++20 协程接口。

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
[YAML](config/datasource.yaml.example)。生产环境建议使用 `password_env`，不要把密码写入配置文件。

### 3. 查询和执行

SQL 使用 `?` 占位，参数由驱动原生绑定：

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

指定数据源时，把名称作为第一个参数：

```cpp
dbmw::DBMW::query("analytics", "SELECT count(*) FROM events", rows);
```

### 4. 事务

多条语句必须通过 `transaction()` 固定在同一连接上。回调成功时提交，返回失败或抛出异常时自动回滚：

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

### 5. 实体映射（可选）

`dbmw/mapping.h` 是 header-only 适配层，按业务手写的字段声明在 `ResultSet` 与业务结构体之间搬运数据。它不是 ORM——SQL 仍由业务书写，核心引擎零改动：

```cpp
#include "dbmw/mapping.h"

struct User {
    std::int64_t id;
    std::string  name;
    std::optional<std::string> email;   // 自动接 SQL NULL
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

类型不符、NULL 落到非 `optional` 成员返回 `MappingError`（不填默认值）；缺列默认跳过、多余列默认忽略，可分别用 `.missingColumns(...)` / `.extraColumns(...)` 收紧。写方向有 `paramsOf` / `insertSql` / `updateSql` / `insertAs` / `updateAs` / `insertBatchAs`，并支持生成键回填。异步侧 `dbmw::async::queryAs<T>` 提供回调 / future / 协程三形态。

### 5.x 例程与索引（`dbmw/util.h`）

`common::util` 管的是**调用协议与生命周期**，不做 SQL 方言翻译——例程体由业务按目标方言书写。

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

结构化调用可以一次拿回**全部结果集**，并支持 OUT / INOUT 参数：

```cpp
util::RoutineRef proc{"p", util::RoutineKind::Procedure, "my"};

// 纯 IN：池路径即可，sets 里是这次调用产生的每个结果集
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));
util::CallResult r;
util::call(proc, params, r);   // r.sets / r.rowCount() / r.affected

// OUT / INOUT：必须用 Session 重载（需要在同一条连接上回读会话变量）
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});
DBMW::transaction("my", [&](core::Session &s) { return util::call(s, proc, params, r); });
// r.outParams[0] 即 OUT 值
```

OUT / INOUT 的方言支持范围：MySQL（需 `Session`）、postgres 函数（池路径即可，值来自结果行前 N 列）；
postgres 存储过程与 SQL Server 返回 `NotSupported`，异步路径同样不支持（无连接亲和）。
异步侧用 `async::util::callAll()` 收集多结果集。

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

util::runScriptsInDir("./migrations", o);                                  // 递归执行目录下所有 .sql
util::runScripts({"./a.sql", "./b.sql"}, o);                               // 显式文件列表
util::runScriptText("CREATE TABLE t(id INT); INSERT INTO t VALUES (1);", o); // 内存脚本
```

失败时：文件 / 目录问题返回 `IoError`；语句错误按 `stopOnError`（默认 `true`）首错即停，
`stopOnError=false` 跑完全部、最后一条错误胜出。逐文件结果落在 `perFile`。
异步见 `async::util::runScriptText` / `runScripts` / `runScriptsInDir`（回调 / future / 协程）；
语句严格串行调度，回调形态返回的 `Handle` 可查询状态或取消剩余脚本。

### 6. 运行测试

```bash
cmake -S . -B build -DDBMW_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

真实数据库集成测试支持 PostgreSQL、MySQL、Oracle 和 SQL Server（ODBC）。分别通过
`DBMW_TEST_PG_*`、`DBMW_TEST_MYSQL_*`、`DBMW_TEST_ORACLE_*`、`DBMW_TEST_ODBC_*`
环境变量提供连接信息：

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

## 详细文档

连接池、异步 API、实体映射、例程与脚本、PostgreSQL 类型、故障转移、可观测性、
错误码、配置项和驱动扩展等内容见 [dbmw 详细指南](docs/guide.md)。
版本功能摘要见 [CHANGELOG](CHANGELOG.md)。
