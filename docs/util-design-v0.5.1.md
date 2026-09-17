# dbmw v0.5.1 设计：`common::util` —— 例程（函数 / 存储过程）与索引管理

> 目标版本：`v0.5.1`（基线 `v0.5.0`，实体映射层已落地）
> 关联文档：`docs/async-design-v0.2.0.md`（异步三层形态）、`docs/roadmap-design-v0.4.0.md`（非目标与永不实现清单）、
> `docs/mapping-design-v0.5.0.md`（v0.5.0 适配层的定位与边界，本方案沿用其"适配而非魔法"的取向）
> 本文所有"现状"结论均来自对当前代码的逐行核对（文件 + 行号已标注），不是推测。

---

## 0. 结论先行

| 项 | 结论 |
|---|---|
| 做什么 | **例程（函数 / 存储过程）与索引的生命周期管理 + 调用协议封装** |
| **不做什么** | **不做 SQL 方言翻译**：例程体（`BEGIN...END` / `$$ ... $$` / `AS ...`）必须由业务按目标方言书写 |
| 形态 | 单个头文件 `include/dbmw/util.h`（命名空间 `dbmw::common::util`），同步 + 异步三形态 |
| 交付的价值 | ① **正确的调用协议**（`CALL` / `SELECT f()` / `EXEC` / `{CALL}`）；② **生命周期**（CREATE / DROP + `IF EXISTS` 方言差异）；③ **治理链路正确**（强制主库、禁止影子库、不可重试、缓存失效、审计不误判） |
| 需要改动核心 | 3 处（审计多语句判定、DDL 影子库豁免、能力/方言探测），均为**扩展而非改写** |
| 是否需要新错误码 | **不需要**，复用 `NotSupported` / `QueryError` |

**一句话定位**：业务写方言 SQL，util 负责"让它被正确地送到该去的地方、按正确的协议调用、事后清理干净"。

---

## 1. 为什么不做方言翻译（必须先说清的边界）

`docs/roadmap-design-v0.4.0.md` §1.2 把「SQL 方言自动翻译」列为非目标，理由是"需完整解析器，且与'诚实跨驱动'语义冲突"。

把 MySQL 的 `CREATE PROCEDURE ... BEGIN ... END` 翻译成 PostgreSQL 的 `CREATE FUNCTION ... $$ ... $$ LANGUAGE plpgsql`，
等价于**实现一个存储过程语言的解析器与转译器**——这不是"适配层"，是要重写 PL/pgSQL 与 T-SQL 的语义。
因此本方案明确：

- ✅ util **生成**的部分：调用语句（`CALL` / `SELECT` / `EXEC`）、`DROP` 语句、`CREATE INDEX` 骨架（列清单 + 选项）。
  这些是**结构固定、方言差异可枚举**的；
- ❌ util **不生成**的部分：例程体（`CREATE PROCEDURE/FUNCTION` 的函数体）。差异不可枚举（流程控制、游标、异常处理、临时表……）。

> 若业务需要跨库部署同一套例程，正确做法是**维护 N 份方言脚本**，由 util 统一管理与执行，
> 而不是让中间件"猜"出另一份。这与 v0.5.0 映射层"SQL 仍由业务书写"的取向一致。

---

## 2. 现状核对（真实代码事实）

### 2.1 已有的能力

| 能力 | 位置 | 对本方案的意义 |
|---|---|---|
| `StatementKind::Ddl`（CREATE/ALTER/DROP/GRANT/...） | `src/common/sql_analyze.cpp:323-325` | DDL 已被识别 |
| `isWrite(Ddl) == true` | `src/common/sql_analyze.cpp:329-333` | DDL 天然走主库写路由 |
| `hasMultipleStatements(sql)` | `src/common/sql_analyze.cpp:343-355` | **会误判例程体，见 C1** |
| `maskLiteralRegions` 已处理 `$tag$...$tag$` | `src/common/sql_analyze.cpp:202-219` | PG 的 `$$` 例程体**已被正确 mask**（好消息） |
| `QueryCache::invalidate(dataSource)` | `include/dbmw/core/query_cache.h:30` | DDL 后可失效缓存 |
| `Idempotency::{Unspecified, Idempotent, NonIdempotent}` | `include/dbmw/common/context.h:10-14` | DDL 应标 `NonIdempotent` |
| `Session::execute/query` | `include/dbmw/core/database_manager.h:70-89` | 事务内例程调用可用 |
| 异步三形态（回调 / future / 协程） | `include/dbmw/async/dbmw_async.h`、`async/task.h` | util 可镜像三形态 |

### 2.2 四个必须解决的冲突

#### C1（最高优先级）：审计会把例程体判为"多语句"并拦截

```cpp
// src/core/sql_auditor.cpp:70-74
if (hasMultipleStatements(sql)) {
    return verdict(policy->block, policy->log_blocked,
                   "multiple SQL statements are not allowed", ...);
}
```

`hasMultipleStatements` 的判定是"顶层分号之后还有非空白字符"。而 MySQL / SQL Server 的例程体：

```sql
CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END   -- ← 顶层分号 2 个，必然被判多语句
```

