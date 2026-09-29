# Changelog

This file contains the user-facing highlights for each SQLConduit release. A matching version section is
required before pushing a `v*` tag; the release workflow uses that section as the GitHub Release
description.

## [Unreleased]

## [1.0.0]

### First stable release

- Froze the documented 1.x public source API, configuration Schema, error-code values, and stable
  metrics contract after the 1.0 release-candidate validation cycle.
- Published componentized Core, MySQL, PostgreSQL, ODBC, and Oracle targets so consumers only need
  the database client SDKs for drivers they select.
- Validated live MySQL, PostgreSQL, SQL Server through unixODBC/FreeTDS, and Oracle Database Free
  with OCI 23.26.3 paths. The Microsoft ODBC Driver path has Windows compile/test validation only
  in 1.0.0 and is not claimed as live-driver validated.
- Documented the implemented, expanding-validation, and scheduled capability boundaries for 1.x,
  including compatibility fallback for ODBC/Oracle async and the source-compatibility policy.
- Added platform archives, SHA-256 checksums, an SPDX SBOM, build/SBOM attestations, installed
  consumer smoke tests, sanitizer gates, core coverage reports, and release benchmarks.

## [1.0.0-rc.3]

### Validation and release quality

- Added targeted query-cache validation for disabled-mode short circuiting, TTL expiry, LRU and
  memory bounds, oversized entries, copy isolation, datasource-scoped invalidation,
  reconfiguration, and concurrent access.
- Added connection-pool and executor contention/lifecycle tests, including repeated teardown and
  release of captures owned by cancelled delayed tasks. These tests now run under a dedicated
  ThreadSanitizer release gate.
- Expanded the optimized benchmark suite with eight-thread pool contention and query-cache
  disabled, hit, miss, and replacement paths.
- Added browsable core coverage reports and explicit test-strategy documentation. Coverage report
  generation tolerates gcov's known negative branch-hit output while retaining failures for other
  parse errors.
- Fixed the mapping reload fixture when Linux tests are launched from a Windows parent whose
  temporary-directory environment contains a Windows-style path.

## [1.0.0-rc.2]

### Compatibility contract

- Declared the first stable public API and configuration contract. All `1.x` releases preserve
  source compatibility for documented application and extension APIs; incompatible changes require
  a new major version. Public enum and `ErrorCode` numeric values remain stable.
- Changed CMake package compatibility to `SameMajorVersion` and exported the C++17 and threads
  usage requirements so installed-package consumers receive the same build contract as in-tree
  targets.
- Documented supported platforms, database validation coverage, native/fallback asynchronous
  behavior, binary-package components, and known limitations for production adoption.
- Added an explicit feature-coverage and evolution matrix separating implemented, expanding
  validation, scheduled, evaluating, and out-of-scope work, with 1.x API-preservation rules for
  every planned optimization.

### Release quality

- Fixed Windows release ZIP creation to use portable forward-slash entry names, and made SBOM ZIP
  extraction normalize legacy backslash entries with traversal checks.
- Added warning-as-error ASan/UBSan validation, live MySQL/PostgreSQL/SQL Server integration jobs,
  and per-component installed-package consumer checks to the release gate. Oracle remains a
  recorded manual release-candidate validation because GitHub-hosted runners do not provide OCI.
- Removed credentials from checked-in configuration examples and aligned the JSON Schema identity,
  public version macros, consumer contract, and project version at `1.0.0`.
- Release archives include support, security, contribution, and known-limitations documents in
  addition to checksums, SPDX SBOM, and build provenance already produced by the release workflow.

### Included since 0.7

- Introduced the move-only `Client` entry point and removed the process-wide facade; split the
  monolithic archive into opt-in Core, MySQL, PostgreSQL, Oracle, and ODBC components.
- Added strict JSON/YAML configuration validation, portable SQL Builder CRUD construction,
  consistent NULL mapping, real batch execution with safe fallbacks, cursor/streaming APIs,
  observability contracts, and performance baselines.
- Added explicit asynchronous execution modes. PostgreSQL and eligible MySQL operations support
  native nonblocking execution; unsupported driver and topology paths use the documented bounded
  compatibility executor rather than implying native behavior.

## [0.9.0]

### Fixed

