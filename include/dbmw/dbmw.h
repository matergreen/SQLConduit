#ifndef DBMW_DBMW_H
#define DBMW_DBMW_H

#include "dbmw/common/types.h"
#include "dbmw/common/observer.h"
#include "dbmw/core/database_manager.h"
#include "dbmw/core/interceptor.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace dbmw
{
    class DBMW
    {
    public:
        DBMW() = delete;

        static common::Status init(const std::string& configPath);

        static common::Status reload(
            const std::string &configPath,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        static common::Status query(const std::string& sql, common::ResultSet& out);

        static common::Status execute(const std::string& sql, std::int64_t& affected);

        static common::Status query(const std::string& dataSource, const std::string& sql, common::ResultSet& out);

        static common::Status execute(const std::string& dataSource, const std::string& sql, std::int64_t& affected);

        static common::Status query(const std::string& sql, const common::Params& params, common::ResultSet& out);

        static common::Status query(const std::string& dataSource, const std::string& sql, const common::Params& params, common::ResultSet& out);

        static common::Status execute(const std::string& sql, const common::Params& params, std::int64_t& affected);

        static common::Status execute(const std::string& dataSource, const std::string& sql, const common::Params& params, std::int64_t& affected);

        static common::Status queryAll(const std::string& sql,
                                       std::vector<common::ResultSet>& out);
        static common::Status queryAll(const std::string& sql, const common::Params& params,
                                       std::vector<common::ResultSet>& out);
        static common::Status queryAll(const std::string& dataSource, const std::string& sql,
                                       std::vector<common::ResultSet>& out);
        static common::Status queryAll(const std::string& dataSource, const std::string& sql,
                                       const common::Params& params,
                                       std::vector<common::ResultSet>& out);

        static common::Status queryEach(const std::string &sql,
                                        const common::Params &params,
                                        const common::RowCallback &callback,
                                        std::uint64_t &rows);

        static common::Status queryEach(const std::string &dataSource,
                                        const std::string &sql,
                                        const common::Params &params,
                                        const common::RowCallback &callback,
                                        std::uint64_t &rows);

        static common::Status executeBatch(const std::string &sql,
                                           const common::ParamBatch &batch,
                                           common::BatchResult &out);

        static common::Status executeBatch(const std::string &dataSource,
                                           const std::string &sql,
                                           const common::ParamBatch &batch,
                                           common::BatchResult &out);

        static common::Status openCursor(const std::string &sql, const common::Params &params,
                                        const core::CursorOptions &opts,
                                        std::unique_ptr<core::Cursor> &out);

        static common::Status openCursor(const std::string &dataSource, const std::string &sql,
                                        const common::Params &params,
                                        const core::CursorOptions &opts,
                                        std::unique_ptr<core::Cursor> &out);

        static common::Status transaction(const core::SessionFn& fn);

        static common::Status transaction(const std::string& dataSource, const core::SessionFn& fn);

        static common::Status transaction(const common::TransactionOptions &options,
                                          const core::SessionFn &fn);

        static common::Status transaction(const std::string &dataSource,
                                          const common::TransactionOptions &options,
                                          const core::SessionFn &fn);

        static common::Status withSession(const std::string& dataSource, const core::SessionFn& fn);

        static common::Status withSession(const core::SessionFn& fn);

        static std::shared_ptr<core::DataSource> dataSource(const std::string& name = "");

        static bool poolStats(core::ConnectionPool::Stats &out,
                              const std::string &name = "");

        static std::vector<core::NamedPoolStats> allPoolStats();

        static std::vector<common::SlowSqlStats> slowSqlStats(
            std::size_t limit = 100, const std::string &dataSource = "");
        static std::vector<common::SlowSqlRecord> recentSlowSql(
            std::size_t limit = 100, const std::string &dataSource = "");
        static void clearSlowSqlStats();

        static void shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        static void setObserver(common::OperationObserver observer);

        static void addInterceptor(std::shared_ptr<core::ISqlInterceptor> interceptor);

        static void clearInterceptors();

        static void setDefaultRateLimiter(std::shared_ptr<core::IRateLimiter> limiter);

        static common::Status addDataSource(
            const config::DataSourceConfig &cfg,
            const core::DataSourceOptions &opts = core::DataSourceOptions{});

        static common::Status removeDataSource(
            const std::string &name,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        static common::Status addGroup(
            const config::DataSourceGroupConfig &cfg,
            const core::GroupOptions &opts = core::GroupOptions{});

        static common::Status removeGroup(
            const std::string &name,
            std::chrono::milliseconds grace = std::chrono::milliseconds(5000));
    };
}

#endif
