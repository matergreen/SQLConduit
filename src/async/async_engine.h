#ifndef SQLCONDUIT_ASYNC_ASYNC_ENGINE_H
#define SQLCONDUIT_ASYNC_ASYNC_ENGINE_H

#include "sqlconduit/async/async_types.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/core/connection_pool.h"
#include "sqlconduit/core/database_manager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace sqlconduit::async::detail {
    class AsyncEngine {
    public:
        using QueryCompletion = std::function<void(QueryResult)>;
        using ExecuteCompletion = std::function<void(ExecResult)>;
        using SessionOperation = std::function<common::Status(core::Session &)>;
        using StatusCompletion = std::function<void(common::Status)>;

        // Returns false without starting work when the datasource topology requires the full
        // synchronous compatibility path (groups, retries, cache, or shadow routing).
        static bool query(const std::shared_ptr<core::DataSource> &source,
                          std::string sql, common::Params params,
                          common::SqlContext context,
                          std::chrono::milliseconds borrowTimeout,
                          core::AsyncIo io, QueryCompletion completion);

        static bool execute(const std::shared_ptr<core::DataSource> &source,
                            std::string sql, common::Params params,
                            common::SqlContext context,
                            std::chrono::milliseconds borrowTimeout,
                            core::AsyncIo io, ExecuteCompletion completion);

        static bool runSession(const std::shared_ptr<core::DataSource> &source,
                               std::string sql, common::OperationType type,
                               bool write, common::SqlContext context,
                               std::chrono::milliseconds borrowTimeout,
                               core::AsyncIo io, SessionOperation operation,
                               StatusCompletion completion);

        static bool transaction(const std::shared_ptr<core::DataSource> &source,
                                common::TransactionOptions options,
                                core::SessionFn operation,
                                common::SqlContext context,
                                std::chrono::milliseconds borrowTimeout,
                                core::AsyncIo io, StatusCompletion completion);

    private:
        static bool directLeafEligible(const core::DataSource &source, bool query);
    };
}

#endif
