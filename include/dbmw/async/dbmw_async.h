#ifndef DBMW_ASYNC_DBMW_ASYNC_H
#define DBMW_ASYNC_DBMW_ASYNC_H

#include "dbmw/async/async_types.h"
#include "dbmw/async/executor.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/core/database_manager.h"

#include <future>
#include <string>

namespace dbmw::async {
    Handle query(const std::string &sql, QueryCallback cb, Options opts = {});

    Handle query(const std::string &dataSource, const std::string &sql,
                 QueryCallback cb, Options opts = {});

    Handle query(const std::string &sql, const common::Params &params,
                 QueryCallback cb, Options opts = {});

    Handle query(const std::string &dataSource, const std::string &sql,
                 const common::Params &params, QueryCallback cb, Options opts = {});

    Handle queryAll(const std::string &sql, MultiQueryCallback cb, Options opts = {});

    Handle queryAll(const std::string &dataSource, const std::string &sql,
                    MultiQueryCallback cb, Options opts = {});

    Handle queryAll(const std::string &sql, const common::Params &params,
                    MultiQueryCallback cb, Options opts = {});

    Handle queryAll(const std::string &dataSource, const std::string &sql,
                    const common::Params &params, MultiQueryCallback cb, Options opts = {});

    Handle execute(const std::string &sql, ExecCallback cb, Options opts = {});

    Handle execute(const std::string &dataSource, const std::string &sql,
                   ExecCallback cb, Options opts = {});

    Handle execute(const std::string &sql, const common::Params &params,
                   ExecCallback cb, Options opts = {});

    Handle execute(const std::string &dataSource, const std::string &sql,
                   const common::Params &params, ExecCallback cb, Options opts = {});

    Handle execute(const std::string &sql, const common::Params &params,
                   ExecKeysCallback cb, Options opts = {});

    Handle execute(const std::string &dataSource, const std::string &sql,
                   const common::Params &params, ExecKeysCallback cb, Options opts = {});

    Handle queryEach(const std::string &sql, const common::Params &params,
                     const common::RowCallback &rowCb, EachCallback done, Options opts = {});

    Handle queryEach(const std::string &dataSource, const std::string &sql,
                     const common::Params &params,
                     const common::RowCallback &rowCb, EachCallback done, Options opts = {});

    Handle executeBatch(const std::string &sql, const common::ParamBatch &batch,
                        BatchCallback cb, Options opts = {});

    Handle executeBatch(const std::string &dataSource, const std::string &sql,
                        const common::ParamBatch &batch, BatchCallback cb, Options opts = {});

    Handle transaction(const core::SessionFn &fn, OpCallback cb, Options opts = {});

    Handle transaction(const std::string &dataSource, const core::SessionFn &fn,
                       OpCallback cb, Options opts = {});

    Handle transaction(const common::TransactionOptions &txOpts, const core::SessionFn &fn,
                       OpCallback cb, Options opts = {});

    Handle transaction(const std::string &dataSource, const common::TransactionOptions &txOpts,
                       const core::SessionFn &fn, OpCallback cb, Options opts = {});

    Handle withSession(const core::SessionFn &fn, OpCallback cb, Options opts = {});

    Handle withSession(const std::string &dataSource, const core::SessionFn &fn,
                       OpCallback cb, Options opts = {});

    std::future<QueryResult> query(const std::string &sql);

    std::future<QueryResult> query(const std::string &sql, const common::Params &params);

    std::future<QueryResult> query(const std::string &dataSource, const std::string &sql,
                                   const common::Params &params);

    std::future<ExecResult> execute(const std::string &sql);

    std::future<ExecResult> execute(const std::string &sql, const common::Params &params);

    std::future<ExecResult> execute(const std::string &dataSource, const std::string &sql,
                                    const common::Params &params);

    std::future<ExecKeysResult> executeKeys(const std::string &sql,
                                            const common::Params &params);

    std::future<ExecKeysResult> executeKeys(const std::string &dataSource,
                                            const std::string &sql,
                                            const common::Params &params);

    std::future<EachResult> queryEach(const std::string &sql, const common::Params &params,
                                      const common::RowCallback &rowCb);

    std::future<BatchResult> executeBatch(const std::string &sql,
                                          const common::ParamBatch &batch);

    std::future<OpResult> transaction(const core::SessionFn &fn);

    std::future<OpResult> transaction(const std::string &dataSource,
                                      const common::TransactionOptions &txOpts,
                                      const core::SessionFn &fn);

    void setExecutor(std::shared_ptr<IExecutor> ex);

    void setCompletionExecutor(std::shared_ptr<IExecutor> ex);

    ExecutorStats stats();

    namespace detail {
        void initEngine(const config::AsyncConfig &cfg);

        void drainAndStop(std::chrono::milliseconds grace);

        std::size_t inFlight();
    }
}

#endif
