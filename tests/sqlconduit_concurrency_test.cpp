#include "sqlconduit/async/executor.h"
#include "sqlconduit/core/connection_pool.h"
#include "sqlconduit/driver/idriver.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace sqlconduit;

    int failures = 0;

    void check(const bool condition, const std::string &name) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!condition) ++failures;
    }

    class TrackedConnection final : public core::IDatabaseConnection {
    public:
        static std::atomic<int> alive;
        static std::atomic<std::uint64_t> created;
        static std::atomic<std::uint64_t> closed;

        ~TrackedConnection() override { close(); }

        common::Status connect(const config::DataSourceConfig &) override {
            if (!open_) {
                open_ = true;
                ++alive;
                ++created;
            }
            return common::Status::OK();
        }

        common::Status ping() override {
            return open_ ? common::Status::OK()
                         : common::Status::error(common::ErrorCode::NotConnected, "closed");
        }

        common::Status query(const std::string &, common::ResultSet &out) override {
            out.clear();
            return ping();
        }

        common::Status execute(const std::string &, std::int64_t &affected) override {
            affected = open_ ? 1 : 0;
            return ping();
        }

        common::Status begin() override { return ping(); }
        common::Status commit() override { return ping(); }
        common::Status rollback() override { return ping(); }

        void close() override {
            if (open_) {
                open_ = false;
                --alive;
                ++closed;
            }
        }

        [[nodiscard]] bool isOpen() const override { return open_; }
        [[nodiscard]] bool inTransaction() const override { return false; }

    private:
        bool open_ = false;
    };

    std::atomic<int> TrackedConnection::alive{0};
    std::atomic<std::uint64_t> TrackedConnection::created{0};
    std::atomic<std::uint64_t> TrackedConnection::closed{0};

    class TrackedDriver final : public driver::IDriver {
    public:
        [[nodiscard]] const char *name() const override { return "tracked"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<TrackedConnection>();
        }
    };

    std::shared_ptr<core::ConnectionPool> makePool() {
        config::DataSourceConfig config;
        config.name = "concurrency";
        config.type = "tracked";
        return std::make_shared<core::ConnectionPool>(
            std::make_unique<TrackedDriver>(), config, 0, 4,
            std::chrono::milliseconds(2000), std::chrono::milliseconds(0),
            std::chrono::milliseconds(0), std::chrono::milliseconds(0),
            std::chrono::hours(1), true, true);
    }
}

int main() {
    TrackedConnection::alive = 0;
    TrackedConnection::created = 0;
    TrackedConnection::closed = 0;

    {
        auto pool = makePool();
        constexpr int threadCount = 12;
        constexpr int iterations = 1500;
        std::atomic<bool> start{false};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> errors{0};
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (int thread = 0; thread < threadCount; ++thread) {
            workers.emplace_back([&] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < iterations; ++i) {
                    common::ErrorCode code = common::ErrorCode::Unknown;
                    std::string error;
                    auto handle = pool->borrow(code, error);
                    if (!handle || !(*handle)->ping().ok()) {
                        ++errors;
                        continue;
                    }
                    ++completed;
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto &worker: workers) worker.join();

        const auto stats = pool->stats();
        check(errors == 0 && completed == threadCount * iterations,
              "contended connection borrows all complete successfully");
        check(stats.borrowed == 0 && stats.total <= 4 && stats.maxBorrowed <= 4,
              "pool counters and maximum size remain consistent under contention");
        pool->shutdown(std::chrono::milliseconds(2000));
        check(TrackedConnection::alive == 0,
              "pool shutdown releases every connection after contention");
    }
    check(TrackedConnection::created == TrackedConnection::closed,
          "connection lifecycle counters prove balanced creation and close");

    {
        auto executor = async::makeThreadPoolExecutor(4, 64);
        constexpr int producers = 6;
        constexpr int tasksPerProducer = 1000;
        std::atomic<std::uint64_t> accepted{0};
        std::atomic<std::uint64_t> executed{0};
        std::vector<std::thread> threads;
        threads.reserve(producers);
        for (int producer = 0; producer < producers; ++producer) {
            threads.emplace_back([&] {
                for (int i = 0; i < tasksPerProducer; ++i) {
                    while (!executor->tryPost([&executed] { ++executed; })) {
                        std::this_thread::yield();
                    }
                    ++accepted;
                }
            });
        }
        for (auto &thread: threads) thread.join();
        executor->shutdown(std::chrono::milliseconds(5000));
        const auto stats = executor->stats();
        check(accepted == producers * tasksPerProducer && executed == accepted,
              "concurrent executor submissions drain without lost tasks");
        check(stats.active == 0 && stats.queueDepth == 0 && stats.completed == accepted,
              "executor statistics stay consistent after concurrent submission");
    }

    {
        auto executor = async::makeThreadPoolExecutor(1, 4);
        auto lifetime = std::make_shared<int>(42);
        std::weak_ptr<int> observer = lifetime;
        executor->postAfter([captured = lifetime] { (void) captured; },
                            std::chrono::hours(1));
        lifetime.reset();
        executor->shutdown(std::chrono::milliseconds(0));
        check(observer.expired(),
              "shutdown releases captures owned by cancelled delayed tasks");
    }

    for (int cycle = 0; cycle < 50; ++cycle) {
        auto pool = makePool();
        common::ErrorCode code = common::ErrorCode::Unknown;
        std::string error;
        auto handle = pool->borrow(code, error);
        handle.reset();
        pool->shutdown(std::chrono::milliseconds(0));
    }
    check(TrackedConnection::alive == 0 &&
              TrackedConnection::created == TrackedConnection::closed,
          "repeated pool lifecycle leaves no live connection resources");

    std::cout << "concurrency and lifecycle stress tests: "
              << (failures == 0 ? "passed" : "failed") << '\n';
    return failures == 0 ? 0 : 1;
}
