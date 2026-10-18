#include "sqlconduit/client.h"
#include "sqlconduit/pagination.h"
#include "sqlconduit/util.h"
#include <mutex>

namespace sqlconduit {
    namespace {
        detail::PageRows emptyPage(const PageOptions &options) {
            detail::PageRows result;
            result.page = options.page; result.pageSize = options.pageSize;
            return result;
        }
        template<class Query>
        detail::PageRows runPage(const sql::PageQuery &query, const PageOptions &options,
                                common::util::Dialect dialect, Query run) {
            auto result = emptyPage(options);
            const auto prepared = detail::preparePage(query, options, dialect);
            result.status = prepared.status;
            if (!result.status.ok()) return result;
            result.mode = async::ExecutionMode::Native;
            result.status = run(prepared.data, result.rows);
            if (result.status.ok() && prepared.count) {
                common::ResultSet count;
                result.status = run(*prepared.count, count);
                std::int64_t total = 0;
                if (result.status.ok()) result.status = detail::readPageTotal(count, total);
                if (result.status.ok()) result.total = total;
            }
            detail::finishPageRows(result);
            return result;
        }
        struct PendingPage {
            std::mutex mutex;
            bool done = false;
            int pending = 1;
            detail::PageRows result;
            std::function<void(detail::PageRows)> completion;
            bool finished() { std::lock_guard<std::mutex> lock(mutex); return done; }
            void accept(async::QueryResult raw, bool count) {
                std::function<void(detail::PageRows)> target;
                detail::PageRows final;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (done) return;
                    if (raw.mode == async::ExecutionMode::CompatibilityFallback) result.mode = raw.mode;
                    result.status = std::move(raw.status);
                    if (result.status.ok()) {
                        if (count) {
                            std::int64_t total = 0;
                            result.status = detail::readPageTotal(raw.rows, total);
                            if (result.status.ok()) result.total = total;
                        } else result.rows = std::move(raw.rows);
                    }
                    --pending;
                    if (result.status.ok() && pending) return;
                    // An early error cannot know the still-pending sibling's mode.
                    // Be conservative rather than claim an entirely native page.
                    if (!result.status.ok() && pending) result.mode = async::ExecutionMode::CompatibilityFallback;
                    done = true;
                    detail::finishPageRows(result);
                    final = std::move(result);
                    target = std::move(completion);
                }
                target(std::move(final));
            }
        };
    }

    detail::PageRows Client::queryPageRaw(const std::string &name, const sql::PageQuery &query,
                                         const PageOptions &options) const {
        if (!query.ok()) { auto page = emptyPage(options); page.status = query.status; return page; }
        std::shared_ptr<core::DataSource> source;
        auto status = resolvePageSource(name, source);
        if (!status.ok()) { auto page = emptyPage(options); page.status = std::move(status); return page; }
        return runPage(query, options, common::util::dialectFromDriverType(source->driverType()),
            [source](const sql::Statement &statement, common::ResultSet &rows) {
                return source->query(statement.sql, statement.params, rows);
            });
    }

    void Client::queryPageRawAsync(const std::string &name, const sql::PageQuery &query, const PageOptions &options,
                                   std::function<void(detail::PageRows)> completion) const {
        auto initial = emptyPage(options);
        if (!query.ok()) { initial.status = query.status; completion(std::move(initial)); return; }
        std::shared_ptr<core::DataSource> source;
        initial.status = resolvePageSource(name, source);
        if (!initial.status.ok()) { completion(std::move(initial)); return; }
        const auto prepared = detail::preparePage(query, options, common::util::dialectFromDriverType(source->driverType()));
        initial.status = prepared.status;
        if (!initial.status.ok()) { completion(std::move(initial)); return; }
        auto state = std::make_shared<PendingPage>();
        state->pending = prepared.count ? 2 : 1;
        state->result = std::move(initial);
        state->result.mode = async::ExecutionMode::Native;
        state->completion = std::move(completion);
        // Both requests use this topology handle. Neither callback captures Client/Impl,
        // waits on a future, borrows synchronously, or requires an extra waiting thread.
        queryAsyncCompleteOnSource(source, prepared.data, [state](async::QueryResult raw) {
            state->accept(std::move(raw), false);
        });
        if (prepared.count && !state->finished())
            queryAsyncCompleteOnSource(source, *prepared.count, [state](async::QueryResult raw) {
                state->accept(std::move(raw), true);
            });
    }

    detail::PageRows core::Session::queryPageRaw(const sql::PageQuery &query, const PageOptions &options) const {
        return runPage(query, options, common::util::dialectFromDriverType(driverType()),
            [this](const sql::Statement &statement, common::ResultSet &rows) {
                return this->query(statement.sql, statement.params, rows);
            });
    }
}
