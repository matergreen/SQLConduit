#include "dbmw/core/database_manager.h"

#include <algorithm>

#include "dbmw/driver/driver_factory.h"
#include "dbmw/common/logger.h"
#include "dbmw/common/observer.h"
#include "dbmw/common/sql_analyze.h"
#include "dbmw/core/sql_auditor.h"
#include "dbmw/core/query_cache.h"
#include "dbmw/core/stats_reporter.h"
#include "dbmw/core/interceptor.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <thread>
#include <unordered_set>
#include <variant>

namespace dbmw::core {
    namespace {
        thread_local int gTxDepth = 0;
    }

    int currentTransactionDepth() noexcept {
        return gTxDepth;
    }

    namespace {
        int resolveWriteAttempts(const config::RetryConfig &retry) {
            const auto idem = common::ContextScope::current().idempotency;
            if (idem == common::Idempotency::NonIdempotent) return 1;
            if (idem == common::Idempotency::Idempotent) return std::max(1, retry.max_attempts);
            return retry.retry_writes ? std::max(1, retry.max_attempts) : 1;
        }

        void pinRequestWrite() {
            auto &s = common::ContextScope::stack();
            if (s.empty()) return;
            const auto sz = s.size();
            if (sz >= 2) s[sz - 2].wroteInThisRequest = true;
            else s.back().wroteInThisRequest = true;
        }

        template<typename Fn>
        common::Status runWithInterceptors(ExecutionView &view,
                                           common::ResultSet *result,
                                           std::int64_t *affected,
                                           Fn &&fn) {
            auto guard = detail::makeInterceptorGuard(view);
            if (!guard.active()) return std::forward<Fn>(fn)();
            if (auto st = detail::runBeforeExecution(view); !st.ok()) {
                view.status = st;
                view.result = nullptr;
                return st;
            }
            const auto t0 = std::chrono::steady_clock::now();
            const common::ContextScope scope(view.ctx);
            auto st = std::forward<Fn>(fn)();
            view.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0);
            view.status = st;
            view.result = st.ok() ? result : nullptr;
            if (st.ok() && affected) view.affected = *affected;
            detail::runAfterExecution(view);
            return st;
        }

        std::int64_t randomJitter(const std::int64_t range) {
            if (range <= 0) return 0;
            static thread_local std::mt19937_64 engine = [] {
                std::uint64_t seed = std::random_device{}();
                seed ^= static_cast<std::uint64_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count());
                seed ^= static_cast<std::uint64_t>(
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
                return std::mt19937_64(seed);
            }();
            return std::uniform_int_distribution<std::int64_t>(0, range - 1)(engine);
        }

        std::atomic<bool> gPreparedEnabled{true};
        std::atomic<int> gPreparedMaxPerConn{0};

        void configurePreparedCache(const config::PreparedCacheConfig &cfg) {
            gPreparedEnabled.store(cfg.enabled);
            gPreparedMaxPerConn.store(cfg.max_per_connection);
        }

        bool preparedPathUsable(const IDatabaseConnection &conn) {
            return gPreparedEnabled.load(std::memory_order_relaxed) && conn.supportsPrepared();
        }

        template<typename Fn>
        common::Status observe(const std::string &dataSource,
                               const common::OperationType type,
                               std::uint64_t &rows, Fn &&fn) {
            const auto start = std::chrono::steady_clock::now();
            common::Status status = fn();
            common::OperationEvent event;
            event.dataSource = dataSource;
            event.type = type;
            event.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
            event.status = status;
            event.status.message.clear();
            event.rowCount = rows;
            common::Observability::emit(event);
            return status;
        }

        template<typename Fn>
        common::Status observeSqlImpl(const std::string &dataSource,
                                      const common::OperationType type,
                                      const std::string &sql,
                                      const common::Params &params,
                                      IDatabaseConnection *connection,
                                      common::ResultSet *result,
                                      std::uint64_t &rows, Fn &&fn);

        template<typename Fn>
        common::Status observeSql(const std::string &dataSource,
                                  const common::OperationType type,
                                  const std::string &sql,
                                  const common::Params &params,
                                  IDatabaseConnection *connection,
                                  std::uint64_t &rows, Fn &&fn) {
            return observeSqlImpl(dataSource, type, sql, params, connection,
                                  nullptr, rows, std::forward<Fn>(fn));
        }

        template<typename Fn>
        common::Status observeSql(const std::string &dataSource,
                                  const common::OperationType type,
                                  const std::string &sql,
                                  const common::Params &params,
                                  IDatabaseConnection *connection,
                                  common::ResultSet *result,
                                  std::uint64_t &rows, Fn &&fn) {
            return observeSqlImpl(dataSource, type, sql, params, connection,
                                  result, rows, std::forward<Fn>(fn));
        }

        template<typename Fn>
        common::Status observeSqlImpl(const std::string &dataSource,
                                      const common::OperationType type,
                                      const std::string &sql,
                                      const common::Params &params,
                                      IDatabaseConnection *connection,
                                      common::ResultSet *result,
                                      std::uint64_t &rows, Fn &&fn) {
            const auto start = std::chrono::steady_clock::now();
            common::Status status = fn();
            common::OperationEvent event;
            event.dataSource = dataSource;
            event.type = type;
            event.duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
            event.status = status;
            event.status.message.clear();
            event.rowCount = rows;
            common::SqlRenderer renderer;
            if (connection) {
                renderer = [connection, &sql, &params](
                    const common::SqlRenderOptions &options, std::string &out) {
                            return connection->renderSqlForLogging(sql, params, options, out);
                        };
            }
            common::Observability::emitSql(std::move(event), sql, renderer, result);
            return status;
        }

        common::Status runGuarded(Session &s, const SessionFn &fn) {
            try {
                return fn(s);
            } catch (const std::exception &e) {
                return common::Status::error(common::ErrorCode::TxError,
                                             std::string("exception in session: ") + e.what());
            } catch (...) {
                return common::Status::error(common::ErrorCode::TxError,
                                             "unknown exception in session");
            }
        }

        constexpr std::chrono::milliseconds kUsePoolDefault{-1};

        std::shared_ptr<IRateLimiter> makeRateLimiter(const config::RateLimitConfig &cfg) {
            if (cfg.enabled && cfg.global_qps > 0)
                return std::make_shared<RateLimiter>(
                    static_cast<double>(cfg.global_qps),
                    static_cast<double>(cfg.per_fingerprint_qps),
                    cfg.burst, cfg.fingerprint_mode);
            return DatabaseManager::defaultRateLimiter_;
        }

