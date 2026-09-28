# SQLConduit public API stability

This document defines the compatibility boundary for SQLConduit 1.x. It applies to documented
installed-package entry points. A header being installed for transitive type completeness does not,
by itself, make every declaration in that header stable API.

Capability status and planned additive evolution are tracked separately in
[Feature coverage and evolution](feature_coverage.md); roadmap status never overrides this
compatibility contract.

## Compatibility levels

### Stable application API

These are the preferred entry points for application code:

- `sqlconduit/api.h`, `sqlconduit/public.h`, and `sqlconduit/client.h`: export/deprecation macros,
  the compact stable-header aggregate, and the sole high-level runtime entry, an independently owned, move-only
  `Client`;
- `sqlconduit/sqlconduit.h`: the full convenience umbrella header;
- `sqlconduit/common/types.h`, `common/context.h`, and `common/observer.h`;
- `sqlconduit/config/datasource_config.h` and `config/config_loader.h`;
- `sqlconduit/async/async_types.h`: result and executor-statistics types returned by `Client`;
- `sqlconduit/common/pg_types.h`, `common/oracle_types.h`, `sqlconduit/mapping.h`,
  `sqlconduit/util.h`, and `sqlconduit/version.h`;
- `sqlconduit/exporters/prometheus.h` and its documented metric names and bounded labels;
- `sqlconduit/sql_builder.h` and `sqlconduit/common/sql_dialect.h` for portable CRUD construction
  and explicit identifier dialect selection.

Existing names, signatures, enum numeric values, and documented behavior in this group will not be
changed incompatibly within 1.x. New overloads and fields may be added when existing source
continues to compile.

### Stable extension API

Applications implementing drivers, interceptors, or session callbacks may depend on:

- `driver/idriver.h`, `driver/driver_registry.h`, and the lightweight built-in registration
  factories under `sqlconduit/drivers/`;
- `core/idatabase_connection.h`, `core/interceptor.h`, and `core/rate_limiter.h`;
- `core/database_manager.h`, `core/cursor.h`, and `core/connection_pool.h` for the `DataSource`,
  `Session`, `Cursor`, options, and statistics types exposed by `Client`.
- `core/query_cache.h` and `core/sql_auditor.h` for the statistics types returned by `Client`.

Concrete built-in driver classes and vendor SDK headers are internal. Applications select a built-in
component at link time and pass its `DriverRegistration` to `Client::addDriver()` before `init()`.

### Installed support surface and internal implementation

Some headers, including `common/logger.h`, `core/heartbeat_manager.h`, and `core/write_buffer.h`, are
installed because stable declarations need their complete types or because the build package uses
them. They are not supported direct entry points unless listed above. `RuntimeServices`,
`StatsReporter`, source-tree-only headers, undocumented declarations, and names in a `detail`
namespace are internal and carry no compatibility promise.

## 1.x policy

- Minor and patch releases in the `1.x` line preserve source compatibility for the stable
  application and extension API. CMake package matching therefore uses `SameMajorVersion`.
- A stable API may be deprecated in a minor release but remains available for at least that minor
  line. Removal or an incompatible semantic change requires 2.0 and a changelog migration path.
- The documented JSON/YAML configuration keys, validation behavior, metric names, and bounded label
  names follow the same rule. New optional keys or metrics may be added in a minor release.
- `ErrorCode` numeric values are part of the compatibility contract and are never renumbered.
- Public enums use explicit numeric values. Existing values are never reordered or reused.
- Deprecations use `SQLCONDUIT_DEPRECATED(message)` from `sqlconduit/api.h`.
- This is a source-compatibility contract, not a cross-toolchain binary ABI promise. Rebuild static
  libraries and consumers together when changing compiler, standard library, or runtime mode.

## Lifecycle, concurrency, and callbacks

- A `Client` starts empty, becomes running after a successful `init()`, and becomes permanently
  closed after `shutdown()`. Failed initialization leaves it reusable. A second successful
  initialization requires `reload()`.
