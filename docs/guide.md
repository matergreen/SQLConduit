# SQLConduit 详细指南

> English version: [guide_en.md](guide_en.md) · 快速入门：[README.md](../README.md)

一个使用 C++17 开发的数据库连接中间件，支持：

- **多数据源**：一份 JSON 配置描述任意多个数据源，按名字分发。
- **连接池**：每个数据源独立的线程安全连接池，支持借出校验、失效重建、最小/最大连接数。
- **池化可开关**：`pool.enabled` 决定是否复用连接；关闭后每次操作新建并关闭一条物理连接，
  适合低频定时任务、短命进程，或数据库侧对长连接有严格限制的场景。上层 API 用法不变。
- **心跳保活**：后台线程周期 `ping` 空闲连接，失效自动回收，并补足到最小连接数。
- **生产连接生命周期**：空闲回收、最大寿命轮换、借出泄漏告警、池运行指标。
- **事务与会话**：`transaction()` 在一条独占连接上执行多条语句，成功提交、失败或抛异常自动回滚。
- **事务增强**：隔离级别、只读事务、整体超时取消和保存点。
- **参数化查询**：`?` 占位符 + 绑定参数，杜绝 SQL 字符串拼接带来的注入风险。
- **预编译语句 / 生成键 / 大参数流式**：连接级预编译句柄缓存（热点 SQL 透明只 prepare 一次）、`execute` 回吐自增主键（`GeneratedKeys`）、超大 BLOB 按块流式写入（`StreamSource`）。详见下文「预编译语句 / 生成键 / 大参数流式」一节。
- **韧性与路由**：只读查询安全重试、指数退避、熔断/半开、主从读写路由及写后读窗口。
- **主库故障转移**：在外部系统已保证单主/fencing 且配置显式确认后，写路径才会按序切换候选；易失写缓冲也需单独确认丢失与重复风险，且绝不用于事务。
- **限流与背压**：按数据源总 QPS 与（可选）单 SQL 指纹 QPS 做令牌桶限速，超限快速失败返回 `RateLimited`，不重试、不打满连接池。
- **SQL 审计与拦截**：执行前对 SQL 做轻量静态分析，可拦截无 WHERE 的 UPDATE/DELETE、无 LIMIT 的 SELECT、只读数据源上的写，以及按指纹黑名单/白名单拦截；灰度期 `action=warn` 仅告警。
- **查询结果缓存**：按 `(数据源 + 原始SQL + 类型标记参数)` 缓存非事务读，LRU + TTL + 内存上限，写后按数据源失效；默认关闭。
- **大数据处理**：逐行回调、批量执行；MySQL/ODBC 按行消费，PostgreSQL 使用服务端游标分块。
- **可观测性**：可控的完整 SQL 日志、慢 SQL 聚合/最近记录、连接池明细快照与操作事件回调。
- **定时统计落日志**：后台线程按 `observability.stats_report` 周期采样连接池与慢 SQL 统计，
  追加写入日志文件（text 可读 / json 一行一对象，便于采集器摄入），不占用业务线程。
- **结果集行数护栏**：每个数据源可设 `max_result_rows`，`query()` 物化超过上限直接报错
  并提示改用 `queryEach()` 流式消费，防止一条漏 LIMIT 的查询吃爆进程内存。
- **安全配置**：环境变量密码、TLS、驱动错误脱敏与结构化 SQLSTATE。
- **热加载**：新配置完整创建后原子切换，并在宽限期内排空旧连接池。
- **多数据库类型**：内置 **MySQL / PostgreSQL / Oracle（OCI）/ ODBC（SQL Server）** 驱动，
  并预留**驱动扩展接口**，新增数据库只需实现 `IDriver` 并注册。

