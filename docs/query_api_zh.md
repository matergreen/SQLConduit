# 简化查询 API（1.2 起）

本文功能需要 SQLConduit 1.2 或更新版本。原有输出参数接口和
`queryAs` / `queryOneAs` 自由函数保留。完整重载与边界说明见[英文 API 清单](query_api.md)。

## 一次定义映射，查询直接返回对象

```cpp
#include <sqlconduit/sqlconduit.h>

struct User {
    std::int64_t id = 0;
    std::string name;
    std::optional<std::string> nickname;
};

namespace sqlconduit::mapping {
    template<> struct RowMapper<User> {
        static Mapping<User> describe() {
            return Mapping<User>().field(&User::id, "id").field(&User::name, "name")
                .field(&User::nickname, "nickname").missingColumns(MissingColumns::Error);
        }
    };
}
```

C++17 无法自动反射结构体字段名，因此需要为每个实体注册一次映射；之后不再逐次编写
字段读取、类型转换与 NULL 判断。SQL 别名与映射列名保持一致。可空字段使用 `std::optional<T>`。

完成驱动注册与 `client.init(...)` 后：

```cpp
// 列表：直接得到 std::vector<User>。
auto users = client.query<User>(
    "SELECT id, name, nickname FROM users WHERE active=?", {true});
if (!users.ok()) return users.status;
for (const auto &user : users.items) { /* user.id, user.name */ }

// 单条：直接得到 std::optional<User>；命名参数不用记位置。
auto user = client.queryOneNamed<User>(
    "SELECT id, name, nickname FROM users WHERE id=:id", {{"id", 42}});
if (!user.ok()) return user.status;
if (user.value) { /* user.value->name */ }

// 标量：COUNT、SUM、单个字段等，无须自己拆 ResultSet。
auto count = client.queryScalar<std::int64_t>("SELECT COUNT(*) FROM users");
if (!count.ok()) return count.status;
if (count.value) { /* *count.value */ }

// 原始行结果也支持直接返回，不必提前创建输出变量。
auto rows = client.query("SELECT id FROM users");
```

这些是函数内使用示例，`return result.status` 表示向上层返回错误。失败仍需检查一次，
不能将失败后的空容器误当作“查不到”。内置转换失败返回 `MappingError`，不会返回半份实体列表。
映射继承现有列策略；示例显式设置 `MissingColumns::Error`，避免漏查字段后悄悄使用默认值。
自定义转换器主动抛出的异常同步向上传播，异步存入 future，不会被改成默认值。

## 方法与返回值

补充的便捷接口包括 `queryColumn<T>` / `queryColumnNamed<T>`（单列多行，返回 `items`），
以及 Client 对应的 `*Async` 方法。必须只投影一列；可空值使用 `std::optional<T>`，
类型转换失败不会返回半份列表。

Client 和 Session 的同步查询、写入、实体/单条/标量/单列方法都可直接传 SQL Builder
的 `BuildResult` 或 `Statement`；Client 也支持异步形式。构建失败不执行 SQL。
Session 新增同样的同步便捷接口和命名参数接口，始终复用已有连接，不会另借连接、
自动提交或额外开启事务。原有 Session 同步约定不变。

可用以下写法替代显式 `RowMapper<User>`，两者不要同时定义。放在全局命名空间作用域，
每个实体定义一次（通常放在公共头文件中）；不同列名使用 `SQLCONDUIT_NAMED_FIELD(name, "alias")`。

```cpp
SQLCONDUIT_MAP(User,
    SQLCONDUIT_FIELD(id)
    SQLCONDUIT_FIELD(name)
    SQLCONDUIT_FIELD(nickname))

auto built = sqlconduit::sql::Builder::select("users").columns({"id", "name", "nickname"})
    .where(sqlconduit::sql::eq("id", 42)).build();
auto user = client.queryOne<User>(built);
auto names = client.queryColumn<std::string>("SELECT name FROM users");
```