- Concurrent query, execute, session, statistics, observer, and interceptor operations on a running
  client are supported. Moving a client object must be externally synchronized.
- `reload()` atomically publishes a new topology. Calls that already resolved a `DataSource` may
  finish on the previous topology during the supplied grace period; later calls use the new one.
- `shutdown()` rejects newly started client operations with `ClientClosed`, drains that client's
  queued future operations, then closes its pools. A previously obtained `DataSource` remains a
  valid C++ object but database calls return a closed/not-connected status after its pool closes.
- Synchronous observers and interceptors run on the calling thread. `Client::*Async` observers,
  interceptors, row callbacks, and transaction callbacks run on that client's worker threads.
  Stats reporting and pool sampling may run on their background threads. User callbacks must be
  thread-safe; exceptions do not cross the SQLConduit boundary.
- A callback must not destroy, move, `reload()`, or `shutdown()` the same client from inside one of
  that client's operations. Schedule lifecycle changes after the callback returns.

## Async boundary

The future-returning `Client::*Async` methods use an executor, data-source topology, cache, audit
policy, interceptors, and observability state owned by that client. Shutting down one client drains
only its executor. Async result types expose whether an operation completed through a driver-native
path or SQLConduit's compatibility fallback; `asyncStats()` reports native, fallback, and timeout
counts. These methods intentionally expose no cancellation handle or coroutine wrapper yet.
Compatibility fallback honors `async.statement_timeout_ms` for late read classification but does not
rewrite successful write, batch, or transaction results after the driver call has already completed.
Eligible leaf-data-source paths use asynchronous pool handoff, including an active pool-wait deadline.
Leaf query-cache hits also complete inside the async state machine without borrowing a connection;
cache misses can continue into a native driver operation.
The driver SPI now includes optional `queryAsync` / `executeAsync` callbacks; returning `false`
means no operation was started and preserves compatibility fallback. PostgreSQL implements these
callbacks with a bundled libpq socket reactor for eligible leaf query/execute operations. MySQL
8.0.16+ implements parameter-free single-statement SELECT and DML/DDL through its official
nonblocking C API; parameterized operations remain prepared-statement fallbacks. Oracle, ODBC,
older MySQL/MariaDB clients, and transaction/cursor/batch or complex-topology paths retain the
compatibility fallback while their native state machines remain under development.

There is no process-wide async facade. Callback, cancellation-handle, and coroutine wrappers were
removed before the 0.7 API freeze; asynchronous work starts from an explicit `Client` and returns a
standard future.

## Automated checks

`cmake/public_api.cmake` is the reviewed installed-header inventory, not an assertion that every
declaration is stable. Configuration fails if an installed header is added, removed, or renamed
without updating that baseline. When tests are enabled, every installed header is compiled as the
first and only SQLConduit include under C++17.
`sqlconduit_public_api_contract_test` locks the key `Client` signatures,
move-only lifecycle, version macros, runtime statistic types, and every `ErrorCode` numeric value.
The install consumer test validates the exported CMake package. These checks prevent accidental
surface changes, transitive-include dependencies, and private-header installation.

---

# SQLConduit 公共 API 稳定性约定

本文定义 SQLConduit 1.x 的兼容边界。约定以文档明确列出的安装包入口为准；某个头文件因
类型完整性而被安装，并不代表其中所有声明都是稳定公共 API。

## 稳定性分层

- **应用 API**：`api.h`、`public.h`、唯一高层入口 `Client`、公共数据类型、配置、观测、
  future 异步结果、mapping、util、结构化 SQL Builder、Prometheus 指标和版本信息。整个
  1.x 保持源码兼容。
- **扩展 API**：驱动、连接、拦截器、限流器接口，以及 `Client` 签名中公开的
  `DataSource`、`Session`、`Cursor`、选项、查询缓存、SQL 审计和统计类型。