- Disabled ODBC parameter-array batching for FreeTDS, which advertises per-parameter-set row
  counts but returns only an aggregate result. FreeTDS now uses the transactional per-row fallback
  so `BatchResult::affected` remains exact and portable.



### Async execution

- Added the driver-native query/execute callback protocol to `IDatabaseConnection`. Drivers report
  `AsyncCapability::Native` only when they start work without blocking and complete the callback
  exactly once; otherwise SQLConduit keeps the explicit compatibility path.
- Routed eligible leaf-datasource future APIs through asynchronous pool borrowing. Pool exhaustion now
  parks requests in `asyncWaiting` without consuming a worker, hands a returned connection directly
  to the oldest waiter, and releases the handle normally after query, write, batch, stream, or
  transaction completion. Complex group, retry, cache, and shadow paths retain their existing
  compatibility execution semantics until their asynchronous state machines are migrated.
- Added active deadlines for asynchronous pool waiters. A waiter now completes with
  `PoolExhausted` even when no connection is returned and no heartbeat runs after it is queued.
- Preserved bounded-executor backpressure across the new bridge: rejected submissions return
  retryable `Overloaded` without executing database work on the caller thread, and rejected
  connection-creation tasks release their reserved pool capacity.
- Added regression coverage for native driver callbacks, native/fallback accounting, client-level
  pool handoff, active waiter timeout, and the 0.9.0 public version contract.
- Native query and execute completions now run the same route, before-execution,
  after-execution, completion, SQL-audit, rate-limit, and observability lifecycle as synchronous
  operations; enabling interceptors no longer forces a native-capable driver back to a worker.
- Native reads now enforce `async.statement_timeout_ms` with an active deadline: the future resolves
  once with `QueryTimeout`, driver cancellation is requested exactly once, and the borrowed
  connection remains quarantined until the driver's eventual completion callback makes it safe to
  return or discard. Writes retain the existing no-late-reclassification rule because commit state
  may be ambiguous.
- Added the first bundled native adapter for PostgreSQL. Eligible query and execute operations now
  use one shared libpq socket reactor instead of occupying executor workers; parameter binding,
  typed result conversion, concurrent connections, cancellation, and broken-connection reporting
  are covered by live PostgreSQL integration tests. Transaction, cursor, batch, cache, retry,
  group, and shadow paths intentionally remain on the compatibility state machine.
- Added a MySQL 8.0.16+ native adapter using the official nonblocking C API and a shared polling
  reactor. Single-statement, parameter-free SELECT and DML/DDL operations can run in `Native` mode;
  parameterized statements deliberately retain prepared-statement fallback because MySQL exposes
  no asynchronous prepared-statement API. Older MySQL/MariaDB clients, routines, multi-statements,
  transactions, cursors, and batches remain compatible fallbacks. Live MySQL 8.4 tests cover
  concurrent connections, execution-mode reporting, `KILL QUERY` cancellation, and error mapping.
- Migrated leaf-data-source query-cache handling into the asynchronous state machine. Cache hits
  now complete without borrowing a connection, while misses can continue into a driver-native
  operation and populate the cache before result-transforming interceptors run. Hit/miss execution
  modes and interceptor lifecycle are covered by client regression tests.

## [0.8.0]

### Async execution

- Made the async execution mode explicit on every future result. `mode=Native` is reserved for
  driver-native asynchronous paths; current compatibility execution reports
  `mode=CompatibilityFallback` instead of silently looking like native async.
- Added `nativeOperations`, `fallbackOperations`, and `timedOutOperations` to `asyncStats()` so
  applications can verify which async path they are exercising.
- Tightened compatibility fallback timeout semantics: late read fallback can still report
  `QueryTimeout`, but successful write, batch, and transaction fallback results are no longer
  rewritten to timeout after the database call has already completed.

### Performance

- Reworked batch execution so PostgreSQL sends bounded chunks through `pqxx::pipeline`, ODBC uses
  capability-gated parameter arrays, and MySQL sends safely escaped, bounded multi-statement
  chunks for eligible DML. Safe fallbacks preserve atomic rollback, per-parameter-set affected counts, and generated
  keys. All batch paths now reject inconsistent parameter-group shapes before executing the first
  row and clear partial results on failure.