**结果：审计开启时，创建存储过程一定被拒。** 这不是"可以绕过"的告警，是 `block=true` 时的硬拒绝。

注意不对称性：PG 的 `$$ ... $$` 体已被 `maskLiteralRegions` 正确 mask，所以 **PG 不受影响**——
这个 bug 只会在 MySQL / ODBC 上暴露，很容易在 PG 环境测试通过后漏到生产。

**修法（不降低安全性）**：
在 `common::sql` 增加"例程体"判定，让审计区分"两条独立语句"与"一条语句内部有分号"：

```cpp
// 新增
bool isRoutineDdl(const std::string &sql);                       // CREATE PROCEDURE/FUNCTION + 体包裹
bool hasMultipleStatements(const std::string &sql,
                           bool allowRoutineBody = false);       // 默认 false，行为不变
```

判定依据：`classifyStatement` 已返回 `Ddl` + 首个动词是 `CREATE`/`ALTER` + 体内存在
`BEGIN ... END`（MySQL/SQL Server）或 `$tag$ ... $tag$`（PG，已 mask）。
黑白名单、read-only、`require_limit_select` 等检查**全部保留**，只豁免"多语句"这一条。

#### C2：写操作在 `shadow` 上下文会发到影子库——DDL 绝不能

```cpp
// src/core/database_manager.cpp:867-869
if (shadow_ && common::ContextScope::current().shadow) {
    const auto st = attempt(shadow_);   // ← 写操作被发到影子库
    return st;
}
```

若调用方的 `SqlContext.shadow` 为 true（灰度/影子流量），util 发出的 `CREATE PROCEDURE` 会**落到影子库**，
而业务以为改的是主库。DDL 是结构变更，这类"改错库"极难发现。

**修法**：util 的 DDL / 索引操作**在执行前强制清掉 shadow 标记**（构造一个 `shadow=false` 的 `ContextScope` 副本），
或走不经过影子判定的专用路径。推荐前者（改动最小、语义明确）。

#### C3：DDL 默认会被重试——但重试几乎必然失败且有害

`CREATE PROCEDURE` 失败后重试，第二次会得到"already exists"；`DROP` 重试会得到"not exists"。
更糟的是部分方言（MySQL）DDL 隐式提交，重试发生在事务语义之外。

**修法**：util 执行时把 `SqlContext.idempotency` 置为 `NonIdempotent`（`resolveWriteAttempts` 会据此把 max_attempts 降为 1）。
用户若写了 `IF EXISTS` / `IF NOT EXISTS` 这类真幂等的语句，可显式声明 `Idempotency::Idempotent` 覆盖。

#### C4：事务内 DDL 的方言差异（MySQL 隐式提交）

| 方言 | 事务内 DDL |
|---|---|
| MySQL | **隐式提交**（DDL 前自动 COMMIT，不可回滚） |
| PostgreSQL | 事务性 DDL，可回滚 |
| SQL Server | 事务性 DDL，可回滚 |

util 无法改变这一点，只能**诚实传递并明确警告**。建议默认约束：
`createRoutine` / `dropRoutine` / `createIndex` **不带事务**（走自动提交），
`call` / `callQuery` 可以在 `Session` 内使用。PG 的 `CREATE INDEX CONCURRENTLY` 反向受限——**不能**在事务块内，
util 需在 `concurrent=true` 且处于事务中时报错（或警告）。

---

## 3. 接口设计

### 3.1 命名空间与物理位置（决策 D1）

用户要求命名空间 `common::util`。但 `include/dbmw/common/` 是基础层（types / observer / context / sql_analyze），
**不依赖 core**。而 util 要执行 SQL，必须依赖 `dbmw.h` → 若把执行函数放进 `include/dbmw/common/util.h`，
会造成 **common 反向依赖上层**，破坏分层。

**推荐（方案 C）**：文件放顶层 `include/dbmw/util.h`，命名空间仍是 `dbmw::common::util`。
命名空间与目录不必一一对应，这样既满足命名要求，又不破坏物理分层——与 `mapping.h` 的做法一致
（v0.5.0 的映射层也在顶层、却依赖 core 与 async）。

| 方案 | 做法 | 代价 |
|---|---|---|
| A | `common/util.h` 只放纯函数（SQL 生成 / 方言判定），执行入口放门面 | 命名空间被拆成两处，用起来别扭 |
| B | `common/util.h` 放全部（含执行） | **破坏分层**，common 反向依赖 core |
| **C（推荐）** | `include/dbmw/util.h` + 命名空间 `common::util` | 无 |

### 3.2 类型

