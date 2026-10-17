#include "sqlconduit/client.h"
#include "sqlconduit/common/logger.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace {
    using namespace sqlconduit;
    struct Faults {
        std::atomic<int> connects{0}, reads{0}, writes{0}, commits{0};
        std::atomic<bool> dropRead{false}, ambiguousWrite{false}, ambiguousCommit{false};
    };
    common::Status disconnected() {
        auto s = common::Status::error(common::ErrorCode::QueryError, "injected connection reset after dispatch");
        s.sqlState = "08006";
        s.retryable = true;
        s.connectionBroken = true;
        return s;
    }
    class Connection final : public core::IDatabaseConnection {
        std::shared_ptr<Faults> faults;
        bool open = false, tx = false;
    public:
        explicit Connection(std::shared_ptr<Faults> f) : faults(std::move(f)) {}
        common::Status connect(const config::DataSourceConfig &) override {
            ++faults->connects; open = true; return common::Status::OK();
        }
        common::Status ping() override { return common::Status::OK(); }
        common::Status query(const std::string &, common::ResultSet &) override {
            ++faults->reads;
            if (faults->dropRead.exchange(false)) { open = false; return disconnected(); }
            return common::Status::OK();
        }
        common::Status execute(const std::string &, std::int64_t &affected) override {
            ++faults->writes; affected = 1;
            if (faults->ambiguousWrite.exchange(false)) { open = false; return disconnected(); }
            return common::Status::OK();
        }
        common::Status begin() override { tx = true; return common::Status::OK(); }
        common::Status commit() override {
            ++faults->commits; tx = false;
            if (faults->ambiguousCommit.exchange(false)) { open = false; return disconnected(); }
            return common::Status::OK();
        }
        common::Status rollback() override { tx = false; return common::Status::OK(); }
        void close() override { open = false; }
        bool isOpen() const override { return open; }
        bool inTransaction() const override { return tx; }
    };
    class Driver final : public driver::IDriver {
        std::shared_ptr<Faults> faults;
    public:
        explicit Driver(std::shared_ptr<Faults> f) : faults(std::move(f)) {}
        const char *name() const override { return "fault"; }
        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<Connection>(faults);
        }
    };
    void require(bool ok, const char *message) {
        if (!ok) throw std::runtime_error(message);
    }

    void scenario() {
        const auto faults = std::make_shared<Faults>();
        Client client;
        require(client.addDriver({"fault", [faults] { return std::make_unique<Driver>(faults); }}).ok(), "register");
        config::GlobalConfig c;
        c.default_datasource = "db";
        c.heartbeat_interval_ms = 0;
        c.pool.min = 0; c.pool.max = 2; c.pool.borrow_timeout_ms = 20;
        c.retry.max_attempts = 3; c.retry.initial_backoff_ms = 0; c.retry.max_backoff_ms = 0;
        c.async.threads = 2;
        config::DataSourceConfig d;
        d.name = "db"; d.type = "fault";
        c.datasources.push_back(d);
        require(client.init(c).ok(), "init");
        faults->dropRead = true;
        auto read = client.queryAsync("SELECT 1").get();
        require(read.status.ok() && faults->reads == 2 && faults->connects >= 2,
                "retryable reset must invalidate and reconnect before retrying a read");
        faults->ambiguousWrite = true;
        const auto write = client.executeAsync("UPDATE t SET value=1").get();
        require(!write.status.ok() && write.status.connectionBroken && faults->writes == 1,
                "ambiguous write must not be replayed by default");
        faults->ambiguousCommit = true;
        const auto tx = client.transactionAsync([](core::Session &) { return common::Status::OK(); }).get();
        require(!tx.status.ok() && faults->commits == 1,
                "ambiguous commit must remain an error and must not replay a transaction");

        std::promise<void> release;
        const auto gate = release.get_future().share();
        std::promise<void> enteredA, enteredB;
        auto hold = [&](std::promise<void> &entered) {
            return std::async(std::launch::async, [&client, &entered, gate] {
                return client.withSession([&entered, gate](core::Session &) {
                    entered.set_value(); gate.wait(); return common::Status::OK();
                });
            });
        };
        auto a = hold(enteredA), b = hold(enteredB);
        enteredA.get_future().wait(); enteredB.get_future().wait();
        auto starved = client.queryAsync("SELECT 1").get();
        release.set_value();
        require(a.get().ok() && b.get().ok(), "held sessions release cleanly");
        require(starved.status.code == common::ErrorCode::PoolExhausted, "exhausted async pool times out");
        require(client.queryAsync("SELECT 1").get().status.ok(), "pool recovers after pressure");

        std::atomic<bool> stop{false};
        std::atomic<int> unexpected{0};
        auto reader = std::async(std::launch::async, [&] {
            while (!stop.load(std::memory_order_acquire)) {
                const auto result = client.queryAsync("SELECT 1").get();
                if (!result.status.ok() && result.status.code != common::ErrorCode::PoolClosed)
                    ++unexpected;
                (void) client.diagnostics();
            }
        });
        bool reloadOk = true;
        for (int i = 0; i < 20; ++i) {
            if (!client.reload(c, std::chrono::milliseconds(0)).ok()) reloadOk = false;
        }
        stop.store(true, std::memory_order_release);
        reader.get();
        require(reloadOk && unexpected == 0, "concurrent reload must preserve structured completion");
        client.shutdown();
    }
}

int main() {
    sqlconduit::common::Logger::setMinLevel(sqlconduit::common::LogLevel::Error);
    try {
        int seconds = 0;
        if (const char *value = std::getenv("SQLCONDUIT_SOAK_SECONDS")) seconds = std::stoi(value);
        if (seconds < 0 || seconds > 10800) throw std::runtime_error("soak duration must be 0..10800 seconds");
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        std::uint64_t passes = 0;
        do { scenario(); ++passes; } while (std::chrono::steady_clock::now() < end);
        std::cout << "fault/reset/ambiguity/pressure/reload scenarios passed: " << passes << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