- Bounded the prepared-statement cache to 128 entries per connection by default and changed all
  four driver LRU hit paths from linear list scans to constant-time iterator moves.
- Removed the per-row copy from `queryEach` when interceptors are disabled or absent, fixed cursor
  single-row fetches that accidentally copied from a const result, and avoided mapping every row
  before `queryOneAs` reports a multi-row contract violation.
- Raised the default idle-connection validation interval to 30 seconds and made heartbeat checks
  honor that interval instead of pinging every idle connection on every heartbeat.

### Structured SQL construction

- Added a deterministic, parameter-only CRUD builder for `SELECT`, `INSERT`, `UPDATE`, and
  `DELETE`, including structured predicates, ordering, identifier quoting, and explicit protection
  against accidental full-table updates/deletes.
- Kept the non-translation boundary: pagination, upsert, locks, joins, expressions, generated-key
  clauses, and other dialect features remain explicit application SQL. MySQL quoting requires an
  explicit dialect, and ODBC never guesses its backend dialect.
- Reused the builder from entity mapping INSERT/UPDATE generation and added unit, public-contract,
  integration-matrix, and microbenchmark coverage.

### Public API surface

- Added a curated re-export facade in `sqlconduit/public.h` (pulled in by the `sqlconduit.h`
  umbrella): the most-used public types and SQL-builder free functions are now reachable directly
  under `sqlconduit::` (e.g. `sqlconduit::Dialect`, `sqlconduit::Value`, `sqlconduit::Builder`,
  `sqlconduit::eq`). The deep namespace paths (`sqlconduit::common::util::Dialect`,
  `sqlconduit::core::Cursor`, ...) remain valid for backward compatibility.
- Extended the facade to cover the remaining 3-level public namespaces: routine/DDL options and
  results (`sqlconduit::RoutineRef`, `sqlconduit::ExecOptions`, `sqlconduit::CallOptions`,
  `sqlconduit::IndexSpec`, `sqlconduit::CallResult`, `sqlconduit::ScriptResult`,
  `sqlconduit::CallParams`), the SQL-analysis utilities (`sqlconduit::StatementKind`,
  `sqlconduit::classifyStatement`, `sqlconduit::hasWhereClause`, ...), and the routine/DDL free
  functions (`sqlconduit::call`, `sqlconduit::createRoutine`, `sqlconduit::dropRoutine`,
  `sqlconduit::createIndex`, `sqlconduit::runScripts`, `sqlconduit::quoteIdent`, ...).

### Configuration contract

- Added a distributable JSON Schema 2020-12 contract covering every configuration block, field,
  type, enum, range, default, and supported conditional constraint.
- Configuration loading now rejects unknown fields and invalid pool/reporting ranges instead of
  silently ignoring or normalizing them. Structural diagnostics include a category and JSON Pointer.
- Added editor association and CI validation for the canonical JSON and YAML examples, and
  documented the boundary between `ConfigError`, `UnknownDriver`, and `ConnectionFailed`.

### Performance and operational contracts

- Added dependency-free microbenchmarks for connection borrow/return, parameter binding, structured
  SQL construction, row mapping, batching, cursor fetches, and the disabled SQL logging fast path.
  CI archives a JSON baseline without applying unreliable hosted-runner thresholds.
- Published a compatibility contract for Prometheus metric names, types, and labels. Fingerprint
  series now have a hard limit of 1000; SQL text, trace IDs, tenant IDs, and errors remain forbidden
  as labels.

### Release engineering and governance

- Release assets now include SHA-256 checksums, an SPDX JSON SBOM, and GitHub/Sigstore build
  provenance plus SBOM attestation bundles.
- Updated JavaScript actions to Node.js 24 releases and pinned every third-party action by immutable
  commit SHA.
- Added security reporting, contribution, supported-version, and release-checklist documentation.

## [0.7.0]

### Public API foundation

- **Breaking (preview packaging/API):** split the monolithic static archive into
  `sqlconduit::core` plus independently linkable `mysql`, `postgres`, `odbc`, and `oracle`
  components. `Client::addDriver()` now registers a selected driver for one client before
  initialization; core-only consumers no longer need any database client SDK installed.
- Replaced installed concrete driver headers (which exposed vendor SDK types) with lightweight
  `sqlconduit/drivers/*.h` registration factories, and removed `registerBuiltinDrivers()`.
