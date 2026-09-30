# Feature coverage and evolution plan

> [中文版](feature_coverage_zh.md)

This is the current capability map for SQLConduit 1.0. It answers three separate questions:

1. Is the feature implemented?
2. How broadly has the implementation been validated?
3. Is further work committed, being evaluated, or intentionally outside the project?

The snapshot date is **2026-09-30**. Release notes remain authoritative for a particular version;
this document describes the direction of the supported `1.x` line and is reviewed at each release.

## Status vocabulary

| Status | Meaning |
| --- | --- |
| **Implemented / validated** | Available through documented API and covered by unit/contract tests plus applicable live integration tests |
| **Implemented / expanding validation** | Usable now, but the cross-version, failure, load, or platform matrix is still being broadened |
| **In progress** | Active test, hardening, or implementation work; not yet promoted to the next status |
| **Scheduled** | Accepted for a future `1.x` iteration; exact release may move until assigned to a milestone |
| **Evaluating** | Design or vendor feasibility is being investigated; not a delivery promise |
| **Out of scope** | Intentionally not provided by SQLConduit |

Compatibility fallback is an implemented mode, not a synonym for unfinished behavior. An operation
reports `Native` only when the vendor driver genuinely starts it without occupying a compatibility
worker; all other future operations report `CompatibilityFallback` and remain bounded and
observable.

## 1.0 feature coverage

### Runtime and data access

| Area | Status | Current coverage / boundary |
| --- | --- | --- |
| Instance-owned `Client` lifecycle | **Implemented / validated** | Driver registration, init/reload, move-only ownership, graceful shutdown, independent runtime state |
| Synchronous query and execute | **Implemented / validated** | Default or named datasource, native parameter binding, structured status/error codes |
| Sessions and transactions | **Implemented / validated** | Pinned connection, transaction options, commit/rollback, savepoints, exception-safe rollback |
| Connection pools | **Implemented / validated** | Min/max sizing, timeouts, validation, idle/lifetime eviction, leak warnings, synchronous and async waiters |
| Prepared statements | **Implemented / validated** | Transparent per-connection LRU cache and explicit session handles |
| Batch execution | **Implemented / validated** | Atomic contract, exact per-item results, vendor optimization with safe transactional fallback |
| Cursors | **Implemented / validated** | PostgreSQL server cursor, MySQL unbuffered fetch, ODBC cursor, Oracle forward-only OCI cursor |
| Row callbacks (`queryEach`) | **Implemented / validated** | Early stop and row accounting; behavior is driver-specific but contract is shared |
| Generated keys | **Implemented / expanding validation** | Shared result contract; SQL Server callers still provide explicit `OUTPUT` SQL |
| Large values and LOB input | **Implemented / expanding validation** | Text/blob streaming parameter surface and driver LOB paths; `StreamSource` is currently buffered before driver consumption |
| Multiple result sets and routine calls | **Implemented / expanding validation** | Shared `queryAll`/`call` contract; exact OUT/INOUT and result-set capability depends on the vendor |

### Configuration, SQL, and mapping

| Area | Status | Current coverage / boundary |
| --- | --- | --- |
| JSON and YAML configuration | **Implemented / validated** | One versioned JSON Schema, strict unknown/type/range checks, cross-reference checks, environment-backed passwords |
| Error contract | **Implemented / validated** | Stable `ErrorCode` values, SQLSTATE/driver context where available, config errors include paths and categories |
| Portable SQL Builder | **Implemented / validated** | Safe portable `SELECT`/`INSERT`/`UPDATE`/`DELETE`, identifier quoting, ordered bound parameters, full-table guard |
| Arbitrary SQL and dialect features | **Implemented / validated** | Caller-provided SQL remains the escape hatch and primary model for joins, locking, upsert, expressions, and routines |
| Entity mapping | **Implemented / validated** | Hand-declared row mappings, read/write helpers, strict conversions, nullable columns through `std::optional<T>` |
| Extended value types | **Implemented / expanding validation** | Decimal/date/time/UUID/JSON/blob plus PostgreSQL and Oracle-specific typed values |
| Script/routine/index utilities | **Implemented / expanding validation** | Lifecycle SQL helpers and script execution while routine bodies remain caller-owned dialect SQL |

### Resilience, routing, and governance

