#include "sqlconduit/client.h"
#include "sqlconduit/driver/driver_registry.h"
#include "sqlconduit/driver/idriver.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <future>
#include <mutex>
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
        static std::atomic<int> executeDelayMs;
        static std::atomic<int> cancelCalls;
        static std::atomic<int> nativeQueryCalls;
        static std::atomic<int> nativeQueryCompletions;
        static std::atomic<int> nativeExecuteCalls;
        static std::atomic<int> retryableQueryFailures;
        static std::atomic<int> retryableExecuteFailures;
        static std::mutex nativeQueryGateMutex;
        static std::shared_future<void> nativeQueryGate;

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
            if (const auto delay = executeDelayMs.load(); delay > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            affected = 1;
            return common::Status::OK();
        }

        [[nodiscard]] core::AsyncCapability asyncCapability() const override {
            return identity_.rfind("native", 0) == 0
                       ? core::AsyncCapability::Native
                       : core::AsyncCapability::ThreadPoolFallback;
        }

        bool queryAsync(const std::string &, const common::Params &,
                        AsyncQueryCompletion completion) override {
            if (!open_ || identity_.rfind("native", 0) != 0 || !completion) return false;
            ++nativeQueryCalls;
            const auto identity = identity_;
            const auto delay = queryDelayMs.load();
            const bool fail = retryableQueryFailures.fetch_sub(1) > 0;
            std::shared_future<void> gate;
            {
                std::lock_guard<std::mutex> lock(nativeQueryGateMutex);
                gate = nativeQueryGate;
            }
            std::thread([identity, delay, fail, gate = std::move(gate),
                         completion = std::move(completion)]() mutable {
                if (gate.valid()) gate.wait();
                else if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                if (fail) {
                    auto status = common::Status::error(
                        common::ErrorCode::QueryError, "injected retryable query failure");
                    status.retryable = true;
                    completion(std::move(status), {});
                    ++nativeQueryCompletions;
                    return;
                }
                common::ResultSet rows;
                common::Row row;
                row.set("client", identity);
                rows.addRow(std::move(row));
                completion(common::Status::OK(), std::move(rows));
                ++nativeQueryCompletions;
            }).detach();
            return true;
        }

        bool executeAsync(const std::string &, const common::Params &,
                          AsyncExecuteCompletion completion) override {
            if (!open_ || identity_.rfind("native", 0) != 0 || !completion) return false;
            ++nativeExecuteCalls;
            const auto delay = executeDelayMs.load();
            const bool fail = retryableExecuteFailures.fetch_sub(1) > 0;
            std::thread([delay, fail, completion = std::move(completion)]() mutable {
                if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                if (fail) {
                    auto status = common::Status::error(
                        common::ErrorCode::QueryError, "injected retryable execute failure");
                    status.retryable = true;
                    completion(std::move(status), 0);
                    return;
                }
                completion(common::Status::OK(), 1);
            }).detach();
            return true;
        }

        common::Status cancel() override {
            ++cancelCalls;
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
    std::atomic<int> ClientTestConnection::executeDelayMs{0};
    std::atomic<int> ClientTestConnection::cancelCalls{0};
    std::atomic<int> ClientTestConnection::nativeQueryCalls{0};
    std::atomic<int> ClientTestConnection::nativeQueryCompletions{0};
    std::atomic<int> ClientTestConnection::nativeExecuteCalls{0};
    std::atomic<int> ClientTestConnection::retryableQueryFailures{0};
    std::atomic<int> ClientTestConnection::retryableExecuteFailures{0};
    std::mutex ClientTestConnection::nativeQueryGateMutex;
    std::shared_future<void> ClientTestConnection::nativeQueryGate;

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
    check(firstAsyncResult.mode == async::ExecutionMode::CompatibilityFallback,
          "client async query exposes compatibility fallback mode");
    check(secondAsyncResult.status.ok() && identityFrom(secondAsyncResult.rows) == "second",
          "second client async query resolves its own datasource");
    check(first.asyncStats().submitted >= 1 && second.asyncStats().submitted >= 1 &&
          first.asyncStats().fallbackOperations >= 1 &&
          second.asyncStats().fallbackOperations >= 1,
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

    Client nativeAsync;
    auto nativeConfig = makeConfig("native");
    check(nativeAsync.init(nativeConfig).ok(), "native async test client initializes");
    const auto nativeInterceptor = std::make_shared<CountingInterceptor>();
    nativeAsync.addInterceptor(nativeInterceptor);
    const auto nativeQuery = nativeAsync.queryAsync("SELECT native").get();
    check(nativeQuery.status.ok() &&
          nativeQuery.mode == async::ExecutionMode::Native &&
          identityFrom(nativeQuery.rows) == "native",
          "driver callback protocol completes a native async query");
    const auto nativeExecute = nativeAsync.executeAsync(
        "UPDATE t SET value = 3 WHERE id = 1").get();
    check(nativeExecute.status.ok() && nativeExecute.affected == 1 &&
          nativeExecute.mode == async::ExecutionMode::Native,
          "driver callback protocol completes a native async execute");
    check(nativeAsync.asyncStats().nativeOperations == 2 &&
          nativeAsync.asyncStats().fallbackOperations == 0,
          "native driver operations are accounted separately from fallback");
    check(nativeInterceptor->afterCalls.load() == 2,
          "native driver operations preserve the interceptor lifecycle");
    nativeAsync.shutdown(std::chrono::milliseconds(0));

    Client nativeRetry;
    auto nativeRetryConfig = makeConfig("native-retry");
    nativeRetryConfig.retry.max_attempts = 2;
    nativeRetryConfig.retry.initial_backoff_ms = 1;
    nativeRetryConfig.retry.max_backoff_ms = 1;
    check(nativeRetry.init(nativeRetryConfig).ok(),
          "native async retry test client initializes");
    const auto queryCallsBefore = ClientTestConnection::nativeQueryCalls.load();
    ClientTestConnection::retryableQueryFailures = 1;
    const auto retriedQuery = nativeRetry.queryAsync("SELECT native_retry").get();
    check(retriedQuery.status.ok() &&
          retriedQuery.mode == async::ExecutionMode::Native &&
          ClientTestConnection::nativeQueryCalls.load() == queryCallsBefore + 2,
          "retryable native query is retried by the async state machine");

    const auto executeCallsBefore = ClientTestConnection::nativeExecuteCalls.load();
    ClientTestConnection::retryableExecuteFailures = 1;
    common::SqlContext idempotentContext;
    idempotentContext.idempotency = common::Idempotency::Idempotent;
    async::ExecResult retriedExecute;
    {
        const common::ContextScope scope(idempotentContext);
        retriedExecute = nativeRetry.executeAsync(
            "UPDATE t SET value = 4 WHERE id = 1").get();
    }
    check(retriedExecute.status.ok() &&
          retriedExecute.mode == async::ExecutionMode::Native &&
          ClientTestConnection::nativeExecuteCalls.load() == executeCallsBefore + 2,
          "idempotent native write is retried by the async state machine");

    const auto nonIdempotentCallsBefore = ClientTestConnection::nativeExecuteCalls.load();
    ClientTestConnection::retryableExecuteFailures = 1;
    common::SqlContext nonIdempotentContext;
    nonIdempotentContext.idempotency = common::Idempotency::NonIdempotent;
    async::ExecResult nonRetriedExecute;
    {
        const common::ContextScope scope(nonIdempotentContext);
        nonRetriedExecute = nativeRetry.executeAsync(
            "UPDATE t SET value = value + 1 WHERE id = 1").get();
    }
    check(!nonRetriedExecute.status.ok() &&
          ClientTestConnection::nativeExecuteCalls.load() == nonIdempotentCallsBefore + 1,
          "non-idempotent native write is never retried");
    check(nativeRetry.asyncStats().nativeOperations == 3,
          "native retries are accounted once per public operation");
    nativeRetry.shutdown(std::chrono::milliseconds(0));

    Client nativeCache;
    auto nativeCacheConfig = makeConfig("native-cache", true);
    check(nativeCache.init(nativeCacheConfig).ok(),
          "native cache test client initializes");
    const auto nativeCacheInterceptor = std::make_shared<CountingInterceptor>();
    nativeCache.addInterceptor(nativeCacheInterceptor);
    const auto cacheMiss = nativeCache.queryAsync("SELECT native_cache").get();
    const auto cacheHit = nativeCache.queryAsync("SELECT native_cache").get();
    check(cacheMiss.status.ok() && cacheMiss.mode == async::ExecutionMode::Native,
          "async cache miss can use the native driver path");
    check(cacheHit.status.ok() &&
          cacheHit.mode == async::ExecutionMode::CompatibilityFallback &&
          identityFrom(cacheHit.rows) == "native-cache",
          "async cache hit completes without borrowing a driver connection");
    check(nativeCache.asyncStats().nativeOperations == 1 &&
          nativeCache.asyncStats().fallbackOperations == 1,
          "async cache hit and miss execution modes are accounted separately");
    check(nativeCacheInterceptor->afterCalls.load() == 2,
          "async cache hit and native miss preserve interceptor lifecycle");
    nativeCache.shutdown(std::chrono::milliseconds(0));

    Client nativeTimeout;
    auto nativeTimeoutConfig = makeConfig("native-timeout");
    nativeTimeoutConfig.async.statement_timeout_ms = 10;
    check(nativeTimeout.init(nativeTimeoutConfig).ok(),
          "native timeout test client initializes");
    ClientTestConnection::cancelCalls = 0;
    ClientTestConnection::nativeQueryCompletions = 0;
    std::promise<void> releaseNativeQuery;
    {
        std::lock_guard<std::mutex> lock(ClientTestConnection::nativeQueryGateMutex);
        ClientTestConnection::nativeQueryGate = releaseNativeQuery.get_future().share();
    }
    auto nativeTimedFuture = nativeTimeout.queryAsync("SELECT native_timeout");
    const bool nativeDeadlineReady =
        nativeTimedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    check(nativeDeadlineReady,
          "native query deadline resolves while the driver callback is blocked");
    if (!nativeDeadlineReady) releaseNativeQuery.set_value();
    const auto nativeTimedResult = nativeTimedFuture.get();
    check(nativeTimedResult.status.code == common::ErrorCode::QueryTimeout &&
          nativeTimedResult.mode == async::ExecutionMode::Native,
          "native query deadline returns a native QueryTimeout result");
    for (int i = 0; i < 1000 && ClientTestConnection::cancelCalls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(ClientTestConnection::cancelCalls.load() == 1,
          "native query deadline invokes driver cancellation exactly once");
    if (nativeDeadlineReady) releaseNativeQuery.set_value();
    for (int i = 0; i < 1000 && ClientTestConnection::nativeQueryCompletions.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(ClientTestConnection::nativeQueryCompletions.load() == 1,
          "late native driver callback completes exactly once after timeout");
    {
        std::lock_guard<std::mutex> lock(ClientTestConnection::nativeQueryGateMutex);
        ClientTestConnection::nativeQueryGate = {};
    }
    check(nativeTimeout.asyncStats().nativeOperations == 1 &&
          nativeTimeout.asyncStats().timedOutOperations == 1,
          "native timeout completes and is accounted exactly once");
    nativeTimeout.shutdown(std::chrono::milliseconds(0));

    Client asyncPoolWait;
    auto asyncPoolConfig = makeConfig("async-pool-wait");
    asyncPoolConfig.pool.max = 1;
    check(asyncPoolWait.init(asyncPoolConfig).ok(),
          "async pool-wait test client initializes");
    auto waitSource = asyncPoolWait.dataSource();
    std::promise<void> borrowedPromise;
    auto borrowedFuture = borrowedPromise.get_future();
    std::promise<void> releasePromise;
    auto releaseFuture = releasePromise.get_future().share();
    std::thread holder([waitSource, &borrowedPromise, releaseFuture] {
        (void) waitSource->withSession([&](core::Session &) {
            borrowedPromise.set_value();
            releaseFuture.wait();
            return common::Status::OK();
        });
    });
    borrowedFuture.wait();
    auto waitingQuery = asyncPoolWait.queryAsync("SELECT waits_for_handoff");
    bool observedAsyncWaiter = false;
    for (int i = 0; i < 100; ++i) {
        core::ConnectionPool::Stats stats;
        if (asyncPoolWait.poolStats(stats) && stats.asyncWaiting == 1 &&
            stats.waiting == 0) {
            observedAsyncWaiter = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    check(observedAsyncWaiter,
          "Client queryAsync waits through pool asyncWaiting without a sync waiter");
    releasePromise.set_value();
    holder.join();
    check(waitingQuery.get().status.ok(),
          "Client queryAsync resumes after connection handoff");
    asyncPoolWait.shutdown(std::chrono::milliseconds(0));

    Client overloadedAsync;
    auto overloadedConfig = makeConfig("overloaded");
    overloadedConfig.pool.max = 1;
    overloadedConfig.async.threads = 1;
    overloadedConfig.async.queue_size = 1;
    check(overloadedAsync.init(overloadedConfig).ok(),
          "async overload test client initializes");
    common::ResultSet warmedRows;
    check(overloadedAsync.query("SELECT warm", warmedRows).ok(),
          "async overload test prewarms its single connection");
    ClientTestConnection::queryDelayMs = 100;
    auto activeQuery = overloadedAsync.queryAsync("SELECT active");
    for (int i = 0; i < 100 && overloadedAsync.asyncStats().active == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto queuedQuery = overloadedAsync.queryAsync("SELECT queued");
    for (int i = 0; i < 100 && overloadedAsync.asyncStats().queueDepth == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto rejectedQuery = overloadedAsync.queryAsync("SELECT rejected");
    check(rejectedQuery.wait_for(std::chrono::milliseconds(20)) ==
              std::future_status::ready &&
          rejectedQuery.get().status.code == common::ErrorCode::Overloaded,
          "full async submission queue rejects without running on the caller thread");
    check(activeQuery.get().status.ok() && queuedQuery.get().status.ok() &&
          overloadedAsync.asyncStats().rejected >= 1,
          "accepted work drains normally after an overload rejection");
    ClientTestConnection::queryDelayMs = 0;
    overloadedAsync.shutdown(std::chrono::milliseconds(0));

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
    check(timedAsync.asyncStats().timedOutOperations >= 1,
          "client async timeout is visible in executor stats");
    ClientTestConnection::queryDelayMs = 0;
    timedAsync.shutdown(std::chrono::milliseconds(0));

    Client timedWrite;
    auto timedWriteConfig = makeConfig("timed-write");
    timedWriteConfig.async.statement_timeout_ms = 1;
    check(timedWrite.init(timedWriteConfig).ok(), "client accepts async write timeout config");
    ClientTestConnection::executeDelayMs = 10;
    const auto lateWrite = timedWrite.executeAsync(
        "UPDATE t SET value = 2 WHERE id = 1").get();
    check(lateWrite.status.ok() && lateWrite.affected == 1,
          "client async execute keeps committed success despite late fallback timeout");
    ClientTestConnection::executeDelayMs = 0;
    timedWrite.shutdown(std::chrono::milliseconds(0));

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
