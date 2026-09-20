#include "dbmw/dbmw.h"
#include "dbmw/async/dbmw_async.h"
#include "dbmw/common/context.h"
#include "dbmw/config/config_loader.h"

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace dbmw {
    namespace {
        core::DatabaseManager &mgr() {
            static core::DatabaseManager m;
            return m;
        }

        common::Status notFound(const std::string &name) {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "datasource not found: " + name);
        }

        common::Status noDefault() {
            return common::Status::error(common::ErrorCode::ConfigError,
                                         "no default datasource");
        }

        std::string effectiveDataSourceName(const std::string &name) {
            const auto &target = common::ContextScope::current().targetDataSource;
            return target.empty() ? name : target;
        }

        common::Status resolve(const std::string &name, std::shared_ptr<core::DataSource> &out) {
            const auto effectiveName = effectiveDataSourceName(name);
            out = effectiveName.empty() ? mgr().getDefault() : mgr().getDataSource(effectiveName);
            if (out) return common::Status::OK();
            return effectiveName.empty() ? noDefault() : notFound(effectiveName);
        }
    }

    common::Status DBMW::init(const std::string &configPath) {
        config::GlobalConfig cfg;
        std::string err;
        if (!config::ConfigLoader::loadFromFile(configPath, cfg, err)) {
            return common::Status::error(common::ErrorCode::ConfigError, err);
        }
        core::InterceptorRegistry::setEnabled(cfg.interceptors.enabled);
        const auto st = mgr().init(cfg);
        if (st.ok()) async::detail::initEngine(cfg.async);
        return st;
    }

    common::Status DBMW::reload(const std::string &configPath,
                                const std::chrono::milliseconds grace) {
        config::GlobalConfig cfg;
        std::string error;
        if (!config::ConfigLoader::loadFromFile(configPath, cfg, error)) {
            return common::Status::error(common::ErrorCode::ConfigError, error);
        }
        core::InterceptorRegistry::setEnabled(cfg.interceptors.enabled);
        const auto st = mgr().init(cfg, grace);
        if (st.ok()) async::detail::initEngine(cfg.async);
        return st;
    }

    common::Status DBMW::query(const std::string &sql, common::ResultSet &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->query(sql, out);
    }

    common::Status DBMW::execute(const std::string &sql, std::int64_t &affected) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->execute(sql, affected);
    }

    common::Status DBMW::query(const std::string &dataSource, const std::string &sql, common::ResultSet &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->query(sql, out);
    }

    common::Status DBMW::execute(const std::string &dataSource, const std::string &sql, std::int64_t &affected) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->execute(sql, affected);
    }

    common::Status DBMW::query(const std::string &sql, const common::Params &params, common::ResultSet &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->query(sql, params, out);
    }

    common::Status DBMW::query(const std::string &dataSource, const std::string &sql, const common::Params &params,
                               common::ResultSet &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->query(sql, params, out);
    }

    common::Status DBMW::execute(const std::string &sql, const common::Params &params, std::int64_t &affected) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->execute(sql, params, affected);
    }

    common::Status DBMW::execute(const std::string &dataSource, const std::string &sql, const common::Params &params,
                                 std::int64_t &affected) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->execute(sql, params, affected);
    }

    common::Status DBMW::queryAll(const std::string &sql,
                                  std::vector<common::ResultSet> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->queryAll(sql, out);
    }

    common::Status DBMW::queryAll(const std::string &sql, const common::Params &params,
                                  std::vector<common::ResultSet> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->queryAll(sql, params, out);
    }

    common::Status DBMW::queryAll(const std::string &dataSource, const std::string &sql,
                                  std::vector<common::ResultSet> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->queryAll(sql, out);
    }

    common::Status DBMW::queryAll(const std::string &dataSource, const std::string &sql,
                                  const common::Params &params,
                                  std::vector<common::ResultSet> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->queryAll(sql, params, out);
    }

    common::Status DBMW::queryEach(const std::string &sql, const common::Params &params,
                                   const common::RowCallback &callback, std::uint64_t &rows) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->queryEach(sql, params, callback, rows);
    }

    common::Status DBMW::queryEach(const std::string &dataSource, const std::string &sql,
                                   const common::Params &params,
                                   const common::RowCallback &callback, std::uint64_t &rows) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->queryEach(sql, params, callback, rows);
    }

    common::Status DBMW::executeBatch(const std::string &sql,
                                      const common::ParamBatch &batch,
                                      common::BatchResult &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->executeBatch(sql, batch, out);
    }

    common::Status DBMW::executeBatch(const std::string &dataSource,
                                      const std::string &sql,
                                      const common::ParamBatch &batch,
                                      common::BatchResult &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->executeBatch(sql, batch, out);
    }

    common::Status DBMW::openCursor(const std::string &sql, const common::Params &params,
                                    const core::CursorOptions &opts,
                                    std::unique_ptr<core::Cursor> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->openCursor(sql, params, opts, out);
    }

    common::Status DBMW::openCursor(const std::string &dataSource, const std::string &sql,
                                    const common::Params &params,
                                    const core::CursorOptions &opts,
                                    std::unique_ptr<core::Cursor> &out) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->openCursor(sql, params, opts, out);
    }

    common::Status DBMW::transaction(const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->transaction(fn);
    }

    common::Status DBMW::transaction(const std::string &dataSource, const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->transaction(fn);
    }

    common::Status DBMW::transaction(const common::TransactionOptions &options,
                                     const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->transaction(options, fn);
    }

    common::Status DBMW::transaction(const std::string &dataSource,
                                     const common::TransactionOptions &options,
                                     const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->transaction(options, fn);
    }

    common::Status DBMW::withSession(const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(std::string(), ds); !st.ok()) return st;
        return ds->withSession(fn);
    }

    common::Status DBMW::withSession(const std::string &dataSource, const core::SessionFn &fn) {
        std::shared_ptr<core::DataSource> ds;
        if (const auto st = resolve(dataSource, ds); !st.ok()) return st;
        return ds->withSession(fn);
    }

    std::shared_ptr<core::DataSource> DBMW::dataSource(const std::string &name) {
        const auto effectiveName = effectiveDataSourceName(name);
        return effectiveName.empty() ? mgr().getDefault() : mgr().getDataSource(effectiveName);
    }

    bool DBMW::poolStats(core::ConnectionPool::Stats &out, const std::string &name) {
        const auto source = dataSource(name);
        return source && source->poolStats(out);
    }

    std::vector<core::NamedPoolStats> DBMW::allPoolStats() {
        return mgr().allPoolStats();
    }

    std::vector<common::SlowSqlStats> DBMW::slowSqlStats(
        const std::size_t limit, const std::string &dataSource) {
        return common::Observability::slowSqlStats(limit, dataSource);
    }

    std::vector<common::SlowSqlRecord> DBMW::recentSlowSql(
        const std::size_t limit, const std::string &dataSource) {
        return common::Observability::recentSlowSql(limit, dataSource);
    }

    void DBMW::clearSlowSqlStats() {
        common::Observability::clearSlowSqlStats();
    }

    void DBMW::shutdown(const std::chrono::milliseconds grace) {
        async::detail::drainAndStop(grace);
        mgr().shutdown(grace);
    }

    void DBMW::setObserver(common::OperationObserver observer) {
        common::Observability::setObserver(std::move(observer));
    }

    void DBMW::addInterceptor(std::shared_ptr<core::ISqlInterceptor> interceptor) {
        core::InterceptorRegistry::add(std::move(interceptor));
    }

    void DBMW::clearInterceptors() {
        core::InterceptorRegistry::clear();
    }

    void DBMW::setDefaultRateLimiter(std::shared_ptr<core::IRateLimiter> limiter) {
        core::DatabaseManager::setDefaultRateLimiter(std::move(limiter));
    }

    common::Status DBMW::addDataSource(
        const config::DataSourceConfig &cfg,
        const core::DataSourceOptions &opts) {
        return mgr().addDataSource(cfg, opts);
    }

    common::Status DBMW::removeDataSource(const std::string &name,
                                          const std::chrono::milliseconds grace) {
        return mgr().removeDataSource(name, grace);
    }

    common::Status DBMW::addGroup(
        const config::DataSourceGroupConfig &cfg,
        const core::GroupOptions &opts) {
        return mgr().addGroup(cfg, opts);
    }

    common::Status DBMW::removeGroup(const std::string &name,
                                     const std::chrono::milliseconds grace) {
        return mgr().removeGroup(name, grace);
    }
}
