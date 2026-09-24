# Contributing to SQLConduit

Thank you for helping improve SQLConduit. Bug reports, focused design discussions, documentation,
tests, and code changes are welcome.

## Before opening a change

- Search existing issues and pull requests.
- For a public API, configuration, metric, packaging, or behavioral contract change, open a design
  discussion first. SQLConduit freezes these surfaces deliberately.
- Report security issues privately as described in [SECURITY.md](SECURITY.md).

## Build and test

The dependency-free core build is the minimum local check:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSQLCONDUIT_BUILD_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Enable only the driver components whose client SDKs are installed. Integration tests require live
databases and are enabled separately with `SQLCONDUIT_BUILD_INTEGRATION_TESTS=ON`; never commit
credentials or private connection strings.

Performance-sensitive changes should also run the suite in [benchmarks/README.md](benchmarks/README.md)
on the same machine before and after the change. Include the JSON results and environment details
in the pull request when the difference is material.

## Change requirements

- Keep C++ code portable across GCC, Clang, and MSVC and within the target C++ standard.
- Add focused tests for fixes and new behavior. Avoid timing-only assertions in correctness tests.
- Update both Chinese and English user documentation when user-visible behavior changes.
- Update `CHANGELOG.md` for public API, configuration, metrics, packaging, security, and migration
  changes.
- Public headers must remain synchronized with `cmake/public_api.cmake`.
- Prometheus changes must follow [the metrics contract](docs/metrics.md). Never add SQL, trace IDs,
  tenant IDs, user IDs, or error text as labels.
- Configuration changes must update the JSON Schema, JSON/YAML examples, runtime validation, and
  schema tests together.

## Pull requests

Keep commits and pull requests scoped. Describe the problem, the compatibility impact, validation
performed, and any remaining risk. CI must pass on all supported platforms. Maintainers may ask for
changes to preserve API, configuration, metric, or package compatibility.

By contributing, you agree that your contribution is licensed under the repository's Apache-2.0
license.