- Added component-aware CMake package discovery and per-component pkg-config files. A database
  client library is discovered only when its matching component is requested.
- Added a move-only, RAII `sqlconduit::Client` with configuration-file and programmatic
  initialization, explicit lifecycle errors, reload, synchronous statement/session/cursor APIs,
  pool statistics, and dynamic data-source/group management.
- **Breaking (preview API):** removed the process-wide static `SQLConduit` facade and the
  `sqlconduit::async` callback, cancellation-handle, and coroutine layers. Applications now create
  a `Client`; mapping and utility operations that need a runtime take `Client&`, and asynchronous
  operations use the future-returning `Client::*Async` methods.
- Made query-cache contents and policy, SQL-audit policy and counters, and prepared-statement-cache
  settings runtime-scoped. Separate `Client` instances can now use the same data-source names with
  different cache and audit configurations without affecting each other.
- Made interceptor enablement, registration and re-entrancy tracking runtime-scoped, and added
  `Client::addInterceptor()` / `clearInterceptors()`. Nested calls into a different client no longer
  suppress that client's interceptor chain, while recursion through the same chain remains guarded.
- Moved observers, pool metric collectors, and slow-SQL aggregation into each `Client` runtime.
  Added `Client::setObserver()`, `slowSqlStats()`, `recentSlowSql()`, and `clearSlowSqlStats()`.
- Added `sqlconduit/version.h`, a documented public-API stability policy, and a build check that
  compiles every installed header independently under C++17. Internal `RuntimeServices` and
  `StatsReporter` headers are no longer installed.
- Added future-based instance async operations to `Client`. Each client owns its executor and
  resolves asynchronous queries, writes, batches, streams, and transactions through its own
  data-source topology; shutting down one client does not stop another client's executor.
- Added a frozen public-header inventory, compile-time API contract tests, explicit values for all
  public enums, and the portable `SQLCONDUIT_DEPRECATED` marker. Runtime state classes now live in
  `detail`, and public `DataSource` / `DatabaseManager` construction no longer exposes runtime
  service ownership.
- Assigned explicit numeric values to `ErrorCode` and added `NotInitialized`,
  `AlreadyInitialized`, and `ClientClosed` for deterministic client lifecycle reporting.
- Changed pre-1.0 CMake package matching from `SameMajorVersion` to `SameMinorVersion`, preventing a
  future breaking 0.x minor from being selected as a compatible package automatically.

## [0.6.0]

### Highlights

- Added the Oracle driver (OCI): `OCILogon2` connections, `?` to `:n` placeholder rewriting,
  SQLT-based type mapping, native parameter binding, LOB reads and writes, transactions and
  savepoints, statement caching, and generated-key back-fill via `RETURNING ... INTO`. Enabled with
  `SQLCONDUIT_ENABLE_ORACLE=ON`; requires the Oracle Instant Client.
- Added `Dialect::Oracle` to the dialect machinery — identifier quoting, routine and index helpers,
  and call plans — so the entity mapping layer emits Oracle-compatible SQL.
- Added an explicit `datasources[].oracle` configuration block for service names or SIDs, client
  character sets, LOB limits and bind policy, wallets, and server certificate DNs. Legacy
  `database` and `extra.*` settings remain compatible, with deterministic connection-string
  precedence and validation for ambiguous or ineffective TLS combinations.
- Added forward-only OCI statement cursors and Oracle 12c+ implicit multi-result support through
  `OCIStmtGetNextResult`.
- Added native OCI array DML for compatible batches, including per-iteration affected-row counts.
  Generated-key, LOB, oversized-value, and older-client cases safely retain the transactional
  per-row path.
- Added safe Oracle routine deletion with `ifExists=true` by suppressing only ORA-04043 inside an
  anonymous PL/SQL block.
- Added a driver-neutral callable API with typed OUT/INOUT parameters and REF CURSOR result sets.
  Oracle procedures now support scalar output binds and REF CURSOR reads; named collection/object
  inputs carry explicit type names and expand to safely bound Oracle constructors.
- Added `IntervalYearMonth`, `IntervalDaySecond`, `TypedArray`, and `TypedComposite` to the public
  value model so interval and Oracle UDT semantics no longer depend on string guessing.

### Packaging and integration

