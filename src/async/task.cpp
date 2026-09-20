#include "dbmw/async/task.h"

namespace dbmw::async {
    Task<QueryResult> queryAsync(std::string sql, common::Params params, Options opts) {
        co_return co_await detail::OpAwaiter<QueryResult>{
            [sql = std::move(sql), params = std::move(params), opts](
        detail::OpAwaiter<QueryResult>::Callback cb) {
                query(sql, params, QueryCallback(std::move(cb)), opts);
            }
        };
    }

    Task<QueryResult> queryAsync(std::string dataSource, std::string sql,
                                 common::Params params, Options opts) {
        co_return co_await detail::OpAwaiter<QueryResult>{
            [dataSource = std::move(dataSource), sql = std::move(sql),
                params = std::move(params), opts](
        detail::OpAwaiter<QueryResult>::Callback cb) {
                query(dataSource, sql, params, QueryCallback(std::move(cb)), opts);
            }
        };
    }

    Task<ExecResult> executeAsync(std::string sql, common::Params params, Options opts) {
        co_return co_await detail::OpAwaiter<ExecResult>{
            [sql = std::move(sql), params = std::move(params), opts](
        detail::OpAwaiter<ExecResult>::Callback cb) {
                execute(sql, params, ExecCallback(std::move(cb)), opts);
            }
        };
    }

    Task<ExecKeysResult> executeAsync(std::string dataSource, std::string sql,
                                      common::Params params, Options opts) {
        co_return co_await detail::OpAwaiter<ExecKeysResult>{
            [dataSource = std::move(dataSource), sql = std::move(sql),
                params = std::move(params), opts](
        detail::OpAwaiter<ExecKeysResult>::Callback cb) {
                execute(dataSource, sql, params, ExecKeysCallback(std::move(cb)), opts);
            }
        };
    }

    Task<ExecKeysResult> executeKeysAsync(std::string sql, common::Params params,
                                          Options opts) {
        co_return co_await detail::OpAwaiter<ExecKeysResult>{
            [sql = std::move(sql), params = std::move(params), opts](
        detail::OpAwaiter<ExecKeysResult>::Callback cb) {
                execute(sql, params, ExecKeysCallback(std::move(cb)), opts);
            }
        };
    }

    Task<BatchResult> executeBatchAsync(std::string sql, common::ParamBatch batch,
                                        Options opts) {
        co_return co_await detail::OpAwaiter<BatchResult>{
            [sql = std::move(sql), batch = std::move(batch), opts](
        detail::OpAwaiter<BatchResult>::Callback cb) {
                executeBatch(sql, batch, BatchCallback(std::move(cb)), opts);
            }
        };
    }

    Task<OpResult> transactionAsync(common::TransactionOptions txOpts,
                                    core::SessionFn fn) {
        co_return co_await detail::OpAwaiter<OpResult>{
            [txOpts, fn = std::move(fn)](detail::OpAwaiter<OpResult>::Callback cb) {
                transaction(txOpts, fn, OpCallback(std::move(cb)));
            }
        };
    }
}
