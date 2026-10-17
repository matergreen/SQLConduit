#include "async_engine.h"

#include "sqlconduit/core/interceptor.h"
#include "sqlconduit/core/runtime_services.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

namespace sqlconduit::async::detail {
    namespace {
        core::AsyncIo normalizedIo(core::AsyncIo io) {
            return io;
        }

        struct NativeQueryControl {
            std::atomic<bool> resolved{false};
            std::atomic<bool> driverCompleted{false};
            std::atomic<bool> timeoutDelivered{false};
            std::mutex handleMutex;
            std::shared_ptr<std::unique_ptr<core::ConnectionPool::Handle> > handle;
        };
    }

    bool AsyncEngine::directLeafEligible(const core::DataSource &source,
                                         const bool query) {
        (void) query;
        if (source.primary_ || source.shadow_)
            return false;
        return !source.pool_.expired();
    }

    bool AsyncEngine::query(const std::shared_ptr<core::DataSource> &source,
                            std::string sql, common::Params params,
                            common::SqlContext context,
                            const std::chrono::milliseconds borrowTimeout,
                            core::AsyncIo io, QueryCompletion completion) {
        if (!source || !io.usable() || !completion) return false;
        if (source->primary_) {
            return queryGroup(source, std::move(sql), std::move(params),
                              std::move(context), borrowTimeout,
                              std::move(io), std::move(completion));
        }
        return queryLeaf(source, std::move(sql), std::move(params),
                         std::move(context), borrowTimeout, std::move(io),
                         true, true, std::move(completion));
    }