```cpp
namespace dbmw::common::util {

    enum class RoutineKind { Function, Procedure };

    // 方言：决定调用协议与 DROP 语法。Auto = 从数据源的驱动类型推断。
    enum class Dialect { Auto, MySQL, Postgres, SqlServer };

    struct RoutineRef {
        std::string name;                       // 可含 schema 限定，如 "public.f"
        RoutineKind kind = RoutineKind::Procedure;
        std::string dataSource;                 // 空 = 默认数据源
    };

    struct ExecOptions {                        // 所有执行型接口的公共部分
        std::string dataSource;
        Dialect dialect = Dialect::Auto;
        bool invalidateCache = true;            // 结构变更后失效该数据源的查询缓存
        bool forcePrimary = true;               // 清 shadow 标记（C2），DDL 恒为 true
        Idempotency idempotency = Idempotency::NonIdempotent;   // C3
        std::chrono::milliseconds timeout{};    // 0 = 不设语句超时
    };

    struct CreateRoutineOptions : ExecOptions {
        bool replace = false;                   // CREATE OR REPLACE（仅 PG 支持，其余 → NotSupported）
        bool ifNotExists = false;               // 三个方言都不支持 → NotSupported（见 §4）
        bool stripDelimiter = true;             // 剥离 MySQL 脚本里的 DELIMITER 指令（§3.5）
    };

    struct DropRoutineOptions : ExecOptions {
        bool ifExists = true;
        bool cascade = false;                   // PG CASCADE；MySQL/SQLServer → NotSupported
    };

    struct CallOptions : ExecOptions {
        bool readOnly = false;                  // true → 允许路由到从库（默认走主库）
    };

    struct IndexSpec {
        std::string table;
        std::string name;
        std::vector<std::string> columns;       // 可为表达式，原样透传
        bool unique = false;
        bool ifNotExists = false;               // 仅 PG 支持
        bool concurrent = false;                // 仅 PG 支持，且不得在事务内
        std::string usingMethod;                // "BTREE" / "HASH" / "GIN" ... 原样透传
        std::string options;                    // 方言特有尾巴，原样透传
    };
}
```

### 3.3 执行型接口（同步）

```cpp
// ---- 例程：创建 / 删除 ----
// sql 由业务按方言书写（含例程体）；util 只负责送达、治理与事后清理。
common::Status createRoutine(const std::string &sql,
                             const CreateRoutineOptions &opts = {});

common::Status dropRoutine(const RoutineRef &ref,
                           const DropRoutineOptions &opts = {});

// ---- 例程：调用 ----
// 无结果集（或只关心受影响行数）：CALL p(?) / EXEC p ? / CALL p()
common::Status call(const std::string &sql,
                    const common::Params &params,
                    std::int64_t &affected,
                    const CallOptions &opts = {});

// 有结果集：MySQL CALL / SELECT * FROM f(?) / EXEC p ?
common::Status callQuery(const std::string &sql,
                         const common::Params &params,
                         common::ResultSet &out,
                         const CallOptions &opts = {});

// 流式（大结果集，避免一次性物化）
common::Status callEach(const std::string &sql,
                        const common::Params &params,
                        const common::RowCallback &cb,
                        std::uint64_t &rows,
                        const CallOptions &opts = {});

// ---- 索引 ----
common::Status createIndex(const IndexSpec &spec,
                           const CreateIndexOptions &opts = {});   // util 生成 SQL
common::Status createIndexSql(const std::string &sql,
                              const CreateIndexOptions &opts = {}); // 业务给原生 SQL
common::Status dropIndex(const std::string &table, const std::string &name,
                         const DropIndexOptions &opts = {});

// ---- 事务内（Session 形态，用于 call / callQuery）----
common::Status call(core::Session &s, const std::string &sql,
                    const common::Params &params, std::int64_t &affected);
common::Status callQuery(core::Session &s, const std::string &sql,
                         const common::Params &params, common::ResultSet &out);
```

### 3.4 SQL 生成（纯函数，可单测、不执行）

```cpp
// 例程体不生成；只生成结构固定的部分
std::string makeCallSql(const RoutineRef &ref, std::size_t argCount,
                        Dialect d, bool returnsRows);
std::string makeDropRoutineSql(const RoutineRef &ref, const DropRoutineOptions &o, Dialect d);
std::string makeCreateIndexSql(const IndexSpec &spec, Dialect d);
std::string makeDropIndexSql(const std::string &table, const std::string &name,
                             bool ifExists, Dialect d);

// 辅助
Dialect   detectDialect(const std::string &dataSource);   // 失败返回 Auto → 由调用方决定
bool      stripDelimiterDirectives(std::string &sql);     // 剥离 "DELIMITER //" 等，返回是否改动
bool      isRoutineDdl(const std::string &sql);           // 供审计使用（C1）
```

`makeCallSql` 的产物（这是 util 最核心的方言知识）：

| 方言 | 无结果集 | 有结果集 |
|---|---|---|
| MySQL | `CALL p(?, ?)` | `CALL p(?, ?)` |
| PostgreSQL | `CALL p(?, ?)`（PG11+ procedure） | `SELECT * FROM f(?, ?)`（function） |
| SQL Server | `EXEC p ?, ?` | `{CALL p(?, ?)}`（ODBC 转义序列，最稳） |

### 3.5 `DELIMITER` 处理（实用细节）

`DELIMITER` 是 **mysql 命令行客户端**的分句指令，**不是服务端语法**，通过 C API 发送会直接语法错误。
但业务常常从 `.sql` 脚本拷贝例程定义，天然带 `DELIMITER //` …… `DELIMITER ;`。

