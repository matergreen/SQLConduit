# SQLConduit public API stability

This document defines the compatibility boundary used while SQLConduit moves from the 0.x series
toward 1.0. It applies to installed packages; files that exist only in the source tree are not
public API.

## Compatibility levels

### Stable application API

These are the preferred entry points for application code:

- `sqlconduit/client.h`: the sole high-level runtime entry, an independently owned, move-only
  `Client`;
- `sqlconduit/sqlconduit.h`: the convenience umbrella header;
- `sqlconduit/common/types.h`, `common/context.h`, and `common/observer.h`;
- `sqlconduit/config/datasource_config.h` and `config/config_loader.h`;
- `sqlconduit/async/async_types.h`: result and executor-statistics types returned by `Client`;
- `sqlconduit/mapping.h`, `sqlconduit/util.h`, and `sqlconduit/version.h`.

Existing names, overloads, enum numeric values, and documented behavior in this group will not be
changed incompatibly within a minor release line. New overloads and fields may be added when old
source continues to compile.

### Stable extension API

Applications implementing drivers, interceptors, or session callbacks may depend on:

- `driver/idriver.h`, `driver/driver_registry.h`, and `driver/driver_factory.h`;
- `core/idatabase_connection.h`, `core/interceptor.h`, and `core/rate_limiter.h`;
- `core/database_manager.h`, `core/cursor.h`, and `core/connection_pool.h` for the `DataSource`,
  `Session`, `Cursor`, options, and statistics types exposed by `Client`.

Concrete built-in driver classes are supported, but most applications should register or select
drivers through the factory instead of constructing them directly.

### Internal implementation

`RuntimeServices` and `StatsReporter` are implementation details. Their headers are available to
the library build but are deliberately excluded from installed packages. No compatibility promise
applies to source-tree-only headers or names in a `detail` namespace.

## 0.x policy

- Patch releases in the same `0.minor` line preserve source compatibility. CMake package matching
  therefore uses `SameMinorVersion`.
- A necessary breaking change before 1.0 is made only in a new minor release and is recorded in the
  changelog with a migration path.
- Before 1.0, a new minor release may deliberately remove an obsolete preview API without a
  deprecation cycle. The changelog must identify the break and give a migration path.
- `ErrorCode` numeric values are part of the compatibility contract and are never renumbered.
- Public enums use explicit numeric values. Existing values are never reordered or reused.
- Deprecations use `SQLCONDUIT_DEPRECATED(message)` from `sqlconduit/api.h`.

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
only its executor. These methods intentionally expose no cancellation handle or per-operation
timeout yet; they do honor the client's configured `async.statement_timeout_ms` and report a
best-effort `QueryTimeout` after a driver call returns.

There is no process-wide async facade. Callback, cancellation-handle, and coroutine wrappers were
removed before the 0.7 API freeze; asynchronous work starts from an explicit `Client` and returns a
standard future.

## Automated checks

`cmake/public_api.cmake` is the reviewed installed-header inventory. Configuration fails if a public
header is added, removed, or renamed without updating that baseline. When tests are enabled, every
installed public header is compiled as the first and only SQLConduit include under C++17.
`sqlconduit_public_api_contract_test` locks the key `Client` signatures,
move-only lifecycle, version macros, runtime statistic types, and every `ErrorCode` numeric value.
The install consumer test validates the exported CMake package. These checks prevent accidental
surface changes, transitive-include dependencies, and private-header installation.

---

# SQLConduit 公共 API 稳定性约定

本文定义 SQLConduit 从 0.x 走向 1.0 期间的兼容边界。约定以安装包为准；仅存在于源码树
中的文件不属于公共 API。

## 稳定性分层

- **应用 API**：唯一高层入口 `Client`、公共数据类型、配置、观测、future 异步结果、
  mapping、util 和版本信息。相同 0.x 次版本内保持源码兼容。
- **扩展 API**：驱动、连接、拦截器、限流器接口，以及 `Client` 签名中公开的
  `DataSource`、`Session`、`Cursor`、选项和统计类型。
- **内部实现**：`RuntimeServices`、`StatsReporter` 和 `detail` 命名空间。内部头不会安装，
  不承诺兼容。

补丁版本不得破坏同一次版本的源码兼容；1.0 前确需破坏的改动只能进入新的次版本，并在
CHANGELOG 中给出迁移方式。1.0 前的预览 API 可以在新次版本中直接移除，不要求弃用期；
`ErrorCode` 的数值属于持久兼容契约，不再重排。

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

`cmake/public_api.cmake` 是受审查的安装头清单；未同步更新基线和 CHANGELOG 的增删改名会
在 CMake 配置阶段失败。测试构建会逐个独立编译公共头，并通过
`sqlconduit_public_api_contract_test` 锁定关键 `Client` 签名、move-only 生命周期、版本宏、
统计返回类型和全部 `ErrorCode` 数值。

返回 future 的 `Client::*Async` 方法使用本实例拥有的执行器、数据源拓扑、缓存、审计、
拦截器和观测状态；关闭一个实例只排空自己的执行器。这组接口目前有意不提供取消句柄或
单次操作超时，但会遵守本实例配置的 `async.statement_timeout_ms`，并在驱动调用返回后以
best-effort 方式报告 `QueryTimeout`。0.7 冻结前已移除进程级 `SQLConduit`、异步自由函数、
取消句柄和协程包装；所有异步操作都从显式 `Client` 发起并返回标准 future。