宏默认严格要求全部映射列；需要自定义缺列策略/转换器时保留显式映射。
结果统一提供 `ok()`。偏好异常的调用方可显式使用 `rowsOrThrow()`、`itemsOrThrow()`、
单条/标量的 `valueOrThrow()`，抛出保留原始 `status()` 的 `common::StatusError`。
查不到或 NULL 的 `valueOrThrow()` 抛 `MappingError`，绝不返回默认对象；默认仍是返回状态，
不会强迫业务代码改为异常。右值访问器移出结果，左值引用不能超过结果对象的生命周期。

## 成员指针 Builder 与一行实体注册

包含 `sqlconduit/typed_sql_builder.h` 或完整 `sqlconduit/sqlconduit.h`：

```cpp
struct Account {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    std::optional<std::string> nickname;
};
SQLCONDUIT_ENTITY(Account, "accounts", id, name, age, nickname)

std::string nameFilter = "Ada";
auto statement = sqlconduit::sql::select<Account>()
    .ge(&Account::age, 18)
    .eq(!nameFilter.empty(), &Account::name, nameFilter)
    .orderByDesc(&Account::id)
    .build();
auto accounts = client.query<Account>(statement);
if (!accounts.ok()) return accounts.status;
```

这是类似 MyBatis-Plus 字段引用的 C++ 写法，不解析任意 Lambda，也不要求 getter。
同一份元数据同时负责表名、Builder 列名和查询结果转换，不必再写第二套映射。
`SQLCONDUIT_ENTITY` 放在全局命名空间作用域，每个实体注册一次（通常放在公共头文件中），
支持 1–32 个公开成员，默认列名与成员同名，严格要求结果包含全部映射列。
不会猜表名或自动把驼峰改成下划线。超过 32 个字段使用详细映射即可。

特殊列名、主键、自增或只读字段仍可用详细形式；与上面的注册二选一，不要重复定义：

```cpp
SQLCONDUIT_MAP(Account,
    SQLCONDUIT_TABLE("accounts")
    SQLCONDUIT_FLAGGED_FIELD(id, "account_id",
        sqlconduit::mapping::FieldFlags::PrimaryKey | sqlconduit::mapping::FieldFlags::Generated)
    SQLCONDUIT_NAMED_FIELD(name, "display_name")
    SQLCONDUIT_FIELD(age)
    SQLCONDUIT_FIELD(nickname))

auto changed = sqlconduit::sql::update<Account>()
    .value(&Account::name, "Ada").eq(&Account::id, 42).build();
auto result = client.execute(changed);
```

显式 `Mapping<Account>().table("accounts").field(...)` 也支持。旧映射没有表名时，
可使用 `sql::select<Account>("accounts")`，其他三个 CRUD 工厂同样支持显式表名和方言。
Builder 默认 SELECT 全部映射列；`columns(&Account::id, &Account::name)` 可选择投影。

条件支持 `eq/ne/gt/ge/lt/le/like/in/notIn/between/isNull/isNotNull`，
都可增加第一个 `bool enabled` 参数，决定是否加入条件。排序支持
`orderByAsc/orderByDesc/orderBy`，写入使用 `value(member, value)`，也支持条件式赋值。
条件默认 AND 连接，复杂分组可用 `where(sql::all/any/not_(...))` 复用原有结构化条件；
这个显式扩展口使用字符串列名，不提供成员指针校验。

注意以下边界：

- 不同实体字段、明显不兼容的值类型会在编译期报错；未注册、空或有歧义的成员指针，
  重复列名会在 `build()` 报 `MappingError`，不生成可执行语句。
- 值继续参数绑定，沿用严格转换规则与自定义转换器；溢出、隐式窄化不悄悄截断。
  `common::Value` 和 IN 列表逐值运行时检查。自定义转换异常仍向上传播。