- **内部实现**：`RuntimeServices`、`StatsReporter` 和 `detail` 命名空间。内部头不会安装，
  不承诺兼容。`logger.h`、`heartbeat_manager.h`、`write_buffer.h` 等支持头即使随包安装，
  也不是建议直接依赖的稳定入口。

1.x 的次版本和补丁版本不得破坏上述稳定 API 的源码兼容；弃用接口至少保留到当前次版本
结束，移除或不兼容语义变更必须进入 2.0 并在 CHANGELOG 给出迁移方式。JSON/YAML 配置键、
校验行为、指标名称和受限标签名遵守同一规则；允许在次版本增加可选项和新指标。
`ErrorCode` 的数值属于持久兼容契约，不再重排。该承诺是源码兼容，不承诺跨编译器、标准库
或运行库模式的 C++ 二进制 ABI。

所有公共枚举均使用显式数值，已有数值不得重排或复用。弃用接口统一使用
`sqlconduit/api.h` 中的 `SQLCONDUIT_DEPRECATED(message)`。

## 生命周期、并发与回调契约

- `Client` 初始为空；`init()` 成功后进入运行态；`shutdown()` 后永久关闭。初始化失败仍可
  重试，初始化成功后应使用 `reload()` 更新配置。
- 运行态客户端支持并发查询、写入、会话、统计、observer 和 interceptor 操作；移动
  `Client` 对象本身必须由调用方同步。
- `reload()` 原子发布新拓扑。已经解析到 `DataSource` 的调用可在 grace period 内继续使用
  旧拓扑，后续调用使用新拓扑。
- `shutdown()` 先让新请求返回 `ClientClosed`，排空本实例已提交的 future 任务，再关闭连接
  池。此前取得的 `DataSource` C++ 对象仍然有效，但池关闭后数据库操作返回关闭或未连接状态。
- 同步 observer/interceptor 在调用线程执行；`Client::*Async` 的 observer、interceptor、
  行回调和事务回调在本实例工作线程执行；统计上报和池采样可能使用后台线程。用户回调必须
  自身线程安全，异常不会穿透 SQLConduit 边界。
- 回调内部不得销毁、移动、`reload()` 或 `shutdown()` 同一个客户端；生命周期修改应安排在
  回调返回后执行。

`cmake/public_api.cmake` 是受审查的安装头清单，不等于清单内所有声明都稳定；未同步更新
基线和 CHANGELOG 的增删改名会在 CMake 配置阶段失败。测试构建会逐个独立编译安装头，并通过
`sqlconduit_public_api_contract_test` 锁定关键 `Client` 签名、move-only 生命周期、版本宏、
统计返回类型和全部 `ErrorCode` 数值。

返回 future 的 `Client::*Async` 方法使用本实例拥有的执行器、数据源拓扑、缓存、审计、
拦截器和观测状态；关闭一个实例只排空自己的执行器。异步结果会标明本次操作使用驱动
native 异步路径还是 SQLConduit 兼容 fallback，`asyncStats()` 也会分别统计 native、
fallback 和 timeout 数。这组接口目前有意不提供取消句柄或协程包装；兼容 fallback 会用
`async.statement_timeout_ms` 对超时读请求做 late classification，但不会在写、批量或事务已经
成功完成后把结果重写成 timeout。PostgreSQL 叶子数据源的普通查询/执行已经使用共享 libpq
socket reactor；MySQL 8.0.16+ 的无参数单语句 SELECT 和 DML/DDL 使用官方 nonblocking C API
与共享轮询 reactor。带参数 MySQL、旧 MySQL/MariaDB、两者的事务/游标/批量和复杂拓扑路径，
以及 Oracle、ODBC 仍走受支持且有明确统计的兼容 fallback。0.7 冻结前已移除进程级 `SQLConduit`、异步自由函数、
取消句柄和协程包装；所有异步操作都从显式 `Client` 发起并返回标准 future。
