# Changelog

This file contains the user-facing highlights for each dbmw release. A matching version section is
required before pushing a `v*` tag; the release workflow uses that section as the GitHub Release
description.

## [0.6.0]

### Highlights

- Added the Oracle driver (OCI): `OCILogon2` connections, `?` to `:n` placeholder rewriting,
  SQLT-based type mapping, native parameter binding, LOB reads and writes, transactions and
  savepoints, statement caching, and generated-key back-fill via `RETURNING ... INTO`. Enabled with
  `DBMW_ENABLE_ORACLE=ON`; requires the Oracle Instant Client.
- Added `Dialect::Oracle` to the dialect machinery — identifier quoting, routine and index helpers,
  and call plans — so the entity mapping layer emits Oracle-compatible SQL.

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