- 自增/只读字段可以查询、过滤，不能通过类型化 `value` 赋值。
- 条件为 false 时不解析字段或绑定值，但 C++ 仍会先求值参数；空 optional 要使用
  `value_or(...)`，不能因为条件为 false 就直接解引用。全部条件跳过后，全表写入保护仍有效。
- 部分投影再映射成完整严格实体仍会缺列报错，应使用 DTO 映射、原始结果或 `queryColumn<T>`。
- `build()` 返回原有 BuildResult，Client/Session、异步方法和 `withOptions` 都可直接执行。
  这是轻量 SQL 便捷层，原有字符串 Builder 和手写 SQL 接口不变。

## 单次超时与取消

类型化分页、可选总数和 Builder 自动 COUNT 的用法见[分页查询](pagination_zh.md)。
包含 `query_page.h` 或完整聚合头文件即可。

```cpp
sqlconduit::common::CancellationSource cancel;
sqlconduit::common::OperationOptions options;
options.timeout = std::chrono::milliseconds(250);
options.cancellation = cancel.token();
// options.dataSource = "reporting";
// options.allowCompatibilityFallback = false; // 异步时禁止同步兼容回退
auto pending = client.withOptions(options).queryOneNamedAsync<User>(
    "SELECT id, name, nickname FROM users WHERE id=:id", {{"id", 42}});
cancel.requestCancellation(); // 幂等，也可由另一线程调用
auto result = pending.get();
if (!result.ok()) { /* result.status.code / result.status.outcomeUnknown */ }
```

`withOptions` 支持原始/类型化/命名参数查询、单列、Builder 和写入；Session 支持同步形式。
视图不拥有 Client/Session，不能超过宿主生命周期。复用视图时，每次调用重新计算相对超时。
`timeout=0` 不附加相对限制，负值报错；`deadline` 使用 steady_clock 绝对时间。
嵌套 `OperationScope` 继承取消并取更早的截止时间。数据源仍按现有上下文优先级路由；
Session 的 `dataSource` 不会切换已借出的连接。此次不扩展游标与事务生命周期取消接口。

预算覆盖排队、借连接、重试退避与 SQL 执行；取消后不再开启下一次重试。
`allowCompatibilityFallback=false` 只约束异步方法，不禁止同步调用。
同步调用无法抢占 SDK 建连或不支持取消的阻塞调用；异步 future 可以先返回，物理连接仍
保留到驱动完成及正在进行的取消清理结束，停止的在途连接会被丢弃。驱动取消通过共享有界
工作池尽力执行，不阻塞全局计时线程；自定义结果转换也不能被抢占。已有驱动/配置限制仍可能更早触发。

**取消不等于回滚。** 写入已经开始时提前返回的超时/取消会标记 `outcomeUnknown=true`，
且不可自动重试：服务端可能已经提交，业务不能盲目重放。同步写入已有明确成功结果时保留成功。
只在连接池等待阶段停止不会标记写入结果不确定。此标记仅描述单次停止的写入风险，
不能替代对其他驱动/提交异常的既有歧义判断。

## 基础方法清单

| 方法 | 可直接使用的数据 |
| --- | --- |
| `query<T>` / `queryNamed<T>` | `result.items`，实体列表 |
| `queryOne<T>` / `queryOneNamed<T>` | `result.value`，可选单个实体 |
| `queryScalar<T>` / `queryScalarNamed<T>` | `result.value`，可选单个值 |
| `query` / `queryNamed` | `result.rows`，原始结果集 |
| `executeNamed` | `result.affected`，影响行数 |

以上均有 `Async` 版本，如 `queryNamedAsync<User>`、`queryOneNamedAsync<User>`、
`queryScalarAsync<int>`、`executeNamedAsync`。异步返回 `std::future<结果类型>`。
每种方法都有 `(sql, params)` 和 `(source, sql, params)` 两种形式；前者参数可省略。
路由继续遵循既有 `ContextScope` 优先级，不另造一套规则。

