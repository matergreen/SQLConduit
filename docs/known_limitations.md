# Known limitations in SQLConduit 1.0

This document distinguishes supported compatibility behavior from features that 1.0 does not
claim to provide. None of these paths silently pretends to be a stronger capability: status,
execution mode, and statistics remain observable.

For the complete implemented/testing/scheduled matrix, see
[Feature coverage and evolution](feature_coverage.md) or its
[Chinese version](feature_coverage_zh.md).

## Asynchronous execution

- PostgreSQL leaf queries and writes can use the bundled libpq socket reactor. Eligible MySQL
  8.0.16+ parameter-free, single-statement operations can use the MySQL nonblocking C API.
- Parameterized MySQL statements, older MySQL/MariaDB clients, transactions, cursors, batches, and
  cache/retry/group/shadow topologies use the bounded `CompatibilityFallback` executor. In the
  1.0.1 development line, eligible Oracle operations use OCI nonblocking mode and eligible ODBC
  operations use polling only when the selected driver reports statement- or connection-level
  async. FreeTDS reports no native async. Microsoft ODBC Driver remains compile-tested but not yet
  live-driver validated. Oracle temporary-LOB input binding falls back; LOB result materialization
  runs on a reactor worker because OCI secure-file LOB reads are unsupported in nonblocking mode.
- Fallback read deadlines can classify a late result as `QueryTimeout`, but SQLConduit cannot safely
  force-kill an arbitrary blocking vendor call. Successful write, batch, or transaction results are
  not rewritten after completion because commit state may otherwise become ambiguous.
- `StreamSource` is currently buffered by the driver before the caller reads it; it bounds the
  application-facing interface but is not a promise of constant-memory server-side streaming.

## SQL and mapping

- SQL Builder intentionally covers portable `SELECT`, `INSERT`, `UPDATE`, and `DELETE`. Joins,
  upsert, generated-key clauses, locking, expressions, stored procedures, and arbitrary dialect
  translation remain explicit application SQL.
- SQL Server generated-key mapping does not inject an `OUTPUT` clause. Supply the appropriate SQL
  and map the returned row explicitly.
- Mapping database `NULL` into a non-`optional` value returns a conversion error; use
  `std::optional<T>` for nullable columns. This applies to scalar, text, and binary values.
- Oracle complex named-object binding and OUT/INOUT LOB convenience mapping are outside the 1.0
  contract. Use supported scalar/LOB input bindings and explicit result queries.

## Packaging and database clients

- Each driver is a separate static component. Linking MySQL, PostgreSQL, ODBC, or Oracle requires a
  compatible vendor client SDK at build/link/runtime as applicable; Core alone has no database SDK
  dependency.
- Linux and macOS release archives contain Core, MySQL, PostgreSQL, and ODBC components. The Windows
  archive contains Core and ODBC. Oracle must be built from source against an installed Oracle
  Instant Client SDK and is validated manually for releases.
- The Microsoft ODBC Driver path is compile-tested on Windows but has no live-driver validation in
  the 1.0.0 release matrix. Live SQL Server integration uses unixODBC and FreeTDS on Linux.
- SQLConduit promises 1.x source compatibility, not C++ ABI compatibility across toolchains or
  runtime-library modes. Build the library and consuming application with compatible settings.

## Operational boundaries

- Retries, failover, and write buffering cannot provide exactly-once writes. Opt-in configuration
  fields require explicit acknowledgement where duplicate or lost writes are possible.
- Connection cancellation and timeout precision depend on the selected vendor driver. After a
  cancellation failure, SQLConduit discards or quarantines a connection when its state cannot be
  proven reusable.
- Metric label sets are deliberately bounded. SQL text, raw parameters, datasource URLs, and other
  unbounded user values are excluded from stable Prometheus labels; use logs or sampled slow-query
  records for detailed diagnosis.

---

# SQLConduit 1.0 已知限制

- PostgreSQL 和部分无参数 MySQL 操作可使用 native 异步。1.0.1 开发线为合格 Oracle 操作加入
  OCI nonblocking，并在 ODBC 驱动报告 statement/connection 级异步能力时使用轮询状态机。
  FreeTDS 不报告 native async；Microsoft ODBC Driver 仍只有编译验证、尚无实库验证。Oracle
  临时 LOB 输入绑定回退兼容执行器；OCI 不支持在 nonblocking 模式读取 secure-file LOB，因而
  LOB 结果会在 reactor worker 上切回 blocking 完成物化。事务、游标、批量和复杂拓扑继续 fallback。
- `StreamSource` 当前仍先由驱动缓冲结果，不承诺服务端常量内存流式读取。
- SQL Builder 只覆盖可移植 CRUD；JOIN、Upsert、生成键、锁、表达式、存储过程和任意方言
  仍应编写显式 SQL。SQL Server 生成键不会自动注入 `OUTPUT`。
- 数据库 `NULL` 映射到非 `std::optional<T>` 字段会返回转换错误；可空列必须使用 optional。
- Oracle 复杂命名对象绑定及 OUT/INOUT LOB 便利映射不属于 1.0 契约。
- 驱动是独立静态组件；只链接实际使用的组件。预编译 Windows 包仅含 Core/ODBC，Linux 与
  macOS 包含 Core/MySQL/PostgreSQL/ODBC；Oracle 需要基于 Instant Client SDK 从源码构建。
- Microsoft ODBC Driver 在 1.0.0 中只有 Windows 编译验证；SQL Server 实库集成使用
  Linux 上的 unixODBC/FreeTDS，不宣称已完成 Microsoft 驱动实库验证。
- 1.x 承诺源码兼容，不承诺跨工具链 C++ ABI。重试、故障切换和写缓冲也不提供 exactly-once
  写入保证。
