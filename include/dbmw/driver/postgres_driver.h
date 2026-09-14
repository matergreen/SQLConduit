#ifndef DBMW_DRIVER_POSTGRES_DRIVER_H
#define DBMW_DRIVER_POSTGRES_DRIVER_H

#include "dbmw/core/idatabase_connection.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/driver/idriver.h"
#include "dbmw/common/types.h"

#include <string>
#include <memory>
#include <mutex>
#include <cstdint>
#include <list>
#include <unordered_map>

#ifdef DBMW_ENABLE_POSTGRES
#include <pqxx/pqxx>
#endif

namespace dbmw::driver {
#ifdef DBMW_ENABLE_POSTGRES
    using PgTx = pqxx::transaction<>;
#endif

    class PostgresConnection : public core::IDatabaseConnection {
    public:
        PostgresConnection() = default;

        ~PostgresConnection() override { PostgresConnection::close(); }

        common::Status connect(const config::DataSourceConfig &cfg) override;

        common::Status ping() override;

        common::Status query(const std::string &sql, common::ResultSet &out) override;

        common::Status execute(const std::string &sql, std::int64_t &affected) override;

        common::Status query(const std::string &sql, const common::Params &params,
                             common::ResultSet &out) override;

        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected) override;

        common::Status execute(const std::string &sql, std::int64_t &affected,
                               common::GeneratedKeys &out) override;
        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected, common::GeneratedKeys &out) override;

        [[nodiscard]] bool supportsPrepared() const override;
        common::Status prepare(const std::string &sql, const common::Params &typesSample,
                              core::PreparedStatementHandle &out) override;
        common::Status executePrepared(const core::PreparedStatementHandle &h,
                                      const common::Params &params,
                                      common::ResultSet &out) override;
        common::Status executePrepared(const core::PreparedStatementHandle &h,
                                      const common::Params &params,
                                      std::int64_t &affected) override;
        void closeAllPrepared() override;
        void setPreparedCacheLimit(int maxPerConnection) override;

        common::Status queryEach(const std::string &sql, const common::Params &params,
                                 const common::RowCallback &callback,
                                 std::uint64_t &rows) override;

        common::Status executeBatch(const std::string &sql,
                                    const common::ParamBatch &batch,
                                    common::BatchResult &out) override;

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                 const core::CursorOptions &opts,
                                 std::unique_ptr<core::ICursor> &out) override;

        std::string escapeLiteral(const common::Value &v) const override;

        bool supportsParams() const override {
#ifdef DBMW_ENABLE_POSTGRES
            return true;
#else
            return false;
#endif
        }

        common::Status begin() override;

        common::Status begin(const common::TransactionOptions &options) override;

        common::Status commit() override;

        common::Status rollback() override;
        common::Status savepoint(const std::string &name) override;
        common::Status releaseSavepoint(const std::string &name) override;
        common::Status rollbackToSavepoint(const std::string &name) override;

        void close() override;

        bool isOpen() const override { return open_; }

        [[nodiscard]] bool inTransaction() const override {
#ifdef DBMW_ENABLE_POSTGRES
            return tx_ != nullptr;
#else
            return false;
#endif
        }

        common::Status cancel() override;

    private:
        common::Status lastError(const char *where) const;

        friend class PgCursor;

        bool open_ = false;
        config::DataSourceConfig cfg_;
        std::unordered_map<std::string, core::PreparedStatementHandle> preparedCache_;
        std::unordered_map<std::uint64_t, std::string> preparedNames_;
        std::list<std::string> preparedLru_;
        std::uint64_t preparedSeq_ = 0;
        int preparedLimit_ = 0;
        std::string lastErr_;
        mutable std::mutex operationMtx_;
        bool operationActive_ = false;
#ifdef DBMW_ENABLE_POSTGRES
        std::unique_ptr<pqxx::connection> conn_;
        std::unique_ptr<PgTx> tx_;
#endif
    };

    class PostgresDriver : public IDriver {
    public:
        const char *name() const override { return "postgres"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<PostgresConnection>();
        }
    };

    void registerPostgresDriver();
}

#endif
