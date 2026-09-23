#ifndef SQLCONDUIT_CLIENT_H
#define SQLCONDUIT_CLIENT_H

#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/async/async_types.h"
#include "sqlconduit/config/datasource_config.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/core/interceptor.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace sqlconduit {
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

        Client &operator=(const Client &) = delete;

        [[nodiscard]] common::Status init(const std::string &configPath);

        [[nodiscard]] common::Status init(const config::GlobalConfig &config);

        [[nodiscard]] common::Status reload(
            const std::string &configPath,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        [[nodiscard]] common::Status reload(
            const config::GlobalConfig &config,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        [[nodiscard]] bool isRunning() const noexcept;

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

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
}

#endif