util 的默认行为 `stripDelimiter = true`：
1. 检测到顶层的 `DELIMITER xxx` 指令 → 剥离该行；
2. **不再**按自定义分隔符切分（因为 API 一次只发一条语句，服务端自己知道 `BEGIN...END` 边界）；
3. 通过 observer / 日志提示"已剥离 N 条 DELIMITER 指令"。

设为 `false` 时原样发送（供确实需要保留的场景，例如脚本回放）。

### 3.6 异步三形态（决策 D5）

镜像 v0.5.0 映射层的做法：

```cpp
namespace dbmw::async::util {                    // 或统一在 common::util 下（见 D1）
    Handle call(const std::string &sql, const common::Params &params,
                ExecCallback cb, async::Options opts = {});
    Handle callQuery(const std::string &sql, const common::Params &params,
                     QueryCallback cb, async::Options opts = {});
    Handle createRoutine(const std::string &sql, OpCallback cb, async::Options opts = {});
    Handle dropRoutine(const RoutineRef &ref, OpCallback cb, async::Options opts = {});
    Handle createIndex(const IndexSpec &spec, OpCallback cb, async::Options opts = {});

    std::future<ExecResult>  call(const std::string &sql, const common::Params &params);
    std::future<QueryResult> callQuery(const std::string &sql, const common::Params &params);
    std::future<OpResult>    createRoutine(const std::string &sql);
    // ...
}

// 协程（DBMW_ENABLE_ASYNC_CORO）
Task<QueryResult> callQueryAsync(const std::string &sql, const common::Params &params);
Task<ExecResult>  callAsync(const std::string &sql, const common::Params &params);
```

异步部分**只是同步接口的薄封装**：治理链路（主库 / 审计 / 不可重试 / 缓存失效）与同步完全一致（I7）。

---

## 4. 跨驱动差异矩阵（实现时的真相来源）

| 能力 | MySQL | PostgreSQL | SQL Server (ODBC) |
|---|---|---|---|
| 函数语法 | `CREATE FUNCTION f(...) RETURNS T BEGIN ... END` | `CREATE FUNCTION f(...) RETURNS ... AS $$ ... $$ LANGUAGE plpgsql` | `CREATE FUNCTION f(...) RETURNS ... AS BEGIN ... END` |
| 存储过程 | `CREATE PROCEDURE p(...) BEGIN ... END` | PG11+ `CREATE PROCEDURE ... LANGUAGE plpgsql`；旧版只能用函数 | `CREATE PROCEDURE p(...) AS BEGIN ... END` |
| `CREATE OR REPLACE` | **不支持** | 支持 | 不支持（用 `ALTER PROCEDURE`） |
| `IF NOT EXISTS` | **不支持** | 不支持 | 不支持 |
| 调用（无结果集） | `CALL p(?)` | `CALL p(?)` | `EXEC p ?` |
| 调用（有结果集） | `CALL p(?)`（可能多结果集） | `SELECT * FROM f(?)` | `{CALL p(?)}` |
| OUT / INOUT 参数 | 支持，但需会话变量：`CALL p(@x)` 后 `SELECT @x` | 支持 + 命名参数 | `OUTPUT` 参数 |
| 多结果集 | 可能出现 | 单个（函数返回表） | 可能出现 |
| `DROP` | `DROP {PROCEDURE\|FUNCTION} [IF EXISTS] name` | 同左 + `[CASCADE]` | `DROP {PROCEDURE\|FUNCTION} [IF EXISTS] name` |
| 事务内 DDL | **隐式提交，不可回滚** | 事务性，可回滚 | 事务性，可回滚 |
| `CREATE INDEX` | 支持；无 `CONCURRENTLY`（用 `ALGORITHM=INPLACE, LOCK=NONE`） | 支持 + `CONCURRENTLY`（**不得**在事务块内） | 支持；无 `CONCURRENTLY`（企业版 `ONLINE=ON`） |
| 索引 `IF NOT EXISTS` | 不支持 | 支持 | 不支持（需先查系统表） |
| `DELIMITER` | CLI 概念，API 不需要 | — | — |

**对接口设计的直接后果**：

1. `CreateRoutineOptions::replace` 在 MySQL / SQL Server 上 → `NotSupported`（I5：显式报错，不静默降级）；
2. `ifNotExists` / 索引 `ifNotExists` 在 MySQL / SQL Server 上 → `NotSupported`；
3. `cascade` 仅 PG；
4. `concurrent` 仅 PG 且禁止事务内；
5. 例程体一律业务自写（§1）。

---

## 5. 不变量（实现必须满足）

