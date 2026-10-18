#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/driver/idriver.h"
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

using namespace sqlconduit;
using namespace std::chrono_literals;
namespace {
    std::atomic<int> started{0}, cancelled{0}, writes{0}, delayMs{0}, failures{0};
    std::atomic<bool> ignoreCancel{false};
    std::mutex gateMutex;
    std::shared_future<void> cancelGate;
    void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    template<class Predicate> void waitFor(Predicate predicate, const char *message) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (!predicate() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
        require(predicate(), message);
    }
    template<class T> T await(std::future<T> future) {
        require(future.wait_for(1s) == std::future_status::ready, "future resolves without waiting for driver or queue");
        return future.get();
    }
    struct Active { std::mutex mutex; std::condition_variable cv; bool stop = false; };
    struct Shared { std::mutex mutex; std::shared_ptr<Active> active; bool native = true; bool open = false; };
    class Connection final : public core::IDatabaseConnection {
        std::shared_ptr<Shared> shared = std::make_shared<Shared>();
        static common::Status run(const std::shared_ptr<Shared> &state, bool write, common::ResultSet &out) {
            auto active = std::make_shared<Active>();
            { std::lock_guard<std::mutex> lock(state->mutex); state->active = active; }
            const int delay = delayMs.load(); const bool ignore = ignoreCancel.load();
            ++started;
            {
                std::unique_lock<std::mutex> lock(active->mutex);
                active->cv.wait_for(lock, std::chrono::milliseconds(delay), [&] { return !ignore && active->stop; });
            }
            bool stopped;
            { std::lock_guard<std::mutex> lock(active->mutex); stopped = active->stop; }
            { std::lock_guard<std::mutex> lock(state->mutex); state->active.reset(); }
            if (stopped && !ignore) return common::Status::error(common::ErrorCode::Cancelled, "test driver cancelled");
            if (failures.fetch_sub(1) > 0) {
                auto status = common::Status::error(common::ErrorCode::QueryError, "retry test"); status.retryable = true; return status;
            }
            if (write) ++writes;
            out.setFields({"value"}); common::Row row; row.set("value", std::int64_t{42}); out.addRow(std::move(row));
            return {};
        }
    public:
        common::Status connect(const config::DataSourceConfig &cfg) override {
            shared->native = cfg.database != "fallback"; shared->open = true; return {};
        }
        common::Status ping() override { return {}; }
        common::Status query(const std::string &, common::ResultSet &out) override { return run(shared, false, out); }
        common::Status query(const std::string &, const common::Params &, common::ResultSet &out) override {
            return run(shared, false, out);
        }
        common::Status execute(const std::string &sql, const common::Params &, std::int64_t &affected) override {
            return execute(sql, affected);
        }
        common::Status execute(const std::string &, std::int64_t &affected) override {
            common::ResultSet rows; auto status = run(shared, true, rows); if (status.ok()) affected = 1; return status;
        }
        core::AsyncCapability asyncCapability() const override {
            return shared->native ? core::AsyncCapability::Native : core::AsyncCapability::ThreadPoolFallback;
        }
        bool queryAsync(const std::string &, const common::Params &, AsyncQueryCompletion complete) override {
            if (!shared->native) return false;
            std::thread([state = shared, complete = std::move(complete)]() mutable {
                common::ResultSet rows; auto status = run(state, false, rows); complete(std::move(status), std::move(rows));
            }).detach();
            return true;
        }
        bool executeAsync(const std::string &, const common::Params &, AsyncExecuteCompletion complete) override {
            if (!shared->native) return false;
            std::thread([state = shared, complete = std::move(complete)]() mutable {
                common::ResultSet rows; auto status = run(state, true, rows); complete(std::move(status), 1);
            }).detach();
            return true;
        }
        common::Status cancel() override {
            ++cancelled;
            std::shared_future<void> gate;
            { std::lock_guard<std::mutex> lock(gateMutex); gate = cancelGate; }
            if (gate.valid()) gate.wait();
            std::shared_ptr<Active> active;
            { std::lock_guard<std::mutex> lock(shared->mutex); active = shared->active; }
            if (active) {
                std::lock_guard<std::mutex> lock(active->mutex); active->stop = true; active->cv.notify_all();
            }
            return {};
        }
        common::Status begin() override { return {}; }
        common::Status commit() override { return {}; }
        common::Status rollback() override { return {}; }
        void close() override { shared->open = false; }
        bool isOpen() const override { return shared->open; }
    };
    class Driver final : public driver::IDriver {
    public:
        const char *name() const override { return "operation-test"; }
        std::unique_ptr<core::IDatabaseConnection> createConnection() override { return std::make_unique<Connection>(); }
    };
    void init(Client &client, bool native = true, int retries = 1) {
        require(client.addDriver({"operation-test", [] { return std::make_unique<Driver>(); }}).ok(), "register driver");
        config::GlobalConfig cfg; cfg.default_datasource = "db"; cfg.pool.min = 0; cfg.pool.max = 1;
        cfg.heartbeat_interval_ms = 0; cfg.async.threads = 1;
        cfg.retry.max_attempts = retries; cfg.retry.initial_backoff_ms = 300; cfg.retry.max_backoff_ms = 300;
        cfg.circuit_breaker.failure_threshold = 1;
        config::DataSourceConfig ds; ds.name = "db"; ds.type = "operation-test";
        ds.database = native ? "native" : "fallback"; cfg.datasources.push_back(ds);
        require(client.init(cfg).ok(), "initialize driver");
    }
    void drain(Client &client) {
        waitFor([&] { core::ConnectionPool::Stats stats; return client.poolStats(stats) && stats.borrowed == 0 && stats.asyncWaiting == 0; },
                "cancelled operation releases its connection and waiter");
    }
}

