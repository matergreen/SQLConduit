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