空列表是成功；单条查询无记录时 `value` 为空，多于一条时报错。
标量查询最多一行一列：无记录为 `hasRow=false`；SQL NULL 为
`hasRow=true, isNull=true` 且 `value` 为空；有值时读取 `*value`。
多行、多列、数值溢出、类型不匹配均报错，不静默截断或取第一条。
大结果仍应使用 `queryEach` 或游标；这些便捷方法会物化结果。

## 命名参数与异步

```cpp
auto pending = client.queryOneNamedAsync<User>(
    "SELECT id, name, nickname FROM users WHERE tenant_id=:tenant AND id=:id",
    {{"id", 42}, {"tenant", "acme"}}); // 传参顺序不影响绑定
// 这里可以处理其他业务。
auto user = pending.get();
if (!user.ok()) return user.status;
if (user.value) { /* user.value->name */ }
```

命名参数大小写敏感，名字为 ASCII `[A-Za-z_][A-Za-z0-9_]*`，传参时不带冒号。
同名占位符可重复使用；漏参、重复传入同名参数、多余参数及混用位置参数，会在执行前报错。
值始终交给驱动参数绑定，不拼接到 SQL 中；参数只能代表值，不能代表表名或 SQL 片段。
字符串、引号标识符、美元引用、Oracle 替代引号、嵌套块注释及 PostgreSQL `::` 不会当成参数。

Client 与 Session 均提供同步命名便捷接口，Client 另提供异步形式。自定义适配器可复用
`common::bindNamed` 获取验证后的 SQL 和位置参数。命名方法与位置参数方法分开命名，
避免已有 `queryAsync(sql, {})` 等调用出现重载歧义。

这不是完整方言解析器：`#` 注释/运算符因方言歧义会被拒绝，原始 `?` 运算符保留给位置绑定；
建议使用 `--` 注释和 PostgreSQL `jsonb_exists` 等函数等价写法。不要混用 `?`、`$1`、`:1`。

异步映射在查询完成时进行，不另开等待线程、不占用工作线程等待另一个 future。
SQL 与参数在请求期间拥有独立副本；`mode` 保留原生异步/兼容回退的真实状态。
不要在执行器或完成回调里调用 `get()` 等待其他请求，也不要在映射中重载、关闭同一个 Client。
同步结果的 `mode` 不用于描述异步执行能力。

## 配置初始化与重载预览

```sh
sqlconduitctl init postgres > datasource.yaml
sqlconduitctl init mysql --format json > datasource.json
# 同样支持 odbc（SQL Server Driver 18 示例）、oracle（FREEPDB1 示例）。
# 在安全的环境配置中设置 SQLCONDUIT_DB_PASSWORD，不要将明文密码写入仓库。
sqlconduitctl validate datasource.yaml
sqlconduitctl doctor datasource.yaml --connect
sqlconduitctl reload current.yaml candidate.yaml --dry-run
```

`init` 只向标准输出生成模板，不自行创建文件；Shell 重定向可能覆盖文件，请使用新路径。
模板中的地址、用户、库名、服务名及 TLS 设置需按实际环境修改；`pool.min=0` 只是延迟建连，
不代表数据库已验证可用。

`--dry-run` 离线验证两份配置，输出变更路径、新增/删除数据源及连接池替换范围。
密码、DSN 等变化也能检测，但不会输出敏感值或其摘要。它不会连接数据库、修改文件、
通知运行中的应用，也看不到运行时 `add/removeDataSource` 的变更。

当前真实 `Client::reload` 会重建全部候选连接池，并按调用方给定的 grace 排空旧池，
不是增量更新；无差异也不意味着调用真实 reload 时不会重建。预览不能保证驱动可用或数据库可达，
不承诺请求无限等待或自动重放。候选拓扑构建失败时保留旧拓扑。

另外，`heartbeat_interval_ms: 0` 现明确表示关闭周期心跳；正数才启动心跳线程。
