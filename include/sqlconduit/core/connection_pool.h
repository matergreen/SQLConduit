#ifndef SQLCONDUIT_CORE_CONNECTION_POOL_H
#define SQLCONDUIT_CORE_CONNECTION_POOL_H

#include "sqlconduit/common/connection_pool_stats.h"
#include "sqlconduit/core/idatabase_connection.h"
#include "sqlconduit/config/datasource_config.h"
#include "sqlconduit/driver/idriver.h"

#include <cstdint>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace sqlconduit::core {
    struct AsyncIo {
        std::function<void(std::function<void()>)> post;
        std::function<void(std::function<void()>)> deliver;

        [[nodiscard]] bool usable() const { return static_cast<bool>(post) && static_cast<bool>(deliver); }
    };

    class ConnectionPool : public std::enable_shared_from_this<ConnectionPool> {
        struct State;

    public:
        using Stats = common::ConnectionPoolStats;

        static constexpr std::chrono::milliseconds kDefaultBorrowTimeout{30000};

        ConnectionPool(std::unique_ptr<driver::IDriver> driver,
                       config::DataSourceConfig cfg,
                       int minConn, int maxConn,
                       std::chrono::milliseconds borrowTimeout = kDefaultBorrowTimeout,
                       std::chrono::milliseconds idleTimeout = std::chrono::milliseconds(0),
                       std::chrono::milliseconds maxLifetime = std::chrono::milliseconds(0),
                       std::chrono::milliseconds leakDetectionThreshold =
                               std::chrono::milliseconds(0),
                       std::chrono::milliseconds validationInterval =
                               std::chrono::milliseconds(30000),
                       bool metricsEnabled = true,
                       bool pooled = true);

        ~ConnectionPool();

        ConnectionPool(const ConnectionPool &) = delete;

        ConnectionPool &operator=(const ConnectionPool &) = delete;

        class Handle {
        public:
            ~Handle();

            Handle(const Handle &) = delete;

            Handle &operator=(const Handle &) = delete;

            Handle(Handle &&other) noexcept
                : state_(std::move(other.state_)), conn_(std::move(other.conn_)),
                  createdAt_(other.createdAt_), borrowedAt_(other.borrowedAt_),
                  reusable_(other.reusable_.load()) {
            }

            Handle &operator=(Handle &&other) noexcept {
                if (this != &other) {
                    state_ = std::move(other.state_);
                    conn_ = std::move(other.conn_);
                    createdAt_ = other.createdAt_;
                    borrowedAt_ = other.borrowedAt_;
                    reusable_.store(other.reusable_.load());
                }
                return *this;
            }

            IDatabaseConnection *operator->() const { return conn_.get(); }
            [[nodiscard]] IDatabaseConnection *get() const { return conn_.get(); }

            void invalidate() { reusable_.store(false); }

            [[nodiscard]] bool reusable() const { return reusable_.load(); }

        private:
            friend class ConnectionPool;

            Handle(std::weak_ptr<State> state, std::unique_ptr<IDatabaseConnection> conn,
                   std::chrono::steady_clock::time_point createdAt,
                   std::chrono::steady_clock::time_point borrowedAt)
                : state_(std::move(state)), conn_(std::move(conn)),
                  createdAt_(createdAt), borrowedAt_(borrowedAt) {
            }

            std::weak_ptr<State> state_;
            std::unique_ptr<IDatabaseConnection> conn_;
            std::chrono::steady_clock::time_point createdAt_;
            std::chrono::steady_clock::time_point borrowedAt_;
            std::atomic<bool> reusable_{true};
        };

        std::unique_ptr<Handle> borrow(common::ErrorCode &code, std::string &error,
                                       std::chrono::milliseconds timeout =
                                               std::chrono::milliseconds(-1)) const;

        std::unique_ptr<Handle> borrow(std::string &error) const;

        void borrowAsync(std::chrono::milliseconds timeout,
                         const AsyncIo &io,
                         std::function<void(std::unique_ptr<Handle>, common::Status)> complete) const;

        void healthCheck() const;

        void shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(5000)) const;

        [[nodiscard]] const std::string &name() const { return cfg_.name; }

        [[nodiscard]] size_t idleCount() const;

        [[nodiscard]] size_t totalCount() const;

        [[nodiscard]] size_t borrowedCount() const;

        [[nodiscard]] bool closed() const;

        [[nodiscard]] Stats stats() const;

    private:
        struct State : std::enable_shared_from_this<State> {
            struct IdleConnection {
                std::unique_ptr<IDatabaseConnection> conn;
                std::chrono::steady_clock::time_point createdAt;
                std::chrono::steady_clock::time_point returnedAt;
                std::chrono::steady_clock::time_point lastValidated{};
            };

            struct AsyncWaiter {
                std::chrono::steady_clock::time_point enqueuedAt;
                std::chrono::steady_clock::time_point deadline;
                std::function<void(std::unique_ptr<Handle>, common::Status)> complete;
                AsyncIo io;
            };

            std::mutex mtx;
            std::condition_variable cv;
            std::queue<IdleConnection> idle;
            int total = 0;
            int borrowed = 0;
            int waiting = 0;
            bool closed = false;
            bool metricsEnabled = true;
            bool pooled = true;
            std::string poolName;
            std::chrono::milliseconds maxLifetime{0};
            std::chrono::milliseconds leakDetectionThreshold{0};
            std::chrono::milliseconds validationInterval{30000};
            std::uint64_t connectionsCreated = 0;
            std::uint64_t connectionsClosed = 0;
            std::uint64_t borrowTimeouts = 0;
            std::uint64_t validationFailures = 0;
            std::uint64_t leakWarnings = 0;
            int minConnections = 0;
            int maxConnections = 0;
            int maxBorrowed = 0;
            int maxWaiting = 0;
            std::uint64_t borrowRequests = 0;
            std::uint64_t borrowSuccesses = 0;
            std::uint64_t connectionCreateFailures = 0;
            std::uint64_t invalidatedConnections = 0;
            std::uint64_t idleEvictions = 0;
            std::uint64_t lifetimeEvictions = 0;
            std::chrono::microseconds totalBorrowWait{0};
            std::chrono::microseconds maxBorrowWait{0};
            std::deque<AsyncWaiter> asyncWaiters;

            void returnConn(std::unique_ptr<IDatabaseConnection> conn,
                            std::chrono::steady_clock::time_point createdAt,
                            std::chrono::steady_clock::time_point borrowedAt,
                            bool reusable);
        };

        std::unique_ptr<IDatabaseConnection> createConnection(common::ErrorCode &code,
                                                              std::string &error) const;

        void expireWaiters() const;

        void postCreateTask(const AsyncIo &io,
                            std::function<void(std::unique_ptr<Handle>, common::Status)> complete,
                            bool slotReserved) const;

        std::shared_ptr<State> state_;
        std::unique_ptr<driver::IDriver> driver_;
        config::DataSourceConfig cfg_;
        int minConn_;
        int maxConn_;
        std::chrono::milliseconds borrowTimeout_;
        std::chrono::milliseconds idleTimeout_;
        std::chrono::milliseconds maxLifetime_;
    };
}

#endif