> 状态：核心层（配置/连接池/心跳/事务/参数绑定/门面）已完整实现，
> 并通过 `tests/sqlconduit_core_test.cpp` 的 146 项行为验证（mock 驱动，无需真实数据库）。
>
> 驱动实现进度：
> - **MySQL 已完整实现**（libmysqlclient）：连接超时/字符集、ping、按列类型映射结果集、
>   显式事务、`mysql_real_escape_string` 转义；并支持 **`mysql_stmt_prepare` 服务端预编译**
>   （连接级句柄缓存）、`mysql_insert_id` **生成键**、大 BLOB 统一参数接口。
> - **PostgreSQL 已完整实现**（libpqxx）：connstring 拼接/超时/字符集、ping、
>   结果集按 OID 映射、显式事务；通过 `pqxx::params` 支持**服务端参数绑定**（内部经 `execParams()` 封装），
>   并支持命名预备语句 **预编译缓存**（LRU `DEALLOCATE`）、`RETURNING` **生成键**、大 BLOB 统一参数接口。
> - **ODBC 已完整实现**（unixODBC）：DSN/连接串、诊断记录、类型映射、原生参数绑定、
>   查询超时/取消、事务与 SQL Server/标准保存点方言；并支持 **`SQLPrepare`/`SQLExecute` 预编译缓存**、
>   `OUTPUT INSERTED`/`RETURNING` **生成键**、大 BLOB 统一参数接口。
> - **Oracle 已完整实现**（OCI / Instant Client）：`OCILogon2` 连接、`?` → `:n` 占位符改写、
>   按 SQLT 类型映射结果集、原生 `OCIBindByPos` 参数绑定、LOB 流式读取、事务与保存点、
>   `OCIBreak` 取消；并支持 **`OCIStmtPrepare2` 语句缓存**（LRU 淘汰）、
>   `RETURNING ... INTO` **生成键**、大 BLOB 统一参数接口。详见[Oracle 驱动](#oracle-驱动oci)。
>
> 预编译与生成键为四个驱动各自实现；大参数流式（`StreamSource`）当前四驱动统一以**缓冲降级**实现
> （一次性读成 `Blob` 再按普通参数绑定），调用代码保持一致，MySQL `send_long_data` / ODBC `SQLPutData` 真分块为后续增强。
> 四个驱动均由 `SQLCONDUIT_ENABLE_*` 编译期开关控制。

---

## 目录结构

```
include/sqlconduit/
  common/    types.h(值/行/结果集/状态/错误码)  observer.h(观测事件)  logger.h(轻量日志)
  config/    datasource_config.h  config_loader.h(解析 JSON)
  core/      idatabase_connection.h(连接抽象 + 流式/批量默认能力)
             connection_pool.h     heartbeat_manager.h  database_manager.h
  driver/    idriver.h  driver_registry.h  driver_factory.h
             mysql_driver.h  postgres_driver.h  odbc_driver.h
  async/     async_types.h(结果体/Handle)  executor.h(IExecutor/线程池)
             sqlconduit_async.h(异步门面)  task.h(协程层，可选 C++20)
  mapping.h  (实体映射层 v0.5.0：header-only，Row ↔ 业务实体，读写双向)
  sqlconduit.h     (对外门面)
src/         对应实现
tests/       sqlconduit_core_test.cpp  sqlconduit_async_test.cpp  sqlconduit_coro_test.cpp(coro=ON)
             sqlconduit_mapping_test.cpp(实体映射)
config/      datasources.json.example  datasource.yaml.example
third_party/nlohmann/json.hpp  (vendored 单头，离线可用)
scripts/     setup-wsl.sh
```

## 构建（WSL / Linux）

```bash
# 1) 安装工具链（按需开启的驱动选择性安装）
sudo apt update
sudo apt install -y build-essential cmake
# 开启 MySQL:  sudo apt install -y default-libmysqlclient-dev
# 开启 PG:     sudo apt install -y libpqxx-dev libpq-dev
# 开启 ODBC:   sudo apt install -y unixodbc-dev
# 开启 Oracle: 手工装 Oracle Instant Client（Basic + SDK），见「Oracle 驱动（OCI）」

# 2) 配置 + 构建
mkdir -p build && cd build
cmake ..                                   # 仅核心层
# 启用驱动示例：
# cmake .. -DSQLCONDUIT_ENABLE_MYSQL=ON -DSQLCONDUIT_ENABLE_POSTGRES=ON -DSQLCONDUIT_ENABLE_ODBC=ON
# 启用协程层（可选，仅 task.cpp 提标 C++20）：
# cmake .. -DSQLCONDUIT_ENABLE_ASYNC_CORO=ON
cmake --build .

```

运行测试（可选，不需要真实数据库，用 mock 驱动验证核心语义）：

```bash
cmake .. -DSQLCONDUIT_BUILD_TESTS=ON && cmake --build . && ctest --output-on-failure
# 或直接执行： ./tests/sqlconduit_core_test
```

也可一键执行 `scripts/setup-wsl.sh`（按参数安装依赖并构建）。

## 构建（macOS）

macOS 用 [Homebrew](https://brew.sh) 管理依赖，编译器走系统 **clang++**（需先装 Xcode Command Line Tools）。Homebrew 的包装在 `/opt/homebrew`（Apple Silicon）或 `/usr/local`（Intel），CMake 默认搜索路径未必覆盖，建议显式用 `CMAKE_PREFIX_PATH` 指明客户端库位置。

> **注意**：`SQLCONDUIT_ENABLE_ODBC` 与 `SQLCONDUIT_ENABLE_ORACLE` 默认都是 `OFF`。需要 ODBC 时先安装 unixODBC，
> 需要 Oracle 时先装 Instant Client（Basic + SDK），再显式传入对应开关。

```bash
# 1) 命令行工具（提供 clang++ / make）
xcode-select --install

# 2) 安装依赖（与 Linux apt 对应的三个客户端库）
brew install mysql-client libpqxx libpq unixodbc
#    - mysql-client 是 keg-only，不会自动软链到 /usr/local，必须显式加入 CMAKE_PREFIX_PATH
#    - libpqxx 依赖 libpq；unixodbc 提供 ODBC 头与 libodbc

# 3) 配置 + 构建（用 brew --prefix 定位头文件与库；分号分隔多个路径）
mkdir -p build && cd build
cmake .. \
  -DCMAKE_PREFIX_PATH="$(brew --prefix);$(brew --prefix mysql-client)" \
  -DSQLCONDUIT_ENABLE_MYSQL=ON -DSQLCONDUIT_ENABLE_POSTGRES=ON -DSQLCONDUIT_ENABLE_ODBC=ON
# 启用 Oracle（Instant Client 解压后的目录）：
#   -DSQLCONDUIT_ENABLE_ORACLE=ON -DOCI_INCLUDE_DIR=.../sdk/include -DOCI_LIBRARY=.../libclntsh.dylib
cmake --build . -j"$(sysctl -n hw.ncpu)"

```

> 只启用部分驱动时，删掉对应 `-DSQLCONDUIT_ENABLE_*` 并去掉 `CMAKE_PREFIX_PATH` 里未安装的包（未安装的 `brew --prefix <pkg>` 会报错）；核心层不需要任何客户端库，可直接 `cmake ..` 构建。

运行测试（可选，mock 驱动、无需真实数据库）：

```bash
cmake .. -DSQLCONDUIT_BUILD_TESTS=ON && cmake --build . -j"$(sysctl -n hw.ncpu)" && ctest --output-on-failure
```

## 快速使用

```cpp
#include "sqlconduit/sqlconduit.h"

sqlconduit::SQLConduit::init("config/datasources.json");   // 加载多数据源 + 启动心跳

sqlconduit::common::ResultSet rs;
auto st = sqlconduit::SQLConduit::query("SELECT 1", rs);    // 默认数据源
if (st.ok()) { /* 处理 rs */ }

int64_t n = 0;
sqlconduit::SQLConduit::execute("UPDATE t SET c = 1 WHERE id = 2", n); // 默认数据源

sqlconduit::SQLConduit::shutdown();
```

指定数据源：`sqlconduit::SQLConduit::query("pg", "SELECT now()", rs);`

## 事务与会话

`query()` / `execute()` 每次都会**重新借一条连接**，因此跨多条语句的事务必须先把连接固定下来：

```cpp
auto st = sqlconduit::SQLConduit::transaction([](sqlconduit::core::Session& s) {
    int64_t n = 0;
    if (auto r = s.execute("UPDATE accounts SET bal = bal - 100 WHERE id = 1", n); !r.ok())
        return r;                       // 返回失败 -> 自动 rollback
    return s.execute("UPDATE accounts SET bal = bal + 100 WHERE id = 2", n);
});                                      // 返回成功 -> 自动 commit
```

- 回调**抛异常**同样会触发回滚（异常被捕获后转为 `TxError`，不会逃逸出 `transaction`）。
- 回调内部可以自行 `commit()` / `rollback()`，外层检测到事务已结束就不会重复提交。
- 不需要事务、只想在一条连接上连做几件事（临时表、会话变量等）时用 `withSession()`。

## 参数化查询

SQL 中用 `?` 作占位符，参数值通过 `common::Params` 传入，**不参与 SQL 字符串拼接**：

```cpp
sqlconduit::common::ResultSet rs;
sqlconduit::common::Params p{ std::string("O'Brien"), std::int64_t(42) };
auto st = sqlconduit::SQLConduit::query("SELECT * FROM t WHERE name = ? AND age > ?", p, rs);
```

- PostgreSQL / MySQL / Oracle / ODBC 均走**原生参数绑定**。
  其中 Oracle 会把 `?` 改写成 `:n`（1-based）后再 `OCIBindByPos`；改写器会跳过字符串、标识符与注释里的 `?`。
- 自定义驱动未实现原生绑定时默认返回 `NotSupported`，不会静默退化为 SQL 拼接。
- 仅明确覆盖 `allowsLiteralInterpolation()` 的兼容驱动才会启用字面量插值；扫描器会跳过
  字符串、标识符与注释里的 `?`。
- 占位符数量与参数数量不一致时返回 `QueryError`，不会静默产生错误 SQL。

常用数据库专有类型会保留语义，而不是全部退化成 `string`/`double`：

| C++ 类型 | 数据库类型 | 说明 |
|---|---|---|
| `std::uint64_t` | MySQL unsigned integer | 完整覆盖 `BIGINT UNSIGNED`；PostgreSQL 绑定时以十进制文本发送 |
| `common::Decimal` | DECIMAL / NUMERIC | 保存原始十进制文本，金额和高精度数不会经过 `double` |
| `common::Date` / `common::Time` | DATE / TIME | 与时间点 `Timestamp` 分开，避免日期、时刻被错误附加时区 |
| `common::Uuid` | PostgreSQL UUID / ODBC GUID | 保留 UUID 类型语义 |
| `common::Json` | PostgreSQL JSON/JSONB、MySQL JSON | 保留 JSON 文本，解析策略由业务决定 |

这些包装类型都有公开的 `value` 字段，例如 `common::Decimal{"12.3400"}`；参数绑定、
完整 SQL 诊断、查询缓存键和预编译类型签名都会区分这些类型。

## 预编译语句 / 生成键 / 大参数流式

三个官方驱动都具备、此前 `IDatabaseConnection` 尚未封装的高频能力，现已统一接入。三者都**不破坏现有架构不变量**
（闸门只在 `DataSource` 入口过一次、结果缓存键不变、故障转移/写缓冲不用于事务）。

> 注意：`prepare` / `executePrepared` 显式句柄 API 只存在于 `Session`（句柄绑定具体连接，无状态门面持有不了跨调用的句柄）；
> 生成键与大参数流式在 `DataSource`（`SQLConduit::dataSource()` 取得）与 `Session` 上都有。

### 预编译语句复用（连接级句柄缓存）

`SQLConduit::query(sql, params)` / `execute(sql, params)` 在驱动支持且 `prepared_cache.enabled` 开启时，
内部按 `(归一化 SQL + 参数类型签名)` 在本连接的缓存里查已编译句柄，没有就 `prepare` 并存入，再用
`executePrepared` 执行。**对调用方完全透明、签名不变**——热点 SQL 自动只 prepare 一次。

```cpp
// 透明自动缓存：用法与原来完全一致，无需任何改动
sqlconduit::common::ResultSet rs;
sqlconduit::common::Params p{ std::int64_t(1) };
auto st = sqlconduit::SQLConduit::query("SELECT * FROM t WHERE id = ?", p, rs);
```

需要在稳定连接上精细控制、或批量复用同一句柄时，用 `Session` 的显式句柄 API：

```cpp
auto st = sqlconduit::SQLConduit::transaction([](sqlconduit::core::Session& s) {
    sqlconduit::core::PreparedStatementHandle h;
    // typesSample 仅用于推导参数类型签名（占位值即可，不需要真实数据）
    if (auto r = s.prepare("INSERT INTO t(a,b) VALUES(?,?)",
                           sqlconduit::common::Params{std::int64_t(0), std::string("")}, h); !r.ok())
        return r;
    int64_t n = 0;
    for (const auto& row : rowsToInsert)
        if (auto r = s.executePrepared(h, sqlconduit::common::Params{row.a, row.b}, n); !r.ok())
            return r;
    return sqlconduit::common::Status::OK();
});
```

- 句柄生命周期绑定到"当前这条物理连接"，仅在连接/`Session` 存活期内有效；连接归还/关闭后句柄失效，
  驱动随 `close()` 调 `closeAllPrepared()` 释放原生句柄（MySQL `mysql_stmt_close` / PG `DEALLOCATE` / ODBC `SQLFreeHandle` / Oracle `OCIStmtRelease`）。
- 缓存挂在驱动连接对象上，连接归还池后保留、下次借到同一连接直接复用；池是每数据源独立的，不会跨数据源串。
- `max_per_connection > 0` 时按 LRU 驱逐最久未用句柄；`0` = 不限制（靠连接关闭自然回收）。
- 预编译执行仍经过 `DataSource` 入口的 `preGate`（一次），结果缓存键逻辑不变，审计仍按 SQL 文本分类。

### 生成键 / 自增 ID（GeneratedKeys）

`execute` 新增带生成键的重载，回吐刚插入生成的列：

```cpp
auto ds = sqlconduit::SQLConduit::dataSource();          // 默认数据源（也可传名字取指定源）
int64_t n = 0;
sqlconduit::common::GeneratedKeys keys;

// MySQL：开箱即得，无需改 SQL
ds->execute("INSERT INTO t(name) VALUES('x')", n, keys);
int64_t id = keys.lastInsertId();            // MySQL 自增主键

// PostgreSQL / Oracle / ODBC：靠 SQL 自带 RETURNING / OUTPUT 直出，SQLConduit 不自动补
ds->execute("INSERT INTO t(name) VALUES('x') RETURNING id", n, keys);
if (!keys.empty()) id = keys.rows[0].asInt64(0);  // 取 RETURNING 出来的第一列
```

统一模型：`GeneratedKeys` 始终是"生成列的结果集"——MySQL 用 `mysql_insert_id` 合成一行一列，
PG/Oracle/ODBC 用 `RETURNING`/`OUTPUT` 直出。**SQLConduit 不会给你自己写的 SQL 自动追加 `RETURNING`**（那会改写语义并耦合方言），
因此 PG/ODBC 想拿自增 id 就在 SQL 里自己写 `RETURNING id`；Oracle 需写成
`RETURNING id INTO :2`（`:1` 已被 `VALUES(?)` 占用），驱动会解析 `RETURNING ... INTO` 并回读。
无 `RETURNING` 且非 MySQL 自增时 `keys.empty()` 为真（不报错）。
复用同一 `GeneratedKeys` 对象前调用 `keys.clear()`，避免重试着法残留旧行被当成这次生成的键。

> 这条约束只针对**调用方传入的 SQL**。SQL 由 SQLConduit 自己生成的场景（实体映射层的
> `insertAs` / `insertBatchAs`）不在此列：那里 SQLConduit 会按方言补全，见「写」一节。
> 区界线：谁写的 SQL 谁负责，SQLConduit 只对自己生成的那部分负责。

### 大参数流式（StreamSource）

超大 BLOB/CLOB 不必整体物化进内存：用 `StreamSource` 包裹一个同步读取回调或 `std::istream`，
执行期间由驱动按块拉取。这是**输入方向**的流式，与结果集的流式消费（`queryEach`/游标）方向相反，不要混用。

```cpp
auto ds = sqlconduit::SQLConduit::dataSource();
std::ifstream f("big.bin", std::ios::binary);
sqlconduit::common::StreamParams sp{ std::int64_t(1), sqlconduit::common::StreamSource(f) };
int64_t n = 0;
ds->execute("INSERT INTO t(id, blob) VALUES(?, ?)", sp, n);   // 或 query / executeBatch
```

- `StreamSource(read, totalSize, isBinary)`：自定义 `read(buf, n)` 回调返回本块字节数（0=EOF）；
  也可直接 `StreamSource(std::istream&)` 便捷构造。用 istream 构造时，流必须在本次执行期间保持存活。
- `isBinary=true` 表二进制（bytea/blob），`false` 表文本（clob）。
- **当前四驱动统一以缓冲降级实现**：`StreamSource` 一次性读成 `Blob` 再按普通参数绑定
  （libpq 协议不支持参数 data-at-execution，PG 天然如此；MySQL `send_long_data` / ODBC `SQLPutData` 真分块为后续增强）。
  调用代码保持一致、不受驱动差异影响。
- 含 `StreamSource` 的查询**不进结果缓存**（流式内容不是定值，无法参与 `cacheKey`），审计照常按 SQL 文本分类、流内容绝不进日志。

### 配置

```json
{
  "prepared_cache": {
    "enabled": true,
    "max_per_connection": 0
  }
}
```

- `enabled` 默认 `true`（纯性能优化、透明、无副作用）；关掉则退化为每次重绑路径。
- `max_per_connection`：`0` = 不限制；`>0` 触发 LRU 驱逐。
- 生成键 / 大参数流式为 API 驱动，无需配置开关。

## 超时、取消与事务选项

数据源的 `query_timeout_ms` 会映射到 PostgreSQL `statement_timeout`、ODBC
`SQL_ATTR_QUERY_TIMEOUT` 和 MySQL 客户端读写期限；Oracle 映射到 `OCI_ATTR_CALL_TIME`
（服务端调用超时）。此外 `cancel()` 会走 `OCIBreak` 中断在途语句。事务还可设置隔离级别、只读和整体期限：

```cpp
sqlconduit::common::TransactionOptions options;
options.isolation = sqlconduit::common::IsolationLevel::Serializable;
options.readOnly = false;
options.timeout = std::chrono::seconds(5);

auto st = sqlconduit::SQLConduit::transaction(options, [](sqlconduit::core::Session& s) {
    s.savepoint("before_optional_step");
    // ...
    return sqlconduit::common::Status::OK();
});
```

期限到达时中间件会从监控线程请求驱动取消当前语句，并回滚事务。

> **`options.timeout` 是尽力而为的上限，不是硬中断。**
> 驱动实现了 `cancel()`（三个内置驱动都实现了）时语句会被真正打断；驱动未实现时
> C++ 无法安全地强杀正在执行的用户回调，只能等它自然结束后把结果改写成
> `QueryTimeout`。后一种情况下返回的 `message` 会带有 `could not cancel` 提示，
> 便于区分"被及时取消"和"其实没打断，只是事后判了超时"。

取消路径本身是异常安全的：看门狗线程会吞掉驱动 `cancel()` 抛出的任何异常。
线程里逃逸异常会导致 `std::terminate`，这类"为了健壮性而加的机制反而成为崩溃点"
的问题必须在框架侧挡住。

## 流式读取与批量执行

```cpp
std::uint64_t rows = 0;
sqlconduit::SQLConduit::queryEach("SELECT * FROM large_table", {},
    [](const sqlconduit::common::Row& row) {
        // 返回 false 可提前停止。
        return consume(row);
    }, rows);

sqlconduit::common::ParamBatch batch{
    {std::int64_t(1), std::string("a")},
    {std::int64_t(2), std::string("b")}
};
sqlconduit::common::BatchResult result;
sqlconduit::SQLConduit::executeBatch("INSERT INTO t(id, name) VALUES(?, ?)", batch, result);
```

**批量执行是原子的**，三个驱动行为一致：调用方未开事务时中间件自动包一层事务，
中途任何一组失败都整批回滚，且 `BatchResult` 不会留下部分影响行数（避免调用方
误以为前几组已落库）。调用方已在事务中时则沿用外层事务，回滚范围由调用方决定。

> 实现注意：驱动若覆盖 `executeBatch` 追求更高性能（数组绑定 / COPY），
> 必须同时覆盖 `inTransaction()` 返回真实事务状态，并保持同样的原子性保证。

## 游标（Cursor）

`query()` 一次借连接、物化全部结果、归还；`queryEach()` 流式但每条回调内仍是一次性消费。
**游标**则把连接生命周期从"借→用→还"变成"借→钉住→取 N 次→显式关→还"：
一条物理连接（PostgreSQL 上连带其事务）被游标持有，直到 `close()` 或析构才归还。
适合"结果集大、想按批可控消费、且不想一次物化进内存"的场景。

```cpp
sqlconduit::core::CursorOptions opts;
opts.batch_size = 1000;          // 每次 fetch 预取行数（也可用配置 default_batch_size 兜底）
opts.auto_transaction = true;    // PG 未开事务时由游标自建事务兜底

std::unique_ptr<sqlconduit::core::Cursor> cur;
auto st = sqlconduit::SQLConduit::openCursor("SELECT * FROM large_table WHERE k > ?",
                                 sqlconduit::common::Params{std::int64_t(0)}, opts, cur);
if (!st.ok()) { /* 处理错误 */ }

sqlconduit::common::ResultSet batch;
while (cur->fetch(0, batch).ok() && cur->hasNext()) {  // fetch(0) = 按 batch_size 取
    consume(batch);
    batch.clear();
}
cur->close();   // 显式归还连接；不调也会在析构时关 + 还
```

门面 `SQLConduit::openCursor` 有两个重载：默认数据源，或指定数据源名。事务/会话内另可用
`Session::openCursor(...)`（连接不额外占用，随会话结束归还）。`fetch(n, out)` 把至多 n 行**追加**
写入 `out`（不清空，多次 fetch 可累积同一结果集）；`n == 0` 由驱动按 batch_size 决定。`fetchRow`
取单行、`close` 显式关闭（幂等）、`isOpen` / `hasNext` / `rowsFetched` 暴露状态。

### 两种绑定

- **`OwnsHandle`（独立游标，默认）**：从连接池借一条连接并钉住，直到游标关闭/析构才归还。
  期间该连接不参与池的其他借用，适合长时间、跨多次 fetch 的消费。
- **`BorrowedInSession`（会话内游标）**：在 `transaction` / `withSession` 内打开，复用会话已有的
  那条连接（及若已开的事务快照），不额外占用池连接；`close()` 只关服务端游标、不归还连接，
  连接仍归 `Session`，随其析构归还。

### 各驱动的行为

- **PostgreSQL**：服务端游标 `DECLARE CURSOR` + `FETCH FORWARD n` + `CLOSE`。游标必须活在事务里——
  已在事务中则借用现有事务；否则 `auto_transaction=true` 时自建 `pqxx::work` 兜底（关闭时提交），
  `auto_transaction=false` 且无事务则直接报 `CursorError`（不静默降级）。`scrollable=true` 仅当
  配置允许时生效，否则返回 `NotSupported`。
- **MySQL**：非缓冲结果集（`mysql_stmt_*` 且**不**调 `mysql_stmt_store_result`），按批
  `mysql_stmt_fetch` 流式消费，结果不落客户端内存；无事务要求。
- **ODBC**：真游标（`SQL_ATTR_CURSOR_TYPE` + `SQLFetch`）；配置 `allow_scrollable` 时设
  `SQL_CURSOR_STATIC` 支持滚动，其余驱动不支持滚动（`scrollable=true` 返回 `NotSupported`）。
- **Oracle**：前向 OCI statement cursor；`openCursor()` 持有 statement 与 define/LOB 描述符，
  `fetch(n)` 通过 `OCIStmtFetch2` 增量抓取，`close()` 释放 statement。支持普通参数和 LOB 参数；
  `scrollable=true` 仍明确返回 `NotSupported`。

### 配置与资源护栏

每数据源可在 JSON 里配：

```json
{
  "cursor": {
    "enabled": true,
    "default_batch_size": 256,
    "max_open_cursors": 0,
    "allow_scrollable": false
  }
}
```

- `enabled=false`：该数据源开游标直接返回 `NotSupported`。
- `default_batch_size`：调用方用驱动默认 batch_size 时以此兜底。
- `max_open_cursors`：每数据源并发游标上限，**>0 时启资源护栏**（原子 CAS 配额，超限返回
  `CursorLimit`）；`0` = 不限制。
- `allow_scrollable`：是否允许滚动游标（仅 ODBC 生效）。

游标经 `preGate`（审计 + 限流）但**不进查询缓存**（流式结果不可直接缓存，且可能跨事务快照）。
游标以 `OperationType::Select` 过审计，据此**豁免 `require_limit_select`**——游标分批消费、本就有界，
强制 LIMIT 会废掉"全量游标扫描"这一正当用法；`enforce_read_only` / `block_no_where_dml` /
黑白名单等对游标照常生效（审计主体仍按 SQL 文本分类）。

## 重试、熔断与读写路由

- 只重试 `Status::retryable == true` 的查询错误。退避时长带真随机抖动，
  用于打散并发重试、避免故障恢复瞬间的惊群。
- 写入和批量写默认不重试；只有显式设置 `retry_writes: true` 才会重试。
- 流式查询一旦向回调交付过数据便不会重放，避免重复消费。
- 数据源组的查询按权重轮询副本；写入、会话和事务始终走主库。
- 发生过写之后的 `read_after_write_ms` 窗口内，读请求固定走主库。
  会话（`withSession`）里的写同样会触发——只要回调中成功执行过
  `execute` / `executeBatch`，窗口内的读就不会打到从库。
- 副本发生连接类错误或熔断时，可自动回退主库。
- **熔断覆盖所有入口**：查询、写入、流式、批量、会话和事务都过同一个闸门。
  会话与事务只做快速失败、不自动重试（回调内容未必幂等）。

```json
{
  "groups": [{
    "name": "app",
    "primary": "main",
    "replicas": [{"name": "replica_1", "weight": 2}, "replica_2"],
    "read_after_write_ms": 1000,
    "fallback_to_primary": true
  }]
}
```

调用 `SQLConduit::reload(path, grace)` 可原子加载新配置，并等待旧连接池中的在途操作归还。

## 限流、审计、缓存与主库故障转移

这四项能力都是**默认关闭、可整体开关**，且只在 `DataSource` 入口过一次闸门——组转发给叶子走 `*Ungated` 内部路径，重复扣令牌会让配置的 QPS 上限凭空腰斩、重复审计会刷出成倍告警。

### 主库故障转移（failover）

组配置 `failover.primaries` 给出有序可写候选（主自动置顶）。只有能确定 SQL
尚未下发的失败（建连失败、连接池未借到连接、熔断前置拒绝等）才会切换候选。
执行中断线或超时存在“已提交但回包丢失”的歧义，中间件会直接返回错误，
不会在另一个主库上盲目重放。全部候选在执行前就不可用时：

SQLConduit 不执行选主、租约或 fencing，因此自动写切换默认拒绝启用。只有数据库集群已通过
外部机制保证单主时，才可设置 `acknowledge_external_fencing=true`。这个配置只是显式风险
确认，不会凭空提供 fencing 能力。

- 若配了 `failover.write_buffer`，写请求进入有界内存队列，由后台 flush 线程在主恢复后补发，立即返回 `Buffered`（**软降级**：不代表已提交；进程崩溃会丢数据，补发也可能重复）；该能力必须显式设置 `acknowledge_data_loss_and_duplicates=true`；
- 否则返回 `CircuitOpen`（标记为可重试，由上层重试/熔断处理）。

约束：**故障转移和写缓冲都不用于事务**——事务回调未必幂等，重放可能造成重复写入，因此主不可用时事务直接失败，由调用方决定补发。

```json
{
  "groups": [{
    "name": "app",
    "primary": "main",
    "replicas": [{ "name": "replica_1", "weight": 2 }],
    "read_after_write_ms": 1000,
    "failover": {
      "primaries": ["main_standby"],
      "acknowledge_external_fencing": true,
      "require_healthy": false,
      "write_buffer": {
        "enabled": false,
        "acknowledge_data_loss_and_duplicates": false,
        "max_queue": 1000,
        "ttl_ms": 30000,
        "flush_interval_ms": 1000
      }
    }
  }]
}
```

### 限流与背压（rate_limit）

令牌桶限速，优先失败而非把连接池打满后雪崩。超限返回 `RateLimited`（`retryable=false`，调用方应本地排队或降级，不要重试——重试会放大流量）。

```json
{
  "rate_limit": {
    "enabled": false,
    "global_qps": 0,
    "per_fingerprint_qps": 0,
    "burst": 0,
    "fingerprint_mode": "off"
  }
}
```

- `global_qps`：每数据源总 QPS 上限（令牌桶 refill 速率）；`per_fingerprint_qps`：单 SQL 指纹 QPS（保护热点语句）。
- `burst`：突发容量，0 = 等于对应 qps。
- `fingerprint_mode`：`off`（不按指纹）/ `template`（结构化模板）/ `full`（模板+参数）。只有启用指纹限流时才计算指纹，纯总量场景不付这笔开销。

### 可插拔限流器（自定义算法）

内置的 `RateLimiter` 是 `core::IRateLimiter` 的默认实现（`acquire(fingerprint)` 走令牌桶）。
要换算法——滑动窗口、Redis 集中式限流、按租户配额、恒定放行等——只需实现该接口并挂到中间件，无需改动任何调用点：

```cpp
#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/core/rate_limiter.h"

class SlidingWindowLimiter : public sqlconduit::core::IRateLimiter {
public:
    bool acquire(std::uint64_t fingerprint) override {
        // 返回 true=放行；false=限流（中间件转成 RateLimited，retryable=false）
        return window_.allow(fingerprint);
    }
    // 不按指纹限流时保持默认 usesFingerprint()==false 即可
};
```

两种挂载方式：

- **全局默认**：`SQLConduit::setDefaultRateLimiter(std::make_shared<SlidingWindowLimiter>());`
  之后任意未显式指定限流器的数据源，在配置未启用 `rate_limit` 时回退到这个默认实现。
  可在 `SQLConduit::init()` 之前或之后调用；后调用时会立即更新所有继承默认值的已有数据源和组。
- **逐数据源覆盖**：`DataSourceOptions::rate_limiter`（或 `GroupOptions::rate_limiter`）传入
  `shared_ptr<IRateLimiter>`，该数据源优先用你给的实现，**优先于全局默认**。

```cpp
sqlconduit::SQLConduit::init("datasources.json");

// 全局默认：未显式指定的数据源都走滑动窗口
sqlconduit::SQLConduit::setDefaultRateLimiter(std::make_shared<SlidingWindowLimiter>());

// 某数据源单独挂一个高吞吐放行实现（测试 / 白名单）
sqlconduit::core::DataSourceOptions opts;
opts.rate_limiter = std::make_shared<sqlconduit::core::RateLimiter>(100000.0, 0.0, 100000, "off");
mgr.addDataSource(cfg, opts);
```

优先级（高 → 低）：`opts.rate_limiter`（逐源） > 配置 `rate_limit`（`global_qps` 或 `per_fingerprint_qps` 任一启用即构造 `RateLimiter`） > `SQLConduit::setDefaultRateLimiter`（全局默认）。
调用点 `preGate` / `gateSession` 只调 `acquire`，因此替换算法对上层完全透明、零侵入。

### SQL 审计与拦截（sql_audit）

执行前对 SQL 做轻量静态分析（启发式分类，非完整解析器，可能误判动态 SQL/存储过程）。命中策略时按 `action` 决定：

- `block`：拦截，返回 `SqlBlocked`；
- `warn`：仅记录告警、放行（**灰度期默认**，用于评估"这条策略会拦掉多少流量"，靠 `SqlAuditor::stats()` 计数判断何时切到 block）。

```json
{
  "sql_audit": {
    "enabled": false,
    "action": "warn",
    "block_no_where_dml": false,
    "require_limit_select": false,
    "enforce_read_only": false,
    "log_blocked": true,
    "blacklist_fingerprints": [],
    "whitelist_fingerprints": []
  }
}
```

- `block_no_where_dml`：无 WHERE 的 UPDATE/DELETE 拦截（防全表误改/误删）。
- `require_limit_select`：无 LIMIT 的 SELECT 拦截（防一次性拉全表）。**游标(`openCursor`)豁免此规则**，
  详见上文"游标"一节。
- `enforce_read_only`：配合 group 的 `read_only`，拦截只读数据源上的任何写。
- `blacklist_fingerprints`：命中即拦截；`whitelist_fingerprints`：非空时"仅放行名单内"，其余一律拦截（允许列表模式）。

审计在单条 `query`/`execute` 走 `DataSource` 入口时执行；`withSession`/`transaction` 的语句由用户回调临时拼出，入口处看不到，因此下沉到 `Session` 逐条把关，且只对会话显式启用审计的 `Session` 执行（不会重复审）。

### 自定义拦截器（挂载即用）

`ISqlInterceptor` 是全局可插拔的 SPI，覆盖一条 SQL 的完整生命周期：

- `onRoute(dataSource, sql, type, ctx)`：路由决策前后，可改写/读取 `SqlContext`（traceId、tenantId、shadow…）。
- `beforeExecution(view)`：执行前最后一道关，返回非 `ok()` 的 `Status` 直接拦截（如租户配额、灰度开关）。
- `afterExecution(view)`：执行后（成功或失败）回调，可记指标、打点、脱敏。
- `onRow(view, row)`：逐行回调（默认空实现），适合按行脱敏或采样。
- `onCompletion(view)`：整条 SQL 收尾，无论成败都触发（并发顶层调用各自独立触发一次）。

挂载只需一行，全局生效，无需改任何调用点：

```cpp
class TenantQuotaInterceptor : public sqlconduit::core::ISqlInterceptor {
public:
    void onRoute(const std::string &, const std::string &, common::OperationType,
                 common::SqlContext &ctx) override {
        // 例如按 ctx.tenantId 打灰度标记
    }
    common::Status beforeExecution(const sqlconduit::core::ExecutionView &view) override {
        if (overQuota(view.ctx.tenantId))
            return common::Status::error(common::ErrorCode::SqlBlocked, "tenant over quota");
        return common::Status::OK();
    }
    void afterExecution(const sqlconduit::core::ExecutionView &view) override { /* 记指标 */ }
    void onCompletion(const sqlconduit::core::ExecutionView &view) override { /* 收尾 */ }
};

sqlconduit::SQLConduit::addInterceptor(std::make_shared<TenantQuotaInterceptor>());
```

- 全局注册表：`core::InterceptorRegistry::add / clear / snapshot / enabled / setEnabled`。
  不想经门面时可直接操作注册表；`setEnabled(false)` 可整体关闭拦截链而不删除实例。
- 健壮性：拦截器内部抛异常会被中间件吞没（不让 SPI 错误拖垮业务）；同一线程内拦截链有递归深度守卫（上限 64 层），避免 `beforeExecution` 里再触发 SQL 造成无限递归。
- 闸门分离不变量（I1）：拦截只在 `DataSource` 入口（`preGate`）与 `Session` 逐条语句处执行；
  组转发叶子走 `*Ungated` 内部路径**不重复触发**拦截与限流，逐源挂载的限流器不会被重复扣令牌。

### 查询结果缓存（query_cache）

仅缓存 `DataSource::query` 路径的非事务、非会话读。key = **原始 SQL + 带类型标记且长度前缀的参数序列**（不用结构模板——模板会把字面量折成 `?` 导致不同取值撞同一 key；不用 `valueToString`——会丢类型信息让 `1` 与 `"1"` 撞 key）。

```json
{
  "query_cache": {
    "enabled": false,
    "ttl_ms": 60000,
    "max_entries": 1000,
    "max_memory_bytes": 0,
    "cache_on_replica_only": false
  }
}
```

- LRU 淘汰 + TTL 过期 + 内存上限（单条结果集超过 `max_memory_bytes` 直接不缓存，否则会为放它一个清空整缓存）。
- `cache_on_replica_only=true` 时只缓存打到副本的读，主库强一致读不缓存。
- 写后按数据源名失效（`markWrite`），避免 read-after-write 读到旧结果。
- 命中率/淘汰/失效计数由 `QueryCache::stats()` 暴露，缓存关着时这些计数仍累计、不随热加载清空。

## 缓存机制详解

中间件内共有**两套真正的 KV 缓存**，外加一处统计型 LRU（慢 SQL 聚合）与两处 TTL 生命周期回收（连接池、写缓冲）。其中两套 KV 缓存在 `datasources.json` 顶层各自有独立配置块（`query_cache` 与 `prepared_cache`），二者**互相独立、不要混用**：

- `query_cache` 缓存的是**结果数据**，键是 `(SQL + 参数值)`；
- `prepared_cache` 缓存的是**语句句柄**，键是 `(SQL + 参数类型签名)`。

### 1. 查询结果缓存（QueryCache，全局单例）

仅作用于 `DataSource::query` 的叶子读路径（非事务、非会话读）。组转发叶子用数据源自身名字作为 key 前缀，使主库与副本的同一条 SQL 成为两条独立缓存项，写后失效才能按节点精确清除。

实现（`src/core/query_cache.cpp`）：全局单例，`std::unordered_map<std::string, Entry> store_` + `std::list<std::string> lru_`（最近使用在表头），一把 `std::mutex mtx_`。开关 `enabled_` / `replicaOnly_` 用 `std::atomic` 镜像——**热路径先无锁读原子标志，缓存关着时连 mtx_ 都不抢**。命中率/淘汰/失效计数均为原子量，`QueryCache::stats()` 暴露，热加载清空缓存不清计数（进程累计量）。

**KV 内容：**
- **key** = `数据源名 + '\0' + cacheKey(sql, params)`，其中 `cacheKey` = 原始 SQL + `\x1e` + 参数个数 + 每参数（`\x1f` + 类型标记 + 长度前缀值）。NULL、bool、int64、uint64、double、Decimal、string、Date、Time、Timestamp、UUID、JSON、Blob 都有独立标记；double 按位序列化，所有文本/二进制值加长度前缀，**确保不同类型或参数值得到不同 key**。
- **value** = `Entry { ResultSet rs; expire; list::iterator lru; bytes; }`，存的是结果集**深拷贝** + TTL 时刻 + 近似字节数。

**过期策略（三重）：**
1. **TTL**：`expire = now + ttl_ms`，`get()` 时过期即 `eraseLocked` 判 miss；`ttl_ms<=0` 整体当关闭。
2. **LRU 容量淘汰**：`evictLocked` 按 `max_entries`（条目数）与 `max_memory_bytes`（近似字节，`approxBytes` 估列名+各列值）双上限，从 `lru_` 尾部淘汰；单条超内存上限直接不收。
3. **写后失效**：`markWrite()` → `QueryCache::invalidate(数据源名)`，按 `数据源名\0` 前缀清掉该数据源全部项。`configure()` 变更配置时整体清空旧数据，避免新旧 TTL/上限混用。

**为什么**：读多写少、重复查询（字典表/配置表）省去重复 DB 往返；代价是最终一致，故**默认关**；`cache_on_replica_only=true` 时只缓存副本读，主库强一致读不缓存。

### 2. 预编译语句缓存（PreparedCache，连接级句柄缓存）

四驱动（`MySQLConnection` / `PostgresConnection` / `OracleConnection` / `OdbcConnection`）各自在 `prepare()` 内维护一份本连接的句柄缓存。门面 `Session::runPreparedQuery` / `runPreparedExec` 在 `preparedPathUsable()` 时调 `conn->prepare`，由驱动内部查"本连接"的缓存——这就是 `DataSource::query/execute(params)` 的**透明自动缓存**（用法见上文「预编译语句复用」）。要生成键时不走这条路径（见下文注意事项）。

实现：每个连接对象持有 SQL→句柄缓存、LRU 链表和句柄 ID→缓存键索引。后者用于 O(1) 验证显式句柄仍属于当前连接且未被淘汰。连接归还池后缓存保留、下次借到同一连接直接复用；连接关闭 `close()` → `closeAllPrepared()` 释放全部原生句柄并清空索引。

**KV 内容：**
- **key** = `sql + common::paramTypeSignature(typesSample)`，用**参数类型签名**而非参数值——同 SQL 不同参数类型在 prepare 阶段必须视为不同语句。
- **value** = `PreparedStatementHandle { uint64_t id; void* native; }`：`native` 对 MySQL 是 `MYSQL_STMT*`、PG 是 `nullptr`（名字另存 `preparedNames_`）、ODBC 是 `SQLHSTMT`。存的是**原生服务端预备语句句柄**，不是结果。

**过期策略：**
- **LRU 容量淘汰**：`max_per_connection > 0` 时，插入后 `while(size > limit)` 从 `lru_` 头（最久未用）淘汰，并 `mysql_stmt_close` / `conn_->unprepare` / `SQLFreeHandle` 释放原生句柄；`0` = 不限制（随连接关闭回收）。命中时把 key 移到 `lru_` 尾。
- 连接关闭：`closeAllPrepared()` 释放本连接全部句柄。

**为什么**：热点语句 prepare-once / execute-many，省服务端硬解析 + 参数类型推导往返；**不改变任何查询结果**，纯性能优化，故**默认开**。

> 显式句柄被 LRU 淘汰后，再执行会稳定返回 `QueryError`；驱动会先按句柄 ID
> 验证其仍属于当前连接缓存，不会解引用已经释放的原生句柄。

### 3. 慢 SQL 聚合统计缓存（Observer LRU，非业务数据）

`observer.cpp` 的慢 SQL 聚合：被判定为慢 SQL 时按 `sqlFingerprint` 聚合到 `g_slowStats`（低频路径，专用锁，不阻塞高频快照读取）。KV：key=指纹（结构模板哈希），value=聚合统计（count/duration/histogram）。按 `aggregate_capacity` 上限淘汰（O(1) LRU）；`histogramBucketsMs` 分桶变化（旧样本无法无损重分桶）时整体清空。与上面两套缓存完全独立，纯观测用途。

### 缓存与一致性边界（重要）

- **`StreamParams`（大参数流式）和游标刻意不进结果缓存**：流式内容不是定值，无法参与 `cacheKey`（要求同参数必得同结果）；审计照常按 SQL 文本分类、流内容绝不进日志。
- **生成键路径不进预编译缓存**：`executePrepared` 拿不到 `RETURNING` 的结果集，为省一次 prepare 让调用方静默拿不到主键是本末倒置。
- **写缓冲 / 连接池的 TTL** 是对象生命周期过期，非 KV 缓存。

## 动态数据源（v0.4.0 M4：运行时增删）

`SQLConduit::init` 启动后，你仍然可以在运行时增删数据源与组——配置不再是一次性快照：

| 方法 | 用途 |
| --- | --- |
| `SQLConduit::addDataSource(cfg, opts)` | 注册一个新的叶子数据源（建池 + 启动心跳 + 插入 DataSource） |
| `SQLConduit::removeDataSource(name, grace=5s)` | 注销一个叶子数据源；被组引用时拒绝 |
| `SQLConduit::addGroup(cfg, opts)` | 注册一个读写组（主 + 副本 + 故障转移 + 可选写缓冲） |
| `SQLConduit::removeGroup(name, grace=5s)` | 注销一个组；停止其写缓冲线程 |

`opts` 走 `core::DataSourceOptions` / `core::GroupOptions`，分别控制 `retry` / `circuit_breaker` / `rate_limiter` / `cursor` / `attach_heartbeat` 与 `acknowledge_external_fencing` / `acknowledge_data_loss_and_duplicates`。后者两个 ack 标志与 `init()` 一致——`addGroup` 不允许隐式启用自动写切换或写缓冲，调用方必须显式表态。

**安全保证**：

- **重名拒绝**：addDataSource / addGroup 对已存在的名字返回 `ConfigError`，**不会覆盖**原池（沿用原 init 的非破坏语义）。
- **引用完整性**：removeDataSource 拒绝注销"被组引用"的叶子，错误消息明确指出冲突的组名；必须先 removeGroup 再 removeDataSource。
- **网络 IO 全部在锁外**：校验、插入、销毁在 `mtx_` 临界区里完成；建池与 `WriteBuffer::start()` 在锁外发起，绝不持锁做 IO。
- **addDataSource 不替换而是新建**：失败的并发插入把刚建的池以 grace=0 关掉，避免泄漏；旧池不受影响。
- **grace 宽限期**：removeDataSource / removeGroup 的 grace 语义与 shutdown 一致——等待在途连接归还，超期强制关闭。grace=0 立即返回（池被标记 closed），适合"想下线但不想等"。

```cpp
sqlconduit::SQLConduit::init("datasources.json");                  // 启动期基线

sqlconduit::core::DataSourceOptions leafOpts;
sqlconduit::SQLConduit::addDataSource(cfg, leafOpts);              // 运行时加一个池

sqlconduit::core::GroupOptions grpOpts;
grpOpts.acknowledge_external_fencing = true;          // 必填：自动写切换需明确同意
sqlconduit::SQLConduit::addGroup(grp, grpOpts);                    // 运行时组一个读写组

sqlconduit::SQLConduit::removeGroup("legacy_grp");                 // 先卸组
sqlconduit::SQLConduit::removeDataSource("legacy_leaf");           // 再卸叶子
```

并发安全由 `mtx_` 保证——多线程同时 addDataSource 不同名互不干扰；同名并发里后到者以 `ConfigError` 优雅失败，**不会**让两个调用者都以为自己成功。详细并发行为见 `tests/sqlconduit_dynamic_test.cpp`（79 项断言，16 个场景覆盖 add/remove/group/ack/并发/grace）。

## 幂等声明（v0.4.0 M5：让调用方决定写是否可重试）

重试原本由引擎**按语句类型猜**：`execute` 是否重试取决于 `retry_writes` 配置。问题在于引擎只能猜、调用方才知道——一条 `UPDATE ... SET balance = balance - 100` 是 `Execute`（类型上看可重试），但它非幂等；而 `UPDATE ... SET status='paid' WHERE id=?` 是幂等的。M5 用 `SqlContext::idempotency` 三态声明把决策权交还调用方：

| 声明 | 语义 | 对写重试的影响 |
| --- | --- | --- |
| `Unspecified`（默认） | 未声明 | 走既有逻辑：`retry_writes` 配置 + Single/Multi |
| `Idempotent` | 声明幂等 | 允许在连接类错误上自动重试写，**即使** `retry_writes=false` |
| `NonIdempotent` | 声明非幂等 | 任何情况下都不重试写，**即使** `retry_writes=true` |

用**枚举而非 `bool`** 是关键：`bool idempotent=false` 无法区分"未声明"与"显式声明非幂等"。`Unspecified` 保证**不声明即保持现状**——引入该特性不改变任何既有行为。

```cpp
// 一次业务请求入口包一层 ContextScope，整段调用链（同步/异步/事务）都透传：
{
    sqlconduit::common::ContextScope scope({.idempotency = sqlconduit::common::Idempotency::Idempotent});
    ds->execute("UPDATE accounts SET status='paid' WHERE id=?", affected); // 失败会重试
}

{
    sqlconduit::common::ContextScope scope({.idempotency = sqlconduit::common::Idempotency::NonIdempotent});
    ds->execute("UPDATE accounts SET balance=balance-100 WHERE id=?", affected); // 绝不重试
}
```

**决策优先级（高→低）**：`NonIdempotent` > `Idempotent` > `Unspecified`。声明只影响"是否重试写"，不改变：

- **读路径**：读本身可重放，`query` 仍按 `retry_writes`/max_attempts 照常重试；
- **事务内不重试**这条不变量（I4）——事务内语句根本不进重试循环；
- **非可重试错误**（业务/约束冲突）照旧不重试——声明只覆盖"连接类可重试错误"这一档。

异步路径同源：`async::execute` 的 `maxAttempts` 读取 submit 时刻栈顶 `ContextScope` 的快照（`entryCtx.idempotency`），与同步 `resolveWriteAttempts` 用同一张优先级表。详细行为见 `tests/sqlconduit_idempotency_test.cpp`（19 项断言，9 个场景覆盖三态 × 同步/异步 × 读路径不受影响）。

## 影子库路由（v0.4.0 M6：把整组流量切到影子数据源）

生产流量回放做全链路压测：把读 / 写流量引到影子库，验证新版本在真实负载下的表现而不污染生产数据。影子库路由由 **M1 SPI 的 `onRoute`** 决策（按 `tenantId`、灰度比例、HTTP 头……都可），落到路由层只是一行 `ctx.shadow = true`。

**配置**：在组上声明影子目标——它必须是一个**普通叶子数据源**，不能是本组成员、不能是另一个组（避免引用歧义）：

```json
{
  "datasources": [
    { "name": "prod",      "type": "mysql",  "host": "primary.db" },
    { "name": "prod_repl", "type": "mysql",  "host": "replica.db" },
    { "name": "shadow_db", "type": "mysql",  "host": "shadow.db" }
  ],
  "groups": [{
    "name": "order_svc",
    "primary": "prod",
    "replicas": [{ "name": "prod_repl", "weight": 1 }],
    "shadow": "shadow_db"
  }]
}
```

**触发**：通过 SPI（最常用——按租户 / 灰度比例灵活切换）：

```cpp
sqlconduit::SQLConduit::addInterceptor({
    .onRoute = [](const std::string&, const std::string&,
                  sqlconduit::common::OperationType,
                  sqlconduit::common::SqlContext &ctx) {
        // 例：每 1% 流量切到影子
        if (shouldReplayToShadow(ctx.tenantId)) ctx.shadow = true;
    }
});
```

或者直接用线程本地 `ContextScope`（同一线程 / 协程全程生效）：

```cpp
sqlconduit::common::ContextScope scope({.shadow = true});
ds->execute("INSERT INTO orders ...", affected);   // 落到 shadow_db
ds->query("SELECT * FROM products ...", rs);        // 落到 shadow_db（命中主/副本按 routing）
```

**影子模式的核心不变量**：

| 行为 | 规则 | 原因 |
| --- | --- | --- |
| 影子读 / 写路由 | `readTarget()` / `writeTargets()` 在 `ctx.shadow` 为真时返回 `shadow_` | 整组流量统一切走 |
| **影子写不进写缓冲**（I12） | `dispatchWrite` 影子短路：尝试影子，失败直接返回错误，不构造 buffered lambda | 缓冲补发会把压测数据写回生产库——数据污染事故 |
| **影子读不进查询缓存**（I10） | `cacheEligible()` 在 `ctx.shadow` 为真时返回 `false` | 避免把压测结果混入生产租户的缓存 |
| 影子故障 | 短路返回错误，**不**追加主回退候选 | 影子不可用就让压测停掉，不悄悄降级污染生产 |
| 异步路径 | `entryCtx` 在 submit 时刻快照 `routeCtx`，worker 与 I12 / I10 守卫读到的 `ctx.shadow` 一致 | 跨路径语义同源 |

**配置校验**（`addGroup` 阶段必查，任一不合法返回 `ConfigError`）：

- 影子源必须存在（已 `addDataSource`）；
- 影子源**不得**是本组主（自影自己）；
- 影子源**不得**是本组副本；
- 影子源**不得**与任何组名同名（避免引用歧义）。

校验放在 `resolveShadows`（在 `init()` 与 `addGroup()` 后、对外可见前）。详细行为与代码片段见 `tests/sqlconduit_shadow_test.cpp`（39 项断言，10 个场景覆盖同步 / 异步 / 缓存 / 写缓冲 / 配置校验所有分支）。

## 读后写一致性增强（v0.4.0 M8：会话级粘性读）

`read_after_write_ms`（数据源级时间戳窗口）只能保"近期写过的大概率命中主"——而业务语义是"我刚刚写过这个表，紧接着的 read 必须看到自己的写"。M8 在时间戳窗口之上加了一层**会话级（ContextScope 帧级）粘性**，由 `SqlContext.wroteInThisRequest`（简称 `wIRT`）承载：

| 优先级 | 条件 | 路由 |
|---|---|---|
| 1（最强） | 当前 SqlContext `wroteInThisRequest=true` | primary（写后读一致性） |
| 2 | DataSource `read_after_write_ms` 窗口内 | primary（时间戳兜底） |
| 3 | 副本轮询 | replicas[] |

`wIRT` 由 `DataSource::markWrite` 在 leaf 写成功路径集中触发 `pinRequestWrite()` 置位，对业务零侵入。

### 用法

```cpp
{
    common::ContextScope scope({.traceId = req.header("x-trace-id")});
    g_->execute("UPDATE users SET name=? WHERE id=?", name, id); // markWrite → wIRT=true
    auto rs = g_->query("SELECT name FROM users WHERE id=?", id); // 自动走 primary
}
// 帧析构 → wIRT 跟着销毁，下次读走副本（除非仍在 read_after_write_ms 窗口内）
```

### 不变量

- **栈帧定位**：业务 ContextScope 是单帧时栈顶即业务帧；同步 `runWithInterceptors` 内部还会 push 一帧 `ContextScope`（拷贝 view.ctx），栈顶是中间层、业务帧是 `[size-2]`。`pinRequestWrite` 改业务帧，否则中间 frame 弹出后 wIRT 跟着消失。
- **栈空早返**：业务未建 `ContextScope` 时 `pinRequestWrite()` 不写 default 实例（const-like 语义）。
- **异步隔离**：submit 时冻结 `entryCtx`，worker 写成功后置 `entryCtx.wroteInThisRequest=true`；新 submit 重新拷快照——cross-op 不串。
- **影子正交**：影子写不入生产组 `markWrite`，故也不会污染生产的 wIRT——与 M6（影子库路由）独立工作。
- **幂等正交**：`Idempotency=NonIdempotent` 不重试，但写仍触发 `markWrite` → wIRT 照常置位。

### 配置提醒

配置副本 + `read_after_write_ms=0` = **陈旧读风险**。`ConfigLoader` 在加载时会 `fprintf(stderr, "...")` 打 WARN 提示，但不阻断 load：

```
sqlconduit WARN: datasource group 'g' has 1 replica(s) but read_after_write_ms=0;
writes-then-reads may be served by replicas and return stale data.
Set read_after_write_ms > 0 (e.g. 1000) to pin post-write reads to the primary.
```

详细行为见 `tests/sqlconduit_raw_session_test.cpp`（26 项断言 / 6 个场景覆盖同步 / 异步 / 帧隔离 / 时间戳兜底 / 影子 / 幂等 / config_loader WARN）。

## 结果脱敏（v0.4.0 M7：按角色 / 租户掩码结果集）

合规场景（手机号、身份证、银行卡）需要在结果集返回前按角色 / 租户做掩码。**SQLConduit 不内置任何脱敏规则**——规则是业务 / 合规概念，内置等于替用户做合规决策；只提供 SPI 钩子和 I10 守卫（脱敏结果绝不进缓存）。

**改写时机**：SPI `afterExecution`（M1 §3.3）拿到 `view.result`（`common::ResultSet*`，可改写）。改写完成后置位 `view.result->transformed = true`，**这是中间件识别"已被脱敏"的唯一信号**。

**示例**（业务自己实现 `ISqlInterceptor`）：

```cpp
class MaskingInterceptor : public sqlconduit::core::ISqlInterceptor {
public:
    void onRoute(const std::string&, const std::string&,
                 sqlconduit::common::OperationType, sqlconduit::common::SqlContext&) override {}

    sqlconduit::common::Status beforeExecution(const sqlconduit::core::ExecutionView&) override {
        return sqlconduit::common::Status::OK();
    }

    void afterExecution(const sqlconduit::core::ExecutionView &view) override {
        if (!view.result) return;                  // 非查询（写 / 批 / 游标）不动
        // 这里做你的脱敏：按列名 / 列下标 / 值模式识别敏感字段并掩码
        for (auto &row : view.result->mutableRows()) {
            // 示例：row.set("phone", "***");
        }
        // 关键：标记已被改写。中间件会守卫这一结果不进查询缓存。
        view.result->transformed = true;
    }

    void onRow(const sqlconduit::core::ExecutionView&, sqlconduit::common::Row &row) override {
        // queryEach / 游标不会整体物化 ResultSet，逐行交付前在这里脱敏。
        if (row.has("phone")) row.set("phone", "***");
    }

    void onCompletion(const sqlconduit::core::ExecutionView&) override {}
};

// 在 SQLConduit::init 之前注册：
sqlconduit::SQLConduit::addInterceptor(std::make_shared<MaskingInterceptor>());
sqlconduit::core::InterceptorRegistry::setEnabled(true);
```

**I10 守卫的硬约束**：同步查询先缓存驱动原始结果，再对返回副本执行 `afterExecution`；异步缓存入口通过 `transformed` 标记拒绝改写结果：

| 位置 | 守卫 |
|---|---|
| `DataSource::queryUngated`（同步） | 在顶层 `afterExecution` 之前保存原始结果 |
| `DataSource::cacheStore`（同步） | `if (rows.transformed) return;` |
| `async::query` 的 `step2Statement`（异步） | `if (policy.cacheable && !ctx->entryCtx.shadow) target->cacheStore(...)` ——`cacheStore` 内部守卫命中 |

**为什么脱敏结果不能进缓存**：缓存存的是原始结果，脱敏是角色 / 租户相关的视图。把脱敏结果写进缓存，下一个不同权限的用户会读到上一个用户的视图——**跨用户数据泄漏**。

**缓存命中路径仍要走 `afterExecution`**（§9.4 风险行）：缓存里是原始数据（被守卫拦下，不可能有 transformed=true 的版本），所以缓存命中后必须重新跑一遍 `afterExecution` 才能得到当前用户的视图。同步路径天然被 `runWithInterceptors` 包住；异步路径在 submit 时手动构造视图调一次 `detail::runAfterExecution(view)`。

普通查询使用 `mutableRows()` 原地改写；`queryEach` 与游标使用 `onRow`，中间件不会为了脱敏把流式结果整体物化。

不变量保留：**数据进入拦截器 → 数据出拦截器 → 缓存守卫**全程只看 `transformed` 标记位。详细行为与代码片段见 `tests/sqlconduit_redaction_test.cpp`（38 项断言，5 个场景覆盖同步 / 异步 / 缓存命中 / I10 守卫 / 改写标记）。

## 异步 API（v0.2.0：回调 / future / 协程）

三种调用形态共享同一条执行管线——治理闸门（审计/限流/熔断/缓存）、重试退避、语句超时、取消——只是结果交付方式不同。在配置中开启 `async`：

```json
{ "async": { "enabled": true, "threads": 4, "queue_size": 4096 } }
```

`threads` 是 worker 数（0 = hardware_concurrency），另有 1 个 timer 线程负责重试退避与超时检查；队列满时新操作以 `Overloaded` 快速失败（显式背压，不是隐式排队）。`SQLConduit::shutdown` 会先拒绝新操作、在 `grace` 内等待在途操作，然后协作式停止执行器与连接池。C++ 无法安全强杀仍在访问连接状态的线程；如果底层驱动不支持取消，停机可能继续等到该驱动调用返回/网络超时。

**回调式（热路径）**——完成回调由完成调度器投递，绝不在调用栈上执行；返回的 `Handle` 支持取消：

```cpp
sqlconduit::async::Options opts;
opts.timeout = std::chrono::milliseconds(2000);   // 语句整体期限（兜底）
auto h = sqlconduit::async::query("SELECT id FROM users WHERE age > ?",
                            {sqlconduit::common::Value(std::int64_t(18))},
    [](sqlconduit::async::QueryResult &&r) {            // 跑在完成调度器线程，须短小
        if (r.status.ok()) useRows(std::move(r.rows));
    }, opts);
// 需要中途放弃时：h.cancel() —— Queued 不碰池；Running 尽力转发驱动 cancel
```

**future 式（便利形态）**——无取消能力（需要取消用回调式拿 `Handle`）；未取值即析构是合法用法：

```cpp
auto fut = sqlconduit::async::execute("UPDATE users SET active = 1 WHERE id = ?",
                                {sqlconduit::common::Value(std::int64_t(7))});
auto r = fut.get();   // r.status / r.affected
```

**协程式（可选，C++20）**——惰性 `Task`，`co_await` 时才启动；未 `co_await` 直接析构 = 安全放弃。治理/重试/取消/超时与回调形态完全同源，协程恢复线程 = 完成调度器线程：

```bash
cmake .. -DSQLCONDUIT_ENABLE_ASYNC_CORO=ON   # 仅 task.cpp 提标 C++20，其余 TU 仍为 C++17
```

```cpp
#include "sqlconduit/async/task.h"   // 本 TU 必须以 C++20 编译

sqlconduit::async::Task<void> demo() {
    // 参数先具名构造：co_await 实参里的花括号临时会触发 GCC 13 ICE（见下方注意事项）
    sqlconduit::common::Params params;
    params.push_back(sqlconduit::common::Value(std::int64_t(18)));

    auto q = co_await sqlconduit::async::queryAsync("SELECT id FROM users WHERE age > ?", params);
    if (q.status.ok()) useRows(std::move(q.rows));

    auto tx = co_await sqlconduit::async::transactionAsync({}, [](sqlconduit::core::Session &s) {
        std::int64_t n = 0;
        return s.execute("UPDATE users SET active = 1", n);  // 非 Ok 自动回滚
    });
}

sqlconduit::async::run(demo());   // 受控 fire-and-forget：跑完自毁，不悬垂
```

**自定义执行器（asio 接入）**：实现 `IExecutor::post(std::function<void()>)`，再通过 `sqlconduit::async::setExecutor(...)` 注入适配器；完成回调与协程恢复会发生在自定义事件循环线程上。

约束与注意：

- 回调与事务 fn 跑在 worker 上，须短小、线程安全；**事务 fn 内部禁止调用 `sqlconduit::async::*`**（池偏小时互相等连接造成活锁），直接用同步 `Session` 方法。
- `run()` 启动的顶层协程内未捕获异常会 `terminate`（不静默吞掉）；异常应协程内处理，或经 `co_await` 链传给有 `try/catch` 的外层。
- 协程体不要用捕获局部引用的 lambda——闭包临时对象先于异步完成销毁，捕获会悬垂；用具名函数返回 `Task`。
- GCC 13 已知缺陷：`co_await` 实参中直接写非平凡花括号临时（如 `{Value(1)}`）会触发编译器 ICE（PR109227 系）；参数先具名构造再传入即可规避，GCC 14+ / Clang / MSVC 不受影响。

## 实体映射（v0.5.0：Row ↔ 业务实体，读写双向）

`include/sqlconduit/mapping.h` 是 **header-only** 的适配层：把 `ResultSet` 的行按**业务手写的字段声明**搬进/搬出业务结构体。它不是 ORM——SQL 仍由业务书写、没有脏跟踪与延迟加载、`sqlconduit.h` 与引擎核心**零改动**。

### 一次声明

```cpp
#include "sqlconduit/mapping.h"

struct User {
    std::int64_t id;
    std::string  name;
    std::optional<std::string> email;   // optional 自动接 SQL NULL
    sqlconduit::common::Decimal balance;      // 高精度原样保留，不转 double
    std::int64_t created_at;
};

template <> struct sqlconduit::mapping::RowMapper<User> {
    static auto describe() {
        return sqlconduit::mapping::Mapping<User>()
            .field(&User::id,         "id")
            .field(&User::name,       "name")
            .field(&User::email,      "email")
            .field(&User::balance,    "balance")
            .field(&User::created_at, "created_at",
                   sqlconduit::mapping::FieldFlags::PrimaryKey);
    }
};
```

### 读

```cpp
auto r = sqlconduit::queryAs<User>("SELECT id,name,email,balance,created_at FROM users WHERE age > ?",
                             {sqlconduit::common::Value(std::int64_t(18))});
if (r.status.ok()) for (auto &u : r.items) use(u);      // r.items：std::vector<User>

auto one = sqlconduit::queryOneAs<User>("SELECT * FROM users WHERE id = ?",
                                  {sqlconduit::common::Value(std::int64_t(1))});
// one.value：std::optional<User>；**多于一行是错误**，不静默取第一行

sqlconduit::queryEachAs<User>("SELECT * FROM users", [](User &&u) { use(u); return true; });
```

游标与事务内同样可用：`sqlconduit::fetchAs<T>(cursor)`、`sqlconduit::queryAs<T>(session, sql)`。

### 写

```cpp
User u{0, "alice", "a@x.com", sqlconduit::common::Decimal{"12.50"}, now()};

auto p = sqlconduit::paramsOf(u);                       // 实体 → Params（跳过 Generated/ReadOnly 列）
auto ins = sqlconduit::insertSql<User>("users");        // "INSERT INTO \"users\" (...) VALUES (?, ...)"
auto insM = sqlconduit::insertSql<User>("users",
                                  sqlconduit::common::util::Dialect::MySQL);
                                                 // "INSERT INTO `users` (...) VALUES (?, ...)"
auto upd = sqlconduit::updateSql<User>("users");        // "UPDATE \"users\" SET ... WHERE \"id\" = ?"

auto k = sqlconduit::insertAs("users", u);              // 执行 + 生成键回填到主键字段
auto n = sqlconduit::updateAs("users", u);              // 按 PrimaryKey 定位
auto b = sqlconduit::insertBatchAs("users", std::vector<User>{...});
```

**标识符引号按方言生成**：`insertSql<T>(table)` / `updateSql<T>(table)` 不传 `Dialect` 时是**方言中立**的
构造器，统一用双引号（PG / SQL Server 风格）；要显式控制就传第二个参数
（`Dialect::MySQL` → 反引号）。而 `insertAs` / `updateAs` / `insertBatchAs` 是**执行**接口，
会自动按会话所属数据源（无会话时按默认数据源）的方言选引号，MySQL 下自动用反引号，
无需手工指定——直接拿裸 `insertSql` 的 SQL 去 MySQL 执行会因双引号报语法错误。

生成键回填走「列名匹配 + `lastInsertId()` 兜底」双路——MySQL 合成列名固定为 `insert_id`，PG/ODBC 的 `RETURNING` 按列名匹配。

**生成键回填按方言处理**（`insertAs` / `insertBatchAs`）：

| 方言 | 做法 | 回填 |
|---|---|---|
| MySQL | 不追加任何子句 | ✅ `mysql_insert_id` 合成 `insert_id`，走 `lastInsertId()` 兜底 |
| PostgreSQL | 自动在 INSERT 尾部追加 `RETURNING <Generated 列>` | ✅ 按列名匹配回填 |
| SQL Server | **暂不自动补** `OUTPUT INSERTED.*` | ❌ `item.id` 不会被回填 |

SQL Server 不补的原因有两个，都不是偷懒：`OUTPUT` 的位置与 `RETURNING` 不同（在列列表之后、
`VALUES` 之前），且表上一旦有 enabled trigger，不带 `INTO` 的 `OUTPUT` 会直接报错 334，
没有优雅降级。等有 SQL Server 真机验证过再开。

批量插入：`insertBatchAs` 传**具名非 const `std::vector<T>`** 才会回填（要改实体，必须可写）；
传临时量 / const vector 走的是不回填的重载。驱动侧 PostgreSQL 逐条收集 `RETURNING` 结果集，
MySQL 走基类批量循环里的 `mysql_insert_id`。

### 宽松 / 严格（缺列可配，类型不符与 NULL 始终报错）

类型不符、NULL 落到非 `optional` 成员**直接报错**（`ErrorCode::MappingError`），不填默认值、不返回半成品；缺列与多余列默认宽松、可逐项收紧：

| 情形 | 默认行为 | 收紧方式 |
|---|---|---|
| 声明列在结果集中缺失 | **跳过**该字段（保持默认构造值） | `.missingColumns(MissingColumns::Error)` |
| 结果集有未声明的多余列 | **忽略**（兼容 `SELECT *`） | `.extraColumns(ExtraColumns::Error)` |
| NULL 落到非 `optional` 成员 | 报错 | — |
| `int64 → int32` 等收窄 | 范围检查，溢出报错 | — |
| `Decimal → double`、`Blob → string` 等有损/文本转换 | 默认拒绝 | 显式声明 `FieldFlags::Lossy` / `Textual` |
| `queryOneAs` 命中多行 | 报错 | — |

> 缺列与 NULL 是两回事：前者跳过、后者报错。实现上判断缺列必须用 `row.data().find()`，
> 不能用 `Row::at()`——`at()` 对缺失列返回静态 NULL，会把「SQL 少查一列」伪装成「这列是 NULL」。

### 与既有能力的边界

- **查询缓存**：只缓存原始 `ResultSet`，命中后再映射——实体从不进缓存。
- **脱敏**：映射发生在 `afterExecution` 之后，业务实体拿到的是脱敏后的值。
- **异步**：`sqlconduit::async::queryAs<T>` 提供回调 / future / 协程三形态，映射跑在**完成投递线程**（默认 worker；注入 asio 时是 `io_context` 线程），因此映射逻辑必须轻量——大结果集走 `queryEachAs` 流式分流。

## PostgreSQL 数组 / 复合 / 几何类型

PG 的 `ANYARRAY`、行类型与几何类型此前在驱动里一律退化成原始字符串（`{1,2,3}` / `(a,b)` / `(1,2)`）。
现在 `common::Value` 新增两个备选承载它们，几何类型以 `Json` 承载 **PG 规范文本**。

### Value 的两个新备选

```cpp
struct Array     { std::vector<Value> items; };                        // 可嵌套
struct Composite { std::vector<std::pair<std::string, Value>> fields; }; // 保序 + 按名查找

using ValueBase = std::variant<..., Blob, Array, Composite>;
struct Value : ValueBase { using ValueBase::ValueBase; };
```

`Value` 从 `variant` **别名改成派生结构体**——这是递归 variant 唯一可行的写法
（C++17 起 `std::vector<T>` 允许 T 不完整）。既有 `std::get_if<T>(&v)` / `std::holds_alternative<T>(v)` /
`std::get<T>(v)` 全部照旧工作；**唯一例外是 `std::visit`**，跨标准库实现对派生 variant 的支持不一致，
请统一用 `common::visitValue(visitor, v)`。

`Composite::find(name)` 返回 `const Value *`，未命中返回 `nullptr`（定义在 `types.cpp`，
因为 `std::pair<std::string, Value>` 的实例化必须等 `Value` 完整）。

### 读侧行为

| 列类型 | 之前 | 现在 |
|---|---|---|
| `INT4[]` / `TEXT[]` | `"{1,2,3}"` 字符串 | `Array`，元素按元素 OID 逐个还原（`int64` / `string` …） |
| `INT4[][]` 多维 | `"{{1,2},{3,4}}"` | `Array` 嵌套 `Array` |
| 具名复合类型（`CREATE TYPE ... AS` / 表行类型） | `"(a,b)"` 字符串 | `Composite`，字段名与类型来自 `pg_attribute` |
| `RECORD`（匿名 `ROW(...)`） | 字符串 | **仍是字符串**——服务器不暴露字段元数据，无法拆 |
| `point` / `lseg` / `path` / `box` / `polygon` / `line` / `circle` | `"(1,2)"` 字符串 | `Json{ PG 规范文本 }`，配 `PgPoint` 等 7 个结构体解析 |
| 数组元素为 NULL | 混入字符串 | `nullptr`（`{NULL,a}` 正确区分于 `{,a}` 的空串） |

几何值放 `Json` 而不是新增备选，是为了**能原样写回**：PG 的 `point` 列不接受
`{"x":1,"y":2}`，只接受 `(1,2)`。需要结构化 JSON 时自行调 `common::pgGeometryToJson(...)`。

### 写侧（参数绑定）

```cpp
common::Array tags;  tags.items = {Value{"red"}, Value{"blue"}};
common::Composite addr; addr.fields = {{"city", Value{"Shanghai"}}, {"zip", Value{"200000"}}};

SQLConduit::execute("INSERT INTO t (tags, addr, pt) VALUES (?, ?, ?)",
              Params{ Value{tags}, Value{addr},
                      Value{common::Json{common::pgFormatPoint(common::PgPoint{1, 2})}} }, n);
```

驱动把 `Array` / `Composite` 渲染成 PG 数组 / 行文本后以 **text 参数**下发，
由服务端按目标列类型推断——所以 SQL 里**不要**再手工 `::text[]` 强转，
但脱离列上下文的裸 `SELECT $1` 会被 PG 当成 `text`。MySQL / Oracle / ODBC 收到 `Array` / `Composite`
参数直接返回 `NotSupported`，不会静默绑成 NULL。

### mapping 层绑定

| 成员类型 | 绑定目标 |
|---|---|
| `std::vector<int>` / `std::vector<std::string>` / … | `Array`（元素逐个走元素转换器） |
| `std::vector<std::vector<int>>` | 嵌套 `Array` |
| `std::optional<std::vector<T>>` | NULL → `nullopt` |
| `common::Array` / `common::Composite` | 原样透传 |
| `common::PgPoint` / `PgLine` / `PgLseg` / `PgBox` / `PgPath` / `PgPolygon` / `PgCircle` | `Json`（PG 文本），双向 |

7 个几何结构体定义在 `include/sqlconduit/common/pg_types.h`，配套 `pgParseXxx` / `pgFormatXxx`，
可脱离驱动单独使用（`src/common/pg_types.cpp` 不链接 libpqxx）。

### OID 元数据缓存

复合类型的 OID 是**运行时分配**的，必须查 `pg_type` / `pg_attribute` 才能知道「这个 OID 有几个字段、
分别什么类型」。驱动在**每条连接首次产生结果集时**惰性加载全量 `pg_type` + 复合类型字段表并缓存：

- 之后的类型变更不会自动可见。建完类型后请调 `PostgresConnection::refreshTypeCache()`
  （`IDatabaseConnection` 之外的方法，需要持有具体类型）；
- 兜底：读到未知且属于用户区间（OID ≥ 16384）的类型时会标记 stale，**下一条语句自动重载一次**；
- 加载失败不会让查询失败——退化成原始字符串，和改造前一致。

`pg_types.h` 里的文本编解码（数组/复合的元素切分、引号转义、7 种几何语法）不依赖 libpqxx，
因此有 `tests/sqlconduit_pg_types_test.cpp` 做纯单元测试，不需要真库。

## Oracle 驱动（OCI）

Oracle 走官方 **OCI**（Oracle Call Interface）而非 ODBC，由 `SQLCONDUIT_ENABLE_ORACLE` 控制。
标识符引号与 PG 一致用**双引号**，且**不做大小写折叠**——Oracle 会把未加引号的标识符折成大写，
SQLConduit 一律加引号，因此建表时用什么大小写，SQL 里就写什么大小写。

### 依赖与构建

OCI 没有系统包管理器分发，需手工装 **Oracle Instant Client**（Basic 或 Basic Lite + SDK）：

```bash
# Linux 示例（Debian / Ubuntu，用官方 zip 或 rpm 均可）
unzip instantclient-basiclite-linux.x64-*.zip -d /opt/oracle
unzip instantclient-sdk-linux.x64-*.zip      -d /opt/oracle
echo /opt/oracle/instantclient_* > /etc/ld.so.conf.d/oracle-instantclient.conf && ldconfig

cmake .. -DSQLCONDUIT_ENABLE_ORACLE=ON \
  -DOCI_INCLUDE_DIR=/opt/oracle/instantclient_21_12/sdk/include \
  -DOCI_LIBRARY=/opt/oracle/instantclient_21_12/libclntsh.so
```

CMake 会 `find_path(oci.h)` + `find_library(clntsh oci)`；自动探测不到时 **直接 `FATAL_ERROR`**
（不静默关掉驱动），按提示传上面两个变量即可。

### 连接与会话初始化

推荐使用 Oracle 专用配置块，避免把 service name、SID、wallet 和通用数据库字段混在一起：

```yaml
datasources:
  - name: ora
    type: oracle
    host: db.example.com
    port: 1521
    user: app
    password_env: ORACLE_PASSWORD
    connection_timeout_ms: 5000
    query_timeout_ms: 30000
    tls:
      enabled: true
      verify_peer: true
    oracle:
      service_name: APP_PDB       # 与 sid 二选一
      wallet_location: /opt/oracle/wallet
      server_cert_dn: CN=db.example.com,O=Example
      charset_id: 873             # AL32UTF8
      lob_max_bytes: 4194304
      blob_bind: auto             # auto / raw / lob
```

连接目标的优先级固定为 `extra.connection_string` > `dsn` > 生成的 Oracle Net 描述符。前两项按原文
交给 OCI，中间件不会再叠加 `tls`，因此配置了 `dsn` / `extra.connection_string` 时必须把 TCPS 与证书
校验写进该连接串或 Oracle Net 客户端配置，并且不能同时配置 `tls`，防止出现“看似启用、实际被忽略”。
生成描述符时必须提供 `oracle.service_name` 或 `oracle.sid`，两者不能共存；旧 `database` 仍作为
service name 别名，旧 `extra.service_name / sid / charset_id / lob_max_bytes / blob_bind` 也继续兼容，
但新配置应统一写进 `oracle` 块。

`connection_timeout_ms` 会同时写入 Oracle Net 的 `CONNECT_TIMEOUT` 与
`TRANSPORT_CONNECT_TIMEOUT`，在 `OCILogon2` 前生效。`tls.enabled=true` 使用 TCPS；
`verify_peer` 控制 `SSL_SERVER_DN_MATCH`，`server_cert_dn` 可进一步锁定服务端证书 DN，
`wallet_location` 指定 wallet。Oracle 下 `tls.ca` 仅作为 `oracle.wallet_location` 的兼容别名，
`tls.cert / tls.key` 不会被 OCI 直接消费，配置时会报错，应把客户端证书和私钥放入 wallet。

环境句柄走 `OCIEnvNlsCreate` 并**显式固定客户端字符集为 AL32UTF8（charset id 873）**，不再依赖
`NLS_LANG`——未设 `NLS_LANG` 时 OCI 默认按 US7ASCII 解释客户端缓冲区，中文等多字节数据会静默变
乱码。要换成别的字符集（例如老库的 `UTF8` = 871）就设置 `oracle.charset_id`。

连接成功后**立即执行 4 条 `ALTER SESSION`** 固定 `NLS_DATE_FORMAT`、`NLS_TIMESTAMP_FORMAT`、
`NLS_TIMESTAMP_TZ_FORMAT` 和 `NLS_NUMERIC_CHARACTERS='.,'`——结果集按**文本**抓取，
不固定这四项就会被客户端环境变量（如 `NLS_LANG`）左右，同一条 SQL 在不同机器上解析出不同值。

### 类型映射

| Oracle 类型 | `common::Value` | 说明 |
|---|---|---|
| `CHAR` / `VARCHAR2` / `CLOB` / `LONG` | `string` / `Clob` | |
| `NUMBER(p, 0)` p ≤ 18 | `int64` | 无小数且不溢出 |
| `NUMBER` 超 int64 范围 | `uint64` / `Decimal` | 保留原始十进制文本 |
| `NUMBER(p, s)` 有小数、`BINARY_FLOAT/DOUBLE` | `double` / `Decimal` | 有小数位优先 `Decimal` |
| `DATE` / `TIMESTAMP` / `TIMESTAMP WITH TZ` | `Timestamp` | Oracle `DATE` 含时分秒，不退化成 `Date` |
| `RAW` / `BLOB` | `Blob` | 十六进制往返 |
| `ROWID` / `UROWID` | `string` | |
| `INTERVAL DS` / `INTERVAL YM` | `IntervalDaySecond` / `IntervalYearMonth` | 保留原始规范文本并区分两类 interval |

LOB 默认按 `OCILobRead2` 读成 `Blob`，超过 `oracle.lob_max_bytes`（默认 4 MB）报 `NotSupported`
而不是静默截断。

**写侧分两种绑定**，因为 Oracle 的 `RAW` 列和 `BLOB` 列接受的参数类型不同，而绑定时无从得知目标列：

| 条件 | 绑定方式 | 适用列 |
|---|---|---|
| `Blob` ≤ 4000 字节（默认） | `SQLT_BIN` 裸绑定 | `RAW`（以及小值的 `BLOB`，靠服务端隐式转换） |
| `Blob` > 4000 字节 | 临时 `BLOB` locator（`OCILobCreateTemporary` + `OCILobWrite2` + `SQLT_BLOB`） | `BLOB` |
| `oracle.blob_bind=lob` | 无条件走 locator | `BLOB` |
| `oracle.blob_bind=raw` | 无条件走裸绑定 | `RAW` |

超过 4000 字节仍走 `SQLT_BIN` 会撞 ORA-01461 / ORA-22835，所以自动切成 locator。
超过 32767 字节且显式 `blob_bind=raw` 时报 `NotSupported`。

### 占位符与生成键

SQL 里的 `?` 会被改写为 `:n`（1-based）再 `OCIBindByPos`；改写器跳过字符串、标识符与注释里的 `?`，
PL/SQL 块的 `:=` 不受影响。

Oracle 的 `RETURNING` **必须带 `INTO`**，所以手写 SQL 要写成：

```cpp
ds->execute("INSERT INTO t(name) VALUES(?) RETURNING id INTO :2", n, keys);
```

`:1` 已被 `VALUES(?)` 占用，返回列从 `:2` 起。驱动会解析出 `RETURNING ... INTO` 的列与绑定位，
把它们绑成输出缓冲区并在执行后回读。实体映射层的 `insertAs` / `insertBatchAs` 会**自动**追加
`RETURNING <Generated 列> INTO :<n+1>`，不需要手写。

### 事务与保存点

`begin()` 只置内部标记（OCI 无显式 BEGIN，事务随首条 DML 隐式开启）；带 `TransactionOptions`
时下发 `SET TRANSACTION READ ONLY` / `ISOLATION LEVEL ...`。保存点走 `SAVEPOINT` /
`ROLLBACK TO SAVEPOINT`；Oracle **没有 `RELEASE SAVEPOINT`**，故 `releaseSavepoint()` 是 no-op。
`cancel()` 走 `OCIBreak` 中断在途语句。

### 错误与 SQLSTATE

OCI 的 `OCIErrorGet` 并不填充 sqlstate 参数，所以驱动自己维护一张 `ORA-xxxxx → SQLSTATE` 表
（`common::oracleSqlState()`）。这不是装饰——`Status::databaseError()` 是**靠 SQLSTATE 反推
`ErrorCode` 的**，缺了它错误分类和可重试标记就全丢：

| ORA | SQLSTATE | 推导结果 |
|---|---|---|
| ORA-00001 唯一约束 | `23000` | `ConstraintViolation` |
| ORA-01400 NOT NULL | `23502` | `ConstraintViolation` |
| ORA-02290 / 02291 / 02292 检查/外键 | `23514` / `23503` | `ConstraintViolation` |
| ORA-00060 / 00054 / 08177 | `40001` | `Deadlock`，`retryable=true` |
| ORA-01013 取消 | `57014` | `Cancelled` |
| ORA-03113 / 03114 / 03135 连接断开 | `08S01` | `connectionBroken=true` |
| ORA-00942 / 00904 / 00933 | `42S02` / `42S22` / `42000` | 沿用调用方的 fallback |

未收录的 ORA 码返回空 SQLSTATE，由调用方传入的 fallback 码兜底——**不臆造 SQLSTATE**。

### 存储过程、OUT 参数与命名类型

公共调用模型由 `common::CallParam` 描述 `In` / `Out` / `InOut`、预期 `ValueType`、输出缓冲区
大小和数据库类型名；`common::CallOutput` 同时返回标量输出与 REF CURSOR 结果集：

```cpp
using namespace sqlconduit::common;
CallParams params{
    CallParam{Value{std::int64_t(7)}},
    CallParam::out(ValueType::String, 1024),
    CallParam::refCursor()
};
CallOutput result;
auto status = sqlconduit::SQLConduit::call("BEGIN report_pkg.run(?, ?, ?); END;", params, result);
// result.outParams[0] 是标量 OUT；result.sets[0] 是 REF CURSOR
```

`util::call()` 也会在 Oracle procedure 上自动生成 PL/SQL 块并走同一绑定接口。纯 OUT 或值为
NULL 的 INOUT 必须显式给出 `ValueType`，字符串输出可用 `maxBytes` 调整缓冲区，避免驱动猜类型。

命名集合与对象使用 `TypedArray{typeName, items}` / `TypedComposite{typeName, fields}`。在过程的
IN 参数中，驱动会把它们安全展开成 Oracle 类型构造器并逐项绑定，类型名只接受点分标识符；对象
字段按声明顺序传给构造器。OCI 命名对象 OUT/INOUT、BLOB OUT 尚未实现，会明确返回
`NotSupported`，不会退化成字符串或 NULL。

### 能力状态与后续开发边界

以下条目按原因区分，`NotSupported` 不再笼统表示“不会开发”：

- **公共 API 已补齐**：`TypedArray` / `TypedComposite` 携带数据库类型名，两个强类型
  `INTERVAL` 进入 `common::Value`，`CallParam` / `CallOutput` 表达 OUT、INOUT 与 REF CURSOR；
  Oracle 已接入标量输出、REF CURSOR 和过程 IN 方向的命名类型构造器。
- **仍需 OCI 对象描述符实现**：命名对象 OUT/INOUT 与 BLOB OUT 仍返回 `NotSupported`；这是驱动层
  的对象生命周期与类型描述问题，不再是公共 API 无法表达。
- **已经补齐驱动能力**：`openCursor()` 使用可暂停的 OCI statement；`queryAll()` 使用
  `OCIStmtGetNextResult` 顺序读取 Oracle 12c+ 隐式结果集，`supportsMultipleResultSets()` 在客户端
  OCI 提供该接口时返回 true。
- **已经接入原生批量优化**：无 `RETURNING`、无 Blob/LOB 且参数可安全文本绑定的批次使用
  `OCIBindArrayOfStruct` + 单次 `OCIStmtExecute`，并通过 `OCI_ATTR_DML_ROW_COUNT_ARRAY` 返回每次迭代
  的影响行数。含生成键、LOB、超长值或旧 OCI 缺少 row-count-array 能力时自动回退逐条事务执行，
  不改变既有语义。自管事务中的 array DML 任一行失败会整体回滚；调用方事务中保持错误由调用方处理。
- **已经补齐**：`makeDropRoutineSql(..., ifExists=true)` 使用匿名 PL/SQL 执行 `DROP`，只忽略
  ORA-04043（对象不存在），其他错误继续抛出；`ifExists=false` 仍生成严格的原始 `DROP`。
- `connection_timeout_ms` 通过 Oracle Net 描述符约束连接与传输建立，`query_timeout_ms` 映射到
  `OCI_ATTR_CALL_TIME`；TCPS、wallet 与证书 DN 校验均由生成的描述符显式表达；
- `escapeLiteral` 的 Blob 走 `HEXTORAW`，但 `allowsLiteralInterpolation()` 返回 false，实际不会被调用。

`oracle_types.h` 的类型层不依赖 OCI 头，因此有 `tests/sqlconduit_oracle_types_test.cpp` 做纯单元测试，
不需要 Instant Client；需要真机的是 `tests/sqlconduit_oracle_integration_test.cpp`（用
`SQLCONDUIT_TEST_ORACLE_*` 环境变量提供连接信息）。

## 例程与索引（v0.5.1：函数 / 存储过程 / 索引的生命周期与调用协议）

`sqlconduit/util.h` 提供 `sqlconduit::common::util`。它管的是**调用协议**与**生命周期**，
**不做 SQL 方言翻译**——例程体（`BEGIN ... END` / `$$ ... $$` / `AS ...`）由业务按目标方言书写。

### 方言

```cpp
enum class Dialect { Auto, MySQL, Postgres, SqlServer, Oracle };
```

`Auto` 从数据源的驱动类型推断（`mysql*` → MySQL，`postgres*` → Postgres，`odbc*`/`mssql*` → SQL Server）；
识别不出来时返回 `Auto`，此时任何需要生成 SQL 的接口都返回 `NotSupported`——**绝不猜方言**。
自建驱动 / mock 驱动请显式传 `Dialect`。

### 调用协议（`makeCallSql`）

| 方言 | 无结果集 | 有结果集 |
|---|---|---|
| MySQL | `CALL p(?, ?)` | `CALL p(?, ?)` |
| PostgreSQL | `CALL p(?, ?)`（存储过程） | `SELECT * FROM f(?, ?)`（函数） |
| SQL Server | `EXEC p ?, ?` | `{CALL p(?, ?)}` |
| Oracle | `BEGIN p(?, ?); END;` | `SELECT * FROM TABLE(f(?, ?))`（表函数）<br>`SELECT f(?) FROM DUAL`（标量函数） |

PG 存储过程要结果集 → `NotSupported`（PG 的过程不返回结果集，请改用函数）。
Oracle 过程要结果集或有 OUT 参数 → `NotSupported`（需 `REF CURSOR` / `DBMS_OUTPUT` 绑定，
语义与其他方言不对齐）；标量函数走 `DUAL`，表函数走 `TABLE()`。
Oracle 无原生 `CREATE ... IF EXISTS`；删除的 `ifExists=true` 由匿名 PL/SQL 包装，只忽略
ORA-04043，其他数据库错误照常返回。

### 创建 / 删除

```cpp
util::CreateRoutineOptions o;
o.dataSource = "main";
o.stripDelimiter = true;          // 默认：剥离脚本里的 DELIMITER 指令并留痕
util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END", o);

util::RoutineRef ref{"public.p", util::RoutineKind::Procedure, "pg"};
util::DropRoutineOptions d;
d.cascade = true;                 // 仅 PG；其余方言 → NotSupported
util::dropRoutine(ref, d);
```

`replace`（`CREATE OR REPLACE`）仅 PG 支持，创建时的 `ifNotExists` 各方言都不支持 → 一律
`NotSupported`。Oracle 删除时的 `ifExists` 由中间件安全模拟。

### 索引

```cpp
util::IndexSpec spec{"t", "idx_t_a", {"a", "b"}};
spec.unique = true;
spec.usingMethod = "BTREE";       // MySQL 放列清单之后，PG 放 ON 之后，SQL Server 不支持
util::createIndex(spec);
util::dropIndex("t", "idx_t_a");
```

`ifNotExists` / `concurrent` 仅 PG；`CONCURRENTLY` 在事务块内会返回 `TxError`（PG 语义）。
列清单原样透传，因此可以是表达式（如 `lower(name)`）。

### 结构化调用：多结果集与 OUT / INOUT

```cpp
util::RoutineRef proc{"p", util::RoutineKind::Procedure, "my"};

// 纯 IN —— 池路径即可，一次拿回所有结果集
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));
util::CallResult r;
util::call(proc, params, r);
// r.sets：本次调用产生的每个结果集；r.rowCount()：行数合计

// OUT / INOUT —— 必须走 Session 重载
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});
SQLConduit::transaction("my", [&](core::Session &s) { return util::call(s, proc, params, r); });
// r.outParams[0] 即 OUT 值

// 只要 affected：returnsRows = false
util::CallOptions o;
o.returnsRows = false;
util::call(proc, params, r, o);
```

| 方言 | OUT | INOUT | 机制与限制 |
|---|---|---|---|
| MySQL | ✅ 需 `Session` | ✅ 需 `Session` | `CALL p(?, @sqlconduit_out_1)` → 同连接 `SELECT @sqlconduit_out_1`；INOUT 额外先 `SET @sqlconduit_out_0 = ?` |
| PostgreSQL（函数） | ✅ 池路径即可 | ✅ 池路径即可 | 值就是 `SELECT * FROM f(...)` 结果行的前 N 列 |
| PostgreSQL（存储过程） | ❌ | ❌ | PG 的 `CALL` 不把 OUT 回传客户端 → `NotSupported` |
| SQL Server | ❌ | ❌ | 需先 `DECLARE @var <type>`，SQLConduit 无法推断类型 → `NotSupported` |

异步路径没有连接亲和，`SELECT @var` 可能落到另一条连接上，因此**异步不支持 OUT / INOUT**；
需要多结果集时用 `async::util::callAll()`（回调 / future / 协程三形态）。

多结果集依赖驱动能力：MySQL 实现了真正的 `mysql_next_result` 收集；其余驱动退化为"单结果集"。
MySQL 的 `query` / `execute` 现在会消费完剩余结果集（否则连接会停在 `Commands out of sync`），
被丢弃的结果集会写 WARN 日志并提示改用 `queryAll()`。

### 脚本执行（目录 / 文件列表 / 内存）

`util` 额外提供批量脚本执行：`runScriptsInDir`（递归 / 扁平收集 `.sql`）、`runScripts`（显式文件列表）、
`runScriptText`（内存脚本）。语句拆分 `splitSqlScript` 三趟扫描——先屏蔽字符串字面量与行 / 块注释，
再整段屏蔽 `BEGIN/CASE/IF/LOOP/WHILE/REPEAT … END` 复合块，最后在未屏蔽的 `;` 处切分并丢弃空白片段，
因此 MySQL 存储过程体里的 `;` 不会误拆。

- 治理：每条语句走 `detail::runDdl`，与 `createRoutine` / `createIndex` 同源（强制主库、清除 shadow、默认 `NonIdempotent`、失效缓存）。
- 错误：`readSqlFile` 失败或目录不存在 → `ErrorCode::IoError`；`stopOnError=true`（默认）首错即停，`false` 跑完全部、最后一条错误胜出；逐文件 `ScriptResult`（`path` / `status` / `statements` / `executed`）。
- 异步：`async::util` 同样提供回调 / future / 协程三形态；语句严格串行，前一条完成后才调度下一条。回调形态返回聚合 `Handle`，`state()` 跟踪当前语句，`cancel()` 会取消当前操作并阻止后续语句调度；即使 `stopOnError=false`，最终状态仍保留最后一次错误。每条语句用 `ExecScope` 包治理走 `async::execute`。

### 治理行为（不变量）

| # | 行为 |
|---|---|
| I1 | DDL / 索引操作强制走主库：清除请求上下文的 `shadow` 标记，绝不落到影子库 |
| I2 | 默认 `Idempotency::NonIdempotent`，失败不重试；显式声明 `Idempotent` 才按 `max_attempts` 重试 |
| I3 | 结构变更后失效该数据源的查询缓存（由核心 `markWrite()` 保证） |
| I4 | 审计仍生效：黑白名单 / read-only / `require_limit_select` 全部保留，只豁免"例程体里的分号" |
| I5 | 不支持的方言组合显式 `NotSupported`，不静默降级 |
| I7 | 异步三形态（回调 / future / 协程）与同步同源，治理链路与错误码一致 |

### 审计与例程体（v0.5.1 修的核心 bug）

MySQL / SQL Server 的例程体含顶层分号（`BEGIN SELECT 1; SELECT 2; END`），
旧版审计的"多语句"判定会把它当成两条语句并在 `action=block` 时**硬拒绝**。
PG 的 `$$ ... $$` 体本来就被字面量 mask，所以这个 bug 只在 MySQL / ODBC 上暴露。

现在 `hasMultipleStatements(sql, allowRoutineBody=true)` 会先把 `BEGIN ... END`
（含嵌套 `IF` / `CASE` / `LOOP`）整段 mask，再判定剩余部分。
因此：例程体照常放行，而 `CREATE PROCEDURE ... END; DROP TABLE t` **仍然被拦**。

### 已知限制

1. **多结果集**：MySQL / SQL Server 的 `CALL` 已支持多结果集收集；其余驱动退化为单结果集。
2. **OUT / INOUT 参数**：MySQL（需 `Session`）、postgres 函数已支持；postgres 存储过程 / SQL Server / 异步路径返回 `NotSupported`。
3. **事务内 DDL**：MySQL 隐式提交、不可回滚；util 无法改变，DDL 默认不带事务执行。
4. **例程体不做翻译**：跨库部署请维护 N 份方言脚本，由 util 统一管理与执行。

## 可观测性

完整 SQL、慢 SQL 与池指标通过 `observability` 配置；完整参数值默认关闭：

```json
{
  "observability": {
    "sql_log": {
      "enabled": false,
      "mode": "template",
      "level": "debug",
      "slow_only": false,
      "sample_rate": 1.0,
      "max_sql_length": 8192,
      "max_param_length": 256,
      "include_string_values": false,
      "include_blob_values": false
    },
    "slow_sql": {
      "enabled": true,
      "threshold_ms": 500,
      "aggregate_capacity": 1000,
      "recent_capacity": 200,
      "retain_rendered_sql": false,
      "max_sql_length": 4096,
      "histogram_buckets_ms": [10, 50, 100, 200, 500, 1000, 3000, 10000]
    },
    "pool_metrics": { "enabled": true },
    "stats_report": {
      "enabled": true,
      "interval_ms": 60000,
      "file": "logs/sqlconduit_stats.log",
      "format": "text",
      "include_pool": true,
      "include_slow_sql": true,
      "slow_sql_limit": 10
    }
  }
}
```

`stats_report` 由一条后台线程按 `interval_ms` 周期采样并追加写入 `file`（父目录自动创建；
`file` 为空则只走 logger）。`format` 支持 `text`（多行可读）与 `json`（每次一行一个对象）。
`interval_ms` 低于 1000 会被静默抬到 1000，避免高频写文件反过来拖慢业务。
落盘内容与 `allPoolStats()` / `slowSqlStats()` 口径一致。统计失败永远只吞异常、不影响业务。

`sql_log.mode="full"` 会按实际驱动方言渲染参数，但仍只用于诊断，数据库执行继续使用
原生参数绑定。字符串与 BLOB 可能包含密码、Token 或个人数据，只有显式打开对应的
`include_*_values` 后才会进入日志；SQL 和单参数都有长度上限。

```cpp
sqlconduit::SQLConduit::setObserver([](const sqlconduit::common::OperationEvent& event) {
    // event: 数据源、操作类型、耗时、结构化状态、行数和 SQL 指纹。
    // SQL 日志与慢 SQL 均未开启时，默认仍不包含 SQL 或参数。
});

auto topSlow = sqlconduit::SQLConduit::slowSqlStats(20, "main");       // 平均耗时倒序
auto recent = sqlconduit::SQLConduit::recentSlowSql(50, "main");      // 最近发生倒序
sqlconduit::SQLConduit::clearSlowSqlStats();

sqlconduit::core::ConnectionPool::Stats stats;
if (sqlconduit::SQLConduit::poolStats(stats, "app")) {
    // min/max、utilization()、idle/borrowed/waiting、高水位、借出等待耗时、淘汰计数等。
}

auto physicalPools = sqlconduit::SQLConduit::allPoolStats();
```

慢 SQL 使用参数化模板指纹聚合，并通过固定容量与耗时直方图控制内存。观察器异常会被隔离，
不会改变数据库操作结果。数据源组的 `poolStats` 会聚合成员，`allPoolStats` 则返回每个物理池，
便于定位具体主库或副本。

### 追踪上下文（traceId / spanId）

`OperationEvent` / `SlowSqlRecord` 携带两个字段 `traceId` 与 `spanId`（W3C `traceparent` 节
口径，32 / 16 小写 hex），由 `Observability::emitSql` 在最早期自动从 `ContextScope::current()`
注入：

```cpp
#include "sqlconduit/common/context.h"
sqlconduit::common::SqlContext ctx;
ctx.traceId = "4bf92f3577b34da6a3ce929d0e0e4736";     // 32 hex
ctx.spanId  = "00f067aa0ba902b7";                       // 16 hex（可选）
sqlconduit::common::ContextScope scope(ctx);

ds.execute("UPDATE t SET v = ? WHERE id = ?", ...);
// 进入 observer / sql_log / slowSql 时，event.traceId / spanId 已就位。
```

**关键约定**：
- **traceId 不自动生成**：没有调用方上下文时两字段都保持空串，不会发"幽灵 trace"——
  与 `nextSpanId`「不发幽灵 span」同源。
- **spanId 优先沿用调用方**：未填则在有 trace 的前提下按语句自动生成 16 hex，
  跨线程不保证唯一但同一 trace 内不重复，便于按"次请求 = 多条 SQL"颗粒度对齐。
- **`emitSql` 之外的 `emit` 不读 ctx**：`Begin` / `Commit` / `Rollback` 等没有 SQL 语义
  的操作保留空 trace，避免给不存在的链路伪造一条 trace 误导聚合。
- **日志格式**：`sql_log.mode == "full"` 且 trace 非空时，行尾追加 `trace=... span=...`
  便于按 trace 拉一段窗口，不污染无 trace 传统链路。
- **异步自动传递**：M1 已为 `StatementOp` / `SessionOp` 拍 `entryCtx` 快照，worker
  线程执行前会自动 `ContextScope(entryCtx)` 装回，调用方无需手工跨线程。

**W3C `traceparent` 解析与格式化**：

```cpp
auto ctxOpt = sqlconduit::common::parseTraceparent(
    "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
// 校验：长度必须 55；version 必须 `00`；trace-id 不能全 0；trace/span 必须是合法 hex。
// 非法一律返回 std::nullopt，由业务决定丢弃还是回退。

auto parent = sqlconduit::common::formatTraceparent(traceId, spanId);
// 输出严格 55 字节：`00-<32 hex>-<16 hex>-<01 flags>`（flags 缺省 01 = sampled）。
```

`parseTraceparent` 与 `formatTraceparent` 严格按 W3C Trace Context §3.2 节要求的固定
55 字节格式实现，与 OpenTelemetry / Jaeger 兼容；flags 段是"采样标记"位，独立于
traceId / spanId 解析，不会反向污染字段。

### 流量与脱敏标签（shadow / transformed）

`OperationEvent` 额外带两个布尔位，让监控 / 告警 / 计费能在不引入第二条事件流
的情况下，把生产流量和影子流量、原始结果和脱敏结果分开计数：

| 字段 | 数据来源 | true 的含义 | 关键不变量 |
|---|---|---|---|
| `shadow`     | emitSql 读栈顶 `SqlContext.shadow` | 本次 SQL 走的是影子数据源 | 影子流量失败要单独告警；不能和生产共用同一阈值 |
| `transformed`| observeSql 透传 `ResultSet*`，emitSql 读 `result->transformed` | SPI 已改写 / 裁剪 / 脱敏了结果集 | 失败也要标记——合规审计关心「脱敏路径上是否出现真实数据泄漏」|

```cpp
sqlconduit::common::SqlContext ctx;
ctx.tenantId = "t-acme";
ctx.shadow   = true;            // 这个租户分流到影子库
sqlconduit::common::ContextScope scope(ctx);

// SPI afterExecution 已置 view.result->transformed=true
sqlconduit::SQLConduit::setObserver([](const sqlconduit::common::OperationEvent &event) {
    if (event.shadow && event.status.ok()) {
        shadowQps[event.dataSource]++;
    }
    if (event.transformed && !event.status.ok()) {
        // 失败 + 脱敏：要告警——可能脱敏逻辑自身异常
        alert("redaction-failure", event.sqlFingerprint);
    }
});
```

**关键约定**：

- **影子标记的同步路径**：`runWithInterceptors` 把 `onRoute` 决策后的 `routeCtx`
  作为 `ContextScope` 压入线程栈顶，`emitSql` 在最早期读栈顶 → 影子标记与
  `readTarget` / `writeTargets` 用的是同一份决策，不会出现"标记为影子但路由到主"。
- **影子标记的异步路径**：`StatementOp::entryCtx` 在 submit 时拍快照，worker
  执行前 `ContextScope(entryCtx)` 装回，影子标记随 op 跨线程传递且不污染下一次提交。
- **`transformed` 只来自查询**：write / batch / stream / executePrepared 不带
  `ResultSet`，`observeSql` 传 `nullptr`，`event.transformed` 默认 false——失败的
  写不该被算成脱敏失败，标记语义对调用方清晰。
- **告警阈值建议**：影子流量与生产流量分开告警（影子流量 QPS 高、错误率容忍更大）；
  脱敏失败要单独告警（哪怕事务失败也可能部分数据已落库/日志，需即时上报）。

测试覆盖 `tests/sqlconduit_observer_event_test.cpp`（22 项断言 / 8 个场景，包括失败
事件也必须带这两个标记——告警归因需要）。

### 指标导出（Prometheus 文本适配器）

M3 把池指标与慢 SQL 统计暴露为标准 Prometheus 文本格式（0.0.4）。**库不内置 HTTP 服务**——
`/metrics` 端口是应用或 sidecar 的职责，本节展示如何把数据源接给它们。

#### 1. 注册池指标观察者

```cpp
#include "sqlconduit/common/observer.h"

// 在 DatabaseManager::init() 完成后注入 collector；
// init() 内部已经做了，所以通常不必手动再调一次。
sqlconduit::common::Observability::setPoolMetricsCollector([&mgr] {
    return mgr.allPoolStats();
});

sqlconduit::common::Observability::setPoolMetricsObserver(
    [](const sqlconduit::common::PoolMetricsEvent &e) {
        // 立即采一次：可挂在 Prometheus exporter 自己的周期里。
        // 也可以等 StatsReporter::writeOnce 每 interval_ms 触发一次。
        const auto text = sqlconduit::exporters::toPrometheusText(e, {});
        // text 交给 Prometheus scraper（pushgateway / HTTP handler）。
    });
```

或者**不写观察者**，直接调用 `Observability::samplePoolMetrics()` 拿快照（按需拉取）。

#### 2. Prometheus 文本格式

```cpp
const auto pools = sqlconduit::common::Observability::samplePoolMetrics();
const auto slow  = sqlconduit::common::Observability::slowSqlStats(100);
const auto text  = sqlconduit::exporters::toPrometheusText(pools, slow);

// 关键指标名（默认 prefix="sqlconduit"）：
//   sqlconduit_pool_connections{data_source="app",state=...}
//   sqlconduit_pool_connections_idle / _borrowed / _max / _min
//   sqlconduit_pool_utilization_ratio{data_source="..."}
//   sqlconduit_pool_waiting{data_source="..."}
//   sqlconduit_pool_borrow_requests_total / _successes / _timeouts / _wait_seconds_total
//   sqlconduit_pool_connections_created_total / _closed_total
//   sqlconduit_pool_validation_failures_total / _leak_warnings_total
//   sqlconduit_slow_sql_count{data_source="...",fingerprint="..."}
//   sqlconduit_slow_sql_errors / _timeouts / _duration_seconds_sum / _max
//   sqlconduit_slow_sql_duration_seconds_bucket{...,le="0.01|0.1|1|+Inf"}
```

#### 3. 重要约束

- **fingerprint 是高基数标签**：时序库会被撑爆。`toPrometheusText` 接 `maxFingerprintLabels`
  参数限制导出数量（按传入顺序截断），强烈建议填一个合理上限（例如 50）。
- **所有 label value 都按 Prometheus 转义**：`\\` `\"` `\n` 与其它控制字符都不会破坏解析。
- **`+Inf` 桶固定 = count**：histogram bucket 的累积在最后一个有限 bucket 终止，最后
  写入 `le="+Inf"` = 总样本数，符合 Prometheus 直方图惯例。
- **观察者异常不影响业务**：`setPoolMetricsObserver` 的回调抛错会被吞掉，库照常运行；
  但**异常**意味着 exporter 拿不到这次快照——日志与重试由调用方负责。
- **StatsReporter 复用周期**：`cfg.include_pool=true` 时 `StatsReporter::writeOnce` 也会
  调一次 `samplePoolMetrics()`，自动驱动观察者；不必让 exporter 单独再启一条线程。

## 错误码

`common::Status` 携带 `ErrorCode`，可用 `common::errorCodeToString()` 转成字符串。

| 错误码 | 含义 |
| --- | --- |
| `Ok` | 成功 |
| `ConfigError` | 配置解析/校验失败 |
| `ConnectionFailed` | 连接建立失败 |
| `QueryError` | 查询失败 / 参数数量不符 |
| `QueryTimeout` | 查询或事务超过期限 |
| `Cancelled` | 操作被取消 |
| `ConstraintViolation` | 唯一键、外键、非空等约束冲突 |
| `Deadlock` | 死锁或序列化失败，可结合 `retryable` 判断重试 |
| `PingFailed` | 心跳失败 |
| `TxError` | 事务失败（含回调抛异常） |
| `PoolExhausted` | 连接池耗尽（含借出等待超时） |
| `PoolClosed` | 连接池已关闭（`shutdown()` 之后仍在借连接） |
| `CircuitOpen` | 数据源熔断中，未触达数据库 |
| `RateLimited` | 被限流（令牌桶耗尽），非重试，应本地排队或降级 |
| `SqlBlocked` | SQL 被审计策略拦截（如只读写、无 WHERE 的 DML） |
| `Buffered` | 写已入写缓冲，尚未提交（软降级，进程崩溃会丢，且不重试） |
| `NotConnected` | 连接未建立/已断开 |
| `DriverDisabled` | 驱动未在编译期启用 |
| `UnknownDriver` | 未知数据源类型 |
| `NotSupported` | 驱动未实现该能力 |
| `CursorClosed` | 游标已关闭或被移动（move-from）后调用 fetch/close |
| `CursorLimit` | 超过每数据源并发游标上限（max_open_cursors 非 0） |
| `CursorError` | 游标操作失败（DECLARE/FETCH/CLOSE 或驱动取行错误） |

## 安装与下游集成

SQLConduit 可作为 CMake 包安装，下游用 `find_package(sqlconduit)` 直接接入：

```bash
mkdir -p build && cd build
cmake .. -DSQLCONDUIT_ENABLE_POSTGRES=ON   # 按需开启驱动
cmake --build .
cmake --install . --prefix /usr/local
```

### CMake 下游工程

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)

find_package(sqlconduit REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE sqlconduit::sqlconduit)
```

`sqlconduit::sqlconduit` 只导出自身头文件路径与必需的编译定义；驱动客户端库的链接参数在
`find_package` 时按本机环境解析（见下节）。nlohmann/json 是纯构建期私有依赖，不随包安装也不导出，
因此不会与系统或其它依赖的同名头文件冲突。`SQLConduit::shutdown()` 退出前务必调用，回收连接池与
心跳线程。把 SQLConduit 当子项目用时传 `-DSQLCONDUIT_INSTALL=OFF`，上层工程不会多出安装规则。

### 非 CMake 工程（pkg-config）

安装后会生成 `sqlconduit.pc`。只发行静态库，驱动依赖挂在 `Libs.private`，所以**必须带 `--static`**
才会展开成实际库名：

```bash
g++ main.cpp $(pkg-config --cflags sqlconduit) \
    $(pkg-config --libs --static sqlconduit) -o my_app
```

### 驱动客户端库（务必阅读）

开启某个驱动后，安装包**只包含** `libsqlconduit.a` 与头文件，**不含**对应数据库客户端库
（libpqxx / libmysqlclient / unixODBC / OCI）。由于 SQLConduit 是静态库，这些客户端库仍需装在下游
机器上，但**链接参数由包自己解析**：`sqlconduitConfig.cmake` 会载入随包安装的
`sqlconduitDriverDeps.cmake`，在**下游的构建环境里**重新查找本次编译启用的驱动库，再追加到
`sqlconduit::sqlconduit`。导出文件里因此不出现任何绝对路径，安装目录可以整体搬迁。

下游只需装好对应的客户端开发包：

- 开启 MySQL  → `apt install default-libmysqlclient-dev`
- 开启 PG     → 装 `libpqxx-dev libpq-dev`
- 开启 ODBC   → 装 `unixodbc-dev`
- 开启 Oracle → 装 Instant Client（Basic + SDK）

缺库时 `find_package` 会直接报错并指明缺的是哪个驱动。Oracle 客户端通常不在默认搜索路径，可以：

- 在**下游**工程传 `-DSQLCONDUIT_OCI_LIBRARY_DIR=/path/to/instantclient/lib`；
- 或设置环境变量 `ORACLE_HOME` / `LD_LIBRARY_PATH`（会被自动采纳）；
- 或把 `CMAKE_PREFIX_PATH` / `CMAKE_LIBRARY_PATH` 指向客户端目录。

> **预期行为（开箱提示）**
> - 默认 `SQLCONDUIT_ENABLE_*` 全 OFF；未编译期启用的驱动，调用返回 `DriverDisabled`。
> - 程序退出前务必调用 `SQLConduit::shutdown()` 回收连接池与心跳线程。

## 扩展新数据库类型

1. 在 `include/sqlconduit/driver/` 新增 `xxx_driver.h/.cpp`，实现 `MySQLConnection`
   风格的 `IDatabaseConnection` 与 `IDriver`。
2. 在 `.cpp` 中调用 `DriverRegistry::instance().registerDriver("xxx", ...)`。
3. （可选）在 `driver_factory.cpp` 的 `registerBuiltinDrivers()` 中登记，
   或在使用方启动时自行注册。
4. JSON 配置里 `type` 填 `"xxx"` 即可被识别。

基础连接方法仍保持精简；生产驱动应另外覆盖带参数的 `query`/`execute` 并让
`supportsParams()` 返回 `true`。未实现原生绑定时会明确返回 `NotSupported`。
`queryEach`、`executeBatch`、事务选项、保存点和取消都有可选扩展点；流式和批量方法有
兼容默认实现，但大数据驱动应覆盖为游标/按行抓取和数组绑定。

两个容易踩的契约：

- **有事务状态的驱动必须覆盖 `inTransaction()`**。基类默认返回 `false`，
  会导致 `executeBatch` 的默认实现在调用方已开的事务里再套一层 `begin`——
  在 MySQL 上这等于隐式 `COMMIT` 掉调用方的上半段。
- **`cancel()` 从不允许抛异常逃逸**。它会被事务超时的监控线程跨线程调用，
  未捕获的异常会 `std::terminate` 掉整个进程。驱动内部请自行包好 `try/catch`。

## 配置说明（JSON / YAML）

`ConfigLoader` 根据扩展名读取 `.json`、`.yaml` 或 `.yml`；两种格式进入同一套字段解析、
默认值与安全校验逻辑。YAML 支持嵌套对象、对象/标量列表、流式列表、单双引号和行尾注释。

| 字段 | 含义 |
| --- | --- |
| `default_datasource` | 默认数据源名称 |
| `heartbeat_interval_ms` | 心跳间隔 |
| `pool.enabled` | 是否启用连接池（默认 `true`）；关闭后不复用连接、不预热、不受 min/max 约束 |
| `pool.min` / `pool.max` | 每数据源连接池最小/最大连接 |
| `pool.borrow_timeout_ms` | 借出连接的最长等待时间，超时返回 `PoolExhausted`（0 = 不等待） |
| `pool.idle_timeout_ms` | 超过该时间的多余空闲连接回收到 `min` |
| `pool.max_lifetime_ms` | 物理连接最长寿命，到期轮换 |
| `pool.leak_detection_threshold_ms` | 借出超过该时长记录泄漏告警（0 = 关闭） |
| `retry.*` | 最大次数、指数退避上下限、是否允许重试写入 |
| `circuit_breaker.*` | 连续失败阈值与熔断开放时间 |
| `rate_limit.*` | 限流：每数据源总 QPS / 单 SQL 指纹 QPS / 突发容量 / 指纹模式（默认关闭） |
| `sql_audit.*` | SQL 审计：动作（block/warn，默认 warn）、无 WHERE 的 DML、无 LIMIT 的 SELECT、只读拦截、指纹黑白名单（默认关闭） |
| `query_cache.*` | 查询结果缓存：TTL、条目数上限、内存上限、仅副本缓存（默认关闭） |
| `prepared_cache.*` | 预编译语句缓存：每连接最大句柄数（0=不限，LRU 驱逐）、是否启用（默认 true） |
| `datasources[].name` | 数据源名（唯一） |
| `datasources[].type` | `mysql` / `postgres` / `oracle` / `odbc` / 自定义 |
| `datasources[].host/port/user/password/database` | 连接参数 |
| `datasources[].dsn` | ODBC 数据源名 |
| `datasources[].password_env` | 从环境变量读取密码，优先于明文 `password` |
| `datasources[].oracle.service_name` / `sid` | Oracle 服务名或 SID（二选一） |
| `datasources[].oracle.wallet_location` / `server_cert_dn` | Oracle TCPS wallet 与可选服务端证书 DN |
| `datasources[].oracle.charset_id` | OCI 客户端字符集 ID，默认 AL32UTF8（873） |
| `datasources[].oracle.lob_max_bytes` / `blob_bind` | LOB 读取上限与 Blob 绑定策略（`auto` / `raw` / `lob`） |
| `datasources[].query_timeout_ms` | 单条语句执行期限 |
| `datasources[].max_result_rows` | `query()` 单次物化的最大行数；超限返回错误并提示改用 `queryEach()`（0 = 不限制） |
| `datasources[].tls` | TLS 开关、证书校验、CA/客户端证书与私钥 |
| `datasources[].extra` | 驱动自定义扩展参数 |
| `groups[]` | 主库、副本权重、写后读窗口、主库回退、只读标志与故障转移；自动换主需 `acknowledge_external_fencing`，易失缓冲需 `acknowledge_data_loss_and_duplicates` |

完整模板见 `config/datasources.json.example` 和 `config/datasource.yaml.example`。

## 开源协议

本项目以 **Apache License 2.0** 发布。许可证全文见仓库根目录的 [`LICENSE`](../LICENSE) 文件。

- 使用、修改、分发本项目须遵守该许可证的条款。
- 在源码或文档中引用本项目时，请保留版权与许可证声明。
- 贡献代码即表示同意在 Apache-2.0 条款下授权你的贡献。
