#include "sqlconduit/client.h"
#include "sqlconduit/driver/driver_registry.h"
#include "sqlconduit/driver/idriver.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace sqlconduit;

namespace {
    int failed = 0;

    void check(const bool condition, const char *name) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!condition) ++failed;
    }

    class ClientTestConnection final : public core::IDatabaseConnection {
    public:
        static std::atomic<int> firstQueries;
        static std::atomic<int> secondQueries;
        static std::atomic<int> queryDelayMs;

        common::Status connect(const config::DataSourceConfig &config) override {
            identity_ = config.database;
            open_ = true;
            return common::Status::OK();
        }

        common::Status ping() override { return common::Status::OK(); }

        common::Status query(const std::string &, common::ResultSet &out) override {
            if (!open_)
                return common::Status::error(common::ErrorCode::NotConnected, "closed");
            if (identity_ == "first") ++firstQueries;
            if (identity_ == "second") ++secondQueries;
            if (const auto delay = queryDelayMs.load(); delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            common::Row row;
            row.set("client", identity_);
            out.addRow(std::move(row));
            return common::Status::OK();
        }

        common::Status execute(const std::string &, std::int64_t &affected) override {
            affected = 1;
            return common::Status::OK();
        }

        common::Status begin() override { return common::Status::OK(); }
        common::Status commit() override { return common::Status::OK(); }
        common::Status rollback() override { return common::Status::OK(); }

        void close() override { open_ = false; }

        bool isOpen() const override { return open_; }

    private:
        std::string identity_;
        bool open_ = false;
    };

    std::atomic<int> ClientTestConnection::firstQueries{0};
    std::atomic<int> ClientTestConnection::secondQueries{0};
    std::atomic<int> ClientTestConnection::queryDelayMs{0};

    class ClientTestDriver final : public driver::IDriver {
    public:
        const char *name() const override { return "client-test"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<ClientTestConnection>();
        }
    };

    class CountingInterceptor final : public core::ISqlInterceptor {
    public:
        std::atomic<int> afterCalls{0};

        void onRoute(const std::string &, const std::string &,
                     common::OperationType, common::SqlContext &) override {
        }

        common::Status beforeExecution(const core::ExecutionView &) override {
            return common::Status::OK();
        }

        void afterExecution(const core::ExecutionView &) override {
            ++afterCalls;
        }

        void onCompletion(const core::ExecutionView &) override {
        }
    };

    config::GlobalConfig makeConfig(const std::string &identity,
                                    const bool cacheEnabled = false,
                                    const bool blockUnsafeDml = false,
                                    const bool slowSqlEnabled = false,
                                    const bool asyncEnabled = true) {
        config::GlobalConfig config;
        config.default_datasource = "main";
        config.pool.min = 0;
        config.pool.max = 2;
        config.heartbeat_interval_ms = 60000;
        config.observability.pool_metrics.enabled = false;
        config.observability.slow_sql.enabled = slowSqlEnabled;
        config.observability.slow_sql.threshold_ms = 0;
        config.async.enabled = asyncEnabled;
        config.query_cache.enabled = cacheEnabled;
        config.query_cache.ttl_ms = 60000;
        config.sql_audit.enabled = blockUnsafeDml;
        config.sql_audit.action = "block";
        config.sql_audit.block_no_where_dml = blockUnsafeDml;
        config.interceptors.enabled = true;

        config::DataSourceConfig source;
        source.name = "main";
        source.type = "client-test";
        source.database = identity;
        config.datasources.push_back(std::move(source));
        return config;
    }

    std::string identityFrom(const common::ResultSet &rows) {
        if (rows.rows().empty()) return {};
        const auto &value = rows.rows().front().at("client");
        const auto *text = std::get_if<std::string>(&value);
        return text ? *text : std::string{};
    }
}

