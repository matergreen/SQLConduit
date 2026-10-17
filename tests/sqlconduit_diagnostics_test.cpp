#include "sqlconduit/client.h"
#include "sqlconduit/config/config_loader.h"
#include "sqlconduit/common/context.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <mutex>

namespace {
    int failures = 0;
    void check(bool ok, const char *message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    }
    class Connection final : public sqlconduit::core::IDatabaseConnection {
        bool open = false;
    public:
        sqlconduit::common::Status connect(const sqlconduit::config::DataSourceConfig &) override {
            open = true; return sqlconduit::common::Status::OK();
        }
        sqlconduit::common::Status ping() override { return sqlconduit::common::Status::OK(); }
        sqlconduit::common::Status query(const std::string &, sqlconduit::common::ResultSet &) override {
            return sqlconduit::common::Status::OK();
        }
        sqlconduit::common::Status execute(const std::string &, std::int64_t &) override { return ping(); }
        sqlconduit::common::Status begin() override { return ping(); }
        sqlconduit::common::Status commit() override { return ping(); }
        sqlconduit::common::Status rollback() override { return ping(); }
        void close() override { open = false; }
        bool isOpen() const override { return open; }
    };
    class Driver final : public sqlconduit::driver::IDriver {
    public:
        const char *name() const override { return "mock"; }
        std::unique_ptr<sqlconduit::core::IDatabaseConnection> createConnection() override {
            return std::make_unique<Connection>();
        }
    };
}

int main() {
    using namespace sqlconduit;
    config::GlobalConfig c;
    c.pool.min = 0;
    c.heartbeat_interval_ms = 0;
    config::DataSourceConfig d;
    d.name = "primary"; d.type = "mock"; d.password = "secret-password";
    d.dsn = "secret-dsn"; d.extra["connection_string"] = "secret-extra";
    d.tls_key = "secret-key";
    c.datasources.push_back(d);
    d.name = "replica";
    c.datasources.push_back(d);
    c.default_datasource = "primary";
    std::string error;
    check(config::ConfigLoader::validateTopology(c, error), "valid leaf topology");
    config::DataSourceGroupConfig g;
    g.name = "rw"; g.primary = "primary"; g.replicas.push_back({"replica", 2});
    c.groups.push_back(g);
    c.default_datasource = "rw";
    check(config::ConfigLoader::validateTopology(c, error), "valid group topology");
    c.groups[0].shadow = "replica";
    check(!config::ConfigLoader::validateTopology(c, error), "self shadow is rejected offline");
    c.groups[0].shadow.clear();
    c.groups[0].primary = "unknown";
    check(!config::ConfigLoader::validateTopology(c, error), "unknown primary is rejected offline");
    c.groups[0].primary = "primary";
    const auto text = config::ConfigLoader::explain(c);
    check(text.find("secret-") == std::string::npos, "effective config never exposes credentials/dsn/extras/key");
    const auto json = nlohmann::json::parse(text);
    check(json["pool"]["max"] == 8 && json["async"]["queue_size"] == 4096,
          "effective config includes defaults");

    Client client;
    check(!client.capabilities().status.ok(), "uninitialized capability lookup has structured status");
    check(client.addDriver({"mock", [] { return std::make_unique<Driver>(); }}).ok(), "register driver");
    check(client.init(c).ok(), "init diagnostics fixture");
    const auto before = client.diagnostics();
    check(before.sources.size() == 3 && before.running, "diagnostics includes groups and leaves");
    const auto caps = client.capabilities("rw");
    check(caps.status.ok() && caps.sources.size() == 3, "group capabilities include participating leaves");
    check(!client.capabilities("absent").status.ok(), "unknown capability name fails");
    common::SqlContext context;
    context.targetDataSource = "absent";
    {
        const common::ContextScope scope(context);
        check(client.capabilities("primary").status.ok(), "diagnostics ignores request routing override");
    }
    core::ConnectionPool::Stats pool;
    check(client.poolStats(pool, "primary") && pool.connectionsCreated == 0,
          "diagnostics never creates or borrows connections");
    common::ResultSet rows;
    check(client.query("primary", "SELECT 1", rows).ok(), "create observed connection");
    const auto observed = client.capabilities("primary");
    check(observed.sources.size() == 1 && observed.sources[0].capabilities.observed,
          "capabilities are cached after successful connection");
    std::mutex phaseMutex;
    std::vector<common::ExecutionPhaseEvent> phases;
    client.setExecutionPhaseObserver([&](const common::ExecutionPhaseEvent &event) {
        std::lock_guard<std::mutex> lock(phaseMutex);
        phases.push_back(event);
    });
    common::SqlContext traced;
    traced.traceId = std::string(32, 'a');
    traced.spanId = std::string(16, 'b');
    {
        const common::ContextScope scope(traced);
        check(client.queryAsync("primary", "SELECT 1", {}).get().status.ok(), "async diagnostics query");
    }
    bool queue = false, poolWait = false, fallback = false;
    {
        std::lock_guard<std::mutex> lock(phaseMutex);
        for (const auto &event : phases) {
            check(event.context.traceId == traced.traceId && event.context.spanId == traced.spanId,
                  "phase context follows submitting thread");
            if (event.phase == common::ExecutionPhase::Queue) queue = true;
            if (event.phase == common::ExecutionPhase::PoolWait) poolWait = true;
            if (event.phase == common::ExecutionPhase::Fallback) {
                fallback = event.fallbackReason == common::FallbackReason::DriverUnsupported;
            }
        }
    }
    check(queue && poolWait && fallback, "phase coverage includes queue, pool wait and actual fallback reason");
    client.setExecutionPhaseObserver([](const auto &) { throw std::runtime_error("sink failed"); });
    check(client.queryAsync("primary", "SELECT 1", {}).get().status.ok(), "phase observer exceptions never break futures");
    client.setExecutionPhaseObserver({});
    common::SqlContext unsampled;
    check(common::parseTraceparent("00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-00", unsampled) &&
          unsampled.traceFlags == 0 && common::formatTraceparent(unsampled).substr(53) == "00",
          "unsampled W3C context roundtrips without forced sampling");
    client.shutdown();
    check(!client.diagnostics().running && !client.capabilities().status.ok(), "closed snapshots remain safe");
    return failures ? 1 : 0;
}
