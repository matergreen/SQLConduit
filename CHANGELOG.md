# Changelog

This file contains the user-facing highlights for each SQLConduit release. A matching version section is
required before pushing a `v*` tag; the release workflow uses that section as the GitHub Release
description.

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