| # | 不变量 | 违反后果 |
|---|---|---|
| I1 | DDL 与索引操作**强制主库**，且清除 `shadow` 标记 | 结构变更落到影子库（C2），极难发现 |
| I2 | 默认 `NonIdempotent`；用户显式声明 `Idempotent` 才重试 | DDL 重试必然报错，且 MySQL 下已隐式提交（C3） |
| I3 | 结构变更后失效该数据源查询缓存（可关闭） | 函数定义变了，缓存仍返回旧结果 |
| I4 | 审计**仍然生效**（黑白名单 / read-only / limit），只豁免"例程体分号"这一条 | 要么 DDL 全被拦（现状），要么审计形同虚设 |
| I5 | 不支持的方言组合**显式 `NotSupported`**，不静默降级、不猜 | 静默生成错误 SQL，跑到生产才炸 |
| I6 | 例程体原样传递，util 不改写语义（除 `DELIMITER` 剥离） | 中间件变成不可控的改写者 |
| I7 | 异步三形态与同步**同源**：治理链路、错误码、缓存行为一致 | 两条路径行为分叉 |
| I8 | `DELIMITER` 默认剥离并留痕 | 脚本拷贝过来的例程直接语法错误 |

---

## 6. 需要改动的核心代码（最小侵入）

| 位置 | 改动 | 风险 |
|---|---|---|
| `src/common/sql_analyze.cpp` | 新增 `isRoutineDdl()`；`hasMultipleStatements` 增加 `allowRoutineBody` 参数（默认 false，既有行为不变） | 低：默认路径零变化 |
| `src/core/sql_auditor.cpp:70` | 例程 DDL 传入 `allowRoutineBody=true`；其余检查全部保留 | 低 |
| `src/core/database_manager.cpp:867` | util 的 DDL 路径清 `shadow`（推荐在 util 层用 `ContextScope` 副本实现，不动此文件） | 低 |
| `include/dbmw/common/observer.h:14` | 建议新增 `OperationType::Routine`（可观测性分类；也可复用 `Execute`） | 低（枚举**尾部追加**） |
| 驱动方言上报 | 新增能力查询（如 `driverType()`）供 `detectDialect` 使用；若已有则复用 | 低 |

**不改动**：`ErrorCode`（复用 `NotSupported` / `QueryError`）、`IDatabaseConnection`（用既有 `execute` / `query`）、
`DBMW` 门面既有签名、`mapping.h`。

---

## 7. 已知限制（诚实列出，不藏着）

1. **多结果集**：已实现（D3 补做）。驱动层新增可选能力 `supportsMultipleResultSets()` /
   `queryAll()`，不支持的驱动退化为单个结果集；MySQL 是唯一实现真多结果集的驱动。
   **残留限制**：ODBC / SQL Server 的 `SQLMoreResults` 未接，`SqlServer` 下 `queryAll` 只返回第一个结果集。
2. **OUT / INOUT 参数**：部分实现（D4 补做）。
   - MySQL：OUT → `CALL p(?, @var)` + 同连接 `SELECT @var`；INOUT → 额外先 `SET @var = ?`。**必须用 `core::Session` 重载**（连接亲和）。
   - postgres 函数：OUT / INOUT 值就是 `SELECT * FROM f(...)` 结果行的前 N 列，单语句即可，池路径也能用。
   - postgres 存储过程：PG 的 `CALL` 不把 OUT 回传客户端 → 直接 `NotSupported`。
   - SQL Server：T-SQL 要求先 `DECLARE @var <type>` 才能 `EXEC ... OUTPUT`，dbmw 无法推断类型 → 直接 `NotSupported`，建议改用结果集返回。
   - 异步路径：无连接亲和 → 有 OUT / INOUT 时直接 `NotSupported`。
3. **方言能力探测**：`Auto` 依赖数据源的驱动类型；mock / 自建驱动可能识别不出 → 此时要求显式传 `dialect`，
   否则报 `NotSupported`（不猜）。
4. **DDL 与事务**：MySQL 隐式提交，util 无法改变（C4）；文档明确警告，不做"看起来能用"的补丁。

---

## 8. 测试矩阵（U1–U20）

| # | 用例 | 期望 |
|---|---|---|
| U1 | `makeCallSql` 三方言产物 | MySQL `CALL p(?, ?)` / PG `SELECT * FROM f(?, ?)` / SQLServer `{CALL p(?, ?)}` |
| U2 | `detectDialect` | 按驱动类型返回正确方言；未知 → `Auto` |
| U3 | `stripDelimiterDirectives` | 剥离 `DELIMITER //` 并留痕；`false` 时原样 |
| U4 | 多语句判定（回归） | 普通双语句仍判 true；例程体判 false（PG 的 `$$` 已是 false） |
| U5 | 审计：例程 DDL | 开启审计 + `block=true`，`CREATE PROCEDURE ... BEGIN...; ...END` **不被多语句规则拦下** |
| U6 | 审计：真多语句 | `SELECT 1; DROP TABLE t` **仍被拦下**（I4 不降级） |
| U7 | 审计：黑白名单 | 例程 DDL 不在白名单时仍被拦 |
| U8 | 影子库（C2） | `shadow=true` 上下文执行 `createRoutine` → 落到主库，驱动调用不在影子源 |
| U9 | 幂等（C3） | DDL 失败**不重试**（驱动调用次数 = 1）；显式 `Idempotent` 时才重试 |
| U10 | 缓存失效（I3） | 创建例程后 `QueryCache` 该源失效；`invalidateCache=false` 时不失效 |
| U11 | `replace` / `ifNotExists` 不支持 | MySQL 下 → `NotSupported`（I5） |
| U12 | `cascade` | 仅 PG 通过；其余 `NotSupported` |
| U13 | 索引生成 | `CREATE INDEX` / `UNIQUE` / `USING` / `CONCURRENTLY` 各方言产物正确 |
| U14 | 索引 `IF NOT EXISTS` | 仅 PG；其余 `NotSupported` |
| U15 | `CONCURRENTLY` + 事务 | 在事务内 → 报错（PG 语义） |
| U16 | `call` 无结果集 | affected 正确返回 |
| U17 | `callQuery` 有结果集 | 结果集内容与直接执行等价 |
| U18 | `callEach` 流式 | 行数正确，回调返回 false 可提前终止 |
| U19 | 事务内 `call` | 与 `Session::execute` 语义一致；回滚生效（PG / SQLServer） |
| U20 | 异步三形态 | 回调 / future / 协程与同步同结果、同治理行为（I7） |