int main() {
    try {
        Client client; init(client);
        common::CancellationSource source;
        require(source.requestCancellation() && !source.requestCancellation() && source.token().cancellationRequested(), "idempotent source");
        common::OperationOptions options; options.cancellation = source.token();
        const auto before = started.load();
        require(client.withOptions(options).queryScalar<int>("SELECT value").status.code == common::ErrorCode::Cancelled,
                "pre-cancelled synchronous call never executes");
        require(await(client.withOptions(options).queryScalarAsync<int>("SELECT value")).status.code == common::ErrorCode::Cancelled,
                "pre-cancelled async call never executes");
        require(started == before, "no SQL after pre-cancellation");
        options = {}; options.timeout = -1ms;
        require(!client.withOptions(options).query("SELECT value").ok(), "negative budget rejected");
        options = {}; options.deadline = std::chrono::steady_clock::now() - 1ms;
        require(await(client.withOptions(options).queryAsync("SELECT value")).status.code == common::ErrorCode::QueryTimeout,
                "past absolute deadline rejects before execution");
        options = {}; options.timeout = 30ms;
        delayMs = 250;
        auto timed = await(client.withOptions(options).queryScalarAsync<int>("SELECT value"));
        require(timed.status.code == common::ErrorCode::QueryTimeout && !timed.status.outcomeUnknown, "native query timeout");
        drain(client); delayMs = 0;
        require(client.queryScalar<int>("SELECT value").value == 42, "application timeout does not open circuit breaker");
        options = {}; options.timeout = 200ms;
        require(client.withOptions(options).queryScalarNamed<int>("SELECT :value", {{"value", 42}}).value == 42,
                "options view accepts braced named parameters");
        require(await(client.withOptions(options).queryScalarNamedAsync<int>("SELECT :value", {{"value", 42}})).value == 42,
                "async options view accepts braced named parameters");
        require(client.withOptions(options).execute("UPDATE t SET value=?", {42}).ok(),
                "options view supports value-returning positional writes");
        auto built = sql::Builder::select("t").columns({"value"}).build();
        require(client.withOptions(options).queryScalar<int>(built).value == 42,
                "options view supports builder results");
        require(client.withSession([&](core::Session &session) {
            return session.withOptions(options).queryScalarNamed<int>("SELECT :value", {{"value", 42}}).status;
        }).ok(), "session options view reuses the borrowed connection");
        options.timeout = 20ms; delayMs = 200;
        require(client.withOptions(options).queryScalar<int>("SELECT value").status.code == common::ErrorCode::QueryTimeout,
                "synchronous cancellable driver respects deadline");
        delayMs = 0;
        {
            common::OperationOptions parent; parent.timeout = 20ms;
            common::OperationScope parentScope(parent);
            common::OperationOptions child; child.timeout = 1s; delayMs = 100;
            require(await(client.withOptions(child).queryAsync("SELECT value")).status.code == common::ErrorCode::QueryTimeout,
                    "nested request inherits earlier parent deadline");
        }
        drain(client); delayMs = 0;
        options = {}; options.timeout = 20ms; ignoreCancel = true; delayMs = 40;
        auto confirmedWrite = client.withOptions(options).execute("UPDATE t SET value=1");
        ignoreCancel = false; delayMs = 0;
        require(confirmedWrite.ok() && !confirmedWrite.status.outcomeUnknown,
                "synchronous confirmed write success is not rewritten as an uncertain timeout");

        common::CancellationSource activeSource; options = {}; options.cancellation = activeSource.token();
        delayMs = 250; const auto began = started.load();
        auto active = client.withOptions(options).queryAsync("SELECT value");
        waitFor([&] { return started > began; }, "native query started");
        activeSource.requestCancellation();
        require(await(std::move(active)).status.code == common::ErrorCode::Cancelled, "in-flight native cancellation");
        drain(client); delayMs = 0;

        // A held transaction connection tests cancellation without a physical SQL operation.
        std::promise<void> held, release;
        auto heldFuture = held.get_future(); auto releaseFuture = release.get_future().share();
        std::thread holder([&] { (void) client.withSession([&](core::Session &) { held.set_value(); releaseFuture.wait(); return common::Status{}; }); });
        heldFuture.wait();
        common::CancellationSource waitingSource; options = {}; options.cancellation = waitingSource.token();
        auto waiting = client.withOptions(options).queryAsync("SELECT value");
        waitFor([&] { core::ConnectionPool::Stats stats; return client.poolStats(stats) && stats.asyncWaiting == 1; }, "async borrower queued");
        waitingSource.requestCancellation();
        const auto waited = await(std::move(waiting));
        release.set_value(); holder.join();
        require(waited.status.code == common::ErrorCode::Cancelled && !waited.status.outcomeUnknown, "cancelled pool wait");
        drain(client);

        std::promise<void> syncHeld, syncRelease;
        auto syncHeldFuture = syncHeld.get_future(); auto syncReleaseFuture = syncRelease.get_future().share();
        std::thread syncHolder([&] { (void) client.withSession([&](core::Session &) {
            syncHeld.set_value(); syncReleaseFuture.wait(); return common::Status{};
        }); });
        syncHeldFuture.wait(); options = {}; options.timeout = 20ms;
        auto syncWaiting = client.withOptions(options).query("SELECT value");
        syncRelease.set_value(); syncHolder.join();
        require(syncWaiting.status.code == common::ErrorCode::QueryTimeout, "synchronous pool wait shares operation budget");

        // Slow driver cancel cannot block the global deadline clock or cancel a reused connection.
        std::promise<void> releaseCancel;
        { std::lock_guard<std::mutex> lock(gateMutex); cancelGate = releaseCancel.get_future().share(); }
        common::CancellationSource slowSource; options = {}; options.cancellation = slowSource.token();
        delayMs = 60; const auto slowBegan = started.load(), cancelledBefore = cancelled.load();
        auto slow = client.withOptions(options).queryAsync("SELECT value");
        waitFor([&] { return started > slowBegan; }, "slow-cancel query started");
        slowSource.requestCancellation();
        const auto slowResult = await(std::move(slow));
        waitFor([&] { return cancelled > cancelledBefore; }, "driver cancel entered gate");
        common::OperationOptions shortBudget; shortBudget.timeout = 20ms;
        auto anotherDeadline = await(client.withOptions(shortBudget).queryAsync("SELECT value"));
        releaseCancel.set_value();
        { std::lock_guard<std::mutex> lock(gateMutex); cancelGate = {}; }
        require(slowResult.status.code == common::ErrorCode::Cancelled &&
            anotherDeadline.status.code == common::ErrorCode::QueryTimeout, "slow cancel does not block unrelated deadline");
        drain(client); delayMs = 0;
        require(await(client.queryAsync("SELECT value")).ok(), "late cancellation cannot target reused connection");

        common::CancellationSource writeSource; options = {}; options.cancellation = writeSource.token();
        ignoreCancel = true; delayMs = 60; const auto writeBegan = started.load(), writtenBefore = writes.load();
        auto write = client.withOptions(options).executeAsync("UPDATE t SET value=1");
        waitFor([&] { return started > writeBegan; }, "write started");
        writeSource.requestCancellation();
        auto writeResult = await(std::move(write));
        require(writeResult.status.code == common::ErrorCode::Cancelled && writeResult.status.outcomeUnknown &&
                !writeResult.status.retryable, "in-flight write reports unknown outcome, never rollback or automatic retry");
        drain(client); ignoreCancel = false; delayMs = 0;
        require(writes == writtenBefore + 1, "late committed write executes once");

        Client fallback; init(fallback, false);
        options = {}; options.allowCompatibilityFallback = false;
        const auto noFallbackBefore = started.load();
        require(await(fallback.withOptions(options).queryAsync("SELECT value")).status.code == common::ErrorCode::NotSupported &&
                started == noFallbackBefore, "native-only option rejects compatibility work");
        options = {}; options.timeout = 20ms; delayMs = 200;
        require(await(fallback.withOptions(options).queryScalarAsync<int>("SELECT value")).status.code == common::ErrorCode::QueryTimeout,
                "fallback query deadline resolves without a waiting thread");
        drain(fallback); delayMs = 0;
        delayMs = 100; const auto queuedBefore = started.load();
        auto occupying = fallback.queryAsync("SELECT value");
        waitFor([&] { return started > queuedBefore; }, "fallback worker occupied");
        common::CancellationSource queuedSource; options = {}; options.cancellation = queuedSource.token();
        auto queued = fallback.withOptions(options).queryAsync("SELECT value");
        queuedSource.requestCancellation();
        auto queuedResult = await(std::move(queued));
        require(await(std::move(occupying)).ok(), "unrelated occupying query succeeds");
        drain(fallback); delayMs = 0;
        require(queuedResult.status.code == common::ErrorCode::Cancelled && started == queuedBefore + 1,
                "queued cancellation never starts SQL after worker becomes available");
        options = {}; options.timeout = 200ms;
        auto reusable = fallback.withOptions(options);
        require(reusable.queryScalar<int>("SELECT value").value == 42, "synchronous options support fallback driver");
        std::this_thread::sleep_for(210ms);
        require(reusable.queryScalar<int>("SELECT value").value == 42, "view reuse creates a fresh relative deadline");

        Client retry; init(retry, true, 3); failures = 1;
        options = {}; options.timeout = 20ms; const auto retryBegan = started.load();
        require(await(retry.withOptions(options).queryAsync("SELECT value")).status.code == common::ErrorCode::QueryTimeout,
                "absolute budget covers retry backoff");
        retry.shutdown(0ms); failures = 0;
        require(started == retryBegan + 1, "cancelled backoff never executes another attempt");
        std::cout << "Operation deadlines, cancellation, queue/borrow/retry budgets, write ambiguity and isolation passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
