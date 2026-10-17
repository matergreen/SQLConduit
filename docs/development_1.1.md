# SQLConduit 1.1.0 implementation and acceptance

The 1.1.0 milestone adds production onboarding, diagnosis and performance tooling while preserving
the 1.x public source contract. This is a development checklist, not a release-validation claim.

## Deliverables

- [x] Offline JSON/YAML topology validation and redacted resolved configuration.
- [x] `sqlconduitctl validate`, `explain`, offline `doctor` and explicit connection probes.
- [x] Read-only Client capability/diagnostic reports, group participants, pool pressure, circuit,
      retries, write-buffer depth, cache and executor statistics; cached driver/client metadata.
- [x] Statement-specific fallback reasons and timing for pool waits, executor queueing, retries,
      replica fallback, cancellation and rejection in observer events.
- [x] Optional OpenTelemetry component and SDK-backed export tests; safe attributes and async parent
      context propagation. Core must not acquire an OpenTelemetry dependency.
- [x] Same-host historical benchmark regression gate and JSON/Markdown reports; noise must not pass.
- [x] Tail-latency and large-result benchmarks; optimize only measured allocation/contention costs.
- [x] Platform-aware dependency diagnostics, public installed-component metadata, vcpkg and Conan
      consumption paths, independent component installation and multi-config verification.
- [x] Failure injection for network loss/reset, database restart, ambiguous commit, cancellation
      isolation, concurrent reload, prolonged pool pressure and configurable soak runs.
- [x] Windows Microsoft ODBC Driver live execution gate and macOS MySQL/PostgreSQL smoke where
      services are available.
- [x] Complete local core/driver/sanitizer/installed-consumer verification, developer/API documentation,
      changelog and capability matrix updates with precise unvalidated boundaries.

## Compatibility and safety invariants

Existing Client/Session/Cursor APIs remain usable. New driver SPI hooks have default implementations.
Diagnostics never silently borrow a connection. Unobserved driver capabilities stay distinguishable
from connected capabilities, and group reports identify each participating leaf.

No raw SQL, parameters, connection strings, DSNs, credentials or vendor error messages are exported
by the diagnostic CLI or default tracing attributes. `doctor --connect` may connect and ping each
configured leaf but never executes application writes. Performance comparisons use the same runner,
compiler and build configuration; p95 across whole runs is not advertised as operation tail latency.

The release process (commit, push, merge, tag, publish) is a separate user-authorized step after the
milestone's implementation and verification.

## Local evidence and remaining release validation

On 2026-10-17, the macOS Release build passed 27 tests with the actual OpenTelemetry C++ 1.22.0 SDK.
The strict-warning ASan/UBSan build passed 26 tests, and the targeted ThreadSanitizer run passed
diagnostics, fault/reload, cache and concurrency tests. Linux Docker execution passed all 27 tests,
including MySQL 8.4, PostgreSQL 16 and SQL Server through FreeTDS. Retained-Client database restart
probes passed for MySQL and PostgreSQL, including PostgreSQL native async recovery. A short two-second
soak exercised repeated fault/pressure/reload scenarios; this is not a multi-hour endurance claim.

Core-only and OpenTelemetry installed consumers passed, including Core consumption with SDK package
discovery disabled. A real Conan Core package and consumer passed locally. Workflow static checks
passed (actionlint 1.7.7 ignores only its unknown `artifact-metadata` permission scope).
The final quiet seven-pair Release comparison against v1.0.1 passed all 12 historical cases.
The initial inline disabled-cache experiment improved local microbenchmarks but was subsequently
reverted after repeatable hosted GCC regressions (see below). New tail-latency and
large-result cases are retained in raw candidate-run artifacts without invented baseline values.

The new GitHub-hosted Windows LocalDB, macOS database-server, Conan, performance and OpenTelemetry
jobs still require a pushed revision and actual hosted execution. Oracle capability metadata was
implemented but not rebuilt or live-tested against an OCI SDK in this local run. Windows multi-config
co-installation is covered by the workflow, not claimed as locally executed. Multi-hour soak runs
remain an explicit manual gate. Checked deliverables above mean implementation exists; they do not
replace these release-validation boundaries.

## CI follow-up

GitHub build 77 executed the new hosted gates: macOS live databases, Conan, OpenTelemetry and
MSVC multi-config consumers passed. Windows LocalDB exposed an ODBC narrow-character Unicode
read defect, and the performance job exposed disabled-observability overhead. The follow-up patch
uses wide-character result decoding and an atomic disabled-event gate with concurrent-setter tests.
FreeTDS fixtures explicitly configure UTF-8 so narrow-input/output symmetry cannot hide incorrectly
stored text. Existing incorrectly encoded database values are not automatically repaired.

Performance comparisons now compile the same harness against separately installed packages, and
isolate the unchanged logging workload in a separate translation unit. Seven Linux ARM64/GCC pairs
passed all 15 cases locally; disabled logging measured 28.53 ns/op for v1.0.1 versus 22.19 ns/op for
the patched library. Original regression/noise thresholds remain unchanged. The patched Windows
LocalDB path and x86_64 GitHub performance gate still require actual hosted reruns.

## 1.1.0 release qualification

The patched development commit passed all mandatory hosted jobs, including Windows LocalDB and
the shared-harness performance gate. The final release-metadata commit must pass the gates again.
Oracle/OCI was not revalidated for 1.1.0. On 2026-10-18 the release owner explicitly approved
publication with this exception disclosed in the release notes; no new Oracle validation is claimed.

Two subsequent release-candidate runs reported roughly 13% disabled-cache regressions; one also
reported a cache-miss regression. The inline lookup experiment was removed, restoring the exact
v1.0.1 cache implementation instead of relaxing the performance threshold. All 28 local Release
tests and the targeted ASan/UBSan and TSan cache tests passed after this change. Hosted performance
qualification must still pass for the final commit before the main merge and release tag.

After restoring that implementation, its GCC object file was byte-for-byte identical to the
baseline, yet the monolithic benchmark reported a repeatable hit-path difference. Cache API calls
and literal conversions are now isolated in a shared translation unit, as already done for logging;
neither workload nor thresholds were removed or relaxed. Seven fresh Linux ARM64/GCC pairs passed
all 15 cases (disabled cache +0.4%, hit +3.2%, miss +5.4%, disabled logging -18.8%). These are local
microbenchmarks, not a claim of portable application throughput or hosted qualification.