| Area | Status | Current coverage / boundary |
| --- | --- | --- |
| Retry and idempotency | **Implemented / validated** | Retryable-status filtering, backoff/jitter, explicit idempotency declaration, writes opt in |
| Circuit breaker and rate limiting | **Implemented / validated** | Built-in policies and custom rate-limiter extension point |
| Read/write groups and read-after-write | **Implemented / validated** | Weighted replicas, fallback to primary, session-pinned consistency window |
| Primary failover and write buffering | **Implemented / expanding validation** | Explicit fencing/data-loss acknowledgements; never presented as exactly-once delivery |
| Dynamic datasources and groups | **Implemented / validated** | Runtime add/remove with grace periods and topology publication |
| Shadow routing | **Implemented / expanding validation** | Explicit traffic tags, isolation from primary results, observable outcomes |
| SQL audit and interceptors | **Implemented / validated** | Policy gates, before/after hooks, exception containment, structured execution view |
| Query cache and result redaction | **Implemented / validated** | Per-client cache, invalidation boundaries, raw-cache/transformed-result guard against cross-user leaks |

### Async, observability, packaging, and engineering

| Area | Status | Current coverage / boundary |
| --- | --- | --- |
| Future-based async API | **Implemented / validated** | Query, execute, generated keys, row callback, batch, multi-result, and transaction futures with bounded backpressure |
| Async pool handoff and deadlines | **Implemented / validated** | Pool wait does not occupy a worker; active waiter/read deadlines and connection quarantine are covered |
| PostgreSQL native async | **Implemented / validated** | Eligible leaf query/execute through a shared libpq socket reactor |
| MySQL native async | **Implemented / expanding validation** | Eligible parameter-free single-statement operations on supported MySQL clients; prepared operations fall back |
| ODBC and Oracle native async | **Implemented / expanding validation** | OCI nonblocking and Microsoft ODBC Driver 18 polling are live-tested; FreeTDS capability detection selects fallback |
| Logs, observers, slow SQL, and pool stats | **Implemented / validated** | Per-client observers, fingerprinting, sampling/redaction, bounded aggregation |
| Prometheus exporter | **Implemented / validated** | Stable metric/type/label contract and hard fingerprint-series limit; HTTP serving stays application-owned |
| Performance benchmarks | **Implemented / expanding validation** | Connection borrow/return, binding, mapping, batch, cursor, and disabled-logging baselines; historical regression automation is scheduled |
| Component packaging | **Implemented / validated** | Separate Core/MySQL/PostgreSQL/ODBC/Oracle static targets; installed CMake and pkg-config consumption tests |
| Release integrity | **Implemented / validated** | SHA-256, SPDX SBOM, build provenance, pinned important Actions, release checklist |

## Driver validation matrix

| Capability | MySQL | PostgreSQL | ODBC / SQL Server | Oracle OCI |
| --- | --- | --- | --- | --- |
| Parameter binding and type conversion | Validated | Validated | Validated, including Unicode paths | Validated, including scalar/LOB paths |
| Transactions and savepoints | Validated | Validated | Validated | Validated |
| Prepared reuse | Validated | Validated | Validated | Validated |
| Atomic batch contract | Native chunking + fallback | Pipeline + fallback | Parameter arrays + fallback | Array DML + fallback |
| Cursor | Unbuffered | Server-side | Native ODBC | Forward-only OCI |
| Native async query/execute | Partial eligible paths | Eligible leaf paths | Runtime-gated polling; FreeTDS fallback | Eligible OCI nonblocking paths |
| Future API fallback | Validated | Validated | Validated | Validated |
| Live release validation | Automated | Automated | Automated on Linux with FreeTDS and Microsoft ODBC Driver 18; Windows compile/test | Manual OCI-equipped runner |

The 1.0.0 release validation completed MySQL 8.4.11 (181 checks), PostgreSQL 18.6 (321 checks), SQL
Server 2022 through unixODBC/FreeTDS (79 checks), and Oracle Database Free with OCI 23.26.3
(85 checks). Check counts are a snapshot, not a compatibility contract. See
[Support](../SUPPORT.md) for the platform/client matrix and
[Known limitations](known_limitations.md) for exact exclusions.

The 1.0.1 development validation additionally passes SQL Server 2022 through FreeTDS fallback
(116 checks), Microsoft ODBC Driver 18 native polling (123 checks), and Oracle Database Free through
OCI native/fallback paths (119 checks). These suites cover concurrent operations, deadlines,
cancellation, post-cancel connection reuse, multibyte text, and large LOB/binary values.

## Work currently in extended validation

These capabilities are implemented. The work is test expansion and hardening, not a new public API:

- network interruption, cancellation failure, reconnection, pool exhaustion, and shutdown races;
- long-running concurrency, repeated reload, cursor abandonment, and prepared-cache eviction;
- large/multibyte/binary parameters and batch failure position across all four drivers;
- MySQL native async across Oracle MySQL and compatible MariaDB client-library versions;
- ODBC Unicode, parameter arrays, and cancellation across FreeTDS and Microsoft ODBC Driver;
- Oracle LOB, implicit-result, array-DML fallback, and supported OCI client-version combinations;
- real-database smoke coverage on macOS and Windows in addition to compile/unit coverage;
- benchmark history, variance control, and release-to-release regression thresholds.

