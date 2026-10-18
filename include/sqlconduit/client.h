#ifndef SQLCONDUIT_CLIENT_H
#define SQLCONDUIT_CLIENT_H

#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/named_params.h"
#include "sqlconduit/common/operation_options.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/pagination.h"
#include "sqlconduit/async/async_types.h"
#include "sqlconduit/config/datasource_config.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/core/interceptor.h"
#include "sqlconduit/driver/driver_registry.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace sqlconduit {
    template<class T> struct EntityResult;
    template<class T> struct EntityOne;
    template<class T> struct ScalarResult;
    // Owns a database manager and its connection topology. Client is move-only so
    // that shutdown ownership is unambiguous; DataSource handles obtained from it
    // may still be copied, but become unusable after the client is shut down.
    class Client final {
    public:
        Client();

        ~Client();

        Client(Client &&other) noexcept;

        Client &operator=(Client &&other) noexcept;

        Client(const Client &) = delete;

        [[nodiscard]] OperationView<const Client> withOptions(common::OperationOptions options) const & {
            return {*this, std::move(options)};
        }
        OperationView<const Client> withOptions(common::OperationOptions) const && = delete;

        Client &operator=(const Client &) = delete;

        // Registers a driver only for this Client. Call before init().
        [[nodiscard]] common::Status addDriver(driver::DriverRegistration registration);

        [[nodiscard]] common::Status init(const std::string &configPath);

        [[nodiscard]] common::Status init(const config::GlobalConfig &config);

        [[nodiscard]] common::Status reload(
            const std::string &configPath,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        [[nodiscard]] common::Status reload(
            const config::GlobalConfig &config,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        [[nodiscard]] bool isRunning() const noexcept;

        // Value-returning overloads; existing output-parameter APIs remain unchanged.
        [[nodiscard]] common::ExecuteResult execute(const std::string &sql, const common::Params &params = {}) const;
        [[nodiscard]] common::ExecuteResult execute(const std::string &source, const std::string &sql, const common::Params &params) const;
        [[nodiscard]] common::QueryResult query(const std::string &sql,
                                                const common::Params &params = {}) const;
        [[nodiscard]] common::QueryResult query(const std::string &source, const std::string &sql,
                                                const common::Params &params) const;
        [[nodiscard]] common::QueryResult queryNamed(const std::string &sql,
                                                     const common::NamedParams &params = {}) const;
        [[nodiscard]] common::QueryResult queryNamed(const std::string &source, const std::string &sql,
                                                     const common::NamedParams &params) const;
        [[nodiscard]] common::ExecuteResult executeNamed(const std::string &sql,
                                                         const common::NamedParams &params = {}) const;
        [[nodiscard]] common::ExecuteResult executeNamed(const std::string &source, const std::string &sql,
                                                         const common::NamedParams &params) const;
        [[nodiscard]] std::future<async::QueryResult> queryNamedAsync(
            const std::string &sql, const common::NamedParams &params = {}) const;
        [[nodiscard]] std::future<async::QueryResult> queryNamedAsync(
            const std::string &source, const std::string &sql, const common::NamedParams &params) const;
        [[nodiscard]] std::future<async::ExecResult> executeNamedAsync(
            const std::string &sql, const common::NamedParams &params = {}) const;
        [[nodiscard]] std::future<async::ExecResult> executeNamedAsync(
            const std::string &source, const std::string &sql, const common::NamedParams &params) const;

        // Include sqlconduit/mapping.h for typed query definitions and RowMapper support.
        template<class T>
        [[nodiscard]] EntityResult<T> query(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] EntityResult<T> query(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryAsync(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryAsync(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] EntityOne<T> queryOne(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] EntityOne<T> queryOne(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] std::future<EntityOne<T>> queryOneAsync(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityOne<T>> queryOneAsync(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] ScalarResult<T> queryScalar(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] ScalarResult<T> queryScalar(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] std::future<ScalarResult<T>> queryScalarAsync(
            const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<ScalarResult<T>> queryScalarAsync(
            const std::string &source, const std::string &sql,
            const common::Params &params) const;

        template<class T>
        [[nodiscard]] EntityResult<T> queryNamed(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] EntityResult<T> queryNamed(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryNamedAsync(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryNamedAsync(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        template<class T>
        [[nodiscard]] EntityOne<T> queryOneNamed(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] EntityOne<T> queryOneNamed(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        template<class T>
        [[nodiscard]] std::future<EntityOne<T>> queryOneNamedAsync(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityOne<T>> queryOneNamedAsync(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        template<class T>
        [[nodiscard]] ScalarResult<T> queryScalarNamed(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] ScalarResult<T> queryScalarNamed(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        template<class T>
        [[nodiscard]] std::future<ScalarResult<T>> queryScalarNamedAsync(
            const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<ScalarResult<T>> queryScalarNamedAsync(
            const std::string &source, const std::string &sql,
            const common::NamedParams &params) const;

        [[nodiscard]] common::Status query(const std::string &sql,
                                           common::ResultSet &out) const;

        [[nodiscard]] common::Status query(const std::string &sql,
                                           const common::Params &params,
                                           common::ResultSet &out) const;

        [[nodiscard]] common::Status query(const std::string &dataSource,
                                           const std::string &sql,
                                           common::ResultSet &out) const;

        [[nodiscard]] common::Status query(const std::string &dataSource,
                                           const std::string &sql,
                                           const common::Params &params,
                                           common::ResultSet &out) const;

        [[nodiscard]] common::Status execute(const std::string &sql,
                                             std::int64_t &affected) const;

        [[nodiscard]] common::Status execute(const std::string &sql,
                                             const common::Params &params,
                                             std::int64_t &affected) const;

        [[nodiscard]] common::Status execute(const std::string &dataSource,
                                             const std::string &sql,
                                             std::int64_t &affected) const;

        [[nodiscard]] common::Status execute(const std::string &dataSource,
                                             const std::string &sql,
                                             const common::Params &params,
                                             std::int64_t &affected) const;

        [[nodiscard]] common::Status queryAll(const std::string &sql,
                                              std::vector<common::ResultSet> &out) const;

        [[nodiscard]] common::Status queryAll(const std::string &sql,
                                              const common::Params &params,
                                              std::vector<common::ResultSet> &out) const;

        [[nodiscard]] common::Status queryAll(const std::string &dataSource,
                                              const std::string &sql,
                                              std::vector<common::ResultSet> &out) const;

        [[nodiscard]] common::Status queryAll(const std::string &dataSource,
                                              const std::string &sql,
                                              const common::Params &params,
                                              std::vector<common::ResultSet> &out) const;

        [[nodiscard]] common::Status call(const std::string &sql,
                                          const common::CallParams &params,
                                          common::CallOutput &out) const;

        [[nodiscard]] common::Status call(const std::string &dataSource,
                                          const std::string &sql,
                                          const common::CallParams &params,
                                          common::CallOutput &out) const;

        [[nodiscard]] common::Status queryEach(const std::string &sql,
                                               const common::Params &params,
                                               const common::RowCallback &callback,
                                               std::uint64_t &rows) const;

        [[nodiscard]] common::Status queryEach(const std::string &dataSource,
                                               const std::string &sql,
                                               const common::Params &params,
                                               const common::RowCallback &callback,
                                               std::uint64_t &rows) const;

        [[nodiscard]] common::Status executeBatch(const std::string &sql,
                                                  const common::ParamBatch &batch,
                                                  common::BatchResult &out) const;

        [[nodiscard]] common::Status executeBatch(const std::string &dataSource,
                                                  const std::string &sql,
                                                  const common::ParamBatch &batch,
                                                  common::BatchResult &out) const;

        [[nodiscard]] common::Status openCursor(const std::string &sql,
                                                const common::Params &params,
                                                const core::CursorOptions &options,
                                                std::unique_ptr<core::Cursor> &out) const;

        [[nodiscard]] common::Status openCursor(const std::string &dataSource,
                                                const std::string &sql,
                                                const common::Params &params,
                                                const core::CursorOptions &options,
                                                std::unique_ptr<core::Cursor> &out) const;

        [[nodiscard]] common::Status transaction(const core::SessionFn &fn) const;

        [[nodiscard]] common::Status transaction(const std::string &dataSource,
                                                 const core::SessionFn &fn) const;

        [[nodiscard]] common::Status transaction(
            const common::TransactionOptions &options,
            const core::SessionFn &fn) const;

        [[nodiscard]] common::Status transaction(
            const std::string &dataSource,
            const common::TransactionOptions &options,
            const core::SessionFn &fn) const;

        [[nodiscard]] common::Status withSession(const core::SessionFn &fn) const;

        [[nodiscard]] common::Status withSession(const std::string &dataSource,
                                                 const core::SessionFn &fn) const;

        [[nodiscard]] std::future<async::QueryResult> queryAsync(
            const std::string &sql) const;

        [[nodiscard]] std::future<async::QueryResult> queryAsync(
            const std::string &sql, const common::Params &params) const;

        [[nodiscard]] std::future<async::QueryResult> queryAsync(
            const std::string &dataSource, const std::string &sql,
            const common::Params &params) const;

        [[nodiscard]] std::future<async::MultiQueryResult> queryAllAsync(
            const std::string &sql, const common::Params &params = {}) const;

        [[nodiscard]] std::future<async::MultiQueryResult> queryAllAsync(
            const std::string &dataSource, const std::string &sql,
            const common::Params &params) const;

        [[nodiscard]] std::future<async::ExecResult> executeAsync(
            const std::string &sql) const;

        [[nodiscard]] std::future<async::ExecResult> executeAsync(
            const std::string &sql, const common::Params &params) const;

        [[nodiscard]] std::future<async::ExecResult> executeAsync(
            const std::string &dataSource, const std::string &sql,
            const common::Params &params) const;

        [[nodiscard]] std::future<async::ExecKeysResult> executeKeysAsync(
            const std::string &sql, const common::Params &params = {}) const;

        [[nodiscard]] std::future<async::ExecKeysResult> executeKeysAsync(
            const std::string &dataSource, const std::string &sql,
            const common::Params &params) const;

        [[nodiscard]] std::future<async::EachResult> queryEachAsync(
            const std::string &sql, const common::Params &params,
            common::RowCallback rowCallback) const;

        [[nodiscard]] std::future<async::EachResult> queryEachAsync(
            const std::string &dataSource, const std::string &sql,
            const common::Params &params, common::RowCallback rowCallback) const;

        [[nodiscard]] std::future<async::BatchResult> executeBatchAsync(
            const std::string &sql, const common::ParamBatch &batch) const;

        [[nodiscard]] std::future<async::BatchResult> executeBatchAsync(
            const std::string &dataSource, const std::string &sql,
            const common::ParamBatch &batch) const;

        [[nodiscard]] std::future<async::OpResult> transactionAsync(
            core::SessionFn fn) const;

        [[nodiscard]] std::future<async::OpResult> transactionAsync(
            const std::string &dataSource,
            const common::TransactionOptions &options,
            core::SessionFn fn) const;

        [[nodiscard]] async::ExecutorStats asyncStats() const;

        // Nonblocking snapshots. No SQL, connection borrowing or ContextScope routing.
        [[nodiscard]] common::CapabilityReport capabilities(const std::string &name = {}) const;

        [[nodiscard]] common::DiagnosticsReport diagnostics() const;

        [[nodiscard]] std::shared_ptr<core::DataSource> dataSource(
            const std::string &name = {}) const;

        [[nodiscard]] bool poolStats(core::ConnectionPool::Stats &out,
                                     const std::string &name = {}) const;

        [[nodiscard]] std::vector<core::NamedPoolStats> allPoolStats() const;

        [[nodiscard]] std::vector<common::SlowSqlStats> slowSqlStats(
            std::size_t limit = 100, const std::string &dataSource = {}) const;

        [[nodiscard]] std::vector<common::SlowSqlRecord> recentSlowSql(
            std::size_t limit = 100, const std::string &dataSource = {}) const;

        void clearSlowSqlStats();

        void setObserver(common::OperationObserver observer);

        void setExecutionPhaseObserver(common::ExecutionPhaseObserver observer);

        void setDefaultRateLimiter(std::shared_ptr<core::IRateLimiter> limiter);

        void addInterceptor(std::shared_ptr<core::ISqlInterceptor> interceptor);

        void clearInterceptors();

        [[nodiscard]] common::Status addDataSource(
            const config::DataSourceConfig &config,
            const core::DataSourceOptions &options = core::DataSourceOptions{});

        [[nodiscard]] common::Status removeDataSource(
            const std::string &name,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        [[nodiscard]] common::Status addGroup(
            const config::DataSourceGroupConfig &config,
            const core::GroupOptions &options = core::GroupOptions{});

        [[nodiscard]] common::Status removeGroup(
            const std::string &name,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        void shutdown(std::chrono::milliseconds grace =
                std::chrono::milliseconds(5000)) noexcept;

        template<class T>
        [[nodiscard]] EntityResult<T> queryColumn(const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] EntityResult<T> queryColumn(const std::string &source, const std::string &sql, const common::Params &params) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryColumnAsync(const std::string &sql, const common::Params &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryColumnAsync(const std::string &source, const std::string &sql, const common::Params &params) const;
        template<class T>
        [[nodiscard]] EntityResult<T> queryColumnNamed(const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] EntityResult<T> queryColumnNamed(const std::string &source, const std::string &sql, const common::NamedParams &params) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryColumnNamedAsync(const std::string &sql, const common::NamedParams &params = {}) const;
        template<class T>
        [[nodiscard]] std::future<EntityResult<T>> queryColumnNamedAsync(const std::string &source, const std::string &sql, const common::NamedParams &params) const;
        template<class T> [[nodiscard]] EntityResult<T> query(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] std::future<EntityResult<T>> queryAsync(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] EntityOne<T> queryOne(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] std::future<EntityOne<T>> queryOneAsync(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] ScalarResult<T> queryScalar(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] std::future<ScalarResult<T>> queryScalarAsync(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] EntityResult<T> queryColumn(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] std::future<EntityResult<T>> queryColumnAsync(const sql::BuildResult &statement) const;
        [[nodiscard]] common::QueryResult query(const sql::BuildResult &statement) const;
        [[nodiscard]] std::future<async::QueryResult> queryAsync(const sql::BuildResult &statement) const;
        [[nodiscard]] common::ExecuteResult execute(const sql::BuildResult &statement) const;
        [[nodiscard]] std::future<async::ExecResult> executeAsync(const sql::BuildResult &statement) const;
        template<class T> [[nodiscard]] EntityResult<T> query(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] std::future<EntityResult<T>> queryAsync(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] EntityOne<T> queryOne(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] std::future<EntityOne<T>> queryOneAsync(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] ScalarResult<T> queryScalar(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] std::future<ScalarResult<T>> queryScalarAsync(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] EntityResult<T> queryColumn(const sql::Statement &statement) const;
        template<class T> [[nodiscard]] std::future<EntityResult<T>> queryColumnAsync(const sql::Statement &statement) const;
        [[nodiscard]] common::QueryResult query(const sql::Statement &statement) const;
        [[nodiscard]] std::future<async::QueryResult> queryAsync(const sql::Statement &statement) const;
        [[nodiscard]] common::ExecuteResult execute(const sql::Statement &statement) const;
        [[nodiscard]] std::future<async::ExecResult> executeAsync(const sql::Statement &statement) const;

        template<class T> [[nodiscard]] PageResult<T> queryPage(const sql::PageQuery &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] PageResult<T> queryPage(const sql::BuildResult &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] PageResult<T> queryPage(const sql::Builder &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] PageResult<T> queryPage(const sql::TypedBuilder<T> &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] PageResult<T> queryPage(const std::string &sql, const common::Params &params = {}, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] PageResult<T> queryPageNamed(const std::string &sql, const common::NamedParams &params = {}, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageAsync(const sql::PageQuery &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageAsync(const sql::BuildResult &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageAsync(const sql::Builder &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageAsync(const sql::TypedBuilder<T> &query, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageAsync(const std::string &sql, const common::Params &params = {}, const PageOptions &options = {}) const;
        template<class T> [[nodiscard]] std::future<PageResult<T>> queryPageNamedAsync(const std::string &sql, const common::NamedParams &params = {}, const PageOptions &options = {}) const;

    private:
        common::Status resolvePageSource(const std::string &, std::shared_ptr<core::DataSource> &) const;
        detail::PageRows queryPageRaw(const std::string &, const sql::PageQuery &, const PageOptions &) const;
        void queryPageRawAsync(const std::string &, const sql::PageQuery &, const PageOptions &,
                               std::function<void(detail::PageRows)>) const;
        void queryAsyncCompleteOnSource(const std::shared_ptr<core::DataSource> &, const sql::Statement &,
                                        std::function<void(async::QueryResult)>) const;
        void queryAsyncComplete(const std::string &source, const std::string &sql,
                                const common::Params &params,
                                std::function<void(async::QueryResult)> completion) const;
        template<class Result, class Convert>
        std::future<Result> queryMappedAsync(const std::string &source, const std::string &sql,
                                             const common::Params &params, Convert convert) const;
        common::Status initResolved(const config::GlobalConfig &config, std::string source);
        common::Status reloadResolved(const config::GlobalConfig &config,
                                      std::chrono::milliseconds grace, std::string source);
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}

#endif
