#include "sqlconduit/core/connection_pool.h"
#include "sqlconduit/common/logger.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace sqlconduit::core
{
    namespace
    {
        void deliverBorrowResult(const AsyncIo& io,
                                 std::function<void(std::unique_ptr<ConnectionPool::Handle>,
                                                    common::Status)> complete,
                                 std::unique_ptr<ConnectionPool::Handle> handle,
                                 common::Status status)
        {
            io.deliver([handle = std::make_shared<std::unique_ptr<ConnectionPool::Handle>>(
                        std::move(handle)),
                    complete = std::move(complete),
                    status = std::move(status)]() mutable
                {
                    complete(std::move(*handle), std::move(status));
                });
        }

        std::string poolExhaustedMessage(const std::string& poolName, int maxConn,
                                         int total, int borrowed,
                                         std::chrono::milliseconds waited)
        {
            return "pool '" + poolName + "' exhausted: waited "
                + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    waited).count()) + "ms (max=" + std::to_string(maxConn)
                + ", total=" + std::to_string(total)
                + ", borrowed=" + std::to_string(borrowed) + ")";
        }
    }

    void ConnectionPool::State::returnConn(
        std::unique_ptr<IDatabaseConnection> conn,
        const std::chrono::steady_clock::time_point createdAt,
        const std::chrono::steady_clock::time_point borrowedAt,
        const bool reusable)
    {
        if (!conn) return;
        const auto now = std::chrono::steady_clock::now();
        const bool leaked = leakDetectionThreshold > std::chrono::milliseconds(0) &&
            now - borrowedAt >= leakDetectionThreshold;
        if (leaked)
        {
            SQLCONDUIT_LOG_WARN("pool [" + poolName + "] possible connection leak: borrowed for "
                + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - borrowedAt).count()) + "ms");
        }
        bool doClose = false;
        std::optional<AsyncWaiter> handoff;
        std::unique_ptr<ConnectionPool::Handle> handoffHandle;
        std::vector<std::pair<AsyncWaiter, common::Status>> expiredWaiters;
        {
            std::lock_guard lk(mtx);
            if (leaked) ++leakWarnings;
            if (borrowed > 0) --borrowed;
            const bool expired = maxLifetime > std::chrono::milliseconds(0) &&
                now - createdAt >= maxLifetime;
            if (closed || expired || !reusable || !pooled)
            {
                doClose = true;
                ++connectionsClosed;
                if (!reusable) ++invalidatedConnections;
                else if (expired) ++lifetimeEvictions;
                if (total > 0) --total;
            }
            else
            {
                for (auto it = asyncWaiters.begin(); it != asyncWaiters.end();)
                {
                    if (it->deadline <= now)
                    {
                        if (metricsEnabled) ++borrowTimeouts;
                        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - it->enqueuedAt);
                        auto waiter = std::move(*it);
                        it = asyncWaiters.erase(it);
                        expiredWaiters.emplace_back(
                            std::move(waiter),
                            common::Status::error(
                                common::ErrorCode::PoolExhausted,
                                poolExhaustedMessage(poolName, maxConnections,
                                                     total, borrowed, waited)));
                    }
                    else
                    {
                        ++it;
                    }
                }
                if (!asyncWaiters.empty())
                {
                    handoff = std::move(asyncWaiters.front());
                    asyncWaiters.pop_front();
                    ++borrowed;
                    if (metricsEnabled)
                    {
                        ++borrowSuccesses;
                        maxBorrowed = std::max(maxBorrowed, borrowed);
                    }
                    handoffHandle = std::unique_ptr<ConnectionPool::Handle>(
                        new ConnectionPool::Handle(weak_from_this(), std::move(conn),
                                                   createdAt, now));
                }
                else
                {
                    idle.push(IdleConnection{std::move(conn), createdAt, now});
                }
            }
        }
        if (handoff && handoffHandle)
        {
            deliverBorrowResult(handoff->io, std::move(handoff->complete),
                                std::move(handoffHandle), common::Status::OK());
            return;
        }
        for (auto& w : expiredWaiters)
        {
            deliverBorrowResult(w.first.io, std::move(w.first.complete), nullptr,
                                std::move(w.second));
        }
        if (doClose) conn->close();
        cv.notify_one();
    }

    ConnectionPool::ConnectionPool(std::unique_ptr<driver::IDriver> driver,
                                   config::DataSourceConfig cfg,
                                   const int minConn, const int maxConn,
                                   std::chrono::milliseconds borrowTimeout,
                                   std::chrono::milliseconds idleTimeout,
                                   std::chrono::milliseconds maxLifetime,
                                   std::chrono::milliseconds leakDetectionThreshold,
                                   std::chrono::milliseconds validationInterval,
                                   const bool metricsEnabled,
                                   const bool pooled)
        : state_(std::make_shared<State>()),
          driver_(std::move(driver)), cfg_(std::move(cfg)),
          minConn_(std::max(minConn, 0)),
          maxConn_(std::max(maxConn, 1)),
          borrowTimeout_(borrowTimeout),
          idleTimeout_(std::max(idleTimeout, std::chrono::milliseconds(0))),
          maxLifetime_(std::max(maxLifetime, std::chrono::milliseconds(0)))
    {
        if (minConn_ > maxConn_) minConn_ = maxConn_;
        state_->poolName = cfg_.name;
        state_->metricsEnabled = metricsEnabled;
        state_->minConnections = minConn_;
        state_->maxConnections = maxConn_;
        state_->maxLifetime = maxLifetime_;
        state_->leakDetectionThreshold = std::max(leakDetectionThreshold, std::chrono::milliseconds(0));
        state_->validationInterval = std::max(validationInterval, std::chrono::milliseconds(0));
        state_->pooled = pooled;

        for (int i = 0; pooled && i < minConn_; ++i)
        {
            auto code = common::ErrorCode::Ok;
            std::string err;
            auto conn = createConnection(code, err);
            if (!conn)
            {
                {
                    std::lock_guard<std::mutex> lk(state_->mtx);
                    ++state_->connectionCreateFailures;
                }
                SQLCONDUIT_LOG_WARN("pool [" + name() + "] warmup failed: " + err);
                break;
            }
            bool overflow = false;
            {
                std::lock_guard<std::mutex> lk(state_->mtx);
                if (state_->total >= maxConn_) overflow = true;
                else
                {
                    const auto now = std::chrono::steady_clock::now();
                    state_->idle.push(State::IdleConnection{std::move(conn), now, now});
                    ++state_->total;
                    ++state_->connectionsCreated;
                }
            }
            if (overflow)
            {
                conn->close();
                break;
            }
        }
    }

    ConnectionPool::~ConnectionPool()
    {
        shutdown(std::chrono::milliseconds(0));
    }

    ConnectionPool::Handle::~Handle()
    {
        if (!conn_) return;
        if (const auto st = state_.lock())
        {
            st->returnConn(std::move(conn_), createdAt_, borrowedAt_,
                           reusable_.load());
        }
        else
        {
            conn_->close();
        }
    }

    std::unique_ptr<ConnectionPool::Handle> ConnectionPool::borrow(std::string& error) const
    {
        auto code = common::ErrorCode::Ok;
        return borrow(code, error);
    }

    std::unique_ptr<ConnectionPool::Handle> ConnectionPool::borrow(common::ErrorCode& code,
                                                                   std::string& error,
                                                                   std::chrono::milliseconds timeout) const
    {
        if (timeout < std::chrono::milliseconds(0)) timeout = borrowTimeout_;
        if (timeout < std::chrono::milliseconds(0)) timeout = std::chrono::milliseconds(0);

        code = common::ErrorCode::Ok;
        error.clear();

        const auto borrowStarted = std::chrono::steady_clock::now();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lk(state_->mtx);
        const bool metrics = state_->metricsEnabled;
        if (metrics) ++state_->borrowRequests;
        const auto recordWait = [&]
        {
            if (!metrics) return;
            const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - borrowStarted);
            state_->totalBorrowWait += waited;
            state_->maxBorrowWait = std::max(state_->maxBorrowWait, waited);
        };
        const auto recordSuccess = [&]
        {
            recordWait();
            if (!metrics) return;
            ++state_->borrowSuccesses;
            state_->maxBorrowed = std::max(state_->maxBorrowed, state_->borrowed);
        };

        if (!state_->pooled)
        {
            if (state_->closed)
            {
                code = common::ErrorCode::PoolClosed;
                error = "datasource '" + name() + "' is closed";
                recordWait();
                return nullptr;
            }
            lk.unlock();
            auto conn = createConnection(code, error);
            lk.lock();
            if (!conn)
            {
                ++state_->connectionCreateFailures;
                recordWait();
                return nullptr;
            }
            if (state_->closed)
            {
                conn->close();
                code = common::ErrorCode::PoolClosed;
                error = "datasource '" + name() + "' is closed";
                recordWait();
                return nullptr;
            }
            ++state_->total;
            ++state_->borrowed;
            ++state_->connectionsCreated;
            recordSuccess();
            const auto now = std::chrono::steady_clock::now();
            return std::unique_ptr<Handle>(new Handle(state_, std::move(conn), now, now));
        }

        for (;;)
        {
            if (state_->closed)
            {
                code = common::ErrorCode::PoolClosed;
                error = "pool '" + name() + "' is closed";
                recordWait();
                return nullptr;
            }

            if (!state_->idle.empty())
            {
                auto item = std::move(state_->idle.front());
                state_->idle.pop();

                lk.unlock();
                const auto now = std::chrono::steady_clock::now();
                const bool expired = maxLifetime_ > std::chrono::milliseconds(0) &&
                    now - item.createdAt >= maxLifetime_;
                const bool needsPing = state_->validationInterval <=
                    std::chrono::milliseconds(0) ||
                    (now - item.lastValidated) >=
                    state_->validationInterval;
                const bool alive = !expired && item.conn &&
                    (!needsPing || item.conn->ping().ok());
                if (alive) item.lastValidated = now;
                if (!alive && item.conn) item.conn->close();
                lk.lock();

                if (alive)
                {
                    if (state_->closed)
                    {
                        code = common::ErrorCode::PoolClosed;
                        error = "pool '" + name() + "' is closed";
                        item.conn->close();
                        recordWait();
                        return nullptr;
                    }
                    ++state_->borrowed;
                    recordSuccess();
                    return std::unique_ptr<Handle>(new Handle(
                        state_, std::move(item.conn), item.createdAt,
                        std::chrono::steady_clock::now()));
                }

                --state_->total;
                ++state_->connectionsClosed;
                if (expired) ++state_->lifetimeEvictions;
                else ++state_->validationFailures;
                if (state_->total < 0) state_->total = 0;
                state_->cv.notify_one();
                continue;
            }

            if (state_->total < maxConn_)
            {
                ++state_->total;

                lk.unlock();
                auto conn = createConnection(code, error);
                lk.lock();

                if (conn)
                {
                    if (state_->closed)
                    {
                        code = common::ErrorCode::PoolClosed;
                        error = "pool '" + name() + "' is closed";
                        conn->close();
                        --state_->total;
                        recordWait();
                        return nullptr;
                    }
                    ++state_->borrowed;
                    ++state_->connectionsCreated;
                    recordSuccess();
                    const auto now = std::chrono::steady_clock::now();
                    return std::unique_ptr<Handle>(new Handle(
                        state_, std::move(conn), now, now));
                }

                --state_->total;
                ++state_->connectionCreateFailures;
                if (state_->total < 0) state_->total = 0;
                state_->cv.notify_one();
                recordWait();
                return nullptr;
            }

            if (timeout == std::chrono::milliseconds(0))
            {
                if (metrics) ++state_->borrowTimeouts;
                code = common::ErrorCode::PoolExhausted;
                error = "pool '" + name() + "' exhausted: waited "
                    + std::to_string(timeout.count()) + "ms (max="
                    + std::to_string(maxConn_) + ", total="
                    + std::to_string(state_->total) + ", borrowed="
                    + std::to_string(state_->borrowed) + ")";
                recordWait();
                return nullptr;
            }
            ++state_->waiting;
            if (metrics) state_->maxWaiting = std::max(state_->maxWaiting, state_->waiting);
            const auto waitResult = state_->cv.wait_until(lk, deadline);
            --state_->waiting;
            if (waitResult == std::cv_status::timeout)
            {
                if (metrics) ++state_->borrowTimeouts;
                code = common::ErrorCode::PoolExhausted;
                error = "pool '" + name() + "' exhausted: waited "
                    + std::to_string(timeout.count()) + "ms (max="
                    + std::to_string(maxConn_) + ", total="
                    + std::to_string(state_->total) + ", borrowed="
                    + std::to_string(state_->borrowed) + ")";
                recordWait();
                return nullptr;
            }
        }
    }

    void ConnectionPool::expireWaiters() const
    {
        std::vector<std::pair<State::AsyncWaiter, common::Status>> expired;
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            if (state_->asyncWaiters.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            for (auto it = state_->asyncWaiters.begin();
                 it != state_->asyncWaiters.end();)
            {
                if (it->deadline <= now)
                {
                    if (state_->metricsEnabled) ++state_->borrowTimeouts;
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->enqueuedAt);
                    auto waiter = std::move(*it);
                    it = state_->asyncWaiters.erase(it);
                    expired.emplace_back(
                        std::move(waiter),
                        common::Status::error(
                            common::ErrorCode::PoolExhausted,
                            poolExhaustedMessage(name(), maxConn_, state_->total,
                                                 state_->borrowed, waited)));
                }
                else
                {
                    ++it;
                }
            }
        }
        for (auto& w : expired)
        {
            deliverBorrowResult(w.first.io, std::move(w.first.complete), nullptr,
                                std::move(w.second));
        }
    }

    void ConnectionPool::borrowAsync(const std::chrono::milliseconds timeout,
                                     const AsyncIo& io,
                                     std::function<void(std::unique_ptr<Handle>,
                                                        common::Status)> complete) const
    {
        if (!io.usable() || !complete)
        {
            SQLCONDUIT_LOG_WARN("pool [" + name() + "] borrowAsync called with unusable AsyncIo");
            return;
        }
        auto effective = timeout;
        if (effective < std::chrono::milliseconds(0)) effective = borrowTimeout_;
        if (effective < std::chrono::milliseconds(0)) effective = std::chrono::milliseconds(0);

        expireWaiters();

        const auto now = std::chrono::steady_clock::now();
        const auto deadline = now + effective;

        std::unique_lock<std::mutex> lk(state_->mtx);
        if (state_->metricsEnabled) ++state_->borrowRequests;

        if (state_->closed)
        {
            lk.unlock();
            deliverBorrowResult(io, std::move(complete), nullptr,
                                common::Status::error(common::ErrorCode::PoolClosed,
                                                      "pool '" + name() + "' is closed"));
            return;
        }

        if (!state_->pooled)
        {
            lk.unlock();
            postCreateTask(io, std::move(complete), false);
            return;
        }

        if (!state_->idle.empty())
        {
            auto item = std::move(state_->idle.front());
            state_->idle.pop();
            const bool expired = maxLifetime_ > std::chrono::milliseconds(0) &&
                now - item.createdAt >= maxLifetime_;
            const bool needsPing = state_->validationInterval <= std::chrono::milliseconds(0) ||
                (now - item.lastValidated) >= state_->validationInterval;
            if (expired)
            {
                --state_->total;
                ++state_->connectionsClosed;
                ++state_->lifetimeEvictions;
                if (state_->total < 0) state_->total = 0;
                lk.unlock();
                if (item.conn) item.conn->close();
                borrowAsync(effective, io, std::move(complete));
                return;
            }
            if (!needsPing)
            {
                ++state_->borrowed;
                if (state_->metricsEnabled)
                {
                    ++state_->borrowSuccesses;
                    state_->maxBorrowed = std::max(state_->maxBorrowed, state_->borrowed);
                }
                lk.unlock();
                auto h = std::unique_ptr<Handle>(
                    new Handle(state_, std::move(item.conn), item.createdAt, now));
                deliverBorrowResult(io, std::move(complete), std::move(h),
                                    common::Status::OK());
                return;
            }
            lk.unlock();
            auto connBox = std::make_shared<std::unique_ptr<IDatabaseConnection>>(
                std::move(item.conn));
            io.post([self = shared_from_this(), st = state_, connBox,
                    createdAt = item.createdAt,
                    io, complete = std::move(complete),
                    effective]() mutable
                {
                    auto& conn = *connBox;
                    const bool alive = static_cast<bool>(conn) && conn->ping().ok();
                    const auto finishedAt = std::chrono::steady_clock::now();
                    std::unique_ptr<Handle> handle;
                    bool closed = false;
                    {
                        std::lock_guard<std::mutex> lk2(st->mtx);
                        if (st->closed)
                        {
                            closed = true;
                        }
                        else if (alive)
                        {
                            ++st->borrowed;
                            if (st->metricsEnabled)
                            {
                                ++st->borrowSuccesses;
                                st->maxBorrowed = std::max(st->maxBorrowed, st->borrowed);
                            }
                            handle = std::unique_ptr<Handle>(
                                new Handle(st->weak_from_this(), std::move(conn),
                                           createdAt, finishedAt));
                        }
                    }
                    if (closed)
                    {
                        if (conn) conn->close();
                        deliverBorrowResult(io, std::move(complete), nullptr,
                                            common::Status::error(
                                                common::ErrorCode::PoolClosed,
                                                "pool is closed"));
                        return;
                    }
                    if (handle)
                    {
                        deliverBorrowResult(io, std::move(complete), std::move(handle),
                                            common::Status::OK());
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lk2(st->mtx);
                        --st->total;
                        ++st->connectionsClosed;
                        ++st->validationFailures;
                        if (st->total < 0) st->total = 0;
                    }
                    if (conn) conn->close();
                    self->borrowAsync(effective, io, std::move(complete));
                });
            return;
        }

        if (state_->total < maxConn_)
        {
            ++state_->total;
            lk.unlock();
            postCreateTask(io, std::move(complete), true);
            return;
        }

        if (effective == std::chrono::milliseconds(0))
        {
            if (state_->metricsEnabled) ++state_->borrowTimeouts;
            const auto msg = poolExhaustedMessage(name(), maxConn_,
                                                  state_->total, state_->borrowed,
                                                  std::chrono::milliseconds(0));
            lk.unlock();
            deliverBorrowResult(io, std::move(complete), nullptr,
                                common::Status::error(common::ErrorCode::PoolExhausted, msg));
            return;
        }
        state_->asyncWaiters.push_back(
            State::AsyncWaiter{now, deadline, std::move(complete), io});
    }

    void ConnectionPool::postCreateTask(
        const AsyncIo& io,
        std::function<void(std::unique_ptr<Handle>, common::Status)> complete,
        const bool slotReserved) const
    {
        io.post([self = shared_from_this(), st = state_, io,
                complete = std::move(complete), slotReserved]() mutable
            {
                common::ErrorCode code = common::ErrorCode::Ok;
                std::string err;
                auto conn = self->createConnection(code, err);
                std::unique_ptr<Handle> handle;
                common::Status status;
                bool doClose = false;
                {
                    std::lock_guard<std::mutex> lk(st->mtx);
                    if (st->closed)
                    {
                        if (conn) doClose = true;
                        status = common::Status::error(common::ErrorCode::PoolClosed,
                                                       "pool '" + st->poolName + "' is closed");
                        if (slotReserved && st->total > 0) --st->total;
                    }
                    else if (!conn)
                    {
                        if (slotReserved)
                        {
                            --st->total;
                            ++st->connectionCreateFailures;
                            if (st->total < 0) st->total = 0;
                        }
                        else
                        {
                            ++st->connectionCreateFailures;
                        }
                        status = common::Status::error(code, err);
                        if (code == common::ErrorCode::ConnectionFailed)
                        {
                            status.retryable = true;
                            status.connectionBroken = true;
                        }
                    }
                    else
                    {
                        if (!slotReserved) ++st->total;
                        ++st->borrowed;
                        ++st->connectionsCreated;
                        if (st->metricsEnabled)
                        {
                            ++st->borrowSuccesses;
                            st->maxBorrowed = std::max(st->maxBorrowed, st->borrowed);
                        }
                        const auto createdAt = std::chrono::steady_clock::now();
                        handle = std::unique_ptr<Handle>(
                            new Handle(st->weak_from_this(), std::move(conn),
                                       createdAt, createdAt));
                        status = common::Status::OK();
                    }
                }
                if (doClose && conn) conn->close();
                deliverBorrowResult(io, std::move(complete), std::move(handle),
                                    std::move(status));
            });
    }

    void ConnectionPool::healthCheck() const
    {
        if (!state_->pooled) return;

        expireWaiters();

        const size_t snapshot = idleCount();
        for (size_t i = 0; i < snapshot; ++i)
        {
            State::IdleConnection item;
            bool retireForIdle = false;
            {
                std::lock_guard<std::mutex> lk(state_->mtx);
                if (state_->closed || state_->idle.empty()) break;
                item = std::move(state_->idle.front());
                state_->idle.pop();
                retireForIdle = idleTimeout_ > std::chrono::milliseconds(0) &&
                    std::chrono::steady_clock::now() - item.returnedAt >= idleTimeout_ &&
                    state_->total > minConn_;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool expired = maxLifetime_ > std::chrono::milliseconds(0) &&
                now - item.createdAt >= maxLifetime_;
            const bool alive = !retireForIdle && !expired && item.conn && item.conn->ping().ok();

            {
                std::lock_guard<std::mutex> lk(state_->mtx);
                if (alive && !state_->closed)
                {
                    state_->idle.push(std::move(item));
                }
                else
                {
                    if (item.conn) item.conn->close();
                    --state_->total;
                    ++state_->connectionsClosed;
                    if (retireForIdle) ++state_->idleEvictions;
                    else if (expired) ++state_->lifetimeEvictions;
                    else ++state_->validationFailures;
                    if (state_->total < 0) state_->total = 0;
                }
            }
        }

        int need = 0;
        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            if (state_->closed) return;
            const int want = minConn_ - state_->total;
            const int room = maxConn_ - state_->total;
            need = std::min(std::max(want, 0), std::max(room, 0));
            state_->total += need;
        }

        std::vector<State::IdleConnection> fresh;
        fresh.reserve(static_cast<size_t>(need));
        for (int i = 0; i < need; ++i)
        {
            auto code = common::ErrorCode::Ok;
            std::string err;
            auto conn = createConnection(code, err);
            if (!conn)
            {
                {
                    std::lock_guard<std::mutex> lk(state_->mtx);
                    ++state_->connectionCreateFailures;
                }
                SQLCONDUIT_LOG_WARN("pool [" + name() + "] heartbeat refill failed: " + err);
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            fresh.push_back(State::IdleConnection{std::move(conn), now, now});
        }

        {
            std::lock_guard<std::mutex> lk(state_->mtx);
            state_->total -= (need - static_cast<int>(fresh.size()));
            state_->connectionsCreated += fresh.size();
            if (state_->total < 0) state_->total = 0;
            for (auto& item : fresh)
            {
                if (state_->closed) item.conn->close();
                else state_->idle.push(std::move(item));
            }
            if (!fresh.empty()) state_->cv.notify_all();
        }
    }

    void ConnectionPool::shutdown(const std::chrono::milliseconds grace) const
    {
        std::vector<std::pair<State::AsyncWaiter, common::Status>> waiters;
        std::unique_lock<std::mutex> lk(state_->mtx);
        state_->closed = true;
        state_->cv.notify_all();
        for (auto& w : state_->asyncWaiters)
        {
            waiters.emplace_back(
                std::move(w),
                common::Status::error(common::ErrorCode::PoolClosed,
                                      "pool '" + name() + "' is closed"));
        }
        state_->asyncWaiters.clear();

        while (!state_->idle.empty())
        {
            auto item = std::move(state_->idle.front());
            state_->idle.pop();
            lk.unlock();
            item.conn->close();
            lk.lock();
            ++state_->connectionsClosed;
        }

        if (grace > std::chrono::milliseconds(0))
        {
            const auto deadline = std::chrono::steady_clock::now() + grace;
            while (state_->borrowed > 0)
            {
                if (state_->cv.wait_until(lk, deadline) == std::cv_status::timeout) break;
            }
        }

        state_->total = state_->borrowed;
        lk.unlock();

        for (auto& w : waiters)
        {
            deliverBorrowResult(w.first.io, std::move(w.first.complete), nullptr,
                                std::move(w.second));
        }
    }

    size_t ConnectionPool::idleCount() const
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        return state_->idle.size();
    }

    size_t ConnectionPool::totalCount() const
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        return static_cast<size_t>(state_->total);
    }

    size_t ConnectionPool::borrowedCount() const
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        return static_cast<size_t>(state_->borrowed);
    }

    bool ConnectionPool::closed() const
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        return state_->closed;
    }

    ConnectionPool::Stats ConnectionPool::stats() const
    {
        std::lock_guard<std::mutex> lk(state_->mtx);
        Stats out;
        out.minConnections = static_cast<size_t>(std::max(0, state_->minConnections));
        out.maxConnections = static_cast<size_t>(std::max(0, state_->maxConnections));
        out.idle = state_->idle.size();
        out.total = static_cast<size_t>(std::max(0, state_->total));
        out.borrowed = static_cast<size_t>(std::max(0, state_->borrowed));
        out.waiting = static_cast<size_t>(std::max(0, state_->waiting));
        out.asyncWaiting = state_->asyncWaiters.size();
        if (!state_->metricsEnabled) return out;
        out.connectionsCreated = state_->connectionsCreated;
        out.connectionsClosed = state_->connectionsClosed;
        out.borrowTimeouts = state_->borrowTimeouts;
        out.validationFailures = state_->validationFailures;
        out.leakWarnings = state_->leakWarnings;
        out.maxBorrowed = static_cast<size_t>(std::max(0, state_->maxBorrowed));
        out.maxWaiting = static_cast<size_t>(std::max(0, state_->maxWaiting));
        out.borrowRequests = state_->borrowRequests;
        out.borrowSuccesses = state_->borrowSuccesses;
        out.connectionCreateFailures = state_->connectionCreateFailures;
        out.invalidatedConnections = state_->invalidatedConnections;
        out.idleEvictions = state_->idleEvictions;
        out.lifetimeEvictions = state_->lifetimeEvictions;
        out.totalBorrowWait = state_->totalBorrowWait;
        out.maxBorrowWait = state_->maxBorrowWait;
        return out;
    }

    std::unique_ptr<IDatabaseConnection> ConnectionPool::createConnection(common::ErrorCode& code,
                                                                          std::string& error) const
    {
        code = common::ErrorCode::Ok;
        error.clear();

        auto conn = driver_->createConnection();
        if (!conn)
        {
            code = common::ErrorCode::ConnectionFailed;
            error = "driver '" + std::string(driver_->name()) + "' returned a null connection";
            return nullptr;
        }

        if (const auto st = conn->connect(cfg_); !st.ok())
        {
            code = st.code;
            error = st.message;
            return nullptr;
        }
        return conn;
    }
}
