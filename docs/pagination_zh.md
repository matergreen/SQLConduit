# 分页查询（1.2 起）

包含 `sqlconduit/query_page.h` 或完整的 `sqlconduit/sqlconduit.h`。
以下新增接口需要 SQLConduit 1.2 或更新版本。

## Builder：直接得到分页实体

实体按[简化查询 API](query_api_zh.md)注册一次。驱动注册与 Client 初始化完成后：

```cpp
PageOptions options;
options.page = 1;       // 从 1 开始
options.pageSize = 20;

auto query = sqlconduit::sql::select<User>(sqlconduit::Dialect::Postgres)
    .ge(&User::age, 18)
    .orderByDesc(&User::id);
auto page = client.queryPage<User>(query, options);
if (!page.ok()) return page.status;
for (const auto &user : page.items) { /* 使用 user */ }
// page.page / page.pageSize / page.hasNext
// 默认 page.total 为空，不执行 COUNT。
```

也可使用字符串 Builder。排序列需要包含主键或其他唯一的最终排序键；库只检查顶层
`ORDER BY` 是否存在，不能证明唯一性，更不会自动猜测主键。

默认多取一行来计算 `hasNext`，探测行不进入 `items`。空结果、最后一页、超出最后一页
均为正常成功，不越界回退到第一页，也不会强制查询总数。

## 可选总数

```cpp
options.includeTotal = true;
auto plan = query.buildPage();
auto page = client.queryPage<User>(plan, options);
if (!page.ok()) return page.status;
// 成功时 page.total 包含精确总数，包括超出最后一页的情况。
```

直接传 Builder 或 TypedBuilder 时，内部调用 `buildPage()`。它根据结构化表名与条件
生成 COUNT（SQL Server 使用 COUNT_BIG），不把任意 SQL 字符串改写成 COUNT，
排序不进入 COUNT，参数独立保存。
普通 `build()` 返回的 BuildResult 仍能做无总数分页，但不含生成 COUNT 的元数据。

COUNT 必须返回一行一列、非 NULL、非负且在 int64 范围内的整数；Oracle 精确 Decimal
整数（包括全零小数部分）也支持，不经浮点转换。COUNT、数据或映射失败时整体失败，
不会返回半份列表、已成功的 total 或看似有效的 hasNext。

## 原始 SQL、命名参数和复杂查询

```cpp
PageOptions options;
options.pageSize = 20;
auto page = client.queryPageNamed<User>(
    "SELECT id, name, age FROM users WHERE age>=:age ORDER BY id",
    {{"age", 18}}, options);

// 请求总数时，显式给 COUNT 和它自己的参数，不能假定与数据 SQL 参数相同。
options.includeTotal = true;
options.countQuery = sqlconduit::sql::Statement{
    "SELECT COUNT(*) FROM users WHERE age>=?", {18}};
auto counted = client.queryPageNamed<User>(
    "SELECT id, name, age FROM users WHERE age>=:age ORDER BY id",
    {{"age", 18}}, options);
```

位置参数形式为 `queryPage<T>(sql, params, options)`。可通过 `sql::PageQuery` 保存原始
`statement` 与独立 `countQuery`；`PageOptions::countQuery` 优先于计划中的 COUNT。
命名 COUNT 可先用 `common::bindNamed` 验证绑定，再把它的 sql/params 放入 countQuery。
COUNT 是否与数据筛选、JOIN、GROUP BY、DISTINCT 等逻辑一致，由调用方负责。

原始数据 SQL 必须是单个、未分页、带顶层排序的 SELECT，不带结尾分号；已有
LIMIT/OFFSET/FETCH/TOP、SELECT INTO、锁定或尾随 OPTION 会被拒绝。
CTE 暂不由分页接口处理；保留手写分页 SQL 调用普通 query 的路径。词法检查不等于完整
方言解析器，不能作为 SQL 安全防火墙。不要将表名、列名等标识符交给不可信输入控制。

## 异步、超时与 Session

```cpp
auto pending = client.queryPageAsync<User>(plan, options);
auto page = pending.get(); // 不要在执行器/驱动回调中等待

sqlconduit::common::OperationOptions budget;
budget.timeout = std::chrono::milliseconds(250);
budget.dataSource = "reporting"; // 命名数据源通过 withOptions 或 ContextScope 选择
auto pendingWithBudget = client.withOptions(budget).queryPageAsync<User>(plan, options);

auto status = client.withSession([&](sqlconduit::core::Session &session) {
    auto page = session.queryPage<User>(plan, options);
    return page.status;
});
```

Client 提供 `queryPageAsync<T>` 和 `queryPageNamedAsync<T>`；Session 提供同步
`queryPage<T>` / `queryPageNamed<T>`，复用当前事务连接，不另借连接。
异步通过完成回调汇合数据和 COUNT，不增加等待线程。请求共享捕获的上下文与单次
超时/取消预算；数据源拓扑句柄在提交期间固定，但组内路由、故障转移仍遵循既有策略。
只要其中一条语句使用兼容回退，整体 `mode` 就是 CompatibilityFallback；错误提前返回、
另一条模式仍未知时，也保守标为 CompatibilityFallback。

异步 COUNT 与数据查询可以并发执行，因此可能使用两个连接；池只有一个连接时，
另一条请求非阻塞排队。任一失败可先返回，已经提交的另一条仍由运行时清理，不能把
future 就绪理解为物理连接已经归还。`withOptions` 的取消也遵循已有协作式取消边界。

## 方言与一致性边界

- MySQL/PostgreSQL 使用带绑定参数的 `LIMIT ? OFFSET ?`；SQL Server/Oracle 使用
  `OFFSET ? ROWS FETCH NEXT ? ROWS ONLY`，要求服务器支持该语法。
- `PageOptions::dialect`、PageQuery 的方言、实际驱动类型依次决定分页语法；自定义驱动
  无法识别时必须显式指定。指定分页方言不会重新引用原始 SQL 的标识符。Builder 应选用
  与目标数据库匹配的方言，特别是 MySQL，不能把已经生成的 ANSI 引号 SQL 当作自动重写对象。
- page、pageSize 必须大于零，offset 与多取一行的计算必须在 int64 范围内，不截断也不静默修正。
- COUNT 和数据查询默认不是同一快照，不能保证并发写入下 total 与 items 完全一致。
  强一致读使用 Session 加调用方明确设置的事务隔离级别；接口不暗中开启事务。
- 这是 offset 分页，不是游标/keyset 分页。大 offset 可能昂贵，跨次翻页遇到数据修改
  仍可能重复或遗漏。部分投影映射成完整严格实体依然会缺列报错，应定义 DTO。

完整方法与返回值见[英文说明](pagination.md)。