- Made the installed CMake package relocatable. The driver client libraries are no longer written
  into the export set as absolute paths: a `sqlconduitDriverDeps.cmake` module now ships with the
  package and re-resolves them in the consumer's build environment, so an install prefix can be
  moved between machines and only needs the matching client development packages installed.
- Populated `Libs.private` in `sqlconduit.pc` with the enabled driver libraries and the platform
  thread flag, so `pkg-config --libs --static sqlconduit` produces a link line that actually
  resolves.
- Added `SQLCONDUIT_INSTALL` (default `ON`). Set it to `OFF` to consume SQLConduit through
  `FetchContent` or `add_subdirectory` without contributing install rules to the parent project.
- Demoted nlohmann/json to a purely build-time private dependency and stopped installing a copy of
  it, removing header conflicts with a system or sibling `nlohmann_json`.
- Added a consumer smoke test that installs the package into a scratch prefix and builds a minimal
  out-of-tree project through both `find_package` and, on Linux, `pkg-config`.

### Compatibility and reliability

- Mapped Oracle error codes to SQLSTATE so failures classify consistently with the other drivers:
  constraint violations, deadlocks, and broken connections now surface with the right `ErrorCode`
  and retry behavior.
- Bound large `Blob` parameters through temporary LOB locators instead of a raw byte bind, so values
  past the 4000-byte direct-bind limit no longer fail.
- Pinned the OCI client character set to AL32UTF8, removing the dependency on the `NLS_LANG`
  environment variable for text round-trips.
- Taught `require_limit_select` that `FETCH FIRST n ROWS ONLY` and `ROWNUM <= n` bound a result set,
  so Oracle paging is no longer rejected.
- Moved Oracle connection timeouts into the Oracle Net descriptor so connect and transport limits
  apply before `OCILogon2`; generated TCPS descriptors now carry wallet and server-DN verification
  semantics explicitly.
- Fixed prepared-statement cache eviction, mixed LOB-column indexing, locator cleanup, and UTF-8
  CLOB byte sizing, preventing invalid cache lookups, descriptor leaks, and multibyte truncation.
- Kept caller-owned transaction semantics for array DML failures while rolling back the complete
  batch when SQLConduit owns the transaction.
- Fixed Oracle OCI defects found by running the integration suite against a live database: input
  binds now pass the real `alenp` length (previously the driver bound empty strings, which Oracle
  treats as NULL), CLOB and BLOB reads pass the correct byte/char amounts, implicit result-set child
  handles are no longer released with `OCIStmtRelease`, REF CURSOR OUT parameters bind as
  `SQLT_RSET` with the canonical null indicator/length/return-code pointers, and un-scaled `NUMBER`
  columns — including identity columns, which describe as scale -127 — now map to integers instead
  of doubles.
- Added the missing `<cstdint>` and `<memory>` includes across the public headers so the library
  compiles from a clean tree with `SQLCONDUIT_ENABLE_ORACLE=ON`; several headers had been relying on
  transitive includes.

## [0.5.1]

### Highlights

- Added routine and index lifecycle helpers, multi-result calls, OUT/INOUT support where the driver
  can provide it, and ordered SQL script execution from text, file lists, or directories.
- Added callback, future, and coroutine forms for the utility APIs. Asynchronous scripts now expose
  cancellation and preserve the last error when configured to continue.
- Made rate limiting pluggable through `IRateLimiter`, with global defaults, per-source overrides,
  per-fingerprint-only limiting, and runtime updates for existing data sources and groups.
- Extended entity mapping with dialect-aware `insertAs`, `insertBatchAs`, and `updateAs`, including
  generated-key propagation.
- Added PostgreSQL array, composite, and geometric value support.
- Added YAML configuration support alongside JSON. Both formats use the same validation and runtime
  semantics.

### Compatibility and reliability

- Kept the core at C++17 while supporting the optional C++20 coroutine layer on GCC, Clang, and
  MSVC.
- Tightened failover, write-buffer, session lifecycle, SQL redaction, and idempotency behavior.
- Expanded mock and live-database coverage for PostgreSQL, MySQL, and ODBC/SQL Server.

## [0.5.0]

- Added bidirectional, header-only entity mapping for converting between rows and application
  structures while keeping SQL explicit.