## Planned `1.x` evolution

Priorities are ordered; version assignment happens only when work enters a release milestone.

| Workstream | Status | Planning horizon | Frozen-entry strategy |
| --- | --- | --- | --- |
| Failure injection and four-driver test expansion | **In progress** | 1.0.x hardening | Tests and fixes behind existing contracts |
| Dependency/install diagnostics and config tooling | **Scheduled** | Next 1.x minor | Tooling and additive CMake/config metadata |
| Historical performance regression gate | **Scheduled** | Next 1.x minor | Benchmark/CI change only |
| Allocation, pool contention, batch, and cursor optimization | **Scheduled** | Incremental throughout 1.x | Existing methods and result semantics |
| Incremental `StreamSource` consumption | **Scheduled** | Later 1.x | Reuse current type and overloads |
| Native ODBC and Oracle async baseline | **Implemented / validated** | 1.0.1 | Same future API; capability-gated mode selection, cancellation, concurrency, large values, and connection reuse |
| SQL Builder dialect extensions | **Scheduled** | Later 1.x | Additive extension types; portable builder remains valid |
| Optional coroutine/tracing adapters | **Evaluating** | Unassigned | Separate optional headers over futures/observer events |

### Priority A — reliability and adoption

- Broaden failure-injection and live-database integration scenarios, including upgrade/reconnect and
  transaction ambiguity tests.
- Improve dependency discovery and diagnostics; add documented package-manager consumption paths
  where maintainable without bundling vendor SDKs.
- Publish reproducible benchmark history and gate repeatable regressions above the documented
  threshold.
- Add configuration migration/check tooling around the existing versioned Schema and error model.
- Expand operational troubleshooting guidance and actionable vendor error context.

### Priority B — throughput and memory

- Reduce allocations in parameter conversion, row materialization, fingerprinting, and mapping.
- Extend vendor-native batch fast paths while keeping the same atomic `executeBatch` contract and
  transactional fallback.
- Introduce genuine incremental `StreamSource` consumption where vendor APIs permit it; the existing
  type and overloads remain the entry point.
- Improve cursor prefetch and backpressure without changing `Cursor` ownership or fetch semantics.
- Add cache/pool contention and tail-latency benchmarks, then optimize only against measured data.

### Priority C — async coverage

- Broaden the completed Microsoft ODBC Driver 18 baseline to additional supported driver/server
  versions, Windows live execution, failure injection, and multi-hour cancellation/concurrency soak;
  keep FreeTDS on capability-detected fallback.
- Broaden the completed Oracle OCI nonblocking baseline across supported client versions and
  failure/soak scenarios; retain fallback for temporary-LOB input binding.
- Move eligible parameterized MySQL and complex routing/retry/cache paths off compatibility workers
  when vendor capability allows it.
- Evaluate a coroutine adapter as an optional header layered over the existing futures; futures stay
  supported and unchanged.

### Priority D — SQL construction and observability

- Evolve SQL Builder through additive dialect extension objects for pagination, generated keys,
  upsert, and locking. The portable builder will not silently guess vendor dialects.
- Evaluate additive join/expression composition only if parameter order and identifier safety remain
  mechanically verifiable; raw SQL remains supported.
- Add optional tracing/export adapters (for example OpenTelemetry) around existing observer events;
  metric names and stable labels do not change.
- Improve diagnostics for fallback selection so production users can identify why a path was not
  native or optimized.

## Evaluating, not committed

- Custom executor injection and sender/receiver integrations;
- dynamically loaded binary driver plugins with a C ABI;
- additional database drivers;
- automatic schema migration planning.

SQLConduit does not plan to become a full ORM, infer relationships, provide lazy loading, translate
arbitrary SQL between dialects, embed an HTTP metrics server, or promise exactly-once writes.

## How evolution preserves the frozen API

The authoritative rules are in [Public API stability](api_stability.md). Roadmap work follows these
constraints:

- existing `Client`, `Session`, `Cursor`, `Builder`, result, and status signatures remain valid
  throughout 1.x;
- new behavior is exposed through additive overloads/types or optional configuration with defaults
  that preserve current behavior;
- driver SPI additions have default implementations and never turn an existing optional capability
  into a new pure-virtual requirement inside 1.x;
- performance work changes implementation behind existing contracts; native/fallback selection
  remains observable through `AsyncMode` and statistics;
- stable error-code numbers, configuration keys, metric names, types, and label names are not reused
  or incompatibly changed;
- deprecation precedes removal, and incompatible removal waits for 2.0;
- when a proposed optimization cannot preserve semantics, the project keeps the safe fallback or
  introduces an explicit opt-in instead of silently changing behavior.

Every release must update this document, the known-limitations document, and the release checklist
together when capability status changes.
