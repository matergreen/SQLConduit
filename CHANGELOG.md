# Changelog

This file contains the user-facing highlights for each SQLConduit release. A matching version section is
required before pushing a `v*` tag; the release workflow uses that section as the GitHub Release
description.

## [Unreleased]

## [0.8.0]

### Performance

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