        std::string cacheKey(const std::string &sql, const common::Params &params) {
            std::string key = sql;
            key.push_back('\x1e');
            key += std::to_string(params.size());
            for (const auto &param: params) {
                key.push_back('\x1f');
                std::visit([&key](const auto &value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>) {
                        key.push_back('n');
                    } else if constexpr (std::is_same_v<T, bool>) {
                        key.push_back('b');
                        key.push_back(value ? '1' : '0');
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        key.push_back('i');
                        key += std::to_string(value);
                    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                        key.push_back('u');
                        key += std::to_string(value);
                    } else if constexpr (std::is_same_v<T, double>) {
                        std::uint64_t bits = 0;
                        std::memcpy(&bits, &value, sizeof(bits));
                        key.push_back('d');
                        key += std::to_string(bits);
                    } else if constexpr (std::is_same_v<T, common::Timestamp>) {
                        key.push_back('t');
                        key += std::to_string(value.time_since_epoch().count());
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        key.push_back('s');
                        key += std::to_string(value.size());
                        key.push_back(':');
                        key += value;
                    } else if constexpr (std::is_same_v<T, common::Decimal> ||
                                         std::is_same_v<T, common::Date> ||
                                         std::is_same_v<T, common::Time> ||
                                         std::is_same_v<T, common::Uuid> ||
                                         std::is_same_v<T, common::Json>) {
                        if constexpr (std::is_same_v<T, common::Decimal>) key.push_back('m');
                        else if constexpr (std::is_same_v<T, common::Date>) key.push_back('a');
                        else if constexpr (std::is_same_v<T, common::Time>) key.push_back('o');
                        else if constexpr (std::is_same_v<T, common::Uuid>) key.push_back('g');
                        else key.push_back('j');
                        key += std::to_string(value.value.size());
                        key.push_back(':');
                        key += value.value;
                    } else {
                        key.push_back('x');
                        key += std::to_string(value.size());
                        key.push_back(':');
                        key.append(reinterpret_cast<const char *>(value.data()), value.size());
                    }
                }, param);
            }
            return key;
        }
    }

    Session::~Session() {
        cleanupOpenTransaction();
    }

    void Session::cleanupOpenTransaction() noexcept {
        if (!txOpen_ || !h_) return;
        try {
            if ((*h_)->rollback().ok()) {
                txOpen_ = false;
                if (gTxDepth > 0) --gTxDepth;
                return;
            }
        } catch (...) {
        }
        txOpen_ = false;
        if (gTxDepth > 0) --gTxDepth;
        h_->invalidate();
    }

    common::Status Session::auditStatement(const std::string &sql,
                                           const common::OperationType type) const {
        if (!audit_.enabled) return common::Status::OK();
        return SqlAuditor::check(sql, type, audit_.readOnly);
    }

    common::Status Session::runPreparedQuery(const std::string &sql,
                                             const common::Params &params,
                                             common::ResultSet &out) const {
        IDatabaseConnection *conn = h_->get();
        if (!preparedPathUsable(*conn)) return conn->query(sql, params, out);

        PreparedStatementHandle handle;
        if (const auto st = conn->prepare(sql, params, handle); !st.ok())
            return conn->query(sql, params, out);
        return conn->executePrepared(handle, params, out);
    }

    common::Status Session::runPreparedExec(const std::string &sql,
                                            const common::Params &params,
                                            std::int64_t &affected,
                                            common::GeneratedKeys *keys) const {
        IDatabaseConnection *conn = h_->get();
        if (keys || !preparedPathUsable(*conn)) {
            return keys
                       ? conn->execute(sql, params, affected, *keys)
                       : conn->execute(sql, params, affected);
        }
        PreparedStatementHandle handle;
        if (const auto st = conn->prepare(sql, params, handle); !st.ok())
            return conn->execute(sql, params, affected);
        return conn->executePrepared(handle, params, affected);
    }

    common::Status Session::query(const std::string &sql, common::ResultSet &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Query); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Query,
            nullptr, &out,
            0, std::chrono::microseconds{0},
            common::Status::OK(), false,
            0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            std::uint64_t rows = 0;
            const common::Params params;
            const auto status = observeSql(dataSource_, common::OperationType::Query, sql, params,
                                           h_->get(), &out, rows, [&] {
                                               const auto result = (*h_)->query(sql, out);
                                               rows = out.rowCount();
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::query(const std::string &sql, const common::Params &params,
                                  common::ResultSet &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Query); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Query,
            &params, &out, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            std::uint64_t rows = 0;
            const auto status = observeSql(dataSource_, common::OperationType::Query, sql, params,
                                           h_->get(), rows, [&] {
                                               const auto result = runPreparedQuery(sql, params, out);
                                               rows = out.rowCount();
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::queryAll(const std::string &sql,
                                     std::vector<common::ResultSet> &out) const {
        return queryAll(sql, common::Params{}, out);
    }

    // Collects every result set produced by a single statement (typically CALL).
    // Interceptors are intentionally bypassed: their ExecutionView carries one
    // ResultSet, so there is no well-defined way to hand them N sets.
    common::Status Session::queryAll(const std::string &sql, const common::Params &params,
                                     std::vector<common::ResultSet> &out) const {
        out.clear();
        if (const auto a = auditStatement(sql, common::OperationType::Query); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Query, ctx);
        std::uint64_t rows = 0;
        const auto status = observeSql(dataSource_, common::OperationType::Query, sql, params,
                                       h_->get(), rows, [&] {
                                           const auto r = (*h_)->queryAll(sql, params, out);
                                           for (const auto &set: out) rows += set.rowCount();
                                           return r;
                                       });
        if (status.connectionBroken) h_->invalidate();
        return status;
    }

    common::Status Session::execute(const std::string &sql, std::int64_t &affected) const {
        if (const auto a = auditStatement(sql, common::OperationType::Execute); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const common::Params params;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, sql, params,
                                           h_->get(), rows, [&] {
                                               const auto result = (*h_)->execute(sql, affected);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::execute(const std::string &sql, const common::Params &params,
                                    std::int64_t &affected) const {
        if (const auto a = auditStatement(sql, common::OperationType::Execute); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Execute,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, sql, params,
                                           h_->get(), rows, [&] {
                                               const auto result = runPreparedExec(sql, params, affected, nullptr);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::queryEach(const std::string &sql, const common::Params &params,
                                      const common::RowCallback &callback,
                                      std::uint64_t &rows) const {
        if (const auto a = auditStatement(sql, common::OperationType::Stream); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Stream, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Stream,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            std::uint64_t observedRows = 0;
            std::exception_ptr callbackError;
            const common::RowCallback guardedCallback = [&](const common::Row &row) {
                try {
                    auto transformed = row;
                    detail::runOnRow(view, transformed);
                    return callback(transformed);
                } catch (...) {
                    callbackError = std::current_exception();
                    return false;
                }
            };
            const auto status = observeSql(dataSource_, common::OperationType::Stream, sql, params,
                                           h_->get(), observedRows, [&] {
                                               auto result = (*h_)->queryEach(sql, params, guardedCallback, rows);
                                               if (result.ok() && callbackError) {
                                                   try {
                                                       std::rethrow_exception(callbackError);
                                                   } catch (const std::exception &e) {
                                                       result = common::Status::error(
                                                           common::ErrorCode::QueryError,
                                                           std::string("stream callback threw: ") + e.what());
                                                   } catch (...) {
                                                       result = common::Status::error(
                                                           common::ErrorCode::QueryError,
                                                           "stream callback threw an unknown exception");
                                                   }
                                               }
                                               observedRows = rows;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::executeBatch(const std::string &sql,
                                         const common::ParamBatch &batch,
                                         common::BatchResult &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Batch); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Batch, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Batch,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            std::uint64_t rows = 0;
            const common::Params noParams;
            const auto status = observeSql(dataSource_, common::OperationType::Batch, sql, noParams,
                                           nullptr, rows, [&] {
                                               const auto result = (*h_)->executeBatch(sql, batch, out);
                                               rows = out.totalAffected() > 0
                                                          ? static_cast<std::uint64_t>(out.totalAffected())
                                                          : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::execute(const std::string &sql, std::int64_t &affected,
                                    common::GeneratedKeys &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Execute); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const common::Params params;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, sql, params,
                                           h_->get(), rows, [&] {
                                               const auto result = (*h_)->execute(sql, affected, out);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::execute(const std::string &sql, const common::Params &params,
                                    std::int64_t &affected, common::GeneratedKeys &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Execute); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Execute,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, sql, params,
                                           h_->get(), rows, [&] {
                                               const auto result = runPreparedExec(sql, params, affected, &out);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::query(const std::string &sql, const common::StreamParams &params,
                                  common::ResultSet &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Query); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Query,
            nullptr, &out, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            std::uint64_t rows = 0;
            const common::Params noParams;
            const auto status = observeSql(dataSource_, common::OperationType::Query, sql, noParams,
                                           h_->get(), rows, [&] {
                                               const auto result = (*h_)->query(sql, params, out);
                                               rows = out.rowCount();
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::execute(const std::string &sql, const common::StreamParams &params,
                                    std::int64_t &affected, common::GeneratedKeys &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Execute); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const common::Params noParams;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, sql, noParams,
                                           h_->get(), rows, [&] {
                                               const auto result = (*h_)->execute(sql, params, affected, out);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::executeBatch(const std::string &sql,
                                         const common::StreamParamBatch &batch,
                                         common::BatchResult &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Batch); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Batch, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Batch,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            std::uint64_t rows = 0;
            const common::Params noParams;
            const auto status = observeSql(dataSource_, common::OperationType::Batch, sql, noParams,
                                           nullptr, rows, [&] {
                                               const auto result = (*h_)->executeBatch(sql, batch, out);
                                               rows = out.totalAffected() > 0
                                                          ? static_cast<std::uint64_t>(out.totalAffected())
                                                          : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::prepare(const std::string &sql, const common::Params &typesSample,
                                    PreparedStatementHandle &out) const {
        out = PreparedStatementHandle{};
        if (const auto a = auditStatement(sql, common::OperationType::Query); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Query,
            &typesSample, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            return (*h_)->prepare(sql, typesSample, out);
        });
    }

    common::Status Session::executePrepared(const PreparedStatementHandle &h,
                                            const common::Params &params,
                                            common::ResultSet &out) const {
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, "<prepared>", common::OperationType::Query, ctx);
        ExecutionView view{
            dataSource_, "<prepared>", common::OperationType::Query,
            &params, &out, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            std::uint64_t rows = 0;
            const auto status = observeSql(dataSource_, common::OperationType::Query, "<prepared>",
                                           params, h_->get(), &out, rows, [&] {
                                               const auto result = (*h_)->executePrepared(h, params, out);
                                               rows = out.rowCount();
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    common::Status Session::executePrepared(const PreparedStatementHandle &h,
                                            const common::Params &params,
                                            std::int64_t &affected) const {
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, "<prepared>", common::OperationType::Execute, ctx);
        ExecutionView view{
            dataSource_, "<prepared>", common::OperationType::Execute,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            std::uint64_t rows = 0;
            const auto status = observeSql(dataSource_, common::OperationType::Execute, "<prepared>",
                                           params, h_->get(), rows, [&] {
                                               const auto result = (*h_)->executePrepared(h, params, affected);
                                               rows = affected > 0 ? static_cast<std::uint64_t>(affected) : 0;
                                               if (result.ok()) didWrite_ = true;
                                               return result;
                                           });
            if (status.connectionBroken) h_->invalidate();
            return status;
        });
    }

    Cursor::~Cursor() noexcept {
        if (impl_) {
            try {
                impl_->close();
            } catch (...) {
                DBMW_LOG_WARN("cursor: close on destruction failed");
            }
            impl_.reset();
        }
        cursorLease_.reset();
    }

    common::Status Session::openCursor(const std::string &sql, const common::Params &params,
                                       const CursorOptions &opts,
                                       std::unique_ptr<Cursor> &out) const {
        if (const auto a = auditStatement(sql, common::OperationType::Select); !a.ok()) return a;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(dataSource_, sql, common::OperationType::Select, ctx);
        ExecutionView view{
            dataSource_, sql, common::OperationType::Select,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            std::unique_ptr<ICursor> impl;
            const auto status = (*h_)->openCursor(sql, params, opts, impl);
            if (!status.ok()) return status;
            if (!impl)
                return common::Status::error(common::ErrorCode::CursorError,
                                             "driver opened no cursor");
            Cursor::RowTransform transform = [dataSource = dataSource_, sql, params, ctx]
            (common::Row &row) mutable {
                ExecutionView rowView{
                    dataSource, sql, common::OperationType::Select,
                    &params, nullptr, 0, std::chrono::microseconds{0},
                    common::Status::OK(), false, 0, ctx
                };
                detail::runOnRow(rowView, row);
            };
            out = std::make_unique<Cursor>(nullptr, std::move(impl), audit_,
                                           Cursor::Binding::BorrowedInSession,
                                           std::shared_ptr<void>{}, std::move(transform));
            return common::Status::OK();
        });
    }

    common::Status Session::begin() {
        std::uint64_t rows = 0;
        const auto st = observe(dataSource_, common::OperationType::Begin, rows,
                                [&] { return (*h_)->begin(); });
        if (st.connectionBroken) h_->invalidate();
        if (st.ok()) {
            if (!txOpen_) ++gTxDepth;
            txOpen_ = true;
        }
        return st;
    }

    common::Status Session::begin(const common::TransactionOptions &options) {
        std::uint64_t rows = 0;
        const auto st = observe(dataSource_, common::OperationType::Begin, rows,
                                [&] { return (*h_)->begin(options); });
        if (st.connectionBroken) h_->invalidate();
        if (st.ok()) {
            if (!txOpen_) ++gTxDepth;
            txOpen_ = true;
        }
        return st;
    }

    common::Status Session::commit() {
        std::uint64_t rows = 0;
        const auto st = observe(dataSource_, common::OperationType::Commit, rows,
                                [&] { return (*h_)->commit(); });
        if (st.connectionBroken) h_->invalidate();
        if (txOpen_) {
            txOpen_ = false;
            if (gTxDepth > 0) --gTxDepth;
        }
        return st;
    }

    common::Status Session::rollback() {
        std::uint64_t rows = 0;
        const auto st = observe(dataSource_, common::OperationType::Rollback, rows,
                                [&] { return (*h_)->rollback(); });
        if (st.connectionBroken) h_->invalidate();
        if (txOpen_) {
            txOpen_ = false;
            if (gTxDepth > 0) --gTxDepth;
        }
        return st;
    }

    common::Status Session::savepoint(const std::string &name) {
        std::uint64_t rows = 0;
        const auto status = observe(dataSource_, common::OperationType::Savepoint, rows,
                                    [&] { return (*h_)->savepoint(name); });
        if (status.connectionBroken) h_->invalidate();
        return status;
    }

    common::Status Session::releaseSavepoint(const std::string &name) {
        std::uint64_t rows = 0;
        const auto status = observe(dataSource_, common::OperationType::Savepoint, rows,
                                    [&] { return (*h_)->releaseSavepoint(name); });
        if (status.connectionBroken) h_->invalidate();
        return status;
    }

    common::Status Session::rollbackToSavepoint(const std::string &name) {
        std::uint64_t rows = 0;
        const auto status = observe(dataSource_, common::OperationType::Savepoint, rows,
                                    [&] { return (*h_)->rollbackToSavepoint(name); });
        if (status.connectionBroken) h_->invalidate();
        return status;
    }

    common::Status Session::cancel() const {
        std::uint64_t rows = 0;
        const auto status = observe(dataSource_, common::OperationType::Cancel, rows, [&] {
            try {
                return (*h_)->cancel();
            } catch (const std::exception &e) {
                return common::Status::error(common::ErrorCode::Cancelled,
                                             std::string("driver cancel threw: ") + e.what());
            } catch (...) {
                return common::Status::error(common::ErrorCode::Cancelled,
                                             "driver cancel threw an unknown exception");
            }
        });
        if (status.connectionBroken) h_->invalidate();
        return status;
    }

    std::shared_ptr<DataSource> DataSource::readTarget() const {
        if (!primary_) return nullptr;
        if (shadow_ && common::ContextScope::current().shadow) return shadow_;
        if (common::ContextScope::current().wroteInThisRequest) return primary_;
        if (readAfterWrite_ > std::chrono::milliseconds(0)) {
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto last = lastWriteNs_.load();
            if (last > 0 && now - last < std::chrono::duration_cast<std::chrono::nanoseconds>(
                    readAfterWrite_).count())
                return primary_;
        }
        if (replicas_.empty()) return primary_;
        thread_local std::uint64_t tlsRound = 0;
        const auto start = (tlsRound++) % replicas_.size();
        for (std::size_t offset = 0; offset < replicas_.size(); ++offset) {
            const auto &candidate = replicas_[(start + offset) % replicas_.size()];
            if (candidate && !candidate->isCircuitOpen()) return candidate;
        }
        return primary_;
    }

    void DataSource::markWrite() const {
        if (primary_) {
            lastWriteNs_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            QueryCache::invalidate(name_);
            QueryCache::invalidate(primary_->name_);
            for (const auto &replica: replicas_) QueryCache::invalidate(replica->name_);
            for (const auto &candidate: failoverPrimaries_)
                if (candidate) QueryCache::invalidate(candidate->name_);
            return;
        }
        QueryCache::invalidate(name_);
        pinRequestWrite();
    }

    common::Status DataSource::preGate(const std::string &sql,
                                       const common::OperationType type) const {
        if (const auto s = SqlAuditor::check(sql, type, readOnly_); !s.ok()) return s;
        if (rateLimiter_) {
            const std::uint64_t fp = rateLimiter_->usesFingerprint()
                                         ? common::sql::fingerprintTemplate(sql)
                                         : 0;
            if (!rateLimiter_->acquire(fp)) {
                auto status = common::Status::error(common::ErrorCode::RateLimited,
                                                    "datasource '" + name_ + "' rate limited");
                status.retryable = false;
                return status;
            }
        }
        return common::Status::OK();
    }

    common::Status DataSource::gateSession() const {
        if (!rateLimiter_) return common::Status::OK();
        if (!rateLimiter_->acquire(0)) {
            auto status = common::Status::error(common::ErrorCode::RateLimited,
                                                "datasource '" + name_ + "' rate limited");
            status.retryable = false;
            return status;
        }
        return common::Status::OK();
    }

    bool DataSource::isCircuitOpen() const {
        if (circuitBreaker_.failure_threshold <= 0) return false;
        return circuitOpenUntil_.load(std::memory_order_acquire) >
               std::chrono::steady_clock::now();
    }

    std::vector<std::shared_ptr<DataSource> > DataSource::writeTargets() const {
        std::vector<std::shared_ptr<DataSource> > targets;
        if (!primary_) return targets;
        if (shadow_ && common::ContextScope::current().shadow) {
            targets.push_back(shadow_);
            return targets;
        }
        if (failoverPrimaries_.empty()) {
            targets.push_back(primary_);
            return targets;
        }
        targets.reserve(failoverPrimaries_.size());
        for (const auto &candidate: failoverPrimaries_) {
            if (!candidate) continue;
            if (candidate->isCircuitOpen()) continue;
            if (requireHealthy_ && candidate->pool_.expired()) continue;
            targets.push_back(candidate);
        }
        return targets;
    }

    bool DataSource::safeToFailoverWrite(const common::Status &status) {
        switch (status.code) {
            case common::ErrorCode::ConnectionFailed:
                return !status.connectionBroken && status.sqlState.empty();
            case common::ErrorCode::PoolExhausted:
            case common::ErrorCode::PoolClosed:
            case common::ErrorCode::CircuitOpen:
            case common::ErrorCode::DriverDisabled:
                return true;
            default:
                return false;
        }
    }

    common::Status DataSource::dispatchWrite(
        const std::function<common::Status(const std::shared_ptr<DataSource> &)> &attempt,
        const std::function<common::Status()> &buffered) const {
        if (shadow_ && common::ContextScope::current().shadow) {
            const auto st = attempt(shadow_);
            return st;
        }
        const auto targets = writeTargets();

        auto status = common::Status::error(
            common::ErrorCode::CircuitOpen,
            "group '" + name_ + "': no writable primary available");
        status.retryable = true;

        for (const auto &target: targets) {
            status = attempt(target);
            if (status.ok()) {
                markWrite();
                return status;
            }
            if (!safeToFailoverWrite(status))
                return status;
        }

        if (buffered && writeBuffer_ && writeBuffer_->enabled() &&
            writeBuffer_->enqueue(buffered)) {
            auto accepted = common::Status::error(
                common::ErrorCode::Buffered,
                "group '" + name_ + "': write accepted into buffer, not yet committed");
            accepted.retryable = false;
            DBMW_LOG_WARN("group [" + name_ + "] no writable primary, write buffered");
            return accepted;
        }
        return status;
    }

    common::Status DataSource::beforeAttempt() const {
        if (circuitBreaker_.failure_threshold <= 0) return common::Status::OK();
        const auto now = std::chrono::steady_clock::now();
        const auto openUntil = circuitOpenUntil_.load(std::memory_order_acquire);
        if (openUntil > now) {
            return common::Status::error(common::ErrorCode::CircuitOpen,
                                         "datasource '" + name_ + "' circuit is open");
        }
        if (openUntil != std::chrono::steady_clock::time_point{}) {
            if (bool expected = false; !halfOpenInFlight_.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
                return common::Status::error(common::ErrorCode::CircuitOpen,
                                             "datasource '" + name_ + "' circuit is half-open");
            }
        }
        return common::Status::OK();
    }

    void DataSource::afterAttempt(const common::Status &status) const {
        if (circuitBreaker_.failure_threshold <= 0) return;
        if (status.ok()) {
            consecutiveFailures_.store(0, std::memory_order_release);
            halfOpenInFlight_.store(false, std::memory_order_release);
            circuitOpenUntil_.store(std::chrono::steady_clock::time_point{},
                                    std::memory_order_release);
            return;
        }
        halfOpenInFlight_.store(false, std::memory_order_release);
        if (!status.retryable && !status.connectionBroken) {
            consecutiveFailures_.store(0, std::memory_order_release);
            circuitOpenUntil_.store(std::chrono::steady_clock::time_point{},
                                    std::memory_order_release);
            return;
        }
        if (const int n = ++consecutiveFailures_; n >= circuitBreaker_.failure_threshold) {
            circuitOpenUntil_.store(std::chrono::steady_clock::now()
                                    + std::chrono::milliseconds(circuitBreaker_.open_interval_ms),
                                    std::memory_order_release);
        }
    }

    std::chrono::milliseconds DataSource::retryDelay(const int attempt) const {
        if (retry_.initial_backoff_ms <= 0) return std::chrono::milliseconds(0);
        std::int64_t delay = retry_.initial_backoff_ms;
        for (int i = 1; i < attempt && delay < retry_.max_backoff_ms; ++i)
            delay = std::min<std::int64_t>(delay * 2, retry_.max_backoff_ms);
        const auto range = std::max<std::int64_t>(1, delay / 4 + 1);
        const auto jitter = randomJitter(range);
        return std::chrono::milliseconds(std::min<std::int64_t>(
            retry_.max_backoff_ms, delay + jitter));
    }

    common::Status DataSource::borrowSession(std::unique_ptr<ConnectionPool::Handle> &out,
                                             std::chrono::milliseconds timeout) const {
        const auto pool = pool_.lock();
        if (!pool) {
            return common::Status::error(common::ErrorCode::PoolClosed,
                                         "datasource '" + name_ + "' has been shut down");
        }
        common::ErrorCode code = common::ErrorCode::Ok;
        std::string err;
        auto h = pool->borrow(code, err, timeout);
        if (!h) {
            auto status = common::Status::error(code, err);
            if (code == common::ErrorCode::ConnectionFailed) {
                status.retryable = true;
                status.connectionBroken = true;
            }
            return status;
        }
        (*h)->setPreparedCacheLimit(gPreparedMaxPerConn.load(std::memory_order_relaxed));
        out = std::move(h);
        return common::Status::OK();
    }

    bool DataSource::cacheEligible() const {
        return !primary_ && QueryCache::enabled() &&
               (!QueryCache::replicaOnly() || readReplica_.load(std::memory_order_acquire));
    }

    bool DataSource::cacheLookup(const std::string &sql, const common::Params &params,
                                 common::ResultSet &out, std::string &key) const {
        if (!cacheEligible()) return false;
        if (common::ContextScope::current().shadow) return false;
        key = cacheKey(sql, params);
        return QueryCache::get(name_, key, out);
    }

    void DataSource::cacheStore(const std::string &key, const common::ResultSet &rows) const {
        if (common::ContextScope::current().shadow) return;
        if (rows.transformed) return;
        if (!primary_ && QueryCache::enabled()) QueryCache::put(name_, key, rows);
    }

    common::Status DataSource::query(const std::string &sql, common::ResultSet &out) const {
        if (const auto g = preGate(sql, common::OperationType::Query); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Query,
            nullptr, &out,
            0, std::chrono::microseconds{0},
            common::Status::OK(), false,
            0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            return queryUngated(sql, out);
        });
    }

    common::Status DataSource::queryUngated(const std::string &sql, common::ResultSet &out) const {
        if (primary_) {
            const auto target = readTarget();
            auto status = target->queryUngated(sql, out);
            if (target != primary_ && fallbackToPrimary_ &&
                (status.retryable || status.connectionBroken ||
                 status.code == common::ErrorCode::CircuitOpen)) {
                out.clear();
                status = primary_->queryUngated(sql, out);
            }
            return status;
        }
        const bool caching = QueryCache::enabled() &&
                             (!QueryCache::replicaOnly() || readReplica_.load(std::memory_order_acquire)) &&
                             !common::ContextScope::current().shadow;
        std::string key;
        if (caching) {
            key = cacheKey(sql, common::Params{});
            if (QueryCache::get(name_, key, out)) return common::Status::OK();
        }
        common::Status status;
        const int attempts = std::max(1, retry_.max_attempts);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            if (attempt > 1) out.clear();
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.query(sql, out);
            }
            afterAttempt(status);
            if (status.ok()) {
                if (caching) QueryCache::put(name_, key, out);
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::query(const std::string &sql, const common::Params &params,
                                     common::ResultSet &out) const {
        if (const auto g = preGate(sql, common::OperationType::Query); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Query,
            &params, &out, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            return queryUngated(sql, params, out);
        });
    }

    common::Status DataSource::queryUngated(const std::string &sql, const common::Params &params,
                                            common::ResultSet &out) const {
        if (primary_) {
            const auto target = readTarget();
            auto status = target->queryUngated(sql, params, out);
            if (target != primary_ && fallbackToPrimary_ &&
                (status.retryable || status.connectionBroken ||
                 status.code == common::ErrorCode::CircuitOpen)) {
                out.clear();
                status = primary_->queryUngated(sql, params, out);
            }
            return status;
        }
        const bool caching = QueryCache::enabled() &&
                             (!QueryCache::replicaOnly() || readReplica_.load(std::memory_order_acquire)) &&
                             !common::ContextScope::current().shadow;
        std::string key;
        if (caching) {
            key = cacheKey(sql, params);
            if (QueryCache::get(name_, key, out)) return common::Status::OK();
        }
        common::Status status;
        const int attempts = std::max(1, retry_.max_attempts);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            if (attempt > 1) out.clear();
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.query(sql, params, out);
            }
            afterAttempt(status);
            if (status.ok()) {
                if (caching) QueryCache::put(name_, key, out);
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    // Multiple result sets are never cached: the cache stores a single ResultSet,
    // so caching would silently drop every set after the first.
    common::Status DataSource::queryAll(const std::string &sql,
                                        std::vector<common::ResultSet> &out) const {
        return queryAll(sql, common::Params{}, out);
    }

    common::Status DataSource::queryAll(const std::string &sql, const common::Params &params,
                                        std::vector<common::ResultSet> &out) const {
        out.clear();
        if (const auto g = preGate(sql, common::OperationType::Query); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Query, ctx);
        common::Status status;
        const int attempts = std::max(1, retry_.max_attempts);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            if (attempt > 1) out.clear();
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.queryAll(sql, params, out);
            }
            afterAttempt(status);
            if (status.ok()) return status;
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::execute(const std::string &sql, std::int64_t &affected) const {
        if (const auto g = preGate(sql, common::OperationType::Execute); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            return executeUngated(sql, affected);
        });
    }

    common::Status DataSource::executeUngated(const std::string &sql,
                                              std::int64_t &affected) const {
        if (primary_) {
            std::function<common::Status()> buffered;
            if (writeBuffer_ && writeBuffer_->enabled()) {
                buffered = [primary = primary_, bufferedSql = sql] {
                    std::int64_t ignored = 0;
                    return primary->executeUngated(bufferedSql, ignored);
                };
            }
            return dispatchWrite(
                [&sql, &affected](const std::shared_ptr<DataSource> &target) {
                    affected = 0;
                    return target->executeUngated(sql, affected);
                },
                buffered);
        }
        common::Status status;
        const int attempts = resolveWriteAttempts(retry_);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            affected = 0;
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.execute(sql, affected);
            }
            afterAttempt(status);
            if (status.ok()) {
                markWrite();
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::execute(const std::string &sql, const common::Params &params,
                                       std::int64_t &affected) const {
        if (const auto g = preGate(sql, common::OperationType::Execute); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Execute,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            return executeUngated(sql, params, affected);
        });
    }

    common::Status DataSource::executeUngated(const std::string &sql,
                                              const common::Params &params,
                                              std::int64_t &affected) const {
        if (primary_) {
            std::function<common::Status()> buffered;
            if (writeBuffer_ && writeBuffer_->enabled()) {
                buffered = [primary = primary_, bufferedSql = sql, bufferedParams = params] {
                    std::int64_t ignored = 0;
                    return primary->executeUngated(bufferedSql, bufferedParams, ignored);
                };
            }
            return dispatchWrite(
                [&sql, &params, &affected](const std::shared_ptr<DataSource> &target) {
                    affected = 0;
                    return target->executeUngated(sql, params, affected);
                },
                buffered);
        }
        common::Status status;
        const int attempts = resolveWriteAttempts(retry_);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            affected = 0;
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.execute(sql, params, affected);
            }
            afterAttempt(status);
            if (status.ok()) {
                markWrite();
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::execute(const std::string &sql, std::int64_t &affected,
                                       common::GeneratedKeys &out) const {
        if (const auto g = preGate(sql, common::OperationType::Execute); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            return executeUngated(sql, affected, out);
        });
    }

    common::Status DataSource::executeUngated(const std::string &sql, std::int64_t &affected,
                                              common::GeneratedKeys &out) const {
        if (primary_) {
            return dispatchWrite(
                [&sql, &affected, &out](const std::shared_ptr<DataSource> &target) {
                    affected = 0;
                    out.clear();
                    return target->executeUngated(sql, affected, out);
                },
                {});
        }
        common::Status status;
        const int attempts = resolveWriteAttempts(retry_);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            affected = 0;
            out.clear();
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.execute(sql, affected, out);
            }
            afterAttempt(status);
            if (status.ok()) {
                markWrite();
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::execute(const std::string &sql, const common::Params &params,
                                       std::int64_t &affected,
                                       common::GeneratedKeys &out) const {
        if (const auto g = preGate(sql, common::OperationType::Execute); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Execute,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            return executeUngated(sql, params, affected, out);
        });
    }

    common::Status DataSource::executeUngated(const std::string &sql,
                                              const common::Params &params,
                                              std::int64_t &affected,
                                              common::GeneratedKeys &out) const {
        if (primary_) {
            return dispatchWrite(
                [&sql, &params, &affected, &out](const std::shared_ptr<DataSource> &target) {
                    affected = 0;
                    out.clear();
                    return target->executeUngated(sql, params, affected, out);
                },
                {});
        }
        common::Status status;
        const int attempts = resolveWriteAttempts(retry_);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            affected = 0;
            out.clear();
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                Session s(std::move(h), name_);
                status = s.execute(sql, params, affected, out);
            }
            afterAttempt(status);
            if (status.ok()) {
                markWrite();
                return status;
            }
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    common::Status DataSource::query(const std::string &sql, const common::StreamParams &params,
                                     common::ResultSet &out) const {
        if (const auto g = preGate(sql, common::OperationType::Query); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Query, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Query,
            nullptr, &out, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, &out, nullptr, [&] {
            return queryUngated(sql, params, out);
        });
    }

    common::Status DataSource::queryUngated(const std::string &sql,
                                            const common::StreamParams &params,
                                            common::ResultSet &out) const {
        if (primary_) {
            const auto target = readTarget();
            auto status = target->queryUngated(sql, params, out);
            if (target != primary_ && fallbackToPrimary_ &&
                (status.retryable || status.connectionBroken ||
                 status.code == common::ErrorCode::CircuitOpen)) {
                out.clear();
                status = primary_->queryUngated(sql, params, out);
            }
            return status;
        }
        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, kUsePoolDefault);
        if (status.ok()) {
            Session s(std::move(h), name_);
            status = s.query(sql, params, out);
        }
        afterAttempt(status);
        return status;
    }

    common::Status DataSource::execute(const std::string &sql, const common::StreamParams &params,
                                       std::int64_t &affected,
                                       common::GeneratedKeys &out) const {
        if (const auto g = preGate(sql, common::OperationType::Execute); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Execute, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Execute,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, &affected, [&] {
            return executeUngated(sql, params, affected, out);
        });
    }

    common::Status DataSource::executeUngated(const std::string &sql,
                                              const common::StreamParams &params,
                                              std::int64_t &affected,
                                              common::GeneratedKeys &out) const {
        if (primary_) {
            return dispatchWrite(
                [&sql, &params, &affected, &out](const std::shared_ptr<DataSource> &target) {
                    affected = 0;
                    out.clear();
                    return target->executeUngated(sql, params, affected, out);
                },
                {});
        }
        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
        affected = 0;
        out.clear();
        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, kUsePoolDefault);
        if (status.ok()) {
            Session s(std::move(h), name_);
            status = s.execute(sql, params, affected, out);
        }
        afterAttempt(status);
        if (status.ok()) markWrite();
        return status;
    }

    common::Status DataSource::executeBatch(const std::string &sql,
                                            const common::StreamParamBatch &batch,
                                            common::BatchResult &out) const {
        if (const auto g = preGate(sql, common::OperationType::Batch); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Batch, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Batch,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            return executeBatchUngated(sql, batch, out);
        });
    }

    common::Status DataSource::executeBatchUngated(const std::string &sql,
                                                   const common::StreamParamBatch &batch,
                                                   common::BatchResult &out) const {
        if (primary_) {
            return dispatchWrite(
                [&sql, &batch, &out](const std::shared_ptr<DataSource> &target) {
                    out.clear();
                    return target->executeBatchUngated(sql, batch, out);
                },
                {});
        }
        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, kUsePoolDefault);
        if (status.ok()) {
            Session s(std::move(h), name_);
            status = s.executeBatch(sql, batch, out);
        }
        afterAttempt(status);
        if (status.ok()) markWrite();
        return status;
    }

    common::Status DataSource::queryEach(const std::string &sql,
                                         const common::Params &params,
                                         const common::RowCallback &callback,
                                         std::uint64_t &rows) const {
        if (const auto g = preGate(sql, common::OperationType::Stream); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Stream, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Stream,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            return queryEachUngated(sql, params, callback, rows);
        });
    }

    common::Status DataSource::queryEachUngated(const std::string &sql,
                                                const common::Params &params,
                                                const common::RowCallback &callback,
                                                std::uint64_t &rows) const {
        if (primary_) {
            const auto target = readTarget();
            auto status = target->queryEachUngated(sql, params, callback, rows);
            if (rows == 0 && target != primary_ && fallbackToPrimary_ &&
                (status.retryable || status.connectionBroken ||
                 status.code == common::ErrorCode::CircuitOpen))
                status = primary_->queryEachUngated(sql, params, callback, rows);
            return status;
        }
        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, kUsePoolDefault);
        if (status.ok()) {
            Session session(std::move(h), name_);
            status = session.queryEach(sql, params, callback, rows);
        }
        afterAttempt(status);
        return status;
    }

    common::Status DataSource::executeBatch(const std::string &sql,
                                            const common::ParamBatch &batch,
                                            common::BatchResult &out) const {
        if (const auto g = preGate(sql, common::OperationType::Batch); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Batch, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Batch,
            nullptr, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            return executeBatchUngated(sql, batch, out);
        });
    }

    common::Status DataSource::openCursor(const std::string &sql, const common::Params &params,
                                          const CursorOptions &opts,
                                          std::unique_ptr<Cursor> &out) const {
        if (!cursorEnabled_)
            return common::Status::error(common::ErrorCode::NotSupported,
                                         "cursors are disabled for this datasource");
        if (opts.scrollable && !cursorScrollable_)
            return common::Status::error(common::ErrorCode::NotSupported,
                                         "scrollable cursors are disabled for this datasource");
        CursorOptions effective = opts;
        if (effective.batch_size == 256 && defaultBatchSize_ != 256)
            effective.batch_size = defaultBatchSize_;
        if (const auto g = preGate(sql, common::OperationType::Select); !g.ok()) return g;
        common::SqlContext ctx = common::ContextScope::current();
        detail::runOnRoute(name_, sql, common::OperationType::Select, ctx);
        ExecutionView view{
            name_, sql, common::OperationType::Select,
            &params, nullptr, 0, std::chrono::microseconds{0},
            common::Status::OK(), false, 0, ctx
        };
        return runWithInterceptors(view, nullptr, nullptr, [&] {
            return openCursorUngated(sql, params, effective, out);
        });
    }

    common::Status DataSource::openCursorUngated(const std::string &sql, const common::Params &params,
                                                 const CursorOptions &opts,
                                                 std::unique_ptr<Cursor> &out) const {
        if (primary_) {
            const auto target = readTarget();
            auto status = target->openCursorUngated(sql, params, opts, out);
            if (target != primary_ && fallbackToPrimary_ &&
                (status.retryable || status.connectionBroken ||
                 status.code == common::ErrorCode::CircuitOpen)) {
                out.reset();
                status = primary_->openCursorUngated(sql, params, opts, out);
            }
            return status;
        }
        std::shared_ptr<void> cursorLease;
        if (!cursorBudgetAcquire(cursorLease)) {
            return common::Status::error(common::ErrorCode::CursorLimit,
                                         "datasource '" + name_ + "': cursor limit reached");
        }
        common::Status status;
        const int attempts = std::max(1, retry_.max_attempts);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
            std::unique_ptr<ConnectionPool::Handle> h;
            status = borrowSession(h, kUsePoolDefault);
            if (status.ok()) {
                std::unique_ptr<ICursor> impl;
                status = (*h)->openCursor(sql, params, opts, impl);
                if (status.ok() && impl) {
                    auto rowContext = common::ContextScope::current();
                    Cursor::RowTransform transform = [dataSource = name_, sql, params, rowContext]
                    (common::Row &row) mutable {
                        ExecutionView rowView{
                            dataSource, sql, common::OperationType::Select,
                            &params, nullptr, 0,
                            std::chrono::microseconds{0},
                            common::Status::OK(), false, 0, rowContext
                        };
                        detail::runOnRow(rowView, row);
                    };
                    out = std::make_unique<Cursor>(std::move(h), std::move(impl),
                                                   Session::AuditContext{true, readOnly_},
                                                   Cursor::Binding::OwnsHandle,
                                                   std::move(cursorLease), std::move(transform));
                    return status;
                }
            }
            afterAttempt(status);
            if (status.ok()) return status;
            if (!status.retryable || attempt == attempts) return status;
            std::this_thread::sleep_for(retryDelay(attempt));
        }
        return status;
    }

    bool DataSource::cursorBudgetAcquire(std::shared_ptr<void> &lease) const {
        lease.reset();
        const auto state = cursorBudget_;
        const int limit = state->limit.load();
        if (limit <= 0) return true;
        int open = state->open.load();
        while (open < limit) {
            if (state->open.compare_exchange_weak(open, open + 1)) {
                lease = std::shared_ptr<void>(state.get(), [state](void *) {
                    state->open.fetch_sub(1);
                });
                return true;
            }
        }
        return false;
    }

    common::Status DataSource::executeBatchUngated(const std::string &sql,
                                                   const common::ParamBatch &batch,
                                                   common::BatchResult &out) const {
        if (primary_) {
            std::function<common::Status()> buffered;
            if (writeBuffer_ && writeBuffer_->enabled()) {
                buffered = [primary = primary_, bufferedSql = sql, bufferedBatch = batch] {
                    common::BatchResult ignored;
                    return primary->executeBatchUngated(bufferedSql, bufferedBatch, ignored);
                };
            }
            return dispatchWrite(
                [&sql, &batch, &out](const std::shared_ptr<DataSource> &target) {
                    out.clear();
                    return target->executeBatchUngated(sql, batch, out);
                },
                buffered);
        }
        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;
        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, kUsePoolDefault);
        if (status.ok()) {
            Session session(std::move(h), name_);
            status = session.executeBatch(sql, batch, out);
        }
        afterAttempt(status);
        if (status.ok()) markWrite();
        return status;
    }

    bool DataSource::poolStats(ConnectionPool::Stats &out) const {
        out = {};
        if (!primary_) {
            const auto pool = pool_.lock();
            if (!pool) return false;
            out = pool->stats();
            return true;
        }
        std::unordered_set<const DataSource *> seen;
        bool any = false;
        auto add = [&](const std::shared_ptr<DataSource> &source) {
            if (!source || !seen.insert(source.get()).second) return;
            ConnectionPool::Stats part;
            if (!source->poolStats(part)) return;
            any = true;
            out.minConnections += part.minConnections;
            out.maxConnections += part.maxConnections;
            out.idle += part.idle;
            out.total += part.total;
            out.borrowed += part.borrowed;
            out.waiting += part.waiting;
            out.connectionsCreated += part.connectionsCreated;
            out.connectionsClosed += part.connectionsClosed;
            out.borrowTimeouts += part.borrowTimeouts;
            out.validationFailures += part.validationFailures;
            out.leakWarnings += part.leakWarnings;
            out.maxBorrowed += part.maxBorrowed;
            out.maxWaiting += part.maxWaiting;
            out.borrowRequests += part.borrowRequests;
            out.borrowSuccesses += part.borrowSuccesses;
            out.connectionCreateFailures += part.connectionCreateFailures;
            out.invalidatedConnections += part.invalidatedConnections;
            out.idleEvictions += part.idleEvictions;
            out.lifetimeEvictions += part.lifetimeEvictions;
            out.totalBorrowWait += part.totalBorrowWait;
            out.maxBorrowWait = std::max(out.maxBorrowWait, part.maxBorrowWait);
        };
        add(primary_);
        for (const auto &replica: replicas_) add(replica);
        return any;
    }

    common::Status DataSource::withSession(const SessionFn &fn) const {
        return withSession(fn, kUsePoolDefault);
    }

    common::Status DataSource::withSession(const SessionFn &fn,
                                           const std::chrono::milliseconds borrowTimeout) const {
        if (const auto g = gateSession(); !g.ok()) return g;
        if (primary_) {
            bool wrote = false;
            const auto status = primary_->withSessionInternal(fn, borrowTimeout, &wrote, readOnly_);
            if (wrote) markWrite();
            return status;
        }
        return withSessionInternal(fn, borrowTimeout, nullptr, readOnly_);
    }

    common::Status DataSource::withSessionInternal(const SessionFn &fn,
                                                   const std::chrono::milliseconds borrowTimeout,
                                                   bool *wroteOut,
                                                   const bool enforceReadOnly) const {
        if (wroteOut) *wroteOut = false;
        if (primary_)
            return primary_->withSessionInternal(fn, borrowTimeout, wroteOut,
                                                 enforceReadOnly || readOnly_);

        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;

        std::unique_ptr<ConnectionPool::Handle> h;
        auto status = borrowSession(h, borrowTimeout);
        if (!status.ok()) {
            afterAttempt(status);
            return status;
        }
        Session s(std::move(h), name_, Session::AuditContext{true, enforceReadOnly});
        status = runGuarded(s, fn);
        afterAttempt(status);
        if (s.didWrite()) markWrite();
        if (wroteOut) *wroteOut = s.didWrite();
        return status;
    }

    common::Status DataSource::transaction(const SessionFn &fn) const {
        if (const auto g = gateSession(); !g.ok()) return g;
        return transactionInternal(common::TransactionOptions{}, fn,
                                   kUsePoolDefault, readOnly_);
    }

    common::Status DataSource::transaction(const SessionFn &fn,
                                           const std::chrono::milliseconds borrowTimeout) const {
        if (const auto g = gateSession(); !g.ok()) return g;
        return transactionInternal(common::TransactionOptions{}, fn, borrowTimeout, readOnly_);
    }

    common::Status DataSource::transaction(const common::TransactionOptions &options,
                                           const SessionFn &fn) const {
        if (const auto g = gateSession(); !g.ok()) return g;
        return transactionInternal(options, fn, kUsePoolDefault, readOnly_);
    }

    common::Status DataSource::transaction(const common::TransactionOptions &options,
                                           const SessionFn &fn,
                                           const std::chrono::milliseconds borrowTimeout) const {
        if (const auto g = gateSession(); !g.ok()) return g;
        return transactionInternal(options, fn, borrowTimeout, readOnly_);
    }

    common::Status DataSource::transactionInternal(const common::TransactionOptions &options,
                                                   const SessionFn &fn,
                                                   const std::chrono::milliseconds borrowTimeout,
                                                   const bool enforceReadOnly) const {
        if (primary_) {
            const auto status = primary_->transactionInternal(
                options, fn, borrowTimeout, enforceReadOnly || readOnly_);
            if (status.ok() && !options.readOnly) markWrite();
            return status;
        }

        if (const auto gate = beforeAttempt(); !gate.ok()) return gate;

        std::unique_ptr<ConnectionPool::Handle> h;
        if (const auto st = borrowSession(h, borrowTimeout); !st.ok()) {
            afterAttempt(st);
            return st;
        }

        Session s(std::move(h), name_,
                  Session::AuditContext{true, enforceReadOnly || options.readOnly});
        if (const auto st = s.begin(options); !st.ok()) {
            afterAttempt(st);
            return st;
        }

        std::mutex deadlineMutex;
        std::condition_variable deadlineCv;
        bool finished = false;
        std::atomic<bool> timedOut{false};
        std::atomic<bool> cancelDelivered{false};
        std::thread watcher;
        const bool hasDeadline = options.timeout > std::chrono::milliseconds(0);
        const auto deadline = hasDeadline
                                  ? std::chrono::steady_clock::now() + options.timeout
                                  : std::chrono::steady_clock::time_point::max();
        if (hasDeadline) {
            watcher = std::thread([&] {
                std::unique_lock<std::mutex> lock(deadlineMutex);
                if (!deadlineCv.wait_until(lock, deadline, [&] { return finished; })) {
                    timedOut.store(true);
                    lock.unlock();
                    try {
                        cancelDelivered.store(s.cancel().ok());
                    } catch (...) {
                        cancelDelivered.store(false);
                    }
                }
            });
        }

        auto operationStatus = runGuarded(s, fn);
        if (hasDeadline && std::chrono::steady_clock::now() >= deadline)
            timedOut.store(true);
        {
            std::lock_guard<std::mutex> lock(deadlineMutex);
            finished = true;
        }
        deadlineCv.notify_one();
        if (watcher.joinable()) watcher.join();

        if (timedOut.load()) {
            operationStatus = common::Status::error(
                common::ErrorCode::QueryTimeout,
                "transaction timed out after " + std::to_string(options.timeout.count()) + "ms"
                + (cancelDelivered.load()
                       ? ""
                       : " (driver could not cancel the running statement; "
                       "the callback had to run to completion)"));
            operationStatus.retryable = true;
        }

        afterAttempt(operationStatus);

        if (!operationStatus.ok()) {
            if (const auto rb = s.rollback(); !rb.ok()) {
                DBMW_LOG_WARN("datasource [" + name_ + "] rollback failed: " + rb.message);
                if (s.didWrite()) markWrite();
            }
            return operationStatus;
        }

        if (s.inTransaction()) {
            if (const auto cm = s.commit(); !cm.ok()) {
                afterAttempt(cm);
                if (s.didWrite()) markWrite();
                return cm;
            }
        }
        if (s.didWrite()) markWrite();
        return common::Status::OK();
    }

    struct PoolCollectorLease {
        std::mutex mutex;
        DatabaseManager *owner = nullptr;
    };

    DatabaseManager::DatabaseManager()
        : poolCollectorLease_(std::make_shared<PoolCollectorLease>()) {
        poolCollectorLease_->owner = this;
    }

    DatabaseManager::~DatabaseManager() {
        shutdown(std::chrono::milliseconds(0));
    }

    common::Status DatabaseManager::init(const config::GlobalConfig &cfg,
                                         const std::chrono::milliseconds replacementGrace) {
        driver::registerBuiltinDrivers();

        if (cfg.datasources.empty()) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "no datasource configured");
        }
        std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > newPools;
        std::unordered_map<std::string, std::shared_ptr<DataSource> > newSources;
        std::vector<std::shared_ptr<WriteBuffer> > newWriteBuffers;
        auto newHeartbeat = std::make_unique<HeartbeatManager>(
            std::chrono::milliseconds(cfg.heartbeat_interval_ms));
        const std::chrono::milliseconds borrowTimeout(cfg.pool.borrow_timeout_ms);
        const std::chrono::milliseconds idleTimeout(cfg.pool.idle_timeout_ms);
        const std::chrono::milliseconds maxLifetime(cfg.pool.max_lifetime_ms);
        const std::chrono::milliseconds leakThreshold(cfg.pool.leak_detection_threshold_ms);

        std::unordered_set<std::string> replicaNames;
        for (const auto &group: cfg.groups)
            for (const auto &replica: group.replicas)
                replicaNames.insert(replica.name);

        for (const auto &dsc: cfg.datasources) {
            if (newPools.find(dsc.name) != newPools.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "duplicate datasource name: " + dsc.name);
            }
            std::shared_ptr<ConnectionPool> pool;
            std::shared_ptr<DataSource> source;
            if (const auto st = buildSingleDataSource(
                dsc, cfg.pool, cfg.retry, cfg.circuit_breaker, cfg.cursor,
                makeRateLimiter(cfg.rate_limit), replicaNames,
                false, pool, source); !st.ok()) {
                return st;
            }
            newPools[dsc.name] = std::move(pool);
            newSources[dsc.name] = std::move(source);
            newHeartbeat->addPool(newPools[dsc.name]);
        }

        for (const auto &group: cfg.groups) {
            if (newSources.find(group.name) != newSources.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "duplicate datasource/group name: " + group.name);
            }
            if (!group.failover.primaries.empty() &&
                !group.failover.acknowledge_external_fencing) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + group.name
                    + "' configures automatic write failover without acknowledging "
                    "external fencing");
            }
            if (group.failover.write_buffer.enabled &&
                !group.failover.write_buffer.acknowledge_data_loss_and_duplicates) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + group.name
                    + "' enables volatile write buffering without acknowledging data-loss "
                    "and duplicate-replay risk");
            }
            if (const auto st = validateGroupRefs(group, newPools, replicaNames); !st.ok())
                return st;

            std::shared_ptr<DataSource> source;
            if (const auto st = buildSingleDataSourceGroup(
                group, cfg.pool, {}, newSources, replicaNames,
                newWriteBuffers, source); !st.ok())
                return st;
            newSources[group.name] = std::move(source);
        }

        if (newSources.find(cfg.default_datasource) == newSources.end()) {
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "default_datasource '" + cfg.default_datasource
                + "' is not defined in datasources[] or groups[]");
        }

        SqlAuditor::configure(cfg.sql_audit);
        configurePreparedCache(cfg.prepared_cache);
        QueryCache::configure(cfg.query_cache);

        std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > oldPools;
        std::unordered_map<std::string, std::shared_ptr<DataSource> > oldSources;
        std::vector<std::shared_ptr<WriteBuffer> > oldWriteBuffers;
        std::unique_ptr<HeartbeatManager> oldHeartbeat;
        newHeartbeat->start();
        for (const auto &buffer: newWriteBuffers) buffer->start();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            oldHeartbeat = std::move(heartbeat_);
            oldPools = std::move(pools_);
            oldSources = std::move(datasources_);
            oldWriteBuffers = std::move(writeBuffers_);

            pools_ = std::move(newPools);
            datasources_ = std::move(newSources);
            writeBuffers_ = std::move(newWriteBuffers);
            heartbeat_ = std::move(newHeartbeat);
            defaultName_ = cfg.default_datasource;
        }

        if (const auto rs = resolveShadows(); !rs.ok()) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto stalePools = std::move(pools_);
            auto staleSources = std::move(datasources_);
            auto staleBuffers = std::move(writeBuffers_);
            auto staleHeartbeat = std::move(heartbeat_);
            pools_ = std::move(oldPools);
            datasources_ = std::move(oldSources);
            writeBuffers_ = std::move(oldWriteBuffers);
            heartbeat_ = std::move(oldHeartbeat);
            stalePools.clear();
            staleSources.clear();
            staleBuffers.clear();
            staleHeartbeat.reset();
            (void) stalePools;
            (void) staleSources;
            (void) staleBuffers;
            (void) staleHeartbeat;
            return rs;
        }

        common::Observability::configure(cfg.observability);
        {
            std::lock_guard<std::mutex> lock(poolCollectorLease_->mutex);
            poolCollectorLease_->owner = this;
        }
        common::Observability::setPoolMetricsCollector(
            [lease = poolCollectorLease_] {
                std::lock_guard<std::mutex> lock(lease->mutex);
                return lease->owner
                           ? lease->owner->allPoolStats()
                           : std::vector<common::NamedPoolStats>{};
            },
            this);

        if (!statsReporter_) statsReporter_ = std::make_unique<StatsReporter>();
        statsReporter_->start(cfg.observability.stats_report,
                              [this] { return allPoolStats(); });

        if (oldHeartbeat) oldHeartbeat->stop();
        for (const auto &buffer: oldWriteBuffers) if (buffer) buffer->stop();
        oldWriteBuffers.clear();
        oldSources.clear();
        const auto drainDeadline = std::chrono::steady_clock::now() + replacementGrace;
        for (auto &kv: oldPools) {
            const auto now = std::chrono::steady_clock::now();
            kv.second->shutdown(now < drainDeadline
                                    ? std::chrono::duration_cast<std::chrono::milliseconds>(drainDeadline - now)
                                    : std::chrono::milliseconds(0));
        }
        oldPools.clear();

        return common::Status::OK();
    }

    common::Status DatabaseManager::validateGroupRefs(
        const config::DataSourceGroupConfig &cfg,
        const std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > &candidates,
        const std::unordered_set<std::string> &replicaNames) const {
        if (candidates.find(cfg.primary) == candidates.end()) {
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "group '" + cfg.name + "' references unknown primary '"
                + cfg.primary + "' (must be a plain datasource, not a group)");
        }
        for (const auto &replica: cfg.replicas) {
            if (candidates.find(replica.name) == candidates.end()) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + cfg.name + "' references unknown replica '"
                    + replica.name + "'");
            }
        }
        for (const auto &candidate: cfg.failover.primaries) {
            if (candidates.find(candidate) == candidates.end()) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + cfg.name
                    + "' failover.primaries references unknown datasource '"
                    + candidate + "' (must be a plain datasource, not a group)");
            }
            if (replicaNames.find(candidate) != replicaNames.end()) {
                DBMW_LOG_WARN("group [" + cfg.name + "] failover candidate '"
                    + candidate
                    + "' is also configured as a read replica; make sure it is"
                    " writable when promoted");
            }
        }
        return common::Status::OK();
    }

    common::Status DatabaseManager::checkLeafNotInUse_Unused(const std::string &leafName) const {
        (void) leafName;
        return common::Status::OK();
    }

    common::Status DatabaseManager::buildSingleDataSource(
        const config::DataSourceConfig &dsc,
        const config::PoolConfig &poolCfg,
        const config::RetryConfig &retry,
        const config::CircuitBreakerConfig &circuit,
        const config::CursorConfig &cursor,
        std::shared_ptr<IRateLimiter> rateLimiter,
        const std::unordered_set<std::string> &replicaNames,
        bool,
        std::shared_ptr<ConnectionPool> &outPool,
        std::shared_ptr<DataSource> &outSource) {
        if (dsc.name.empty()) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "datasource name must not be empty");
        }
        auto drv = driver::createDriver(dsc.type);
        if (!drv) {
            return common::Status::error(common::ErrorCode::UnknownDriver,
                                         "unknown datasource type: '" + dsc.type
                                         + "' (name=" + dsc.name + ")");
        }
        const std::chrono::milliseconds borrowTimeout(poolCfg.borrow_timeout_ms);
        const std::chrono::milliseconds idleTimeout(poolCfg.idle_timeout_ms);
        const std::chrono::milliseconds maxLifetime(poolCfg.max_lifetime_ms);
        const std::chrono::milliseconds leakThreshold(poolCfg.leak_detection_threshold_ms);
        outPool = std::make_shared<ConnectionPool>(
            std::move(drv), dsc, poolCfg.min, poolCfg.max, borrowTimeout,
            idleTimeout, maxLifetime, leakThreshold,
            std::chrono::milliseconds(poolCfg.validation_interval_ms),
            true,
            poolCfg.enabled);
        outSource = std::make_shared<DataSource>(
            outPool, dsc.name, retry, circuit, std::move(rateLimiter),
            false,
            replicaNames.find(dsc.name) != replicaNames.end());
        outSource->applyCursorConfig(cursor);
        outSource->driverType_ = dsc.type;
        DBMW_LOG_INFO("datasource registered: " + dsc.describe()
            + (poolCfg.enabled ? "" : " (pooling disabled)"));
        return common::Status::OK();
    }

    common::Status DatabaseManager::buildSingleDataSourceGroup(
        const config::DataSourceGroupConfig &group,
        const config::PoolConfig &,
        const GroupOptions &opts,
        const std::unordered_map<std::string, std::shared_ptr<DataSource> > &sources,
        const std::unordered_set<std::string> &,
        std::vector<std::shared_ptr<WriteBuffer> > &outBuffers,
        std::shared_ptr<DataSource> &outSource) {
        if (group.name.empty()) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "group name must not be empty");
        }
        if (group.read_only && group.failover.write_buffer.enabled) {
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "group '" + group.name
                + "' is read_only but enables failover.write_buffer;"
                " a read-only group never writes");
        }
        const auto primaryIt = sources.find(group.primary);
        if (primaryIt == sources.end()) {
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "group '" + group.name + "' references unknown primary '"
                + group.primary + "'");
        }
        std::vector<std::shared_ptr<DataSource> > weightedReplicas;
        weightedReplicas.reserve(group.replicas.size());
        for (const auto &replica: group.replicas) {
            const auto replicaIt = sources.find(replica.name);
            if (replicaIt == sources.end()) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + group.name + "' references unknown replica '"
                    + replica.name + "'");
            }
            for (int i = 0; i < replica.weight; ++i)
                weightedReplicas.push_back(replicaIt->second);
        }
        std::vector<std::shared_ptr<DataSource> > failoverPrimaries;
        if (!group.failover.primaries.empty()) {
            failoverPrimaries.push_back(primaryIt->second);
            std::unordered_set<std::string> seenCandidates{group.primary};
            for (const auto &candidateName: group.failover.primaries) {
                if (!seenCandidates.insert(candidateName).second) continue;
                const auto candidateIt = sources.find(candidateName);
                if (candidateIt == sources.end()) {
                    return common::Status::error(
                        common::ErrorCode::ConfigError,
                        "group '" + group.name
                        + "' failover.primaries references unknown datasource '"
                        + candidateName + "' (must be a plain datasource, not a group)");
                }
                failoverPrimaries.push_back(candidateIt->second);
            }
        }
        std::shared_ptr<WriteBuffer> writeBuffer;
        if (group.failover.write_buffer.enabled) {
            DBMW_LOG_WARN("group [" + group.name
                + "] volatile write buffer enabled: Buffered means accepted, not "
                "committed; process failure may lose writes and replay may duplicate them");
            WriteBuffer::Config wbc;
            wbc.enabled = true;
            wbc.max_queue = group.failover.write_buffer.max_queue;
            wbc.ttl_ms = group.failover.write_buffer.ttl_ms;
            wbc.flush_interval_ms = group.failover.write_buffer.flush_interval_ms;
            writeBuffer = std::make_shared<WriteBuffer>(wbc);
            outBuffers.push_back(writeBuffer);
        }
        outSource = std::make_shared<DataSource>(
            group.name, primaryIt->second, std::move(weightedReplicas),
            std::chrono::milliseconds(group.read_after_write_ms),
            group.fallback_to_primary,
            opts.rate_limiter,
            group.read_only,
            std::move(failoverPrimaries),
            group.failover.require_healthy,
            writeBuffer);
        outSource->applyCursorConfig(opts.cursor);
        outSource->shadowName_ = group.shadow;
        outSource->driverType_ = primaryIt->second->driverType_;
        DBMW_LOG_INFO("datasource group registered: " + group.name
            + " primary=" + group.primary
            + (group.read_only ? " (read-only)" : "")
            + (group.failover.primaries.empty()
                ? ""
                : " failover=" + std::to_string(
                    group.failover.primaries.size()) + " candidate(s)")
            + (writeBuffer ? " write-buffer=on" : "")
            + (group.shadow.empty() ? "" : " shadow=" + group.shadow));
        return common::Status::OK();
    }

    common::Status DatabaseManager::resolveShadows() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::unordered_set<std::string> groupNames;
        for (const auto &kv: datasources_) {
            if (kv.second && kv.second->primary_) groupNames.insert(kv.first);
        }
        for (const auto &kv: datasources_) {
            const auto &ds = kv.second;
            if (!ds || ds->shadowName_.empty()) continue;
            const auto &name = ds->shadowName_;
            const auto it = datasources_.find(name);
            if (it == datasources_.end() || !it->second) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + ds->name_ + "' references unknown shadow '"
                    + name + "'");
            }
            if (groupNames.find(name) != groupNames.end()) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + ds->name_ + "' shadow '" + name
                    + "' is a group; shadow target must be a plain datasource");
            }
            if (it->second == ds->primary_) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + ds->name_ + "' shadow '" + name
                    + "' is the group's primary; self-shadowing is rejected");
            }
            for (const auto &replica: ds->replicas_) {
                if (replica && replica == it->second) {
                    return common::Status::error(
                        common::ErrorCode::ConfigError,
                        "group '" + ds->name_ + "' shadow '" + name
                        + "' is a replica of the group; self-shadowing is rejected");
                }
            }
            ds->shadow_ = it->second;
        }
        return common::Status::OK();
    }

    void DatabaseManager::setDefaultRateLimiter(std::shared_ptr<IRateLimiter> limiter) noexcept {
        defaultRateLimiter_ = std::move(limiter);
    }

    common::Status DatabaseManager::addDataSource(const config::DataSourceConfig &cfg,
                                                  const DataSourceOptions &opts) {
        if (cfg.name.empty()) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "datasource name must not be empty");
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pools_.find(cfg.name) != pools_.end() ||
                datasources_.find(cfg.name) != datasources_.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "datasource name already exists: " + cfg.name);
            }
        }
        config::PoolConfig runtimePool;
        runtimePool.min = 1;
        runtimePool.max = 32;
        runtimePool.borrow_timeout_ms = 30000;
        runtimePool.idle_timeout_ms = 600000;
        runtimePool.max_lifetime_ms = 1800000;
        runtimePool.leak_detection_threshold_ms = 30000;
        runtimePool.validation_interval_ms = 500;
        runtimePool.enabled = true;

        const std::unordered_set<std::string> emptyReplicaNames;
        config::RateLimitConfig defaultRate;
        std::shared_ptr<ConnectionPool> pool;
        std::shared_ptr<DataSource> source;
        std::shared_ptr<IRateLimiter> limiter = opts.rate_limiter;
        if (!limiter) limiter = makeRateLimiter(defaultRate);
        if (const auto st = buildSingleDataSource(
            cfg, runtimePool, opts.retry, opts.circuit_breaker, opts.cursor,
            std::move(limiter),
            emptyReplicaNames, opts.attach_heartbeat, pool, source); !st.ok())
            return st;
        source->readOnly_ = opts.read_only;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pools_.find(cfg.name) != pools_.end() ||
                datasources_.find(cfg.name) != datasources_.end()) {
                pool->shutdown(std::chrono::milliseconds(0));
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "datasource name already exists: " + cfg.name);
            }
            pools_[cfg.name] = pool;
            datasources_[cfg.name] = source;
            if (opts.attach_heartbeat && heartbeat_) heartbeat_->addPool(pool);
        }
        return common::Status::OK();
    }

    common::Status DatabaseManager::removeDataSource(const std::string &name,
                                                     const std::chrono::milliseconds grace) {
        std::shared_ptr<ConnectionPool> poolToShutdown;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pools_.find(name) == pools_.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "datasource not found: " + name);
            }
            if (name == defaultName_) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "datasource '" + name + "' is the current default and cannot be removed");
            }
            for (const auto &kv: datasources_) {
                const auto &candidate = kv.second;
                if (!candidate || candidate->name() == name) continue;
                if (candidate->primary_) {
                    if (candidate->primary_->name() == name) {
                        return common::Status::error(
                            common::ErrorCode::ConfigError,
                            "datasource '" + name + "' is still referenced by group '"
                            + candidate->name() + "' as primary (remove the group first)");
                    }
                    for (const auto &replica: candidate->replicas_) {
                        if (replica && replica->name() == name) {
                            return common::Status::error(
                                common::ErrorCode::ConfigError,
                                "datasource '" + name + "' is still referenced by group '"
                                + candidate->name() + "' as replica (remove the group first)");
                        }
                    }
                    for (const auto &fp: candidate->failoverPrimaries_) {
                        if (fp && fp->name() == name) {
                            return common::Status::error(
                                common::ErrorCode::ConfigError,
                                "datasource '" + name + "' is still referenced by group '"
                                + candidate->name()
                                + "' as failover candidate (remove the group first)");
                        }
                    }
                    if (candidate->shadow_ && candidate->shadow_->name() == name) {
                        return common::Status::error(
                            common::ErrorCode::ConfigError,
                            "datasource '" + name + "' is still referenced by group '"
                            + candidate->name()
                            + "' as shadow (remove the group first)");
                    }
                }
            }
            poolToShutdown = std::move(pools_.at(name));
            pools_.erase(name);
            datasources_.erase(name);
        }
        if (poolToShutdown) {
            poolToShutdown->shutdown(grace);
        }
        DBMW_LOG_INFO("datasource removed: " + name);
        return common::Status::OK();
    }

    common::Status DatabaseManager::addGroup(const config::DataSourceGroupConfig &cfg,
                                             const GroupOptions &opts) {
        if (cfg.name.empty()) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "group name must not be empty");
        }
        std::vector<std::shared_ptr<WriteBuffer> > stagedBuffers;
        std::shared_ptr<DataSource> source;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pools_.find(cfg.name) != pools_.end() ||
                datasources_.find(cfg.name) != datasources_.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "group name already exists: " + cfg.name);
            }
            if (!cfg.failover.primaries.empty() && !opts.acknowledge_external_fencing) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + cfg.name
                    + "' configures automatic write failover without acknowledging "
                    "external fencing");
            }
            if (cfg.failover.write_buffer.enabled && !opts.acknowledge_data_loss_and_duplicates) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + cfg.name
                    + "' enables volatile write buffering without acknowledging data-loss "
                    "and duplicate-replay risk");
            }
            if (const auto st = validateGroupRefs(cfg, pools_, {}); !st.ok())
                return st;
            if (const auto st = buildSingleDataSourceGroup(
                cfg, {}, opts, datasources_, {},
                stagedBuffers, source); !st.ok())
                return st;
            for (const auto &replica: cfg.replicas) {
                const auto it = datasources_.find(replica.name);
                if (it != datasources_.end() && it->second)
                    it->second->readReplica_.store(true, std::memory_order_release);
            }
            datasources_[cfg.name] = source;
            for (auto &buffer: stagedBuffers) writeBuffers_.push_back(buffer);
        }
        if (!stagedBuffers.empty()) {
            try {
                for (auto &buffer: stagedBuffers) {
                    if (buffer) buffer->start();
                }
            } catch (...) {
                std::lock_guard<std::mutex> lk(mtx_);
                datasources_.erase(cfg.name);
                for (auto &buffer: stagedBuffers) {
                    if (buffer) buffer->stop();
                    writeBuffers_.erase(std::remove(writeBuffers_.begin(),
                                                    writeBuffers_.end(), buffer),
                                        writeBuffers_.end());
                }
                return common::Status::error(common::ErrorCode::Unknown,
                                             "write buffer start failed");
            }
        }
        if (const auto rs = resolveShadows(); !rs.ok()) {
            std::lock_guard<std::mutex> lk(mtx_);
            datasources_.erase(cfg.name);
            for (auto &buffer: stagedBuffers) {
                if (buffer) buffer->stop();
                writeBuffers_.erase(std::remove(writeBuffers_.begin(),
                                                writeBuffers_.end(), buffer),
                                    writeBuffers_.end());
            }
            return rs;
        }
        return common::Status::OK();
    }

    common::Status DatabaseManager::removeGroup(const std::string &name,
                                                const std::chrono::milliseconds grace) {
        (void) grace;
        std::shared_ptr<WriteBuffer> bufferToStop;
        bool wasGroup = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            const auto it = datasources_.find(name);
            if (it == datasources_.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "group not found: " + name);
            }
            if (pools_.find(name) != pools_.end()) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "datasource '" + name
                                             + "' is not a group (use removeDataSource)");
            }
            if (name == defaultName_) {
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "group '" + name + "' is the current default and cannot be removed");
            }
            bufferToStop = it->second->writeBuffer_;
            wasGroup = true;
            datasources_.erase(it);
            for (auto &entry: datasources_) {
                if (entry.second && !entry.second->primary_)
                    entry.second->readReplica_.store(false, std::memory_order_release);
            }
            for (const auto &entry: datasources_) {
                const auto &group = entry.second;
                if (!group || !group->primary_) continue;
                for (const auto &replica: group->replicas_) {
                    if (replica)
                        replica->readReplica_.store(true, std::memory_order_release);
                }
            }
            if (bufferToStop) {
                writeBuffers_.erase(std::remove(writeBuffers_.begin(),
                                                writeBuffers_.end(), bufferToStop),
                                    writeBuffers_.end());
            }
        }
        if (wasGroup && bufferToStop) bufferToStop->stop();
        DBMW_LOG_INFO("datasource group removed: " + name);
        return common::Status::OK();
    }

    std::shared_ptr<DataSource> DatabaseManager::getDataSource(const std::string &name) {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = datasources_.find(name);
        if (it == datasources_.end()) return nullptr;
        return it->second;
    }

    std::shared_ptr<DataSource> DatabaseManager::getDefault() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (defaultName_.empty()) return nullptr;
        const auto it = datasources_.find(defaultName_);
        if (it == datasources_.end()) return nullptr;
        return it->second;
    }

    void DatabaseManager::shutdown(const std::chrono::milliseconds grace) {
        if (statsReporter_) statsReporter_->stop();
        common::Observability::clearPoolMetricsCollector(this);
        if (poolCollectorLease_) {
            std::lock_guard<std::mutex> lock(poolCollectorLease_->mutex);
            poolCollectorLease_->owner = nullptr;
        }

        std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > oldPools;
        std::unordered_map<std::string, std::shared_ptr<DataSource> > oldSources;
        std::vector<std::shared_ptr<WriteBuffer> > oldWriteBuffers;
        std::unique_ptr<HeartbeatManager> oldHeartbeat;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            oldHeartbeat = std::move(heartbeat_);
            oldPools = std::move(pools_);
            oldSources = std::move(datasources_);
            oldWriteBuffers = std::move(writeBuffers_);
            defaultName_.clear();
        }
        if (oldHeartbeat) oldHeartbeat->stop();
        for (const auto &buffer: oldWriteBuffers) if (buffer) buffer->stop();
        oldWriteBuffers.clear();
        oldSources.clear();
        const auto drainDeadline = std::chrono::steady_clock::now() + grace;
        for (auto &kv: oldPools) {
            const auto now = std::chrono::steady_clock::now();
            kv.second->shutdown(now < drainDeadline
                                    ? std::chrono::duration_cast<std::chrono::milliseconds>(drainDeadline - now)
                                    : std::chrono::milliseconds(0));
        }
        oldPools.clear();
    }

    size_t DatabaseManager::dataSourceCount() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return datasources_.size();
    }

    std::vector<NamedPoolStats> DatabaseManager::allPoolStats() const {
        std::vector<NamedPoolStats> result;
        std::lock_guard<std::mutex> lk(mtx_);
        result.reserve(pools_.size());
        for (const auto &[fst, snd]: pools_)
            result.push_back(NamedPoolStats{fst, snd->stats()});
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            return a.dataSource < b.dataSource;
        });
        return result;
    }
}