---

## 9. 里程碑

| 段 | 内容 | 完成口径 |
|---|---|---|
| M1 | `util.h` 类型 + 纯函数（`makeCallSql` / `makeDropRoutineSql` / `makeCreateIndexSql` / `detectDialect` / `stripDelimiterDirectives`） | U1–U3、U13、U14 绿 |
| M2 | 冲突修复：`isRoutineDdl` + 审计豁免 + 主库强制 + 幂等 + 缓存失效 | U5–U10 绿，U4/U6/U7 回归绿 |
| M3 | 执行型同步接口（`createRoutine` / `dropRoutine` / `call` / `callQuery` / `callEach` / 索引） | U11、U12、U15–U19 绿 |
| M4 | 异步三形态（回调 / future / 协程） | U20 绿；coro 构建位通过 |
| M5 | 文档（guide×2、README×2、roadmap 非目标边界）+ 版本号 0.5.1 + 全量回归 | 全绿 |

---

## 10. 需要你拍板的决策

| # | 决策点 | 我的建议 | 理由 |
|---|---|---|---|
| **D1** | 命名空间物理位置 | **`include/dbmw/util.h` + 命名空间 `common::util`（方案 C）** | 满足命名要求，又不让 `common/` 反向依赖 core |
| **D2** | 是否生成例程体 SQL（方言翻译） | **不做** | 与 v0.4.0 非目标冲突；等价实现 PL/pgSQL 转译器 |
| **D3** | 多结果集 | ~~本期不做~~ → **已补做**（用户追加要求） | 驱动层加可选能力接口，不支持的驱动退化为单结果集 |
| **D4** | OUT / INOUT 参数 | ~~本期不做~~ → **已补做（方言受限）** | 见 §14；SQL Server 与 PG 存储过程明确不支持 |
| **D5** | 异步形态 | **做**（回调 + future + 协程） | DDL 低频但 `call` 可能慢；与 v0.5.0 保持一致性 |
| **D6** | `DELIMITER` 默认行为 | **剥离并留痕**（`stripDelimiter=true`） | 脚本拷贝是常态；原样发送必然语法错误 |
| **D7** | 是否新增 `OperationType::Routine` | **新增**（尾部追加） | 可观测性上能把 DDL/例程与业务 DML 分开统计与告警 |

---

## 11. 风险登记

| # | 风险 | 等级 | 应对 |
|---|---|---|---|
| R1 | 审计豁免被滥用为"绕过审计"的后门 | 中 | 只豁免"多语句"一条；黑白名单 / read-only / limit 全保留（U6、U7 覆盖） |
| R2 | 方言差异在 MySQL 上才暴露（PG `$$` 已 mask） | 高 | 三方言各写一套用例；CI 的 MySQL/ODBC 位必须跑（不能只在 PG 验） |
| R3 | 业务误以为 util 会做方言翻译 | 中 | 文档首屏 + `NotSupported` 显式报错（I5） |
| R4 | 影子库 DDL（C2）漏测 | 高 | U8 专项用例；代码注释标明原因 |
| R5 | MySQL 隐式提交让"事务内 DDL 回滚"的期待落空 | 中 | 文档显式警告 + 默认不带事务执行 |
| R6 | `Auto` 方言探测失败导致静默生成错误 SQL | 中 | 探测不到 → `NotSupported`，绝不猜 |

---

## 12. 文件改动清单（预期）

### 新增

| 文件 | 内容 |
|---|---|
| `include/dbmw/util.h` | `common::util`：类型、纯函数、同步执行接口、异步三形态（协程段门控） |
| `tests/dbmw_util_test.cpp` | U1–U20（mock 驱动；方言用例用 SQL 产物断言，不依赖真实库） |

### 改动

| 文件 | 改动 |
|---|---|
| `src/common/sql_analyze.cpp` / `.h` | `isRoutineDdl`；`hasMultipleStatements` 增加 `allowRoutineBody` |
| `src/core/sql_auditor.cpp` | 例程 DDL 传 `allowRoutineBody=true` |
| `include/dbmw/common/observer.h` | `OperationType::Routine`（尾部追加，D7） |
| `tests/CMakeLists.txt` | 新增 `dbmw_util_test` 目标 |
| `CMakeLists.txt` | `project(dbmw VERSION 0.5.1)` |
| `docs/guide.md` / `guide_en.md` | 例程与索引章节 + 方言差异矩阵 |
| `README.md` / `README_en.md` | 简要示例 |