    bool AsyncEngine::queryLeaf(const std::shared_ptr<core::DataSource> &source,
                                std::string sql, common::Params params,
                                common::SqlContext context,
                                const std::chrono::milliseconds borrowTimeout,
                                core::AsyncIo io, const bool applyPreGate,
                                const bool runInterceptors,
                                QueryCompletion completion) {
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, true))
            return false;

        struct RetryState {
            std::shared_ptr<core::DataSource> source;
            std::string sql;
            common::Params params;
            common::SqlContext context;
            std::chrono::milliseconds timeout;
            std::chrono::steady_clock::time_point started;
            core::AsyncIo io;
            QueryCompletion completion;
            int attempts = 1;
            bool applyPreGate = true;
            bool runInterceptors = true;
            std::function<void(int, bool)> run;
        };

        auto state = std::make_shared<RetryState>();
        state->source = source;
        state->sql = std::move(sql);
        state->params = std::move(params);
        state->context = std::move(context);
        state->timeout = borrowTimeout;
        state->started = std::chrono::steady_clock::now();
        state->io = std::move(io);
        state->completion = std::move(completion);
        state->attempts = std::max(1, source->retry_.max_attempts);
        state->applyPreGate = applyPreGate;
        state->runInterceptors = runInterceptors;

        std::weak_ptr<RetryState> weak = state;
        state->run = [weak](const int attempt, const bool applyPreGate) {
            const auto state = weak.lock();
            if (!state) return;
            auto remaining = state->timeout;
            if (state->timeout > std::chrono::milliseconds(0)) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - state->started);
                remaining = state->timeout - elapsed;
                if (remaining <= std::chrono::milliseconds(0)) {
                    QueryResult result;
                    result.status = common::Status::error(
                        common::ErrorCode::QueryTimeout,
                        "query retries exceeded client async statement timeout");
                    result.status.retryable = true;
                    result.mode = ExecutionMode::CompatibilityFallback;
                    state->completion(std::move(result));
                    return;
                }
            }
            const bool started = AsyncEngine::queryOne(
                state->source, state->sql, state->params, state->context,
                remaining, state->io,
                applyPreGate && state->applyPreGate, state->runInterceptors,
                [state, attempt](QueryResult result) mutable {
                    const bool retry = !result.status.ok() && result.status.retryable &&
                                       result.status.code != common::ErrorCode::CircuitOpen &&
                                       result.status.code != common::ErrorCode::Overloaded &&
                                       result.status.code != common::ErrorCode::QueryTimeout &&
                                       attempt < state->attempts;
                    if (!retry) {
                        state->completion(std::move(result));
                        return;
                    }
                    const auto delay = state->source->retryDelay(attempt);
                    state->source->services_->phase(common::ExecutionPhase::Retry, state->context,
                        {},
                        result.status.code, common::FallbackReason::None, attempt);
                    state->io.postAfter(
                        [state, next = attempt + 1] { state->run(next, false); }, delay);
                });
            if (!started) {
                QueryResult result;
                result.status = common::Status::error(
                    common::ErrorCode::PoolClosed,
                    "datasource pool closed before an asynchronous query attempt");
                result.status.retryable = true;
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
            }
        };
        state->run(1, true);
        return true;
    }

    bool AsyncEngine::queryOne(const std::shared_ptr<core::DataSource> &source,
                               std::string sql, common::Params params,
                               common::SqlContext context,
                               const std::chrono::milliseconds borrowTimeout,
                               core::AsyncIo io, const bool applyPreGate,
                               const bool runInterceptors,
                               QueryCompletion completion) {
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, true))
            return false;

        auto pool = source->pool_.lock();
        if (!pool) return false;
        io = normalizedIo(std::move(io));
        const auto operationStarted = std::chrono::steady_clock::now();
        auto start = [source, pool = std::move(pool), sql = std::move(sql),
                 params = std::move(params), context = std::move(context),
                 borrowTimeout, io, applyPreGate, runInterceptors,
                 completion, operationStarted]() mutable {
            const common::ContextScope scope(context);
            source->services_->phase(common::ExecutionPhase::Queue, context,
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - operationStarted));
            QueryResult rejected;
            rejected.mode = ExecutionMode::CompatibilityFallback;
            if (applyPreGate) {
                if (auto status = source->preGate(sql, common::OperationType::Query);
                    !status.ok()) {
                    rejected.status = std::move(status);
                    completion(std::move(rejected));
                    return;
                }
            }
            std::string cacheKey;
            common::ResultSet cachedRows;
            if (source->cacheLookup(sql, params, cachedRows, cacheKey)) {
                source->services_->phase(common::ExecutionPhase::CacheHit, context, {},
                    common::ErrorCode::Ok, common::FallbackReason::CacheHit);
                if (!runInterceptors) {
                    QueryResult result;
                    result.status = common::Status::OK();
                    result.rows = std::move(cachedRows);
                    result.mode = ExecutionMode::CompatibilityFallback;
                    completion(std::move(result));
                    return;
                }
                common::SqlContext routedContext = context;
                core::detail::runOnRoute(source->services_->interceptors,
                                         source->name_, sql,
                                         common::OperationType::Query,
                                         routedContext);
                const auto startedAt = std::chrono::steady_clock::now();
                core::ExecutionView view{
                    source->name_, sql, common::OperationType::Query,
                    &params, &cachedRows, 0, std::chrono::microseconds{0},
                    common::Status::OK(), false, 0, routedContext
                };
                auto status = core::detail::runBeforeExecution(
                    source->services_->interceptors, view);
                if (status.ok()) {
                    view.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - startedAt);
                    core::detail::runAfterExecution(source->services_->interceptors, view);
                }
                view.status = status;
                core::detail::runOnCompletion(source->services_->interceptors, view);
                QueryResult result;
                result.status = std::move(status);
                if (result.status.ok()) result.rows = std::move(cachedRows);
                result.mode = ExecutionMode::CompatibilityFallback;
                completion(std::move(result));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                rejected.status = std::move(status);
                completion(std::move(rejected));
                return;
            }

            const auto poolWaitStarted = std::chrono::steady_clock::now();
            pool->borrowAsync(borrowTimeout, io,
                [source, sql = std::move(sql), params = std::move(params),
                 context = std::move(context), cacheKey = std::move(cacheKey), io,
                 borrowTimeout, operationStarted, poolWaitStarted, runInterceptors,
                 completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    source->services_->phase(common::ExecutionPhase::PoolWait, context,
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poolWaitStarted), status.code);
                    if (!status.ok() || !handle) {
                        source->afterAttempt(status);
                        QueryResult result;
                        result.status = std::move(status);
                        result.mode = ExecutionMode::CompatibilityFallback;
                        completion(std::move(result));
                        return;
                    }
                    handle->get()->setPreparedCacheLimit(
                        source->services_->preparedCacheMaxPerConnection.load(
                            std::memory_order_relaxed));

                    if (handle->get()->asyncCapability() == core::AsyncCapability::Native) {
                        common::SqlContext routedContext = context;
                        if (runInterceptors) {
                            core::detail::runOnRoute(source->services_->interceptors,
                                                     source->name_, sql,
                                                     common::OperationType::Query,
                                                     routedContext);
                        }
                        core::ExecutionView beforeView{
                            source->name_, sql, common::OperationType::Query,
                            &params, nullptr, 0, std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, routedContext
                        };
                        const auto before = runInterceptors
                                                ? core::detail::runBeforeExecution(
                                                      source->services_->interceptors, beforeView)
                                                : common::Status::OK();
                        if (!before.ok()) {
                            beforeView.status = before;
                            source->afterAttempt(before);
                            core::detail::runOnCompletion(
                                source->services_->interceptors, beforeView);
                            QueryResult result;
                            result.status = std::move(before);
                            result.mode = ExecutionMode::CompatibilityFallback;
                            completion(std::move(result));
                            return;
                        }
                        auto control = std::make_shared<NativeQueryControl>();
                        control->handle = std::make_shared<
                            std::unique_ptr<core::ConnectionPool::Handle> >(std::move(handle));
                        const auto nativeContext = routedContext;
                        const bool started = (*control->handle)->get()->queryAsync(
                            sql, params,
                            [source, control, sql, params, cacheKey,
                             context = nativeContext,
                             io, completion, operationStarted,
                             runInterceptors](common::Status nativeStatus,
                                        common::ResultSet rows) mutable {
                                control->driverCompleted.store(true, std::memory_order_release);
                                if (control->resolved.exchange(true, std::memory_order_acq_rel)) {
                                    if (control->timeoutDelivered.load(std::memory_order_acquire)) {
                                        std::lock_guard<std::mutex> lock(control->handleMutex);
                                        control->handle->reset();
                                    }
                                    return;
                                }
                                auto deliver = [source, control, sql, params, cacheKey, context,
                                            completion = std::move(completion),
                                            nativeStatus = std::move(nativeStatus),
                                            rows = std::move(rows), operationStarted,
                                            runInterceptors]() mutable {
                                    const common::ContextScope scope(context);
                                    if (nativeStatus.connectionBroken) {
                                        std::lock_guard<std::mutex> lock(control->handleMutex);
                                        if (*control->handle) (*control->handle)->invalidate();
                                    }
                                    source->afterAttempt(nativeStatus);
                                    if (nativeStatus.ok() && !cacheKey.empty())
                                        source->cacheStore(cacheKey, rows);
                                    common::OperationEvent event;
                                    event.dataSource = source->name_;
                                    event.type = common::OperationType::Query;
                                    event.duration = std::chrono::duration_cast<
                                        std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - operationStarted);
                                    event.status = nativeStatus;
                                    event.status.message.clear();
                                    event.rowCount = rows.rowCount();
                                    core::ExecutionView view{
                                        source->name_, sql, common::OperationType::Query,
                                        &params, nativeStatus.ok() ? &rows : nullptr,
                                        0, event.duration, nativeStatus, false, 0, context
                                    };
                                    core::IDatabaseConnection *connection = nullptr;
                                    {
                                        std::lock_guard<std::mutex> lock(control->handleMutex);
                                        if (*control->handle)
                                            connection = (*control->handle)->get();
                                    }
                                    source->services_->observability.emitSql(
                                        std::move(event), sql,
                                        [connection, sql, params](
                                            const common::SqlRenderOptions &options,
                                            std::string &out) {
                                            return connection->renderSqlForLogging(
                                                sql, params, options, out);
                                        }, &rows);
                                    if (runInterceptors) {
                                        core::detail::runAfterExecution(
                                            source->services_->interceptors, view);
                                        core::detail::runOnCompletion(
                                            source->services_->interceptors, view);
                                    }
                                    QueryResult result;
                                    result.status = std::move(nativeStatus);
                                    result.rows = std::move(rows);
                                    result.mode = ExecutionMode::Native;
                                    {
                                        std::lock_guard<std::mutex> lock(control->handleMutex);
                                        control->handle->reset();
                                    }
                                    completion(std::move(result));
                                };
                                if (!io.deliver(deliver)) deliver();
                            });
                        if (started) {
                            source->services_->phase(common::ExecutionPhase::Native, context);
                            if (borrowTimeout > std::chrono::milliseconds(0)) {
                                const auto elapsed = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - operationStarted);
                                const auto remaining = std::max(
                                    std::chrono::milliseconds(0), borrowTimeout - elapsed);
                                io.postAfter(
                                    [source, control, sql, params, context = nativeContext,
                                     io, completion, operationStarted,
                                     runInterceptors]() mutable {
                                        if (control->resolved.exchange(
                                                true, std::memory_order_acq_rel))
                                            return;
                                        auto timeoutStatus = common::Status::error(
                                            common::ErrorCode::QueryTimeout,
                                            "native query exceeded client async statement timeout");
                                        timeoutStatus.retryable = true;
                                        auto deliverTimeout =
                                            [source, control, sql, params, context,
                                             completion, timeoutStatus,
                                             operationStarted, runInterceptors]() mutable {
                                                const common::ContextScope scope(context);
                                                source->afterAttempt(timeoutStatus);
                                                common::OperationEvent event;
                                                event.dataSource = source->name_;
                                                event.type = common::OperationType::Query;
                                                event.duration = std::chrono::duration_cast<
                                                    std::chrono::microseconds>(
                                                    std::chrono::steady_clock::now() -
                                                    operationStarted);
                                                event.status = timeoutStatus;
                                                event.status.message.clear();
                                                core::ExecutionView view{
                                                    source->name_, sql,
                                                    common::OperationType::Query,
                                                    &params, nullptr, 0, event.duration,
                                                    timeoutStatus, false, 0, context
                                                };
                                                source->services_->observability.emitSql(
                                                    std::move(event), sql);
                                                if (runInterceptors) {
                                                    core::detail::runAfterExecution(
                                                        source->services_->interceptors, view);
                                                    core::detail::runOnCompletion(
                                                        source->services_->interceptors, view);
                                                }
                                                QueryResult result;
                                                result.status = timeoutStatus;
                                                result.mode = ExecutionMode::Native;
                                                completion(std::move(result));
                                                control->timeoutDelivered.store(
                                                    true, std::memory_order_release);
                                                if (control->driverCompleted.load(
                                                        std::memory_order_acquire)) {
                                                    std::lock_guard<std::mutex> lock(
                                                        control->handleMutex);
                                                    control->handle->reset();
                                                }
                                            };
                                        bool delivered = false;
                                        const auto cancelStarted = std::chrono::steady_clock::now();
                                        auto cancelCode = common::ErrorCode::Unknown;
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                control->handleMutex);
                                            delivered = io.deliver(deliverTimeout);
                                            if (*control->handle) {
                                                try {
                                                    cancelCode = (*control->handle)->get()->cancel().code;
                                                } catch (...) {
                                                }
                                            }
                                        }
                                        if (cancelCode != common::ErrorCode::Ok)
                                            source->lastCancellationError_.store(cancelCode, std::memory_order_relaxed);
                                        source->services_->phase(common::ExecutionPhase::Cancel, context,
                                            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - cancelStarted), cancelCode);
                                        if (!delivered) deliverTimeout();
                                    }, remaining);
                            }
                            return;
                        }
                        handle = std::move(*control->handle);
                    }

                    source->services_->phase(common::ExecutionPhase::Fallback, context, {},
                        common::ErrorCode::Ok,
                        handle->get()->asyncCapability() == core::AsyncCapability::Native
                            ? common::FallbackReason::DriverDeclinedStatement
                            : common::FallbackReason::DriverUnsupported);
                    auto session = source->makeSession(std::move(handle));
                    QueryResult result;
                    result.mode = ExecutionMode::CompatibilityFallback;
                    if (!runInterceptors) {
                        result.status = session->queryForAsyncEngine(
                            sql, params, result.rows);
                        source->afterAttempt(result.status);
                        if (result.status.ok() && !cacheKey.empty())
                            source->cacheStore(cacheKey, result.rows);
                        completion(std::move(result));
                        return;
                    }
                    if (!cacheKey.empty()) {
                        common::SqlContext routedContext = context;
                        core::detail::runOnRoute(source->services_->interceptors,
                                                 source->name_, sql,
                                                 common::OperationType::Query,
                                                 routedContext);
                        core::ExecutionView view{
                            source->name_, sql, common::OperationType::Query,
                            &params, nullptr, 0, std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, routedContext
                        };
                        result.status = core::detail::runBeforeExecution(
                            source->services_->interceptors, view);
                        if (!result.status.ok()) {
                            source->afterAttempt(result.status);
                            view.status = result.status;
                            core::detail::runOnCompletion(
                                source->services_->interceptors, view);
                            completion(std::move(result));
                            return;
                        }
                        result.status = session->queryForAsyncEngine(
                            sql, params, result.rows);
                        source->afterAttempt(result.status);
                        if (result.status.ok()) source->cacheStore(cacheKey, result.rows);
                        view.result = result.status.ok() ? &result.rows : nullptr;
                        view.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - operationStarted);
                        view.status = result.status;
                        core::detail::runAfterExecution(
                            source->services_->interceptors, view);
                        core::detail::runOnCompletion(
                            source->services_->interceptors, view);
                        completion(std::move(result));
                        return;
                    }
                    result.status = params.empty()
                                        ? session->query(sql, result.rows)
                                        : session->query(sql, params, result.rows);
                    source->afterAttempt(result.status);
                    completion(std::move(result));
                });
        };
        if (!io.post(start)) {
            QueryResult result;
            source->services_->phase(common::ExecutionPhase::Rejected, common::ContextScope::current(), {}, common::ErrorCode::Overloaded);
            result.status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            completion(std::move(result));
        }
        return true;
    }

    bool AsyncEngine::execute(const std::shared_ptr<core::DataSource> &source,
                              std::string sql, common::Params params,
                              common::SqlContext context,
                              const std::chrono::milliseconds borrowTimeout,
                              core::AsyncIo io, ExecuteCompletion completion) {
        if (!source || !io.usable() || !completion) return false;
        if (source->primary_) {
            return executeGroup(source, std::move(sql), std::move(params),
                                std::move(context), borrowTimeout,
                                std::move(io), std::move(completion));
        }
        return executeLeaf(source, std::move(sql), std::move(params),
                           std::move(context), borrowTimeout, std::move(io),
                           true, true, std::move(completion));
    }

    bool AsyncEngine::executeLeaf(const std::shared_ptr<core::DataSource> &source,
                                  std::string sql, common::Params params,
                                  common::SqlContext context,
                                  const std::chrono::milliseconds borrowTimeout,
                                  core::AsyncIo io, const bool applyPreGate,
                                  const bool runInterceptors,
                                  ExecuteCompletion completion) {
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, false))
            return false;

        int attempts = 1;
        const auto idempotency = context.idempotency;
        if (idempotency == common::Idempotency::Idempotent ||
            (idempotency != common::Idempotency::NonIdempotent &&
             source->retry_.retry_writes)) {
            attempts = std::max(1, source->retry_.max_attempts);
        }

        struct RetryState {
            std::shared_ptr<core::DataSource> source;
            std::string sql;
            common::Params params;
            common::SqlContext context;
            std::chrono::milliseconds timeout;
            core::AsyncIo io;
            ExecuteCompletion completion;
            int attempts = 1;
            bool applyPreGate = true;
            bool runInterceptors = true;
            std::function<void(int, bool)> run;
        };

        auto state = std::make_shared<RetryState>();
        state->source = source;
        state->sql = std::move(sql);
        state->params = std::move(params);
        state->context = std::move(context);
        state->timeout = borrowTimeout;
        state->io = std::move(io);
        state->completion = std::move(completion);
        state->attempts = attempts;
        state->applyPreGate = applyPreGate;
        state->runInterceptors = runInterceptors;

        std::weak_ptr<RetryState> weak = state;
        state->run = [weak](const int attempt, const bool applyPreGate) {
            const auto state = weak.lock();
            if (!state) return;
            const bool started = AsyncEngine::executeOne(
                state->source, state->sql, state->params, state->context,
                state->timeout, state->io,
                applyPreGate && state->applyPreGate, state->runInterceptors,
                [state, attempt](ExecResult result) mutable {
                    const bool retry = !result.status.ok() && result.status.retryable &&
                                       result.status.code != common::ErrorCode::CircuitOpen &&
                                       result.status.code != common::ErrorCode::Overloaded &&
                                       attempt < state->attempts;
                    if (!retry) {
                        state->completion(std::move(result));
                        return;
                    }
                    const auto delay = state->source->retryDelay(attempt);
                    state->source->services_->phase(common::ExecutionPhase::Retry, state->context,
                        {},
                        result.status.code, common::FallbackReason::None, attempt);
                    state->io.postAfter(
                        [state, next = attempt + 1] { state->run(next, false); }, delay);
                });
            if (!started) {
                ExecResult result;
                result.status = common::Status::error(
                    common::ErrorCode::PoolClosed,
                    "datasource pool closed before an asynchronous execute attempt");
                result.status.retryable = true;
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
            }
        };
        state->run(1, true);
        return true;
    }

    bool AsyncEngine::executeOne(const std::shared_ptr<core::DataSource> &source,
                                 std::string sql, common::Params params,
                                 common::SqlContext context,
                                 const std::chrono::milliseconds borrowTimeout,
                                 core::AsyncIo io, const bool applyPreGate,
                                 const bool runInterceptors,
                                 ExecuteCompletion completion) {
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, false))
            return false;
        auto pool = source->pool_.lock();
        if (!pool) return false;
        const auto enqueuedAt = std::chrono::steady_clock::now();
        auto start = [source, pool = std::move(pool), sql = std::move(sql),
                 params = std::move(params), context = std::move(context),
                 borrowTimeout, io, applyPreGate, runInterceptors,
                 completion, enqueuedAt]() mutable {
            const common::ContextScope scope(context);
            source->services_->phase(common::ExecutionPhase::Queue, context,
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - enqueuedAt));
            ExecResult rejected;
            rejected.mode = ExecutionMode::CompatibilityFallback;
            if (applyPreGate) {
                if (auto status = source->preGate(sql, common::OperationType::Execute);
                    !status.ok()) {
                    rejected.status = std::move(status);
                    completion(std::move(rejected));
                    return;
                }
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                rejected.status = std::move(status);
                completion(std::move(rejected));
                return;
            }
            const auto poolWaitStarted = std::chrono::steady_clock::now();
            pool->borrowAsync(borrowTimeout, io,
                [source, sql = std::move(sql), params = std::move(params),
                 context = std::move(context), io,
                 runInterceptors, poolWaitStarted, completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    source->services_->phase(common::ExecutionPhase::PoolWait, context,
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poolWaitStarted), status.code);
                    if (!status.ok() || !handle) {
                        source->afterAttempt(status);
                        ExecResult result;
                        result.status = std::move(status);
                        result.mode = ExecutionMode::CompatibilityFallback;
                        completion(std::move(result));
                        return;
                    }
                    handle->get()->setPreparedCacheLimit(
                        source->services_->preparedCacheMaxPerConnection.load(
                            std::memory_order_relaxed));

                    if (handle->get()->asyncCapability() == core::AsyncCapability::Native) {
                        common::SqlContext routedContext = context;
                        if (runInterceptors) {
                            core::detail::runOnRoute(source->services_->interceptors,
                                                     source->name_, sql,
                                                     common::OperationType::Execute,
                                                     routedContext);
                        }
                        core::ExecutionView beforeView{
                            source->name_, sql, common::OperationType::Execute,
                            &params, nullptr, 0, std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, routedContext
                        };
                        const auto before = runInterceptors
                                                ? core::detail::runBeforeExecution(
                                                      source->services_->interceptors, beforeView)
                                                : common::Status::OK();
                        if (!before.ok()) {
                            beforeView.status = before;
                            source->afterAttempt(before);
                            core::detail::runOnCompletion(
                                source->services_->interceptors, beforeView);
                            ExecResult result;
                            result.status = std::move(before);
                            result.mode = ExecutionMode::CompatibilityFallback;
                            completion(std::move(result));
                            return;
                        }
                        auto boxed = std::make_shared<
                            std::unique_ptr<core::ConnectionPool::Handle> >(std::move(handle));
                        const auto startedAt = std::chrono::steady_clock::now();
                        const bool started = (*boxed)->get()->executeAsync(
                            sql, params,
                            [source, boxed, sql, params,
                             context = std::move(routedContext), io, completion,
                             startedAt, runInterceptors](common::Status nativeStatus,
                                        const std::int64_t affected) mutable {
                                auto deliver = [source, boxed, sql, params, context,
                                            completion = std::move(completion),
                                            nativeStatus = std::move(nativeStatus),
                                            affected, startedAt,
                                            runInterceptors]() mutable {
                                    const common::ContextScope scope(context);
                                    if (nativeStatus.connectionBroken) (*boxed)->invalidate();
                                    source->afterAttempt(nativeStatus);
                                    if (nativeStatus.ok()) source->markWrite();
                                    common::OperationEvent event;
                                    event.dataSource = source->name_;
                                    event.type = common::OperationType::Execute;
                                    event.duration = std::chrono::duration_cast<
                                        std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - startedAt);
                                    event.status = nativeStatus;
                                    event.status.message.clear();
                                    event.rowCount = affected > 0
                                                         ? static_cast<std::uint64_t>(affected)
                                                         : 0;
                                    core::ExecutionView view{
                                        source->name_, sql, common::OperationType::Execute,
                                        &params, nullptr,
                                        nativeStatus.ok() ? affected : 0,
                                        event.duration, nativeStatus, false, 0, context
                                    };
                                    auto *connection = (*boxed)->get();
                                    source->services_->observability.emitSql(
                                        std::move(event), sql,
                                        [connection, sql, params](
                                            const common::SqlRenderOptions &options,
                                            std::string &out) {
                                            return connection->renderSqlForLogging(
                                                sql, params, options, out);
                                        });
                                    if (runInterceptors) {
                                        core::detail::runAfterExecution(
                                            source->services_->interceptors, view);
                                        core::detail::runOnCompletion(
                                            source->services_->interceptors, view);
                                    }
                                    ExecResult result;
                                    result.status = std::move(nativeStatus);
                                    result.affected = affected;
                                    result.mode = ExecutionMode::Native;
                                    boxed->reset();
                                    completion(std::move(result));
                                };
                                if (!io.deliver(deliver)) deliver();
                            });
                        if (started) {
                            source->services_->phase(common::ExecutionPhase::Native, context);
                            return;
                        }
                        handle = std::move(*boxed);
                    }

                    source->services_->phase(common::ExecutionPhase::Fallback, context, {},
                        common::ErrorCode::Ok,
                        handle->get()->asyncCapability() == core::AsyncCapability::Native
                            ? common::FallbackReason::DriverDeclinedStatement
                            : common::FallbackReason::DriverUnsupported);
                    auto session = source->makeSession(std::move(handle));
                    ExecResult result;
                    result.mode = ExecutionMode::CompatibilityFallback;
                    result.status = runInterceptors
                                        ? (params.empty()
                                               ? session->execute(sql, result.affected)
                                               : session->execute(sql, params, result.affected))
                                        : session->executeForAsyncEngine(
                                              sql, params, result.affected);
                    source->afterAttempt(result.status);
                    if (result.status.ok()) source->markWrite();
                    completion(std::move(result));
                });
        };
        if (!io.post(start)) {
            ExecResult result;
            source->services_->phase(common::ExecutionPhase::Rejected, common::ContextScope::current(), {}, common::ErrorCode::Overloaded);
            result.status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            completion(std::move(result));
        }
        return true;
    }

    bool AsyncEngine::queryGroup(const std::shared_ptr<core::DataSource> &source,
                                 std::string sql, common::Params params,
                                 common::SqlContext context,
                                 const std::chrono::milliseconds borrowTimeout,
                                 core::AsyncIo io, QueryCompletion completion) {
        if (!source || !source->primary_ || !io.usable() || !completion) return false;

        struct GroupState {
            std::shared_ptr<core::DataSource> source;
            std::shared_ptr<core::DataSource> target;
            std::string sql;
            common::Params params;
            common::SqlContext context;
            std::chrono::milliseconds timeout;
            std::chrono::steady_clock::time_point started;
            core::AsyncIo io;
            QueryCompletion completion;
            std::function<void(QueryResult)> finish;
        };

        auto state = std::make_shared<GroupState>();
        state->source = source;
        state->sql = std::move(sql);
        state->params = std::move(params);
        state->context = std::move(context);
        state->timeout = borrowTimeout;
        state->started = std::chrono::steady_clock::now();
        state->io = std::move(io);
        state->completion = std::move(completion);

        std::weak_ptr<GroupState> weak = state;
        state->finish = [weak](QueryResult result) mutable {
            const auto state = weak.lock();
            if (!state) return;
            const common::ContextScope scope(state->context);
            core::ExecutionView view{
                state->source->name_, state->sql, common::OperationType::Query,
                &state->params, result.status.ok() ? &result.rows : nullptr,
                0, std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - state->started),
                result.status, false, 0, state->context
            };
            core::detail::runAfterExecution(state->source->services_->interceptors, view);
            core::detail::runOnCompletion(state->source->services_->interceptors, view);
            state->completion(std::move(result));
        };

        auto start = [state]() mutable {
            const common::ContextScope initialScope(state->context);
            if (auto status = state->source->preGate(
                    state->sql, common::OperationType::Query);
                !status.ok()) {
                QueryResult result;
                result.status = std::move(status);
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
                return;
            }
            core::detail::runOnRoute(state->source->services_->interceptors,
                                     state->source->name_, state->sql,
                                     common::OperationType::Query, state->context);
            core::ExecutionView beforeView{
                state->source->name_, state->sql, common::OperationType::Query,
                &state->params, nullptr, 0, std::chrono::microseconds{0},
                common::Status::OK(), false, 0, state->context
            };
            if (auto before = core::detail::runBeforeExecution(
                    state->source->services_->interceptors, beforeView);
                !before.ok()) {
                beforeView.status = before;
                core::detail::runOnCompletion(
                    state->source->services_->interceptors, beforeView);
                QueryResult result;
                result.status = std::move(before);
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
                return;
            }

            {
                const common::ContextScope routedScope(state->context);
                state->target = state->source->readTarget();
            }
            if (!state->target) {
                QueryResult result;
                result.status = common::Status::error(
                    common::ErrorCode::PoolClosed,
                    "group '" + state->source->name_ + "' has no readable target");
                result.status.retryable = true;
                result.mode = ExecutionMode::CompatibilityFallback;
                state->finish(std::move(result));
                return;
            }

            const auto target = state->target;
            const bool started = AsyncEngine::queryLeaf(
                target, state->sql, state->params, state->context,
                state->timeout, state->io, false, false,
                [state, target](QueryResult result) mutable {
                    const bool fallback = target != state->source->primary_ &&
                        state->source->fallbackToPrimary_ &&
                        (result.status.retryable || result.status.connectionBroken ||
                         result.status.code == common::ErrorCode::CircuitOpen);
                    if (!fallback) {
                        state->finish(std::move(result));
                        return;
                    }
                    auto remaining = state->timeout;
                    state->source->services_->phase(common::ExecutionPhase::ReplicaFallback, state->context,
                        {}, result.status.code);
                    if (state->timeout > std::chrono::milliseconds(0)) {
                        const auto elapsed = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - state->started);
                        remaining = state->timeout - elapsed;
                        if (remaining <= std::chrono::milliseconds(0)) {
                            state->finish(std::move(result));
                            return;
                        }
                    }
                    if (!AsyncEngine::queryLeaf(
                            state->source->primary_, state->sql, state->params,
                            state->context, remaining, state->io, false, false,
                            [state](QueryResult primaryResult) mutable {
                                state->finish(std::move(primaryResult));
                            })) {
                        QueryResult closed;
                        closed.status = common::Status::error(
                            common::ErrorCode::PoolClosed,
                            "group primary closed before asynchronous read fallback");
                        closed.status.retryable = true;
                        closed.mode = ExecutionMode::CompatibilityFallback;
                        state->finish(std::move(closed));
                    }
                });
            if (!started) {
                QueryResult result;
                result.status = common::Status::error(
                    common::ErrorCode::PoolClosed,
                    "group read target closed before asynchronous execution");
                result.status.retryable = true;
                result.mode = ExecutionMode::CompatibilityFallback;
                state->finish(std::move(result));
            }
        };

        if (!state->io.post(std::move(start))) {
            QueryResult result;
            result.status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            state->completion(std::move(result));
        }
        return true;
    }

    bool AsyncEngine::executeGroup(const std::shared_ptr<core::DataSource> &source,
                                   std::string sql, common::Params params,
                                   common::SqlContext context,
                                   const std::chrono::milliseconds borrowTimeout,
                                   core::AsyncIo io, ExecuteCompletion completion) {
        if (!source || !source->primary_ || !io.usable() || !completion) return false;

        struct GroupState {
            std::shared_ptr<core::DataSource> source;
            std::vector<std::shared_ptr<core::DataSource> > targets;
            std::string sql;
            common::Params params;
            common::SqlContext context;
            std::chrono::milliseconds timeout;
            std::chrono::steady_clock::time_point started;
            core::AsyncIo io;
            ExecuteCompletion completion;
            std::size_t index = 0;
            bool shadowRoute = false;
            std::function<void(ExecResult)> finish;
            std::function<void()> run;
        };

        auto state = std::make_shared<GroupState>();
        state->source = source;
        state->sql = std::move(sql);
        state->params = std::move(params);
        state->context = std::move(context);
        state->timeout = borrowTimeout;
        state->started = std::chrono::steady_clock::now();
        state->io = std::move(io);
        state->completion = std::move(completion);

        std::weak_ptr<GroupState> weak = state;
        state->finish = [weak](ExecResult result) mutable {
            const auto state = weak.lock();
            if (!state) return;
            const common::ContextScope scope(state->context);
            core::ExecutionView view{
                state->source->name_, state->sql, common::OperationType::Execute,
                &state->params, nullptr,
                result.status.ok() ? result.affected : 0,
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - state->started),
                result.status, false, 0, state->context
            };
            core::detail::runAfterExecution(state->source->services_->interceptors, view);
            core::detail::runOnCompletion(state->source->services_->interceptors, view);
            state->completion(std::move(result));
        };

        state->run = [weak]() {
            const auto state = weak.lock();
            if (!state || state->index >= state->targets.size()) return;
            const auto target = state->targets[state->index];
            const bool started = AsyncEngine::executeLeaf(
                target, state->sql, state->params, state->context,
                state->timeout, state->io, false, false,
                [state](ExecResult result) mutable {
                    if (result.status.ok()) {
                        if (!state->shadowRoute) state->source->markWrite();
                        state->finish(std::move(result));
                        return;
                    }
                    if (state->shadowRoute ||
                        !core::DataSource::safeToFailoverWrite(result.status)) {
                        state->finish(std::move(result));
                        return;
                    }
                    ++state->index;
                    if (state->index < state->targets.size()) {
                        state->run();
                        return;
                    }
                    if (state->source->writeBuffer_ &&
                        state->source->writeBuffer_->enabled()) {
                        const auto primary = state->source->primary_;
                        const auto sql = state->sql;
                        const auto params = state->params;
                        if (state->source->writeBuffer_->enqueue(
                                [primary, sql, params] {
                                    std::int64_t ignored = 0;
                                    return params.empty()
                                               ? primary->executeUngated(sql, ignored)
                                               : primary->executeUngated(
                                                     sql, params, ignored);
                                })) {
                            result.status = common::Status::error(
                                common::ErrorCode::Buffered,
                                "group '" + state->source->name_ +
                                    "': write accepted into buffer, not yet committed");
                            result.status.retryable = false;
                            result.affected = 0;
                            result.mode = ExecutionMode::CompatibilityFallback;
                        }
                    }
                    state->finish(std::move(result));
                });
            if (!started) {
                ExecResult result;
                result.status = common::Status::error(
                    common::ErrorCode::PoolClosed,
                    "group write target closed before asynchronous execution");
                result.status.retryable = true;
                result.mode = ExecutionMode::CompatibilityFallback;
                state->finish(std::move(result));
            }
        };

        auto start = [state]() mutable {
            const common::ContextScope initialScope(state->context);
            if (auto status = state->source->preGate(
                    state->sql, common::OperationType::Execute);
                !status.ok()) {
                ExecResult result;
                result.status = std::move(status);
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
                return;
            }
            core::detail::runOnRoute(state->source->services_->interceptors,
                                     state->source->name_, state->sql,
                                     common::OperationType::Execute, state->context);
            core::ExecutionView beforeView{
                state->source->name_, state->sql, common::OperationType::Execute,
                &state->params, nullptr, 0, std::chrono::microseconds{0},
                common::Status::OK(), false, 0, state->context
            };
            if (auto before = core::detail::runBeforeExecution(
                    state->source->services_->interceptors, beforeView);
                !before.ok()) {
                beforeView.status = before;
                core::detail::runOnCompletion(
                    state->source->services_->interceptors, beforeView);
                ExecResult result;
                result.status = std::move(before);
                result.mode = ExecutionMode::CompatibilityFallback;
                state->completion(std::move(result));
                return;
            }
            {
                const common::ContextScope routedScope(state->context);
                state->shadowRoute = state->context.shadow &&
                                     static_cast<bool>(state->source->shadow_);
                state->targets = state->source->writeTargets();
            }
            if (!state->targets.empty()) {
                state->run();
                return;
            }

            ExecResult result;
            result.status = common::Status::error(
                common::ErrorCode::CircuitOpen,
                "group '" + state->source->name_ +
                    "': no writable primary available");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            if (!state->shadowRoute && state->source->writeBuffer_ &&
                state->source->writeBuffer_->enabled()) {
                const auto primary = state->source->primary_;
                const auto sql = state->sql;
                const auto params = state->params;
                if (state->source->writeBuffer_->enqueue(
                        [primary, sql, params] {
                            std::int64_t ignored = 0;
                            return params.empty()
                                       ? primary->executeUngated(sql, ignored)
                                       : primary->executeUngated(sql, params, ignored);
                        })) {
                    result.status = common::Status::error(
                        common::ErrorCode::Buffered,
                        "group '" + state->source->name_ +
                            "': write accepted into buffer, not yet committed");
                    result.status.retryable = false;
                }
            }
            state->finish(std::move(result));
        };

        if (!state->io.post(std::move(start))) {
            ExecResult result;
            result.status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            state->completion(std::move(result));
        }
        return true;
    }

    bool AsyncEngine::runSession(const std::shared_ptr<core::DataSource> &source,
                                 std::string sql, const common::OperationType type,
                                 const bool write, common::SqlContext context,
                                 const std::chrono::milliseconds borrowTimeout,
                                 core::AsyncIo io, SessionOperation operation,
                                 StatusCompletion completion) {
        if (!source || !io.usable() || !operation || !completion ||
            !directLeafEligible(*source, false))
            return false;
        auto pool = source->pool_.lock();
        if (!pool) return false;
        auto start = [source, pool = std::move(pool), sql = std::move(sql), type, write,
                 context = std::move(context), borrowTimeout, io,
                 operation = std::move(operation), completion,
                 enqueuedAt = std::chrono::steady_clock::now()]() mutable {
            const common::ContextScope scope(context);
            source->services_->phase(common::ExecutionPhase::Queue, context,
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - enqueuedAt));
            if (auto status = source->preGate(sql, type); !status.ok()) {
                completion(std::move(status));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            const auto poolWaitStarted = std::chrono::steady_clock::now();
            pool->borrowAsync(borrowTimeout, io,
                [source, context = std::move(context), write, poolWaitStarted,
                 operation = std::move(operation),
                 completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    source->services_->phase(common::ExecutionPhase::PoolWait, context,
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poolWaitStarted), status.code);
                    if (status.ok() && handle) {
                        source->services_->phase(common::ExecutionPhase::Fallback, context, {}, common::ErrorCode::Ok,
                                                 common::FallbackReason::CompositeOperation);
                        handle->get()->setPreparedCacheLimit(
                            source->services_->preparedCacheMaxPerConnection.load(
                                std::memory_order_relaxed));
                        auto session = source->makeSession(std::move(handle));
                        try {
                            status = operation(*session);
                        } catch (const std::exception &e) {
                            status = common::Status::error(
                                common::ErrorCode::Unknown,
                                std::string("client async session operation threw: ") + e.what());
                        } catch (...) {
                            status = common::Status::error(
                                common::ErrorCode::Unknown,
                                "client async session operation threw an unknown exception");
                        }
                    }
                    source->afterAttempt(status);
                    if (write && status.ok()) source->markWrite();
                    completion(std::move(status));
                });
        };
        if (!io.post(start)) {
            auto status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            source->services_->phase(common::ExecutionPhase::Rejected, common::ContextScope::current(), {}, common::ErrorCode::Overloaded);
            status.retryable = true;
            completion(std::move(status));
        }
        return true;
    }

    bool AsyncEngine::transaction(const std::shared_ptr<core::DataSource> &source,
                                  common::TransactionOptions options,
                                  core::SessionFn operation,
                                  common::SqlContext context,
                                  const std::chrono::milliseconds borrowTimeout,
                                  core::AsyncIo io, StatusCompletion completion) {
        if (!source || !io.usable() || !operation || !completion ||
            source->primary_ || source->shadow_ || source->pool_.expired())
            return false;
        auto pool = source->pool_.lock();
        if (!pool) return false;
        auto start = [source, pool = std::move(pool), options,
                 operation = std::move(operation), context = std::move(context),
                 borrowTimeout, io, completion,
                 enqueuedAt = std::chrono::steady_clock::now()]() mutable {
            const common::ContextScope scope(context);
            source->services_->phase(common::ExecutionPhase::Queue, context,
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - enqueuedAt));
            if (auto status = source->gateSession(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            const auto poolWaitStarted = std::chrono::steady_clock::now();
            pool->borrowAsync(borrowTimeout, io,
                [source, options, operation = std::move(operation), poolWaitStarted,
                 context = std::move(context), completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    source->services_->phase(common::ExecutionPhase::PoolWait, context,
                        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - poolWaitStarted), status.code);
                    if (!status.ok() || !handle) {
                        source->afterAttempt(status);
                        completion(std::move(status));
                        return;
                    }
                    handle->get()->setPreparedCacheLimit(
                        source->services_->preparedCacheMaxPerConnection.load(
                            std::memory_order_relaxed));
                    source->services_->phase(common::ExecutionPhase::Fallback, context, {}, common::ErrorCode::Ok,
                                             common::FallbackReason::CompositeOperation);
                    status = source->transactionWithHandle(
                        std::move(handle), options, operation, source->readOnly_);
                    completion(std::move(status));
                });
        };
        if (!io.post(start)) {
            auto status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            source->services_->phase(common::ExecutionPhase::Rejected, common::ContextScope::current(), {}, common::ErrorCode::Overloaded);
            status.retryable = true;
            completion(std::move(status));
        }
        return true;
    }
}
