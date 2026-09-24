# Metrics compatibility contract

This document is the compatibility contract for the Prometheus text produced by
`sqlconduit::exporters::toPrometheusText`.

## Stability policy

- Starting with 0.8, a patch release will not rename a metric, change its type, remove a label,
  or change the meaning or unit of an existing series.
- Before 1.0, an incompatible metric change requires a new minor release and a migration note in
  `CHANGELOG.md`. Starting with 1.0, it requires a new major release.
- New metric families may be added in a backward-compatible release. A new label is treated as an
  incompatible change because it changes series identity.
- The caller-selected prefix is intentionally outside this promise. The contract below uses the
  default `sqlconduit` prefix.

## Stable metric families

| Metric | Type | Labels | Unit / meaning |
| --- | --- | --- | --- |
| `sqlconduit_pool_connections` | gauge | `data_source` | Current pool connections |
| `sqlconduit_pool_connections_max` | gauge | `data_source` | Configured maximum |
| `sqlconduit_pool_connections_min` | gauge | `data_source` | Configured minimum |
| `sqlconduit_pool_connections_idle` | gauge | `data_source` | Idle connections |
| `sqlconduit_pool_connections_borrowed` | gauge | `data_source` | Borrowed connections |
| `sqlconduit_pool_utilization_ratio` | gauge | `data_source` | Borrowed / maximum |
| `sqlconduit_pool_waiting` | gauge | `data_source` | Synchronous and asynchronous waiters |
| `sqlconduit_pool_borrow_requests_total` | counter | `data_source` | Borrow attempts |
| `sqlconduit_pool_borrow_successes_total` | counter | `data_source` | Successful borrows |
| `sqlconduit_pool_borrow_timeouts_total` | counter | `data_source` | Borrow timeouts |
| `sqlconduit_pool_connection_create_failures_total` | counter | `data_source` | Connection creation failures |
| `sqlconduit_pool_invalidated_connections_total` | counter | `data_source` | Invalidated connections |
| `sqlconduit_pool_validation_failures_total` | counter | `data_source` | Validation failures |
| `sqlconduit_pool_leak_warnings_total` | counter | `data_source` | Leak warnings |
| `sqlconduit_pool_idle_evictions_total` | counter | `data_source` | Idle-time evictions |
| `sqlconduit_pool_lifetime_evictions_total` | counter | `data_source` | Lifetime evictions |
| `sqlconduit_pool_connections_created_total` | counter | `data_source` | Connections created |
| `sqlconduit_pool_connections_closed_total` | counter | `data_source` | Connections closed |
| `sqlconduit_pool_borrow_wait_seconds_total` | counter | `data_source` | Cumulative borrow wait, seconds |
| `sqlconduit_pool_borrow_wait_seconds_max` | gauge | `data_source` | Maximum borrow wait, seconds |
| `sqlconduit_slow_sql_count` | counter | `data_source`, `fingerprint` | Slow statement count |
| `sqlconduit_slow_sql_errors` | counter | `data_source`, `fingerprint` | Slow statement errors |
| `sqlconduit_slow_sql_timeouts` | counter | `data_source`, `fingerprint` | Slow statement timeouts |
| `sqlconduit_slow_sql_duration_seconds` | histogram | `data_source`, `fingerprint`, `le` on buckets | Slow statement duration |
| `sqlconduit_slow_sql_duration_seconds_max` | gauge | `data_source`, `fingerprint` | Maximum duration, seconds |

The histogram also emits the standard `_sum`, `_count`, and `_bucket` samples. `le` is bounded by
the configured histogram bucket list and does not contain user data.

## Cardinality rules

- `data_source` is the configured datasource or group name. Do not create datasource names from
  tenant IDs, request IDs, user IDs, SQL text, or other unbounded input.
- `fingerprint` is the only deliberately high-cardinality label. Callers should pass a small
  `maxFingerprintLabels` value (50 is a reasonable starting point). SQLConduit enforces the public
  hard limit `kPrometheusFingerprintSeriesHardLimit` (1000), including when the argument is `0`.
- SQL text, rendered SQL, trace IDs, span IDs, tenant IDs, error messages, and driver messages are
  never exported as labels.
- Applications that add their own labels around this output own their cardinality and compatibility.

The exporter contract test fails when names, types, label layouts, or the fingerprint hard limit
change without an intentional test and documentation update.
