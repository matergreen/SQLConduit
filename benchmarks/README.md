# SQLConduit microbenchmarks

The benchmark executable covers seven stable library-level paths without requiring a live database:

- pooled connection borrow and return;
- positional parameter binding through the core fallback binder;
- structured SQL Builder construction, identifier quoting, and parameter collection;
- result-row to entity mapping;
- a 32-row parameter batch;
- a 64-row cursor fetch;
- `emitSql` while SQL logging, slow-SQL aggregation, and observers are disabled.

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
runner load are comparable. CI archives a JSON result as a historical signal but does not enforce a
global threshold because hosted-runner hardware varies. For a release comparison, run the old and
new commits on the same idle machine, use at least 100,000 iterations, and investigate a repeatable
regression above 10%.