### 不动（声明）

`include/dbmw/dbmw.h`、`src/core/database_manager.cpp`、`src/async/async_engine.cpp`、
`include/dbmw/mapping.h`、`ErrorCode` 枚举——**零改动**。

---

## 13. 实现状态（v0.5.1 落地后的修订）

本节记录实现与设计文档的**实际偏差**，均为"实现时发现设计不够诚实/不可行"后的修正，不是遗漏。

| # | 设计 | 实现 | 原因 |
|---|---|---|---|
| R1 | `ExecOptions::timeout` | **移除** | 核心没有逐语句超时能力（只有 `borrowTimeout` 与事务超时）。留着就是个永远为 0 的假开关 |
| R2 | `ExecOptions::invalidateCache` | **移除** | I3 已由核心 `markWrite()` 保证：写路径成功即失效该源（及其主/从/故障转移目标）。util 再做一个"关掉它"的开关是撒谎 |
| R3 | `CallOptions::readOnly` | **移除** | 语义无法落地：`call` 走写路径（主库），`callQuery` / `callEach` 走读路径，路由由数据源自身的读写分离策略决定，util 加一个开关只能骗人 |
| R4 | `ExecOptions::forcePrimary` | **保留，但文档写明** | 它做的是"清除 shadow 标记"。写路径本就走主库；对走读路径的 `callQuery` 无效 |
| R5 | `std::string makeCallSql(...)` 等纯函数 | **改为 `Status makeXxx(..., std::string &out)`** | 方言不支持的组合需要带原因报错（I5），返回 `std::string` 只能空串，丢信息 |
| R6 | `bool stripDelimiterDirectives(...)` | **改为返回 `std::size_t`（剥离条数）** | `if (n)` 仍可用，且能直接用于"已剥离 N 条"的留痕日志（I8） |
| R7 | `createIndexSql` 未在设计里 | **已加** | `CREATE INDEX CONCURRENTLY` 之类必须由业务给原生 SQL 时，仍要走同一套治理链路 |
| R8 | 核心改动 3 处 | **实际 5 处** | 额外两处：① `DataSource::driverType()`（方言探测需要驱动类型，此前无接口）；② `core::currentTransactionDepth()`（`CONCURRENTLY` 事务内判定需要，此前无法观测"当前是否在事务里"） |
| R9 | `OperationType::Routine`（D7） | **已加，但当前无人产生** | 门面 `DBMW::execute` 不暴露 `OperationType`，util 发出的事件仍是 `Execute`。枚举值按尾部追加保留，等门面开放后接上；在此之前不要按它建告警 |
| R10 | 审计豁免只在 util 路径 | **放在 `SqlAuditor::check` 全局** | 业务直接 `DBMW::execute("CREATE PROCEDURE ...")` 同样会被误判，只修 util 等于留了一半的坑 |

### 测试

`tests/dbmw_util_test.cpp`，U1–U20 全覆盖：

| 构建 | 用例数 | 结果 |
|---|---|---|
| coro=OFF（C++17） | 74 | 全绿 |
| coro=ON（C++20） | 75（多一条协程断言） | 全绿 |

全量回归：`build-m1` 14/14，`build-coro` 15/15。

### 方言矩阵落在代码里的位置

`makeCallSql` / `makeDropRoutineSql` / `makeCreateIndexSql` / `makeDropIndexSql` 是唯一的方言知识存放点，
新增方言只需改这四个函数 + `detectDialect` 的映射表。

---

## 14. 多结果集与 OUT / INOUT（D3 / D4 补做）

用户追加要求后补做。原设计把这两项判为"本期不做"，理由是"成本独立"——**这个理由不完整**。
核对代码后发现更硬的两条事实：

1. **接口缺能力，不只是成本高**：`IDatabaseConnection` 只有单 `ResultSet` 的 `query()`，
   没有任何"下移结果集"的入口。不补接口，util 层无论怎么写都只能拿到第一个结果集。
2. **MySQL 存在真 bug**：`MySQLConnection::query` / `execute` 只 `mysql_store_result` 一次，
   不消费后续结果集。MySQL 在读完所有结果集前会让连接停在
   `Commands out of sync` 状态——`CALL` 一个会 SELECT 的存储过程后，**这条连接后续全部查询都会失败**。
   这不是"少拿数据"，是连接池被逐步污染。

### 14.1 接口增量

| 位置 | 增量 | 默认行为 |
|---|---|---|
| `IDatabaseConnection` | `supportsMultipleResultSets()` | `false` |
| `IDatabaseConnection` | `queryAll(sql, params, vector<ResultSet>&)` | 调一次 `query()`，非空则放入（退化为单结果集） |
| `Session` | `queryAll()` ×2 | 审计 + 路由 + 指标；**不走拦截器** |
| `DataSource` | `queryAll()` ×2 | preGate + 重试；**不进查询缓存** |
| `DBMW` | `queryAll()` ×4 | 转发 |
| `async` | `MultiQueryResult` + `queryAll()` ×4 | `cacheable = false` |
| `MySQLConnection` | `queryAll()` ×2 + `drainRemainingResults()` | 真 `mysql_next_result` 循环 |

