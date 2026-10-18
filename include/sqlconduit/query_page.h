#ifndef SQLCONDUIT_QUERY_PAGE_H
#define SQLCONDUIT_QUERY_PAGE_H

#include "sqlconduit/pagination.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/typed_sql_builder.h"

namespace sqlconduit::detail {
    inline sql::PageQuery pageQuery(const sql::BuildResult &built) {
        sql::PageQuery query; query.status = built.status; query.statement = built.statement;
        return query;
    }
    inline sql::PageQuery namedPageQuery(const std::string &sql, const common::NamedParams &params) {
        auto bound = common::bindNamed(sql, params);
        sql::PageQuery query; query.status = std::move(bound.status);
        query.statement = {std::move(bound.sql), std::move(bound.params)};
        return query;
    }
    template<class T> PageResult<T> mapPage(PageRows raw) {
        PageResult<T> result;
        result.status = std::move(raw.status); result.page = raw.page; result.pageSize = raw.pageSize;
        result.mode = raw.mode; result.hasNext = raw.hasNext; result.total = raw.total;
        if (result.ok()) result.status = mapping::fromRows<T>(raw.rows, result.items);
        if (!result.ok()) { result.items.clear(); result.hasNext = false; result.total.reset(); }
        return result;
    }
}

namespace sqlconduit {
    template<class T> PageResult<T> Client::queryPage(const sql::PageQuery &query, const PageOptions &options) const {
        return sqlconduit::detail::mapPage<T>(queryPageRaw(std::string{}, query, options));
    }
    template<class T> PageResult<T> Client::queryPage(const sql::BuildResult &query, const PageOptions &options) const {
        return queryPage<T>(sqlconduit::detail::pageQuery(query), options);
    }
    template<class T> PageResult<T> Client::queryPage(const sql::Builder &query, const PageOptions &options) const {
        return queryPage<T>(query.buildPage(), options);
    }
    template<class T> PageResult<T> Client::queryPage(const sql::TypedBuilder<T> &query, const PageOptions &options) const {
        return queryPage<T>(query.buildPage(), options);
    }
    template<class T> PageResult<T> Client::queryPage(const std::string &sql, const common::Params &params, const PageOptions &options) const {
        sql::PageQuery query; query.statement = {sql, params};
        return queryPage<T>(query, options);
    }
    template<class T> PageResult<T> Client::queryPageNamed(const std::string &sql, const common::NamedParams &params, const PageOptions &options) const {
        return queryPage<T>(sqlconduit::detail::namedPageQuery(sql, params), options);
    }
    template<class T> PageResult<T> core::Session::queryPage(const sql::PageQuery &query, const PageOptions &options) const {
        return sqlconduit::detail::mapPage<T>(queryPageRaw(query, options));
    }
    template<class T> PageResult<T> core::Session::queryPage(const sql::BuildResult &query, const PageOptions &options) const {
        return queryPage<T>(sqlconduit::detail::pageQuery(query), options);
    }
    template<class T> PageResult<T> core::Session::queryPage(const sql::Builder &query, const PageOptions &options) const {
        return queryPage<T>(query.buildPage(), options);
    }
    template<class T> PageResult<T> core::Session::queryPage(const sql::TypedBuilder<T> &query, const PageOptions &options) const {
        return queryPage<T>(query.buildPage(), options);
    }
    template<class T> PageResult<T> core::Session::queryPage(const std::string &sql, const common::Params &params, const PageOptions &options) const {
        sql::PageQuery query; query.statement = {sql, params};
        return queryPage<T>(query, options);
    }
    template<class T> PageResult<T> core::Session::queryPageNamed(const std::string &sql, const common::NamedParams &params, const PageOptions &options) const {
        return queryPage<T>(sqlconduit::detail::namedPageQuery(sql, params), options);
    }
    template<class T> std::future<PageResult<T>> Client::queryPageAsync(const sql::PageQuery &query, const PageOptions &options) const {
        auto promise = std::make_shared<std::promise<PageResult<T>>>();
        auto future = promise->get_future();
        queryPageRawAsync(std::string{}, query, options, [promise](detail::PageRows raw) {
            try { promise->set_value(detail::mapPage<T>(std::move(raw))); }
            catch (...) { promise->set_exception(std::current_exception()); }
        });
        return future;
    }
    template<class T> std::future<PageResult<T>> Client::queryPageAsync(const sql::BuildResult &query, const PageOptions &options) const {
        return queryPageAsync<T>(detail::pageQuery(query), options);
    }
    template<class T> std::future<PageResult<T>> Client::queryPageAsync(const sql::Builder &query, const PageOptions &options) const {
        return queryPageAsync<T>(query.buildPage(), options);
    }
    template<class T> std::future<PageResult<T>> Client::queryPageAsync(const sql::TypedBuilder<T> &query, const PageOptions &options) const {
        return queryPageAsync<T>(query.buildPage(), options);
    }
    template<class T> std::future<PageResult<T>> Client::queryPageAsync(const std::string &sql, const common::Params &params, const PageOptions &options) const {
        sql::PageQuery query; query.statement = {sql, params};
        return queryPageAsync<T>(query, options);
    }
    template<class T> std::future<PageResult<T>> Client::queryPageNamedAsync(const std::string &sql, const common::NamedParams &params, const PageOptions &options) const {
        return queryPageAsync<T>(detail::namedPageQuery(sql, params), options);
    }
}
#endif
