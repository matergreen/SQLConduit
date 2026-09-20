#include "dbmw/async/dbmw_async.h"
#include "dbmw/common/context.h"
#include "dbmw/common/logger.h"
#include "dbmw/core/interceptor.h"
#include "dbmw/dbmw.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace dbmw::async {
    namespace {
        std::mutex gMtx;
        std::condition_variable gDrainCv;
        std::shared_ptr<IExecutor> gExecutor;
        std::shared_ptr<IExecutor> gCompletion;
        std::size_t gInFlight = 0;
        std::atomic<bool> gStopping{false};
        std::atomic<std::int64_t> gDefaultTimeoutMs{0};

        class CompletionFallback final {
        public:
            CompletionFallback() : worker_([this] { run(); }) {}
            ~CompletionFallback() {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    stopping_ = true;
                }
                cv_.notify_one();
                if (worker_.joinable()) worker_.join();
            }

            void post(std::function<void()> task) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    queue_.push_back(std::move(task));
                }
                cv_.notify_one();
            }

        private:
            void run() {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this] { return stopping_ || !queue_.empty(); });
                        if (queue_.empty() && stopping_) return;
                        task = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    guardedRun(task);
                }
            }

            static void guardedRun(const std::function<void()> &task) {
                try {
                    if (task) task();
                } catch (const std::exception &e) {
                    DBMW_LOG_ERROR(std::string("async completion callback threw: ") + e.what());
                } catch (...) {
                    DBMW_LOG_ERROR("async completion callback threw an unknown exception");
                }
            }

            std::mutex mtx_;
            std::condition_variable cv_;
            std::deque<std::function<void()> > queue_;
            bool stopping_ = false;
            std::thread worker_;
        };

        CompletionFallback &completionFallback() {
            static CompletionFallback dispatcher;
            return dispatcher;
        }

        std::shared_ptr<IExecutor> workerExecutor() {
            std::lock_guard<std::mutex> lk(gMtx);
            return gExecutor;
        }

        std::shared_ptr<IExecutor> completionExecutor() {
            std::lock_guard<std::mutex> lk(gMtx);
            return gCompletion;
        }

        void registryAdd() {
            std::lock_guard<std::mutex> lk(gMtx);
            ++gInFlight;
        }

        void registryRemove() {
            std::lock_guard<std::mutex> lk(gMtx);
            if (gInFlight > 0) --gInFlight;
            gDrainCv.notify_all();
        }

        void postOrRun(const std::shared_ptr<IExecutor> &ex, const std::function<void()> &task) {
            if (ex && ex->tryPost(task)) return;
            task();
        }

        void postCompletion(const std::shared_ptr<IExecutor> &ex,
                            const std::function<void()> &task) {
            if (ex && ex->tryPost(task)) return;
            completionFallback().post(task);
        }

        core::AsyncIo makePoolIo(const std::shared_ptr<IExecutor> &ex) {
            core::AsyncIo io;
            io.post = [ex](std::function<void()> task) {
                postOrRun(ex, std::move(task));
            };
            io.deliver = [ex](const std::function<void()> &task) {
                postOrRun(ex, task);
            };
            return io;
        }

        template<class F>
        void guarded(F &&f) {
            try {
                f();
            } catch (const std::exception &e) {
                DBMW_LOG_ERROR(std::string("async engine step threw: ") + e.what());
            } catch (...) {
                DBMW_LOG_ERROR("async engine step threw an unknown exception");
            }
        }

        std::shared_ptr<core::DataSource> resolve(const std::string &name) {
            return DBMW::dataSource(name);
        }
    }

    namespace detail {
        struct OpState {
            std::atomic<Handle::State> state{Handle::State::Queued};
            std::atomic<bool> userCancelled{false};
            std::atomic<bool> timedOut{false};
            std::atomic<bool> cancelDelivered{false};
            std::atomic<bool> finished{false};

            std::mutex sessionMtx;
            core::Session *pinnedSession = nullptr;
        };

        namespace {
            enum class RetryMode {
                ReadRetries,
                WriteRetries,
                Single
            };

            struct StatementPolicy {
                bool isWrite = false;
                RetryMode retry = RetryMode::Single;
                bool cacheable = false;
                bool fallbackOnlyIfNoRows = false;
                bool allowWriteBuffer = false;
            };

            template<class R>
            struct StatementOp {
                std::shared_ptr<OpState> op;
                std::shared_ptr<core::DataSource> root;
                std::vector<std::shared_ptr<core::DataSource> > targets;
                std::size_t targetIdx = 0;
                int attempt = 0;
                std::chrono::milliseconds borrowTimeout{-1};

                std::string sql;
                common::Params params;
                std::string cacheKey;

                std::function<void(core::Session &, R &)> attemptFn;
                std::function<std::function<common::Status()>(
                    const std::shared_ptr<core::DataSource> &primary)> bufferedMaker;
                std::function<void(R &&)> cb;

                StatementPolicy policy;

                common::SqlContext entryCtx;
            };

            struct SessionOp {
                std::shared_ptr<OpState> op;
                std::shared_ptr<core::DataSource> root;
                bool transactional = true;
                common::TransactionOptions txOpts;
                core::SessionFn fn;
                std::function<void(OpResult &&)> cb;
                std::chrono::milliseconds borrowTimeout{-1};

                common::SqlContext entryCtx;
            };
        }

        class AsyncEngine {
        public:
            AsyncEngine() = delete;

            template<class R>
            static void deliverResult(std::function<void(R &&)> cb, R result) {
                auto resultBox = std::make_shared<R>(std::move(result));
                auto cbBox = std::make_shared<std::function<void(R &&)> >(std::move(cb));
                postCompletion(completionExecutor(), [resultBox, cbBox] {
                    (*cbBox)(std::move(*resultBox));
                });
            }

            static Handle doneHandle() {
                auto op = std::make_shared<OpState>();
                op->state.store(Handle::State::Done);
                op->finished.store(true);
                return Handle(std::move(op));
            }

            template<class R>
            static Handle failNow(std::function<void(R &&)> cb, common::Status st) {
                R r;
                r.status = std::move(st);
                deliverResult(std::move(cb), std::move(r));
                return doneHandle();
            }

            template<class R>
            static void finishStatement(const std::shared_ptr<StatementOp<R> > &ctx, R result) {
                const auto &op = ctx->op;
                bool expected = false;
                if (!op->finished.compare_exchange_strong(expected, true)) return;

                common::Status &st = result.status;
                if (op->timedOut.load()) {
                    auto timeout = common::Status::error(
                        common::ErrorCode::QueryTimeout,
                        "statement exceeded async timeout");
                    timeout.retryable = true;
                    if (!op->cancelDelivered.load()) {
                        timeout.message +=
                                " (could not cancel; driver may not support it - "
                                "timeout is best-effort)";
                    }
                    st = std::move(timeout);
                } else if (op->userCancelled.load()) {
                    st = common::Status::error(
                        common::ErrorCode::Cancelled,
                        st.ok()
                            ? "operation cancelled (statement may have completed; "
                            "cancel is best-effort)"
                            : "operation cancelled: " + st.message);
                }

                {
                    std::lock_guard<std::mutex> lk(op->sessionMtx);
                    op->pinnedSession = nullptr;
                }
                op->state.store(Handle::State::Done);
                registryRemove();
                deliverResult(std::move(ctx->cb), std::move(result));
            }

            static void armTimeout(const std::shared_ptr<OpState> &op,
                                   const std::chrono::milliseconds timeout) {
                workerExecutor()->postAfter([op] {
                    guarded([&] {
                        if (op->finished.load()) return;
                        op->timedOut.store(true);
                        core::Session *s = nullptr;
                        {
                            std::lock_guard<std::mutex> lk(op->sessionMtx);
                            s = op->pinnedSession;
                        }
                        if (s) op->cancelDelivered.store(s->cancel().ok());
                    });
                }, timeout);
            }

            template<class R>
            static Handle submitStatement(
                const std::shared_ptr<core::DataSource> &root, const std::string &sql,
                const common::Params &params, const common::OperationType gateType,
                const StatementPolicy &policy,
                std::function<void(core::Session &, R &)> attemptFn,
                std::function<std::function<common::Status()>(
                    const std::shared_ptr<core::DataSource> &primary)> bufferedMaker,
                std::function<void(R &&)> cb, const Options &opts) {
                if (!cb) {
                    return failNow<R>([](R &&) {
                                      },
                                      common::Status::error(common::ErrorCode::ConfigError,
                                                            "null callback"));
                }
                if (!root) {
                    return failNow<R>(std::move(cb),
                                      common::Status::error(common::ErrorCode::ConfigError,
                                                            "datasource not found"));
                }
                const auto ex = workerExecutor();
                if (!ex) {
                    return failNow<R>(std::move(cb),
                                      common::Status::error(
                                          common::ErrorCode::ConfigError,
                                          "async not enabled: call DBMW::init first "
                                          "or async::setExecutor"));
                }
                if (gStopping.load()) {
                    return failNow<R>(std::move(cb),
                                      common::Status::error(common::ErrorCode::PoolClosed,
                                                            "dbmw is shutting down"));
                }

                if (const auto g = root->preGate(sql, gateType); !g.ok()) {
                    return failNow<R>(std::move(cb), g);
                }
                if (root->isCircuitOpen()) {
                    return failNow<R>(std::move(cb),
                                      common::Status::error(
                                          common::ErrorCode::CircuitOpen,
                                          "datasource '" + root->name() + "' circuit is open"));
                }

                common::SqlContext routeCtx = common::ContextScope::current();
                core::detail::runOnRoute(root->name(), sql, gateType, routeCtx);
                std::vector<std::shared_ptr<core::DataSource> > targets;
                {
                    const common::ContextScope scope(routeCtx);
                    if (policy.isWrite) {
                        targets = root->writeTargets();
                        if (targets.empty() && !root->primary_) targets.push_back(root);
                    } else {
                        auto t = root->readTarget();
                        if (!t) t = root;
                        targets.push_back(t);
                        if (!routeCtx.shadow && root->primary_ &&
                            root->fallbackToPrimary_ && t != root->primary_)
                            targets.push_back(root->primary_);
                    }
                }

                auto ctx = std::make_shared<StatementOp<R> >();
                ctx->op = std::make_shared<OpState>();
                ctx->root = root;
                ctx->targets = std::move(targets);
                ctx->borrowTimeout = opts.borrowTimeout;
                ctx->sql = sql;
                ctx->params = params;
                ctx->attemptFn = std::move(attemptFn);
                ctx->bufferedMaker = std::move(bufferedMaker);
                ctx->cb = std::move(cb);
                ctx->policy = policy;
                ctx->entryCtx = std::move(routeCtx);

                if (policy.cacheable) {
                    if constexpr (std::is_same_v<R, QueryResult>) {
                        common::ResultSet cached;
                        std::string key;
                        if (ctx->targets.front()->cacheLookup(sql, params, cached, key)) {
                            QueryResult r;
                            r.status = common::Status::OK();
                            r.rows = std::move(cached);
                            {
                                core::ExecutionView view{root->name(), sql,
                                    common::OperationType::Query,
                                    &params, &r.rows, 0,
                                    std::chrono::microseconds{0},
                                    common::Status::OK(),
                                     true,  0,
                                    ctx->entryCtx};
                                core::detail::runAfterExecution(view);
                            }
                            deliverResult(std::move(ctx->cb), std::move(r));
                            return doneHandle();
                        }
                        ctx->cacheKey = std::move(key);
                    }
                }

                registryAdd();
                const Handle handle(ctx->op);

                const auto timeout = opts.timeout > std::chrono::milliseconds(0)
                                         ? opts.timeout
                                         : std::chrono::milliseconds(
                                             gDefaultTimeoutMs.load(std::memory_order_relaxed));
                if (timeout > std::chrono::milliseconds(0)) armTimeout(ctx->op, timeout);

                if (!ex->tryPost([ctx] { step1Statement(ctx); })) {
                    R r;
                    r.status = common::Status::error(
                        common::ErrorCode::Overloaded,
                        "async executor queue full (queue_size reached)");
                    r.status.retryable = true;
                    finishStatement(ctx, std::move(r));
                }
                return handle;
            }

            template<class R>
            static void step1Statement(const std::shared_ptr<StatementOp<R> > &ctx) {
                guarded([&] {
                    const auto &op = ctx->op;
                    Handle::State expected = Handle::State::Queued;
                    op->state.compare_exchange_strong(expected, Handle::State::Running);

                    if (op->finished.load()) return;
                    if (op->userCancelled.load()) {
                        R r;
                        r.status = common::Status::error(
                            common::ErrorCode::Cancelled,
                            "operation cancelled before start");
                        finishStatement(ctx, std::move(r));
                        return;
                    }

                    if (ctx->targetIdx >= ctx->targets.size()) {
                        R r;
                        r.status = common::Status::error(
                            common::ErrorCode::CircuitOpen,
                            "group '" + ctx->root->name() + "': no writable primary available");
                        r.status.retryable = true;
                        ctx->attempt = std::numeric_limits<int>::max();
                        handleAttemptFailure(ctx, std::move(r));
                        return;
                    }

                    const auto target = ctx->targets[ctx->targetIdx];
                    ++ctx->attempt;

                    if (const auto gate = target->beforeAttempt(); !gate.ok()) {
                        R r;
                        r.status = gate;
                        ctx->attempt = std::numeric_limits<int>::max();
                        handleAttemptFailure(ctx, std::move(r));
                        return;
                    }

                    const auto pool = target->pool();
                    if (!pool) {
                        R r;
                        r.status = common::Status::error(
                            common::ErrorCode::PoolClosed,
                            "datasource '" + target->name() + "' has been shut down");
                        handleAttemptFailure(ctx, std::move(r));
                        return;
                    }

                    pool->borrowAsync(ctx->borrowTimeout, makePoolIo(workerExecutor()),
                                      [ctx, target](std::unique_ptr<
                                                        core::ConnectionPool::Handle> h,
                                                    common::Status st) {
                                          step2Statement(ctx, target, std::move(h),
                                                         std::move(st));
                                      });
                });
            }

            template<class R>
            static void step2Statement(const std::shared_ptr<StatementOp<R> > &ctx,
                                       const std::shared_ptr<core::DataSource> &target,
                                       std::unique_ptr<core::ConnectionPool::Handle> h,
                                       common::Status borrowStatus) {
                guarded([&] {
                    const auto &op = ctx->op;
                    if (op->finished.load()) return;

                    if (!h) {
                        target->afterAttempt(borrowStatus);
                        handleAttemptFailure(ctx, [&] {
                            R r;
                            r.status = std::move(borrowStatus);
                            return r;
                        }());
                        return;
                    }

                    if (op->userCancelled.load()) {
                        R r;
                        r.status = common::Status::error(common::ErrorCode::Cancelled,
                                                         "operation cancelled before execution");
                        finishStatement(ctx, std::move(r));
                        return;
                    }

                    auto session = target->makeSession(std::move(h));
                    {
                        std::lock_guard<std::mutex> lk(op->sessionMtx);
                        op->pinnedSession = session.get();
                    }

                    R r;
                    try {
                        common::ContextScope scope(ctx->entryCtx);
                        ctx->attemptFn(*session, r);
                    } catch (const std::exception &e) {
                        r.status = common::Status::error(
                            common::ErrorCode::Unknown,
                            std::string("async attempt threw: ") + e.what());
                    } catch (...) {
                        r.status = common::Status::error(
                            common::ErrorCode::Unknown,
                            "async attempt threw an unknown exception");
                    }

                    {
                        std::lock_guard<std::mutex> lk(op->sessionMtx);
                        op->pinnedSession = nullptr;
                    }
                    target->afterAttempt(r.status);

                    if (r.status.ok()) {
                        if (ctx->policy.isWrite && !ctx->entryCtx.shadow) {
                            ctx->root->markWrite();
                            ctx->entryCtx.wroteInThisRequest = true;
                        }
                        if (!ctx->entryCtx.shadow) target->afterAttempt(r.status);
                        if (ctx->policy.cacheable && !ctx->entryCtx.shadow) {
                            if constexpr (std::is_same_v<R, QueryResult>) {
                                if (!ctx->cacheKey.empty())
                                    target->cacheStore(ctx->cacheKey, r.rows);
                            }
                        }
                        finishStatement(ctx, std::move(r));
                        return;
                    }
                    handleAttemptFailure(ctx, std::move(r));
                });
            }

            template<class R>
            static void handleAttemptFailure(const std::shared_ptr<StatementOp<R> > &ctx,
                                             R r) {
                const auto &op = ctx->op;
                if (op->finished.load()) return;
                const auto &st = r.status;

                if (op->userCancelled.load()) {
                    finishStatement(ctx, std::move(r));
                    return;
                }

                const auto target = ctx->targetIdx < ctx->targets.size()
                                        ? ctx->targets[ctx->targetIdx]
                                        : nullptr;

                if (st.retryable && target
                    && ctx->attempt < maxAttempts(ctx->policy, *target,
                                                  ctx->entryCtx.idempotency)) {
                    const auto delay = target->retryDelay(ctx->attempt);
                    scheduleNext(ctx, delay);
                    return;
                }

                const bool transferable = ctx->policy.isWrite
                    ? core::DataSource::safeToFailoverWrite(st)
                    : (st.retryable || st.connectionBroken ||
                       st.code == common::ErrorCode::CircuitOpen);
                bool rowsOk = true;
                if (ctx->policy.fallbackOnlyIfNoRows) {
                    if constexpr (std::is_same_v<R, EachResult>) rowsOk = r.rows == 0;
                }
                if (transferable && rowsOk && ctx->targetIdx + 1 < ctx->targets.size()) {
                    ++ctx->targetIdx;
                    ctx->attempt = 0;
                    scheduleNext(ctx, std::chrono::milliseconds(0));
                    return;
                }

                if (transferable && ctx->policy.isWrite && ctx->policy.allowWriteBuffer
                    && !ctx->entryCtx.shadow
                    && ctx->root->primary_
                    && ctx->root->writeBuffer_ && ctx->root->writeBuffer_->enabled()
                    && ctx->bufferedMaker) {
                    if (ctx->root->writeBuffer_->enqueue(ctx->bufferedMaker(
                        ctx->root->primary_))) {
                        auto accepted = common::Status::error(
                            common::ErrorCode::Buffered,
                            "group '" + ctx->root->name()
                            + "': write accepted into buffer, not yet committed");
                        accepted.retryable = false;
                        r.status = std::move(accepted);
                        finishStatement(ctx, std::move(r));
                        return;
                    }
                }
                finishStatement(ctx, std::move(r));
            }

            template<class R>
            static void scheduleNext(const std::shared_ptr<StatementOp<R> > &ctx,
                                     const std::chrono::milliseconds delay) {
                const auto ex = workerExecutor();
                if (!ex || gStopping.load()) {
                    R r;
                    r.status = common::Status::error(common::ErrorCode::PoolClosed,
                                                     "dbmw is shutting down");
                    finishStatement(ctx, std::move(r));
                    return;
                }
                ex->postAfter([ctx] { step1Statement(ctx); }, delay);
            }

            static int maxAttempts(const StatementPolicy &policy,
                                   const core::DataSource &target,
                                   const common::Idempotency idem) {
                if (idem == common::Idempotency::NonIdempotent
                    && policy.retry == RetryMode::WriteRetries) {
                    return 1;
                }
                if (idem == common::Idempotency::Idempotent
                    && policy.retry == RetryMode::WriteRetries) {
                    return std::max(1, target.retry_.max_attempts);
                }
                switch (policy.retry) {
                    case RetryMode::ReadRetries:
                        return std::max(1, target.retry_.max_attempts);
                    case RetryMode::WriteRetries:
                        return target.retry_.retry_writes
                                   ? std::max(1, target.retry_.max_attempts)
                                   : 1;
                    case RetryMode::Single:
                        return 1;
                }
                return 1;
            }

            static Handle submitSessionOp(std::shared_ptr<core::DataSource> root,
                                          const bool transactional,
                                          const common::TransactionOptions &txOpts,
                                          const core::SessionFn &fn,
                                          std::function<void(OpResult &&)> cb,
                                          const Options &opts) {
                if (!cb) {
                    return failNow<OpResult>([](OpResult &&) {
                                             },
                                             common::Status::error(
                                                 common::ErrorCode::ConfigError,
                                                 "null callback"));
                }
                if (!root) {
                    return failNow<OpResult>(std::move(cb),
                                             common::Status::error(
                                                 common::ErrorCode::ConfigError,
                                                 "datasource not found"));
                }
                const auto ex = workerExecutor();
                if (!ex) {
                    return failNow<OpResult>(std::move(cb),
                                             common::Status::error(
                                                 common::ErrorCode::ConfigError,
                                                 "async not enabled: call DBMW::init first "
                                                 "or async::setExecutor"));
                }
                if (gStopping.load()) {
                    return failNow<OpResult>(std::move(cb),
                                             common::Status::error(
                                                 common::ErrorCode::PoolClosed,
                                                 "dbmw is shutting down"));
                }

                if (const auto g = root->gateSession(); !g.ok()) {
                    return failNow<OpResult>(std::move(cb), g);
                }

                auto ctx = std::make_shared<SessionOp>();
                ctx->op = std::make_shared<OpState>();
                ctx->root = std::move(root);
                ctx->transactional = transactional;
                ctx->txOpts = txOpts;
                ctx->fn = fn;
                ctx->cb = std::move(cb);
                ctx->borrowTimeout = opts.borrowTimeout;
                ctx->entryCtx = common::ContextScope::current();

                registryAdd();
                const Handle handle(ctx->op);
                if (!ex->tryPost([ctx] { runSessionOp(ctx); })) {
                    OpResult r;
                    r.status = common::Status::error(
                        common::ErrorCode::Overloaded,
                        "async executor queue full (queue_size reached)");
                    r.status.retryable = true;
                    ctx->op->finished.store(true);
                    ctx->op->state.store(Handle::State::Done);
                    registryRemove();
                    deliverResult(std::move(ctx->cb), std::move(r));
                }
                return handle;
            }

            static void runSessionOp(const std::shared_ptr<SessionOp> &ctx) {
                guarded([&] {
                    const auto &op = ctx->op;
                    Handle::State expected = Handle::State::Queued;
                    op->state.compare_exchange_strong(expected, Handle::State::Running);

                    if (op->finished.load()) return;
                    if (op->userCancelled.load()) {
                        op->finished.store(true);
                        op->state.store(Handle::State::Done);
                        registryRemove();
                        OpResult r;
                        r.status = common::Status::error(common::ErrorCode::Cancelled,
                                                         "operation cancelled before start");
                        deliverResult(std::move(ctx->cb), std::move(r));
                        return;
                    }

                    common::Status st;
                    try {
                        common::ContextScope scope(ctx->entryCtx);
                        if (ctx->transactional) {
                            st = ctx->root->transactionInternal(
                                ctx->txOpts, ctx->fn, ctx->borrowTimeout,
                                ctx->root->readOnly_);
                        } else {
                            st = ctx->root->withSessionInternal(
                                ctx->fn, ctx->borrowTimeout, nullptr,
                                ctx->root->readOnly_);
                        }
                    } catch (const std::exception &e) {
                        st = common::Status::error(
                            common::ErrorCode::Unknown,
                            std::string("async session op threw: ") + e.what());
                    } catch (...) {
                        st = common::Status::error(
                            common::ErrorCode::Unknown,
                            "async session op threw an unknown exception");
                    }

                    op->finished.store(true);
                    op->state.store(Handle::State::Done);
                    registryRemove();
                    OpResult r;
                    r.status = std::move(st);
                    deliverResult(std::move(ctx->cb), std::move(r));
                });
            }

            static common::Status bufferedReplayExecute(
                const std::shared_ptr<core::DataSource> &primary,
                const std::string &sql, const common::Params &params) {
                std::int64_t ignored = 0;
                return primary->executeUngated(sql, params, ignored);
            }

            static common::Status bufferedReplayBatch(
                const std::shared_ptr<core::DataSource> &primary,
                const std::string &sql, const common::ParamBatch &batch) {
                common::BatchResult ignored;
                return primary->executeBatchUngated(sql, batch, ignored);
            }
        };

        void initEngine(const config::AsyncConfig &cfg) {
            std::lock_guard<std::mutex> lk(gMtx);
            gStopping.store(false);
            gDefaultTimeoutMs.store(
                std::max(0, cfg.statement_timeout_ms), std::memory_order_relaxed);
            if (!cfg.enabled) return;
            if (gExecutor) return;
            gExecutor = makeThreadPoolExecutor(cfg.threads,
                                               static_cast<std::size_t>(cfg.queue_size));
            gCompletion = gExecutor;
        }

        void drainAndStop(const std::chrono::milliseconds grace) {
            gStopping.store(true);

            {
                std::unique_lock<std::mutex> lk(gMtx);
                const auto deadline = std::chrono::steady_clock::now() + grace;
                while (gInFlight > 0) {
                    if (gDrainCv.wait_until(lk, deadline) == std::cv_status::timeout) break;
                }
            }

            std::shared_ptr<IExecutor> completion, main;
            {
                std::lock_guard<std::mutex> lk(gMtx);
                completion = gCompletion;
                main = gExecutor;
                gCompletion.reset();
                gExecutor.reset();
            }
            if (completion) completion->shutdown(std::chrono::milliseconds(0));
            if (main && main != completion) main->shutdown(grace);
        }

        std::size_t inFlight() {
            std::lock_guard<std::mutex> lk(gMtx);
            return gInFlight;
        }
    }

    Handle::State Handle::state() const {
        if (!s_) return State::Done;
        return s_->state.load(std::memory_order_acquire);
    }

    common::Status Handle::cancel() const {
        if (!s_) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "invalid handle (default-constructed or moved-from)");
        }
        const auto st = s_->state.load(std::memory_order_acquire);
        if (st == State::Done) {
            return common::Status::error(common::ErrorCode::QueryError,
                                         "operation already finished");
        }
        s_->userCancelled.store(true, std::memory_order_release);
        if (st == State::Running) {
            core::Session *s = nullptr;
            {
                std::lock_guard<std::mutex> lk(s_->sessionMtx);
                s = s_->pinnedSession;
            }
            if (s) return s->cancel();
            return common::Status::OK();
        }
        return common::Status::OK();
    }

    Handle query(const std::string &sql, QueryCallback cb, const Options opts) {
        return query(std::string(), sql, std::move(cb), opts);
    }

    Handle query(const std::string &sql, const common::Params &params,
                 QueryCallback cb, const Options opts) {
        return query(std::string(), sql, params, std::move(cb), opts);
    }

    Handle query(const std::string &dataSource, const std::string &sql,
                 QueryCallback cb, const Options opts) {
        return query(dataSource, sql, common::Params{}, std::move(cb), opts);
    }

    Handle query(const std::string &dataSource, const std::string &sql,
                 const common::Params &params, QueryCallback cb, const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = false;
        policy.retry = detail::RetryMode::ReadRetries;
        policy.cacheable = true;
        return detail::AsyncEngine::submitStatement<QueryResult>(
            resolve(dataSource), sql, params, common::OperationType::Query, policy,
            [sql, params](const core::Session &s, QueryResult &r) {
                if (params.empty()) r.status = s.query(sql, r.rows);
                else r.status = s.query(sql, params, r.rows);
            },
            {}, std::move(cb), opts);
    }

    Handle queryAll(const std::string &sql, MultiQueryCallback cb, const Options opts) {
        return queryAll(std::string(), sql, std::move(cb), opts);
    }

    Handle queryAll(const std::string &sql, const common::Params &params,
                    MultiQueryCallback cb, const Options opts) {
        return queryAll(std::string(), sql, params, std::move(cb), opts);
    }

    Handle queryAll(const std::string &dataSource, const std::string &sql,
                    MultiQueryCallback cb, const Options opts) {
        return queryAll(dataSource, sql, common::Params{}, std::move(cb), opts);
    }

    Handle queryAll(const std::string &dataSource, const std::string &sql,
                    const common::Params &params, MultiQueryCallback cb, const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = false;
        policy.retry = detail::RetryMode::ReadRetries;
        policy.cacheable = false;
        return detail::AsyncEngine::submitStatement<MultiQueryResult>(
            resolve(dataSource), sql, params, common::OperationType::Query, policy,
            [sql, params](const core::Session &s, MultiQueryResult &r) {
                r.status = s.queryAll(sql, params, r.sets);
            },
            {}, std::move(cb), opts);
    }

    Handle execute(const std::string &sql, ExecCallback cb, const Options opts) {
        return execute(std::string(), sql, std::move(cb), opts);
    }

    Handle execute(const std::string &sql, const common::Params &params,
                   ExecCallback cb, const Options opts) {
        return execute(std::string(), sql, params, std::move(cb), opts);
    }

    Handle execute(const std::string &dataSource, const std::string &sql,
                   ExecCallback cb, const Options opts) {
        return execute(dataSource, sql, common::Params{}, std::move(cb), opts);
    }

    Handle execute(const std::string &dataSource, const std::string &sql,
                   const common::Params &params, ExecCallback cb, const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = true;
        policy.retry = detail::RetryMode::WriteRetries;
        policy.allowWriteBuffer = true;
        return detail::AsyncEngine::submitStatement<ExecResult>(
            resolve(dataSource), sql, params, common::OperationType::Execute, policy,
            [sql, params](const core::Session &s, ExecResult &r) {
                if (params.empty()) r.status = s.execute(sql, r.affected);
                else r.status = s.execute(sql, params, r.affected);
            },
            [sql, params](const std::shared_ptr<core::DataSource> &primary) {
                return std::function<common::Status()>(
                    [primary, sql, params] {
                        return detail::AsyncEngine::bufferedReplayExecute(primary, sql, params);
                    });
            },
            std::move(cb), opts);
    }

    Handle execute(const std::string &sql, const common::Params &params,
                   ExecKeysCallback cb, const Options opts) {
        return execute(std::string(), sql, params, std::move(cb), opts);
    }

    Handle execute(const std::string &dataSource, const std::string &sql,
                   const common::Params &params, ExecKeysCallback cb, const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = true;
        policy.retry = detail::RetryMode::WriteRetries;
        policy.allowWriteBuffer = false;
        return detail::AsyncEngine::submitStatement<ExecKeysResult>(
            resolve(dataSource), sql, params, common::OperationType::Execute, policy,
            [sql, params](const core::Session &s, ExecKeysResult &r) {
                if (params.empty()) r.status = s.execute(sql, r.affected, r.keys);
                else r.status = s.execute(sql, params, r.affected, r.keys);
            },
            {}, std::move(cb), opts);
    }

    Handle queryEach(const std::string &sql, const common::Params &params,
                     const common::RowCallback &rowCb, EachCallback done,
                     const Options opts) {
        return queryEach(std::string(), sql, params, rowCb, std::move(done), opts);
    }

    Handle queryEach(const std::string &dataSource, const std::string &sql,
                     const common::Params &params, const common::RowCallback &rowCb,
                     EachCallback done, const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = false;
        policy.retry = detail::RetryMode::Single;
        policy.fallbackOnlyIfNoRows = true;
        return detail::AsyncEngine::submitStatement<EachResult>(
            resolve(dataSource), sql, params, common::OperationType::Stream, policy,
            [sql, params, rowCb](const core::Session &s, EachResult &r) {
                r.status = s.queryEach(sql, params, rowCb, r.rows);
            },
            {}, std::move(done), opts);
    }

    Handle executeBatch(const std::string &sql, const common::ParamBatch &batch,
                        BatchCallback cb, const Options opts) {
        return executeBatch(std::string(), sql, batch, std::move(cb), opts);
    }

    Handle executeBatch(const std::string &dataSource, const std::string &sql,
                        const common::ParamBatch &batch, BatchCallback cb,
                        const Options opts) {
        detail::StatementPolicy policy;
        policy.isWrite = true;
        policy.retry = detail::RetryMode::Single;
        policy.allowWriteBuffer = true;
        return detail::AsyncEngine::submitStatement<BatchResult>(
            resolve(dataSource), sql, {}, common::OperationType::Batch, policy,
            [sql, batch](const core::Session &s, BatchResult &r) {
                r.status = s.executeBatch(sql, batch, r.batch);
            },
            [sql, batch](const std::shared_ptr<core::DataSource> &primary) {
                return std::function<common::Status()>(
                    [primary, sql, batch] {
                        return detail::AsyncEngine::bufferedReplayBatch(
                            primary, sql, batch);
                    });
            },
            std::move(cb), opts);
    }

    Handle transaction(const core::SessionFn &fn, OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(std::string()), true, common::TransactionOptions{}, fn,
            std::move(cb), opts);
    }

    Handle transaction(const std::string &dataSource, const core::SessionFn &fn,
                       OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(dataSource), true, common::TransactionOptions{}, fn,
            std::move(cb), opts);
    }

    Handle transaction(const common::TransactionOptions &txOpts, const core::SessionFn &fn,
                       OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(std::string()), true, txOpts, fn, std::move(cb), opts);
    }

    Handle transaction(const std::string &dataSource, const common::TransactionOptions &txOpts,
                       const core::SessionFn &fn, OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(dataSource), true, txOpts, fn, std::move(cb), opts);
    }

    Handle withSession(const core::SessionFn &fn, OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(std::string()), false, common::TransactionOptions{}, fn,
            std::move(cb), opts);
    }

    Handle withSession(const std::string &dataSource, const core::SessionFn &fn,
                       OpCallback cb, const Options opts) {
        return detail::AsyncEngine::submitSessionOp(
            resolve(dataSource), false, common::TransactionOptions{}, fn,
            std::move(cb), opts);
    }

    namespace {
        template<class R>
        std::future<R> makeFuturePair(std::shared_ptr<std::promise<R> > &promiseOut) {
            auto promise = std::make_shared<std::promise<R> >();
            auto future = promise->get_future();
            promiseOut = promise;
            return future;
        }
    }

    std::future<QueryResult> query(const std::string &sql) {
        std::shared_ptr<std::promise<QueryResult> > p;
        auto f = makeFuturePair(p);
        query(sql, QueryCallback([p](QueryResult &&r) { p->set_value(std::move(r)); }),
              {});
        return f;
    }

    std::future<QueryResult> query(const std::string &sql, const common::Params &params) {
        return query(std::string(), sql, params);
    }

    std::future<QueryResult> query(const std::string &dataSource, const std::string &sql,
                                   const common::Params &params) {
        std::shared_ptr<std::promise<QueryResult> > p;
        auto f = makeFuturePair(p);
        query(dataSource, sql, params,
              QueryCallback([p](QueryResult &&r) { p->set_value(std::move(r)); }), {});
        return f;
    }

    std::future<ExecResult> execute(const std::string &sql) {
        return execute(sql, common::Params{});
    }

    std::future<ExecResult> execute(const std::string &sql, const common::Params &params) {
        return execute(std::string(), sql, params);
    }

    std::future<ExecResult> execute(const std::string &dataSource, const std::string &sql,
                                    const common::Params &params) {
        std::shared_ptr<std::promise<ExecResult> > p;
        auto f = makeFuturePair(p);
        execute(dataSource, sql, params,
                ExecCallback([p](ExecResult &&r) { p->set_value(std::move(r)); }), {});
        return f;
    }

    std::future<ExecKeysResult> executeKeys(const std::string &sql,
                                            const common::Params &params) {
        return executeKeys(std::string(), sql, params);
    }

    std::future<ExecKeysResult> executeKeys(const std::string &dataSource,
                                            const std::string &sql,
                                            const common::Params &params) {
        std::shared_ptr<std::promise<ExecKeysResult> > p;
        auto f = makeFuturePair(p);
        execute(dataSource, sql, params,
                ExecKeysCallback([p](ExecKeysResult &&r) { p->set_value(std::move(r)); }),
                {});
        return f;
    }

    std::future<EachResult> queryEach(const std::string &sql, const common::Params &params,
                                      const common::RowCallback &rowCb) {
        std::shared_ptr<std::promise<EachResult> > p;
        auto f = makeFuturePair(p);
        queryEach(std::string(), sql, params, rowCb,
                  EachCallback([p](EachResult &&r) { p->set_value(std::move(r)); }), {});
        return f;
    }

    std::future<BatchResult> executeBatch(const std::string &sql,
                                          const common::ParamBatch &batch) {
        std::shared_ptr<std::promise<BatchResult> > p;
        auto f = makeFuturePair(p);
        executeBatch(std::string(), sql, batch,
                     BatchCallback([p](BatchResult &&r) { p->set_value(std::move(r)); }),
                     {});
        return f;
    }

    std::future<OpResult> transaction(const core::SessionFn &fn) {
        std::shared_ptr<std::promise<OpResult> > p;
        auto f = makeFuturePair(p);
        transaction(std::string(), fn,
                    OpCallback([p](OpResult &&r) { p->set_value(std::move(r)); }), {});
        return f;
    }

    std::future<OpResult> transaction(const std::string &dataSource,
                                      const common::TransactionOptions &txOpts,
                                      const core::SessionFn &fn) {
        std::shared_ptr<std::promise<OpResult> > p;
        auto f = makeFuturePair(p);
        transaction(dataSource, txOpts, fn,
                    OpCallback([p](OpResult &&r) { p->set_value(std::move(r)); }), {});
        return f;
    }

    void setExecutor(std::shared_ptr<IExecutor> ex) {
        std::lock_guard<std::mutex> lk(gMtx);
        gExecutor = std::move(ex);
        if (!gCompletion) gCompletion = gExecutor;
    }

    void setCompletionExecutor(std::shared_ptr<IExecutor> ex) {
        std::lock_guard<std::mutex> lk(gMtx);
        gCompletion = std::move(ex);
    }

    ExecutorStats stats() {
        const auto ex = workerExecutor();
        return ex ? ex->stats() : ExecutorStats{};
    }
}
