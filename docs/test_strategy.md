# Test strategy and coverage

> [中文版](test_strategy_zh.md)

This document describes what SQLConduit tests prove, where each class of test runs, and which
risks are not yet fully automated. A large assertion count is not treated as complete coverage.
The matrix is reviewed for every `1.x` release candidate together with
[feature coverage](feature_coverage.md) and [known limitations](known_limitations.md).

## Release gates

| Risk | Automated evidence | Release gate |
| --- | --- | --- |
| Public API and configuration contracts | Standalone-header compilation, public API assertions, JSON/YAML schema and strict error-path tests | Linux, macOS, and Windows builds |
| Core behavior | 22 non-live CTest executables covering mapping, SQL Builder, pools, sessions, transactions, retry, routing, async, observability, plugins, cache, and lifecycle | All supported CI build platforms |
| Real driver behavior | Live MySQL, PostgreSQL, and SQL Server/ODBC suites; Oracle OCI suite on an equipped environment | MySQL/PostgreSQL and FreeTDS SQL Server are hosted-CI gates; Oracle is a documented manual release gate |
| Undefined behavior and leaks | All non-live tests under ASan, LeakSanitizer, and UBSan | Hosted Linux CI |
| Data races and lock correctness | Cache and pool/executor contention tests under ThreadSanitizer | Hosted Linux CI |
| Performance | Twelve optimized microbenchmarks archived as JSON | Executed on Linux; results are compared on equivalent hardware rather than against a hosted-runner absolute threshold |
| Source coverage visibility | GCC/gcov report for core and public-header code, published as XML and browsable HTML | Report generation must succeed; a numeric threshold will be set after a stable baseline is collected |

## Targeted cache validation

The result cache has direct tests for disabled-mode short circuiting, TTL expiry, hit/miss
counters, LRU ordering, entry and approximate-memory bounds, oversized-entry rejection,
replacement, copy isolation, exact datasource invalidation, reconfiguration, and concurrent
get/put/invalidate operations. Integration tests additionally prove write invalidation and that
redacted or shadow results cannot cross the cache boundary.

Prepared-statement caches are exercised by every live driver suite, including reuse and bounded
eviction where the vendor API exposes observable behavior. Their native handles are also covered
by live-suite shutdown paths; sanitizer coverage currently applies to the driver-independent
ownership layer. Vendor-side cache memory is not included in the result-cache byte estimate.

## Performance and contention

The microbenchmark covers connection borrow/return, eight-thread pool contention, parameter
binding, SQL construction, row mapping, batch execution, cursor fetch, disabled logging, and
result-cache disabled/hit/miss/replacement paths. CI archives the median of five repetitions.

Performance changes must be compared on the same idle machine, compiler, optimization level, and
power policy. A repeatable regression above 10% requires investigation. Hosted runner timings are
diagnostic because CPU allocation and contention are not stable enough for a hard nanosecond gate.

## Memory and resource lifetime

LeakSanitizer runs the complete non-live suite with a non-zero leak exit code. Dedicated lifecycle
tests also verify balanced connection creation/close counts, repeated pool teardown, draining of
accepted executor work, and release of captures held by cancelled delayed tasks. Live integration
tests verify cursor/session return-to-pool behavior.

This detects unreachable allocations, use-after-free, double-free, and common resource-lifetime
regressions. It does not yet impose a peak-RSS or long-running heap-growth budget.

## Known coverage gaps

The following gaps are explicit and must not be described as fully validated:

- Oracle live tests require an OCI-equipped environment and are not run by GitHub-hosted CI.
- Hosted SQL Server integration uses FreeTDS. Microsoft ODBC Driver live execution is not part of
  the 1.0.0 validated matrix; Windows CI provides compile/test validation of the ODBC path only.
- The automated version matrix does not yet cover every supported database minor version or every
  client-library version.
- Microbenchmarks use deterministic in-process drivers. End-to-end database latency, throughput,
  server plan-cache behavior, and network backpressure require a controlled external benchmark.
- There is no scheduled multi-hour soak, process-RSS growth gate, network partition proxy, or
  database failover chaos suite yet.
- ODBC and Oracle native asynchronous state machines remain outside 1.0; their compatibility
  fallback is tested, but cannot stand in for future native-async tests.

## Adding or changing a feature

Every implemented behavior needs: a deterministic unit or contract test; a failure-path test; a
live driver case when vendor behavior is involved; and an appropriate sanitizer or benchmark case
when it changes ownership, concurrency, caching, or a hot path. A release note cannot promote a
feature to “validated” while its applicable row in this document still has an unaddressed gap.
