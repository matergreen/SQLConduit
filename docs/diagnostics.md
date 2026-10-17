# Configuration and runtime diagnostics (1.1.0)

Build `sqlconduitctl` with `SQLCONDUIT_BUILD_TOOLS=ON` (default). It links only enabled driver
components and installs in `bin/`. Core-only builds can still validate and explain configurations.

```sh
sqlconduitctl validate datasource.yaml
sqlconduitctl explain datasource.json
sqlconduitctl doctor datasource.yaml
sqlconduitctl doctor datasource.yaml --connect
```

All commands write JSON. Exit 0 means success, 1 means a validation/diagnostic failure, and 2 means
invalid arguments. `validate` uses the runtime parser's strict field/type/range/environment checks
and offline topology validation. It does not require database SDKs. `explain` includes defaults,
group routing, timeout, retry, buffering and observability settings. Passwords, DSNs, private-key
paths, wallet paths and every vendor `extra` value are redacted. This report is for inspection and
must not be used as a replacement configuration file.

`doctor` checks the drivers linked into this executable and reports client capability metadata.
Only `--connect` opens and pings configured leaf connections. It never executes application writes.
Probes use each datasource's configured connection timeout; vendor calls determine cancellation and
timeout precision. Connection errors report structured codes and remediation advice, with vendor
text omitted because it can contain credentials. A custom driver requires an application-specific
tool that registers that driver; it is not automatically discovered from shared libraries.

The same offline helpers are available to applications:

```cpp
sqlconduit::config::GlobalConfig config;
std::string error;
if (!sqlconduit::config::ConfigLoader::loadFromFile("datasource.yaml", config, error))
    return 1;
if (!sqlconduit::config::ConfigLoader::validateTopology(config, error)) return 1;
std::cout << sqlconduit::config::ConfigLoader::explain(config);
```

The library parser returns diagnostic text to trusted callers; applications should redact it before
publishing it externally. The CLI deliberately withholds parser exception text that could quote raw
input values. A missing password environment variable is an error even for offline commands.

## Runtime snapshots

```cpp
auto health = client.diagnostics();
auto capabilities = client.capabilities("read_write_group");
if (!capabilities.status.ok()) return 1;
for (const auto &source : capabilities.sources) {
    if (!source.group && source.capabilities.observed) {
        std::cout << source.driver << ": " << source.capabilities.asyncConstraints;
    }
}
```

`diagnostics()` includes leaves and groups, pool pressure, current circuit state, consecutive
failures, the last observed operation and cancellation error codes, configured retry count, buffered-write depth,
cache counters and executor statistics. `configSource` is `file` or `programmatic`; actual paths and
credentials are not retained. `capabilities(name)` also includes every leaf participating in a
group's primary, replicas, failover and shadow routing. An unknown name or closed Client returns a
structured status. Diagnostics deliberately ignores the request's `targetDataSource` override.

These calls do not borrow connections or execute SQL. Driver metadata is cached at the last
successful connection creation. `observed=false` means no successful connection has yet supplied
metadata. A cached capability is not a current connectivity check, nor proof that every statement
will run natively. Pool, topology and executor snapshots can change independently while operations
or reload run. `nativeAsync` describes the observed connection's broad capability; inspect actual
Future `mode` and phase events for each operation. Empty version strings mean metadata is unavailable.

## Optional OpenTelemetry adapter

Install OpenTelemetry C++ and build with `-DSQLCONDUIT_ENABLE_OPENTELEMETRY=ON`. Consumers request the
separate component; Core alone has no OpenTelemetry dependency:

```cmake
find_package(sqlconduit REQUIRED COMPONENTS Core Postgres OpenTelemetry)
target_link_libraries(app PRIVATE sqlconduit::postgres sqlconduit::opentelemetry)
```

Configure the application's OpenTelemetry `TracerProvider` and exporters before installing the
observers. The application owns exporter batching, flushing and shutdown.

```cpp
#include "sqlconduit/exporters/opentelemetry.h"

client.setObserver(sqlconduit::exporters::openTelemetryObserver());
client.setExecutionPhaseObserver(sqlconduit::exporters::openTelemetryPhaseObserver());
{
    // Capture the active application span on this thread before async submission.
    const sqlconduit::common::ContextScope scope(
        sqlconduit::exporters::openTelemetryContext());
    auto result = client.queryAsync("SELECT id FROM users WHERE id=?", {std::int64_t(42)}).get();
}
```

Operation spans preserve recorded elapsed time and request trace/span IDs across worker/reactor
completion. Async queue, pool-wait, native/fallback selection, retry dispatch, replica fallback,
rejection, cache-hit and query-deadline cancellation events use separate phase spans. Selection and
dispatch events have zero duration; pool-wait includes completion delivery, and cancellation time
measures the cancellation call, not server-side rollback. These are diagnostic milestones, not a
strictly nested waterfall or a complete server-execution breakdown. Without a valid captured parent, the adapter creates an explicit root
span instead of inheriting a worker's unrelated active span. Trace flags, including the sampling
bit, propagate through `SqlContext` and W3C traceparent parsing/formatting.

Default attributes contain bounded operation/phase names, numeric error codes, row counts, slow and
shadow flags. Raw SQL, parameters, vendor error text, connection URLs and datasource names are
excluded. Fingerprints are opt-in through `OpenTelemetryOptions::includeFingerprint` because they
can still have high cardinality. Installing an observer replaces the previous observer of the same
kind; compose callbacks explicitly if another sink must remain active. Observer exceptions are
contained by the runtime. Callbacks should return quickly and must not reload/shutdown/destroy the
same Client.
