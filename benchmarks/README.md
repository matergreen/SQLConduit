# SQLConduit microbenchmarks

The benchmark executable covers stable library-level paths without requiring a live database:

- pooled connection borrow and return;
- connection borrow and return under eight-thread contention;
- positional parameter binding through the core fallback binder;
- structured SQL Builder construction, identifier quoting, and parameter collection;
- result-row to entity mapping;
- a 32-row parameter batch;
- a 64-row cursor fetch;
- `emitSql` while SQL logging, slow-SQL aggregation, and observers are disabled;
- query-cache disabled, hit, miss, and replacement paths.
- per-operation p95/p99 for pool borrowing under eight-thread contention;
- contended cache hits and copies of a 1024-row result with 1 KiB payloads.

Build and run an optimized binary:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DSQLCONDUIT_BUILD_BENCHMARKS=ON
cmake --build build-bench -j
./build-bench/benchmarks/sqlconduit_benchmark \
  --iterations 100000 --json benchmark-results.json
```

Results report the median of five measured repetitions as nanoseconds per operation and operations
per second. Batch and cursor results treat one complete batch/fetch as one operation and include the
row count in the benchmark name.

Microbenchmark numbers are meaningful only when compiler, build type, CPU model, power policy, and
runner load are comparable. The 1.1 CI gate builds v1.0.1 and the candidate with the same compiler on
the same runner, installs each Core package separately, builds the identical current
`benchmarks/consumer` harness against each package, then alternates their execution order across
seven pairs. This avoids comparing different benchmark programs and their compiler-layout effects.

The disabled-logging workload is compiled in its own translation unit, with the same event
construction, move and destruction included in both measurements. Unrelated new cases therefore
cannot change its inlining/register allocation inside the benchmark's large `main()` function.

```sh
python3 benchmarks/compare.py --baseline /path/to/baseline-harness/sqlconduit_benchmark \
  --candidate /path/to/candidate-harness/sqlconduit_benchmark \
  --baseline-ref v1.0.1 --candidate-ref working-tree --runs 7 --iterations 100000
```

JSON and Markdown reports contain raw run samples, medians, MAD and p95 of whole-run values.
Whole-run p95 is a variance measure, not individual-operation tail latency. A median regression above
10% must repeat in at least 80% of pairs to fail (exit 1). Relative MAD above 10% is inconclusive and
also fails (exit 2), requiring a quieter rerun or investigation. Missing baseline cases and malformed
results fail. New cases are allowed without inventing historical data. CI keeps reports for 90 days.

Only `pool_borrow_latency_contended_8t` reports individual-operation `operation_p95_ns` and
`operation_p99_ns`; these include steady-clock measurement overhead. Its median is sampled across
individual operations. The large-result case includes allocation/copy/destruction costs. New cases
cannot be compared using historical binaries that lack those cases.

The CI shared harness can also measure a new case against an older library when the old public API
supports that case. These are fresh same-host measurements, not invented historical artifacts.
To reproduce, install both library versions to separate prefixes and configure `benchmarks/consumer`
twice with the corresponding `CMAKE_PREFIX_PATH`, using Release and the same compiler for both.