两处"不走"是刻意的：

- **不走拦截器**：`ExecutionView` 只携带一个 `ResultSet*`，把 N 个结果集塞进去没有不丢数据的定义。
- **不进缓存**：`QueryCache` 存的是单个 `ResultSet`，缓存多结果集会静默丢弃第 2 个及以后——
  这正是 R2 里说过的那类"看起来能用"的撒谎。

### 14.2 行为变更（需要知会）

`MySQLConnection::query` / `execute` 现在会在返回前 `drainRemainingResults()`：

- 之前：取第一个结果集后直接返回，连接留在 `out of sync`，**后续查询必失败**。
- 现在：消费完剩余结果集（丢弃，写 WARN 日志提示改用 `queryAll()`），连接可继续复用。

如果业务原本靠"CALL 只取第一个结果集"工作，行为不变（第一个结果集照旧返回）；
变的是连接不再被污染。

### 14.3 OUT / INOUT 方言矩阵

| 方言 | OUT | INOUT | 机制 |
|---|---|---|---|
| MySQL | ✅ 需 `Session` | ✅ 需 `Session` | `CALL p(?, @dbmw_out_1)` → `SELECT @dbmw_out_1`；INOUT 额外先 `SET @dbmw_out_0 = ?` |
| postgres（函数） | ✅ 池路径即可 | ✅ 池路径即可 | OUT / INOUT 就是 `SELECT * FROM f(...)` 结果行的前 N 列 |
| postgres（存储过程） | ❌ | ❌ | PG 的 `CALL` 不把 OUT 回传客户端 → `NotSupported` |
| SQL Server | ❌ | ❌ | T-SQL 要求先 `DECLARE @var <type>` 才能 `EXEC ... OUTPUT`，dbmw 无法推断类型 → `NotSupported` |
| 异步路径 | ❌ | ❌ | 池化执行无连接亲和，`SELECT @var` 可能落在另一条连接上 → `NotSupported` |

**SQL Server 那条是硬墙，不是偷懒**：可以在不知道类型的情况下拼接 `DECLARE @dbmw_out_1`，
但只能猜类型，猜错就是运行时报错或静默截断。宁可现在 `NotSupported` 并提示"改用结果集返回"。

### 14.4 API

```cpp
util::CallParams params;
params.emplace_back(common::Value(std::int64_t(7)));                                  // IN
params.emplace_back(util::CallParam{util::ParamDirection::Out, common::Value(std::int64_t(0))});

// 池路径：无 OUT 时可用，一次拿全部结果集
util::CallResult r;
util::call(ref, params, r, opts);      // r.sets / r.affected / r.outParams

// Session 路径：OUT / INOUT 必须走这条（连接亲和）
DBMW::transaction("my", [&](core::Session &s) { return util::call(s, ref, params, r); });

// 异步：多结果集，但无 OUT / INOUT
async::util::callAll("CALL p(?)", p, cb, opts);
```

`CallOptions::returnsRows`（默认 `true`）决定走 `queryAll` 还是 `execute`：
`true` 收集结果集，`false` 只报 `affected`。

### 14.5 测试

`tests/dbmw_util_test.cpp` 新增 U21–U29：

| 用例 | 覆盖 |
|---|---|
| U21 | `makeCallPlan` 的 SQL 编排：MySQL OUT / INOUT / 纯 IN，PG 函数 OUT，以及 3 条 `NotSupported` |
| U22 | 一次调用收集 2 个结果集，行数与内容正确 |
| U23 | 池路径 + OUT → `NotSupported` |
| U24 | Session 路径读回 OUT=42，同时拿到结果集 |
| U25 | INOUT 先执行 `SET`（`gMainExec == 1`），并回读新值 |
| U26 | PG 函数 OUT 从结果列取值 |
| U27 | 回读失败路径：无行 / 空集 / 列数不足 |
| U28 | 异步三形态多结果集 + 异步拒绝 OUT |
| U29 | `returnsRows=false` 走 execute，只报 `affected` |

| 构建 | util 用例数 | 结果 |
|---|---|---|
| coro=OFF（C++17） | 100 | 全绿 |
| coro=ON（C++20） | 102（多 2 条协程断言） | 全绿 |

全量回归：`build-m1` 14/14，`build-coro` 15/15。

### 14.6 未本地验证项

MySQL 驱动的改动（`queryAll` / `drainRemainingResults` / `query`+`execute` 的 drain）
**未经本地编译验证**——`DBMW_ENABLE_MYSQL` 本地为 `OFF`，本机没有 libmysqlclient。
CI 的 `drivers: 'ON'` 矩阵会打开它，因此这段改动会在 CI 上被编译验证；
推 `dev` 后需要盯一下 Ubuntu 那几个 job 的结果。