int main() {
    driver::DriverRegistry::instance().registerDriver(
        "client-test", [] { return std::make_unique<ClientTestDriver>(); });

    Client locallyRegistered;
    Client missingRegistration;
    auto localConfig = makeConfig("local");
    localConfig.datasources.front().type = "local-only";
    check(locallyRegistered.addDriver({
              "local-only", [] { return std::make_unique<ClientTestDriver>(); }}).ok(),
          "client accepts a driver registration before init");
    check(locallyRegistered.init(localConfig).ok(),
          "client initializes with its own registered driver");
    check(missingRegistration.init(localConfig).code == common::ErrorCode::UnknownDriver,
          "client-local driver registration does not leak to another client");
    locallyRegistered.shutdown(std::chrono::milliseconds(0));

    Client first;
    Client second;

    Client retryable;
    config::GlobalConfig invalid;
    auto status = retryable.init(invalid);
    check(status.code == common::ErrorCode::ConfigError && !retryable.isRunning(),
          "failed init leaves the client reusable");
    check(retryable.init(makeConfig("retryable")).ok(),
          "client can retry initialization after a failure");
    retryable.shutdown(std::chrono::milliseconds(0));

    common::ResultSet rows;
    status = first.query("SELECT 1", rows);
    check(status.code == common::ErrorCode::NotInitialized,
          "operation before init returns NotInitialized");

    check(first.init(makeConfig("first", true, true, true)).ok(), "first client initializes");
    check(second.init(makeConfig("second")).ok(), "second client initializes");
    check(first.isRunning() && second.isRunning(), "both clients are running");

    const auto firstInterceptor = std::make_shared<CountingInterceptor>();
    const auto secondInterceptor = std::make_shared<CountingInterceptor>();
    first.addInterceptor(firstInterceptor);
    second.addInterceptor(secondInterceptor);

    std::atomic<int> firstObserved{0};
    std::atomic<int> secondObserved{0};
    first.setObserver([&](const common::OperationEvent &) { ++firstObserved; });
    second.setObserver([&](const common::OperationEvent &) { ++secondObserved; });

    rows.clear();
    check(first.query("SELECT identity", rows).ok() && identityFrom(rows) == "first",
          "first client resolves its own datasource");

    rows.clear();
    check(second.query("SELECT identity", rows).ok() && identityFrom(rows) == "second",
          "second client resolves its own datasource");
    check(firstObserved.load() == 1 && secondObserved.load() == 1,
          "each client invokes only its own observer");

    auto firstAsync = first.queryAsync("SELECT async_first");
    auto secondAsync = second.queryAsync("SELECT async_second");
    const auto firstAsyncResult = firstAsync.get();
    const auto secondAsyncResult = secondAsync.get();
    check(firstAsyncResult.status.ok() && identityFrom(firstAsyncResult.rows) == "first",
          "first client async query resolves its own datasource");
    check(secondAsyncResult.status.ok() && identityFrom(secondAsyncResult.rows) == "second",
          "second client async query resolves its own datasource");
    check(first.asyncStats().submitted >= 1 && second.asyncStats().submitted >= 1,
          "each client owns an active async executor");
    const auto asyncExec = second.executeAsync(
        "UPDATE t SET value = 2 WHERE id = 1").get();
    check(asyncExec.status.ok() && asyncExec.affected == 1,
          "client async execute returns the instance result");
    const auto asyncTx = second.transactionAsync([](core::Session &session) {
        common::ResultSet txRows;
        return session.query("SELECT async_transaction", txRows);
    }).get();
    check(asyncTx.status.ok(), "client async transaction uses its instance session");

    Client asyncDisabled;
    check(asyncDisabled.init(makeConfig("async-disabled", false, false, false, false)).ok(),
          "client can explicitly disable async execution");
    check(asyncDisabled.queryAsync("SELECT disabled").get().status.code ==
          common::ErrorCode::ConfigError,
          "disabled client async call returns a ready configuration error");
    asyncDisabled.shutdown(std::chrono::milliseconds(0));

    Client invalidAsync;
    auto invalidAsyncConfig = makeConfig("invalid-async");
    invalidAsyncConfig.async.queue_size = 0;
    check(invalidAsync.init(invalidAsyncConfig).code == common::ErrorCode::ConfigError,
          "programmatic client config validates async queue size");

    Client timedAsync;
    auto timedAsyncConfig = makeConfig("timed-async");
    timedAsyncConfig.async.statement_timeout_ms = 1;
    check(timedAsync.init(timedAsyncConfig).ok(), "client accepts async statement timeout");
    ClientTestConnection::queryDelayMs = 10;
    check(timedAsync.queryAsync("SELECT timeout").get().status.code ==
          common::ErrorCode::QueryTimeout,
          "client async future reports configured statement timeout");
    ClientTestConnection::queryDelayMs = 0;
    timedAsync.shutdown(std::chrono::milliseconds(0));

    check(!first.slowSqlStats().empty() && !first.recentSlowSql().empty(),
          "first client retains its enabled slow SQL statistics");
    check(second.slowSqlStats().empty() && second.recentSlowSql().empty(),
          "second client does not see the first client's slow SQL statistics");
    first.clearSlowSqlStats();
    check(first.slowSqlStats().empty(),
          "slow SQL statistics can be cleared per client");

    ClientTestConnection::firstQueries = 0;
    ClientTestConnection::secondQueries = 0;
    common::ResultSet cached1;
    common::ResultSet cached2;
    check(first.query("SELECT cache_probe", cached1).ok() &&
          first.query("SELECT cache_probe", cached2).ok() &&
          ClientTestConnection::firstQueries.load() == 1,
          "first client applies its enabled query cache");
    cached1.clear();
    cached2.clear();
    check(second.query("SELECT cache_probe", cached1).ok() &&
          second.query("SELECT cache_probe", cached2).ok() &&
          ClientTestConnection::secondQueries.load() == 2,
          "second client keeps its query cache disabled");

    std::int64_t affected = 0;
    status = first.execute("UPDATE t SET value = 1", affected);
    check(status.code == common::ErrorCode::SqlBlocked,
          "first client applies its blocking SQL audit policy");
    check(second.execute("UPDATE t SET value = 1", affected).ok(),
          "second client is unaffected by the first client's audit policy");

    const auto firstBefore = firstInterceptor->afterCalls.load();
    const auto secondBefore = secondInterceptor->afterCalls.load();
    common::ResultSet intercepted;
    check(first.query("SELECT interceptor_first", intercepted).ok() &&
          firstInterceptor->afterCalls.load() == firstBefore + 1 &&
          secondInterceptor->afterCalls.load() == secondBefore,
          "first client invokes only its own interceptor chain");
    intercepted.clear();
    check(second.query("SELECT interceptor_second", intercepted).ok() &&
          secondInterceptor->afterCalls.load() == secondBefore + 1 &&
          firstInterceptor->afterCalls.load() == firstBefore + 1,
          "second client invokes only its own interceptor chain");
    first.clearInterceptors();
    intercepted.clear();
    check(first.query("SELECT interceptor_cleared", intercepted).ok() &&
          firstInterceptor->afterCalls.load() == firstBefore + 1,
          "clearing one client does not use the default interceptor registry");

    status = first.init(makeConfig("replacement"));
    check(status.code == common::ErrorCode::AlreadyInitialized,
          "second init is rejected in favor of reload");

    check(first.reload(makeConfig("reloaded"), std::chrono::milliseconds(0)).ok(),
          "reload replaces one client runtime");
    rows.clear();
    check(first.query("SELECT identity", rows).ok() && identityFrom(rows) == "reloaded",
          "first client sees reloaded datasource");
    rows.clear();
    check(second.query("SELECT identity", rows).ok() && identityFrom(rows) == "second",
          "reload does not replace the second client datasource");

    first.shutdown(std::chrono::milliseconds(0));
    status = first.query("SELECT 1", rows);
    check(status.code == common::ErrorCode::ClientClosed,
          "operation after shutdown returns ClientClosed");
    first.shutdown(std::chrono::milliseconds(0));
    check(!first.isRunning(), "shutdown is idempotent");

    rows.clear();
    check(second.query("SELECT identity", rows).ok() && identityFrom(rows) == "second",
          "shutting down one client leaves the other usable");
    check(second.queryAsync("SELECT async_after_other_shutdown").get().status.ok(),
          "shutting down one client leaves the other async executor usable");
    second.shutdown(std::chrono::milliseconds(0));

    return failed == 0 ? 0 : 1;
}
