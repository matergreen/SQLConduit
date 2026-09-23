#include "sqlconduit/client.h"

#include "sqlconduit/async/executor.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/config/config_loader.h"
#include "sqlconduit/core/runtime_services.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

namespace sqlconduit {
    namespace {
        common::Status notInitialized() {
            return common::Status::error(common::ErrorCode::NotInitialized,
                                         "client is not initialized");
        }

        common::Status clientClosed() {
            return common::Status::error(common::ErrorCode::ClientClosed,
                                         "client is closed");
        }

        common::Status dataSourceNotFound(const std::string &name) {
            return common::Status::error(
                common::ErrorCode::ConfigError,
                name.empty() ? "no default datasource" : "datasource not found: " + name);
        }

        common::Status validateAsyncConfig(const config::AsyncConfig &config) {
            if (config.threads < 0 || config.queue_size < 1 ||
                config.statement_timeout_ms < 0) {
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "invalid async configuration");
            }
            return common::Status::OK();
        }
    }

    class Client::Impl {
    public:
        enum class State { Empty, Running, Closed };

        common::Status requireRunning() const {
            const auto current = state.load(std::memory_order_acquire);
            if (current == State::Running) return common::Status::OK();
            return current == State::Closed ? clientClosed() : notInitialized();
        }

        common::Status resolve(const std::string &requested,
                               std::shared_ptr<core::DataSource> &out) const {
            if (const auto st = requireRunning(); !st.ok()) return st;
            const auto &contextual = common::ContextScope::current().targetDataSource;
            const std::string &name = contextual.empty() ? requested : contextual;
            out = name.empty() ? manager.getDefault() : manager.getDataSource(name);
            return out ? common::Status::OK() : dataSourceNotFound(name);
        }

        Impl() : services(std::make_shared<core::detail::RuntimeServices>()), manager(services) {
        }

        void configureAsync(const config::AsyncConfig &config) {
            asyncTimeoutMs.store(config.statement_timeout_ms, std::memory_order_release);
            std::shared_ptr<async::IExecutor> retired;
            {
                std::lock_guard<std::mutex> lock(asyncMutex);
                if (!config.enabled) {
                    retired = std::move(asyncExecutor);
                } else if (!asyncExecutor) {
                    asyncExecutor = async::makeThreadPoolExecutor(
                        config.threads, static_cast<std::size_t>(config.queue_size));
                }
            }
            if (retired) retired->shutdown(std::chrono::milliseconds(0));
        }

        void stopAsync(const std::chrono::milliseconds grace) noexcept {
            std::shared_ptr<async::IExecutor> executor;
            {
                std::lock_guard<std::mutex> lock(asyncMutex);
                executor = std::move(asyncExecutor);
            }
            if (executor) {
                try { executor->shutdown(grace); } catch (...) {
                }
            }
        }

        template<typename Result, typename Fn>
        std::future<Result> submitAsync(const std::string &requested, Fn fn) const {
            auto promise = std::make_shared<std::promise<Result> >();
            auto future = promise->get_future();

            std::shared_ptr<core::DataSource> source;
            if (auto status = resolve(requested, source); !status.ok()) {
                Result result;
                result.status = std::move(status);
                promise->set_value(std::move(result));
                return future;
            }

            std::shared_ptr<async::IExecutor> executor;
            {
                std::lock_guard<std::mutex> lock(asyncMutex);
                executor = asyncExecutor;
            }
            if (!executor) {
                Result result;
                result.status = common::Status::error(
                    common::ErrorCode::ConfigError,
                    "async execution is disabled for this client");
                promise->set_value(std::move(result));
                return future;
            }

            const auto& context = common::ContextScope::current();
            const auto timeoutMs = asyncTimeoutMs.load(std::memory_order_acquire);
            auto task = [promise, source = std::move(source), context,
                        timeoutMs, fn = std::move(fn)]() mutable {
                Result result;
                const auto started = std::chrono::steady_clock::now();
                try {
                    const common::ContextScope scope(context);
                    fn(*source, result);
                } catch (const std::exception &e) {
                    result.status = common::Status::error(
                        common::ErrorCode::Unknown,
                        std::string("client async operation threw: ") + e.what());
                } catch (...) {
                    result.status = common::Status::error(
                        common::ErrorCode::Unknown,
                        "client async operation threw an unknown exception");
                }
                if (timeoutMs > 0 &&
                    std::chrono::steady_clock::now() - started >=
                    std::chrono::milliseconds(timeoutMs)) {
                    result.status = common::Status::error(
                        common::ErrorCode::QueryTimeout,
                        "operation exceeded client async statement timeout; "
                        "driver cancellation is not available in the future API");
                    result.status.retryable = true;
                }
                promise->set_value(std::move(result));
            };

            if (!executor->tryPost(std::move(task))) {
                Result result;
                result.status = common::Status::error(
                    common::ErrorCode::Overloaded,
                    "client async executor queue is full or stopped");
                result.status.retryable = true;
                promise->set_value(std::move(result));
            }
            return future;
        }

        std::shared_ptr<core::detail::RuntimeServices> services;
        mutable core::DatabaseManager manager;
        mutable std::mutex lifecycleMutex;
        mutable std::mutex asyncMutex;
        std::shared_ptr<async::IExecutor> asyncExecutor;
        std::atomic<std::int64_t> asyncTimeoutMs{0};
        std::atomic<State> state{State::Empty};
    };

    Client::Client() : impl_(std::make_unique<Impl>()) {
    }

    Client::~Client() {
        shutdown(std::chrono::milliseconds(0));
    }

    Client::Client(Client &&other) noexcept = default;

    Client &Client::operator=(Client &&other) noexcept = default;

    common::Status Client::addDriver(driver::DriverRegistration registration) {
        if (!impl_) return clientClosed();
        if (registration.type.empty() || !registration.factory)
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "driver registration requires a type and factory");
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        const auto current = impl_->state.load(std::memory_order_acquire);
        if (current == Impl::State::Closed) return clientClosed();
        if (current == Impl::State::Running)
            return common::Status::error(common::ErrorCode::AlreadyInitialized,
                                         "drivers must be registered before Client::init()");
        impl_->manager.addDriver(std::move(registration));
        return common::Status::OK();
    }

    common::Status Client::init(const std::string &configPath) {
        config::GlobalConfig config;
        std::string error;
        if (!config::ConfigLoader::loadFromFile(configPath, config, error))
            return common::Status::error(common::ErrorCode::ConfigError, std::move(error));
        return init(config);
    }

    common::Status Client::init(const config::GlobalConfig &config) {
        if (!impl_) return clientClosed();
        if (const auto st = validateAsyncConfig(config.async); !st.ok()) return st;
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        const auto current = impl_->state.load(std::memory_order_acquire);
        if (current == Impl::State::Closed) return clientClosed();
        if (current == Impl::State::Running)
            return common::Status::error(common::ErrorCode::AlreadyInitialized,
                                         "client is already initialized; use reload()");
        const auto status = impl_->manager.init(config);
        if (status.ok()) {
            impl_->configureAsync(config.async);
            impl_->state.store(Impl::State::Running, std::memory_order_release);
        }
        return status;
    }

    common::Status Client::reload(const std::string &configPath,
                                  const std::chrono::milliseconds grace) {
        config::GlobalConfig config;
        std::string error;
        if (!config::ConfigLoader::loadFromFile(configPath, config, error))
            return common::Status::error(common::ErrorCode::ConfigError, std::move(error));
        return reload(config, grace);
    }

    common::Status Client::reload(const config::GlobalConfig &config,
                                  const std::chrono::milliseconds grace) {
        if (!impl_) return clientClosed();
        if (const auto st = validateAsyncConfig(config.async); !st.ok()) return st;
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        if (const auto st = impl_->requireRunning(); !st.ok()) return st;
        const auto status = impl_->manager.init(config, grace);
        if (status.ok()) impl_->configureAsync(config.async);
        return status;
    }

    bool Client::isRunning() const noexcept {
        return impl_ && impl_->state.load(std::memory_order_acquire) == Impl::State::Running;
    }

    common::Status Client::query(const std::string &sql, common::ResultSet &out) const {
        return query({}, sql, {}, out);
    }

    common::Status Client::query(const std::string &sql, const common::Params &params,
                                 common::ResultSet &out) const {
        return query({}, sql, params, out);
    }

    common::Status Client::query(const std::string &source, const std::string &sql,
                                 common::ResultSet &out) const {
        return query(source, sql, {}, out);
    }

    common::Status Client::query(const std::string &source, const std::string &sql,
                                 const common::Params &params,
                                 common::ResultSet &out) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return params.empty()
                   ? dataSource->query(sql, out)
                   : dataSource->query(sql, params, out);
    }

    common::Status Client::execute(const std::string &sql, std::int64_t &affected) const {
        return execute({}, sql, {}, affected);
    }

    common::Status Client::execute(const std::string &sql, const common::Params &params,
                                   std::int64_t &affected) const {
        return execute({}, sql, params, affected);
    }

    common::Status Client::execute(const std::string &source, const std::string &sql,
                                   std::int64_t &affected) const {
        return execute(source, sql, {}, affected);
    }

    common::Status Client::execute(const std::string &source, const std::string &sql,
                                   const common::Params &params,
                                   std::int64_t &affected) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return params.empty()
                   ? dataSource->execute(sql, affected)
                   : dataSource->execute(sql, params, affected);
    }

    common::Status Client::queryAll(const std::string &sql,
                                    std::vector<common::ResultSet> &out) const {
        return queryAll({}, sql, {}, out);
    }

    common::Status Client::queryAll(const std::string &sql, const common::Params &params,
                                    std::vector<common::ResultSet> &out) const {
        return queryAll({}, sql, params, out);
    }

    common::Status Client::queryAll(const std::string &source, const std::string &sql,
                                    std::vector<common::ResultSet> &out) const {
        return queryAll(source, sql, {}, out);
    }

    common::Status Client::queryAll(const std::string &source, const std::string &sql,
                                    const common::Params &params,
                                    std::vector<common::ResultSet> &out) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return params.empty()
                   ? dataSource->queryAll(sql, out)
                   : dataSource->queryAll(sql, params, out);
    }

    common::Status Client::call(const std::string &sql, const common::CallParams &params,
                                common::CallOutput &out) const {
        return call({}, sql, params, out);
    }

    common::Status Client::call(const std::string &source, const std::string &sql,
                                const common::CallParams &params,
                                common::CallOutput &out) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->call(sql, params, out);
    }

    common::Status Client::queryEach(const std::string &sql, const common::Params &params,
                                     const common::RowCallback &callback,
                                     std::uint64_t &rows) const {
        return queryEach({}, sql, params, callback, rows);
    }

    common::Status Client::queryEach(const std::string &source, const std::string &sql,
                                     const common::Params &params,
                                     const common::RowCallback &callback,
                                     std::uint64_t &rows) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->queryEach(sql, params, callback, rows);
    }

    common::Status Client::executeBatch(const std::string &sql,
                                        const common::ParamBatch &batch,
                                        common::BatchResult &out) const {
        return executeBatch({}, sql, batch, out);
    }

    common::Status Client::executeBatch(const std::string &source, const std::string &sql,
                                        const common::ParamBatch &batch,
                                        common::BatchResult &out) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->executeBatch(sql, batch, out);
    }

    common::Status Client::openCursor(const std::string &sql,
                                      const common::Params &params,
                                      const core::CursorOptions &options,
                                      std::unique_ptr<core::Cursor> &out) const {
        return openCursor({}, sql, params, options, out);
    }

    common::Status Client::openCursor(const std::string &source, const std::string &sql,
                                      const common::Params &params,
                                      const core::CursorOptions &options,
                                      std::unique_ptr<core::Cursor> &out) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->openCursor(sql, params, options, out);
    }

    common::Status Client::transaction(const core::SessionFn &fn) const {
        return transaction({}, common::TransactionOptions{}, fn);
    }

    common::Status Client::transaction(const std::string &source,
                                       const core::SessionFn &fn) const {
        return transaction(source, common::TransactionOptions{}, fn);
    }

    common::Status Client::transaction(const common::TransactionOptions &options,
                                       const core::SessionFn &fn) const {
        return transaction({}, options, fn);
    }

    common::Status Client::transaction(const std::string &source,
                                       const common::TransactionOptions &options,
                                       const core::SessionFn &fn) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->transaction(options, fn);
    }

    common::Status Client::withSession(const core::SessionFn &fn) const {
        return withSession({}, fn);
    }

    common::Status Client::withSession(const std::string &source,
                                       const core::SessionFn &fn) const {
        if (!impl_) return clientClosed();
        std::shared_ptr<core::DataSource> dataSource;
        if (const auto st = impl_->resolve(source, dataSource); !st.ok()) return st;
        return dataSource->withSession(fn);
    }

    std::future<async::QueryResult> Client::queryAsync(const std::string &sql) const {
        return queryAsync({}, sql, {});
    }

    std::future<async::QueryResult> Client::queryAsync(
        const std::string &sql, const common::Params &params) const {
        return queryAsync({}, sql, params);
    }

    std::future<async::QueryResult> Client::queryAsync(
        const std::string &source, const std::string &sql,
        const common::Params &params) const {
        if (!impl_) {
            std::promise<async::QueryResult> promise;
            auto future = promise.get_future();
            async::QueryResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::QueryResult>(source,
                                                      [sql, params](core::DataSource &dataSource,
                                                                    async::QueryResult &result) {
                                                          result.status = params.empty()
                                                                              ? dataSource.query(sql, result.rows)
                                                                              : dataSource.query(
                                                                                  sql, params, result.rows);
                                                      });
    }

    std::future<async::MultiQueryResult> Client::queryAllAsync(
        const std::string &sql, const common::Params &params) const {
        return queryAllAsync({}, sql, params);
    }

    std::future<async::MultiQueryResult> Client::queryAllAsync(
        const std::string &source, const std::string &sql,
        const common::Params &params) const {
        if (!impl_) {
            std::promise<async::MultiQueryResult> promise;
            auto future = promise.get_future();
            async::MultiQueryResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::MultiQueryResult>(source,
                                                           [sql, params](
                                                       core::DataSource &dataSource, async::MultiQueryResult &result) {
                                                               result.status = dataSource.queryAll(
                                                                   sql, params, result.sets);
                                                           });
    }

    std::future<async::ExecResult> Client::executeAsync(const std::string &sql) const {
        return executeAsync({}, sql, {});
    }

    std::future<async::ExecResult> Client::executeAsync(
        const std::string &sql, const common::Params &params) const {
        return executeAsync({}, sql, params);
    }

    std::future<async::ExecResult> Client::executeAsync(
        const std::string &source, const std::string &sql,
        const common::Params &params) const {
        if (!impl_) {
            std::promise<async::ExecResult> promise;
            auto future = promise.get_future();
            async::ExecResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::ExecResult>(source,
                                                     [sql, params](core::DataSource &dataSource,
                                                                   async::ExecResult &result) {
                                                         result.status = params.empty()
                                                                             ? dataSource.execute(sql, result.affected)
                                                                             : dataSource.execute(
                                                                                 sql, params, result.affected);
                                                     });
    }

    std::future<async::ExecKeysResult> Client::executeKeysAsync(
        const std::string &sql, const common::Params &params) const {
        return executeKeysAsync({}, sql, params);
    }

    std::future<async::ExecKeysResult> Client::executeKeysAsync(
        const std::string &source, const std::string &sql,
        const common::Params &params) const {
        if (!impl_) {
            std::promise<async::ExecKeysResult> promise;
            auto future = promise.get_future();
            async::ExecKeysResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::ExecKeysResult>(source,
                                                         [sql, params](core::DataSource &dataSource,
                                                                       async::ExecKeysResult &result) {
                                                             result.status = params.empty()
                                                                                 ? dataSource.execute(
                                                                                     sql, result.affected, result.keys)
                                                                                 : dataSource.execute(
                                                                                     sql, params, result.affected,
                                                                                     result.keys);
                                                         });
    }

    std::future<async::EachResult> Client::queryEachAsync(
        const std::string &sql, const common::Params &params,
        common::RowCallback rowCallback) const {
        return queryEachAsync({}, sql, params, std::move(rowCallback));
    }

    std::future<async::EachResult> Client::queryEachAsync(
        const std::string &source, const std::string &sql,
        const common::Params &params, common::RowCallback rowCallback) const {
        if (!impl_) {
            std::promise<async::EachResult> promise;
            auto future = promise.get_future();
            async::EachResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::EachResult>(source,
                                                     [sql, params, rowCallback = std::move(rowCallback)](
                                                 core::DataSource &dataSource, async::EachResult &result) {
                                                         result.status = dataSource.queryEach(
                                                             sql, params, rowCallback, result.rows);
                                                     });
    }

    std::future<async::BatchResult> Client::executeBatchAsync(
        const std::string &sql, const common::ParamBatch &batch) const {
        return executeBatchAsync({}, sql, batch);
    }

    std::future<async::BatchResult> Client::executeBatchAsync(
        const std::string &source, const std::string &sql,
        const common::ParamBatch &batch) const {
        if (!impl_) {
            std::promise<async::BatchResult> promise;
            auto future = promise.get_future();
            async::BatchResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::BatchResult>(source,
                                                      [sql, batch](core::DataSource &dataSource,
                                                                   async::BatchResult &result) {
                                                          result.status = dataSource.executeBatch(
                                                              sql, batch, result.batch);
                                                      });
    }

    std::future<async::OpResult> Client::transactionAsync(core::SessionFn fn) const {
        return transactionAsync({}, common::TransactionOptions{}, std::move(fn));
    }

    std::future<async::OpResult> Client::transactionAsync(
        const std::string &source, const common::TransactionOptions &options,
        core::SessionFn fn) const {
        if (!impl_) {
            std::promise<async::OpResult> promise;
            auto future = promise.get_future();
            async::OpResult result;
            result.status = clientClosed();
            promise.set_value(std::move(result));
            return future;
        }
        return impl_->submitAsync<async::OpResult>(source,
                                                   [options, fn = std::move(fn)](
                                               core::DataSource &dataSource, async::OpResult &result) {
                                                       result.status = dataSource.transaction(options, fn);
                                                   });
    }

    async::ExecutorStats Client::asyncStats() const {
        if (!impl_) return {};
        std::shared_ptr<async::IExecutor> executor;
        {
            std::lock_guard<std::mutex> lock(impl_->asyncMutex);
            executor = impl_->asyncExecutor;
        }
        return executor ? executor->stats() : async::ExecutorStats{};
    }

    std::shared_ptr<core::DataSource> Client::dataSource(const std::string &name) const {
        if (!impl_) return {};
        std::shared_ptr<core::DataSource> out;
        (void) impl_->resolve(name, out);
        return out;
    }

    bool Client::poolStats(core::ConnectionPool::Stats &out,
                           const std::string &name) const {
        const auto source = dataSource(name);
        return source && source->poolStats(out);
    }

    std::vector<core::NamedPoolStats> Client::allPoolStats() const {
        if (!impl_ || !isRunning()) return {};
        return impl_->manager.allPoolStats();
    }

    std::vector<common::SlowSqlStats> Client::slowSqlStats(
        const std::size_t limit, const std::string &dataSource) const {
        if (!impl_) return {};
        return impl_->services->observability.slowSqlStats(limit, dataSource);
    }

    std::vector<common::SlowSqlRecord> Client::recentSlowSql(
        const std::size_t limit, const std::string &dataSource) const {
        if (!impl_) return {};
        return impl_->services->observability.recentSlowSql(limit, dataSource);
    }

    void Client::clearSlowSqlStats() {
        if (impl_) impl_->services->observability.clearSlowSqlStats();
    }

    void Client::setObserver(common::OperationObserver observer) {
        if (impl_) impl_->services->observability.setObserver(std::move(observer));
    }

    void Client::setDefaultRateLimiter(std::shared_ptr<core::IRateLimiter> limiter) {
        if (impl_) impl_->manager.setDefaultRateLimiter(std::move(limiter));
    }

    void Client::addInterceptor(std::shared_ptr<core::ISqlInterceptor> interceptor) {
        if (impl_) impl_->services->interceptors.add(std::move(interceptor));
    }

    void Client::clearInterceptors() {
        if (impl_) impl_->services->interceptors.clear();
    }

    common::Status Client::addDataSource(const config::DataSourceConfig &config,
                                         const core::DataSourceOptions &options) {
        if (!impl_) return clientClosed();
        if (const auto st = impl_->requireRunning(); !st.ok()) return st;
        return impl_->manager.addDataSource(config, options);
    }

    common::Status Client::removeDataSource(const std::string &name,
                                            const std::chrono::milliseconds grace) {
        if (!impl_) return clientClosed();
        if (const auto st = impl_->requireRunning(); !st.ok()) return st;
        return impl_->manager.removeDataSource(name, grace);
    }

    common::Status Client::addGroup(const config::DataSourceGroupConfig &config,
                                    const core::GroupOptions &options) {
        if (!impl_) return clientClosed();
        if (const auto st = impl_->requireRunning(); !st.ok()) return st;
        return impl_->manager.addGroup(config, options);
    }

    common::Status Client::removeGroup(const std::string &name,
                                       const std::chrono::milliseconds grace) {
        if (!impl_) return clientClosed();
        if (const auto st = impl_->requireRunning(); !st.ok()) return st;
        return impl_->manager.removeGroup(name, grace);
    }

    void Client::shutdown(const std::chrono::milliseconds grace) noexcept {
        if (!impl_) return;
        std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
        const auto previous = impl_->state.exchange(Impl::State::Closed,
                                                    std::memory_order_acq_rel);
        if (previous == Impl::State::Running) {
            impl_->stopAsync(grace);
            impl_->manager.shutdown(grace);
        }
    }
}
