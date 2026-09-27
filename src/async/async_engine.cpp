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
        if (source.primary_ || source.shadow_ || source.retry_.max_attempts != 1)
            return false;
        return !source.pool_.expired();
    }

    bool AsyncEngine::query(const std::shared_ptr<core::DataSource> &source,
                            std::string sql, common::Params params,
                            common::SqlContext context,
                            const std::chrono::milliseconds borrowTimeout,
                            core::AsyncIo io, QueryCompletion completion) {
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, true))
            return false;

        auto pool = source->pool_.lock();
        if (!pool) return false;
        io = normalizedIo(std::move(io));
        const auto operationStarted = std::chrono::steady_clock::now();
        auto start = [source, pool = std::move(pool), sql = std::move(sql),
                 params = std::move(params), context = std::move(context),
                 borrowTimeout, io, completion, operationStarted]() mutable {
            const common::ContextScope scope(context);
            QueryResult rejected;
            rejected.mode = ExecutionMode::CompatibilityFallback;
            if (auto status = source->preGate(sql, common::OperationType::Query);
                !status.ok()) {
                rejected.status = std::move(status);
                completion(std::move(rejected));
                return;
            }
            std::string cacheKey;
            common::ResultSet cachedRows;
            if (source->cacheLookup(sql, params, cachedRows, cacheKey)) {
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

            pool->borrowAsync(borrowTimeout, io,
                [source, sql = std::move(sql), params = std::move(params),
                 context = std::move(context), cacheKey = std::move(cacheKey), io,
                 borrowTimeout, operationStarted,
                 completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
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
                        core::detail::runOnRoute(source->services_->interceptors,
                                                 source->name_, sql,
                                                 common::OperationType::Query,
                                                 routedContext);
                        core::ExecutionView beforeView{
                            source->name_, sql, common::OperationType::Query,
                            &params, nullptr, 0, std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, routedContext
                        };
                        if (auto before = core::detail::runBeforeExecution(
                                source->services_->interceptors, beforeView);
                            !before.ok()) {
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
                             io, completion, operationStarted](common::Status nativeStatus,
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
                                            rows = std::move(rows), operationStarted]() mutable {
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
                                    core::detail::runAfterExecution(
                                        source->services_->interceptors, view);
                                    core::detail::runOnCompletion(
                                        source->services_->interceptors, view);
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
                            if (borrowTimeout > std::chrono::milliseconds(0)) {
                                const auto elapsed = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - operationStarted);
                                const auto remaining = std::max(
                                    std::chrono::milliseconds(0), borrowTimeout - elapsed);
                                io.postAfter(
                                    [source, control, sql, params, context = nativeContext,
                                     io, completion, operationStarted]() mutable {
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
                                             operationStarted]() mutable {
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
                                                core::detail::runAfterExecution(
                                                    source->services_->interceptors, view);
                                                core::detail::runOnCompletion(
                                                    source->services_->interceptors, view);
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
                                        {
                                            std::lock_guard<std::mutex> lock(
                                                control->handleMutex);
                                            delivered = io.deliver(deliverTimeout);
                                            if (*control->handle) {
                                                try {
                                                    (void) (*control->handle)->get()->cancel();
                                                } catch (...) {
                                                }
                                            }
                                        }
                                        if (!delivered) deliverTimeout();
                                    }, remaining);
                            }
                            return;
                        }
                        handle = std::move(*control->handle);
                    }

                    auto session = source->makeSession(std::move(handle));
                    QueryResult result;
                    result.mode = ExecutionMode::CompatibilityFallback;
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
        if (!source || !io.usable() || !completion ||
            !directLeafEligible(*source, false))
            return false;
        auto pool = source->pool_.lock();
        if (!pool) return false;
        auto start = [source, pool = std::move(pool), sql = std::move(sql),
                 params = std::move(params), context = std::move(context),
                 borrowTimeout, io, completion]() mutable {
            const common::ContextScope scope(context);
            ExecResult rejected;
            rejected.mode = ExecutionMode::CompatibilityFallback;
            if (auto status = source->preGate(sql, common::OperationType::Execute);
                !status.ok()) {
                rejected.status = std::move(status);
                completion(std::move(rejected));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                rejected.status = std::move(status);
                completion(std::move(rejected));
                return;
            }
            pool->borrowAsync(borrowTimeout, io,
                [source, sql = std::move(sql), params = std::move(params),
                 context = std::move(context), io,
                 completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
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
                        core::detail::runOnRoute(source->services_->interceptors,
                                                 source->name_, sql,
                                                 common::OperationType::Execute,
                                                 routedContext);
                        core::ExecutionView beforeView{
                            source->name_, sql, common::OperationType::Execute,
                            &params, nullptr, 0, std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, routedContext
                        };
                        if (auto before = core::detail::runBeforeExecution(
                                source->services_->interceptors, beforeView);
                            !before.ok()) {
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
                             startedAt](common::Status nativeStatus,
                                        const std::int64_t affected) mutable {
                                auto deliver = [source, boxed, sql, params, context,
                                            completion = std::move(completion),
                                            nativeStatus = std::move(nativeStatus),
                                            affected, startedAt]() mutable {
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
                                    core::detail::runAfterExecution(
                                        source->services_->interceptors, view);
                                    core::detail::runOnCompletion(
                                        source->services_->interceptors, view);
                                    ExecResult result;
                                    result.status = std::move(nativeStatus);
                                    result.affected = affected;
                                    result.mode = ExecutionMode::Native;
                                    boxed->reset();
                                    completion(std::move(result));
                                };
                                if (!io.deliver(deliver)) deliver();
                            });
                        if (started) return;
                        handle = std::move(*boxed);
                    }

                    auto session = source->makeSession(std::move(handle));
                    ExecResult result;
                    result.mode = ExecutionMode::CompatibilityFallback;
                    result.status = params.empty()
                                        ? session->execute(sql, result.affected)
                                        : session->execute(sql, params, result.affected);
                    source->afterAttempt(result.status);
                    if (result.status.ok()) source->markWrite();
                    completion(std::move(result));
                });
        };
        if (!io.post(start)) {
            ExecResult result;
            result.status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            result.status.retryable = true;
            result.mode = ExecutionMode::CompatibilityFallback;
            completion(std::move(result));
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
                 operation = std::move(operation), completion]() mutable {
            const common::ContextScope scope(context);
            if (auto status = source->preGate(sql, type); !status.ok()) {
                completion(std::move(status));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            pool->borrowAsync(borrowTimeout, io,
                [source, context = std::move(context), write,
                 operation = std::move(operation),
                 completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    if (status.ok() && handle) {
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
                 borrowTimeout, io, completion]() mutable {
            const common::ContextScope scope(context);
            if (auto status = source->gateSession(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            if (auto status = source->beforeAttempt(); !status.ok()) {
                completion(std::move(status));
                return;
            }
            pool->borrowAsync(borrowTimeout, io,
                [source, options, operation = std::move(operation),
                 context = std::move(context), completion = std::move(completion)](
                    std::unique_ptr<core::ConnectionPool::Handle> handle,
                    common::Status status) mutable {
                    const common::ContextScope scope(context);
                    if (!status.ok() || !handle) {
                        source->afterAttempt(status);
                        completion(std::move(status));
                        return;
                    }
                    handle->get()->setPreparedCacheLimit(
                        source->services_->preparedCacheMaxPerConnection.load(
                            std::memory_order_relaxed));
                    status = source->transactionWithHandle(
                        std::move(handle), options, operation, source->readOnly_);
                    completion(std::move(status));
                });
        };
        if (!io.post(start)) {
            auto status = common::Status::error(
                common::ErrorCode::Overloaded,
                "client async executor queue is full or stopped");
            status.retryable = true;
            completion(std::move(status));
        }
        return true;
    }
}
