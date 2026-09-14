#ifndef DBMW_DRIVER_ODBC_DRIVER_H
#define DBMW_DRIVER_ODBC_DRIVER_H

#include "dbmw/core/idatabase_connection.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/driver/idriver.h"
#include "dbmw/common/types.h"

#include <string>
#include <mutex>
#include <list>
#include <unordered_map>

namespace dbmw::driver {
    class OdbcConnection : public core::IDatabaseConnection {
    public:
        ~OdbcConnection() override;

        common::Status connect(const config::DataSourceConfig &cfg) override;

        common::Status ping() override;

        common::Status query(const std::string &sql, common::ResultSet &out) override;

        common::Status execute(const std::string &sql, int64_t &affected) override;

        common::Status query(const std::string &sql, const common::Params &params,
                             common::ResultSet &out) override;

        common::Status execute(const std::string &sql, const common::Params &params,
                               int64_t &affected) override;

        common::Status execute(const std::string &sql, int64_t &affected,
                               common::GeneratedKeys &out) override;
        common::Status execute(const std::string &sql, const common::Params &params,
                               int64_t &affected, common::GeneratedKeys &out) override;

        [[nodiscard]] bool supportsPrepared() const override;
        common::Status prepare(const std::string &sql, const common::Params &typesSample,
                               core::PreparedStatementHandle &out) override;
        common::Status executePrepared(const core::PreparedStatementHandle &h,
                                       const common::Params &params,
                                       common::ResultSet &out) override;
        common::Status executePrepared(const core::PreparedStatementHandle &h,
                                       const common::Params &params,
                                       int64_t &affected) override;
        void closeAllPrepared() override;
        void setPreparedCacheLimit(int maxPerConnection) override;

        common::Status queryEach(const std::string &sql, const common::Params &params,
                                 const common::RowCallback &callback,
                                 std::uint64_t &rows) override;

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                 const core::CursorOptions &opts,
                                 std::unique_ptr<core::ICursor> &out) override;

        [[nodiscard]] bool supportsParams() const override {
#ifdef DBMW_ENABLE_ODBC
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

        [[nodiscard]] bool inTransaction() const override { return txOpen_; }

        common::Status cancel() override;

    private:
        friend class OdbcCursor;

        bool open_ = false;
        bool txOpen_ = false;
        config::DataSourceConfig cfg_;

        void *env_ = nullptr;
        void *dbc_ = nullptr;
        void *activeStmt_ = nullptr;
        std::mutex activeStmtMtx_;
        std::uint64_t defaultIsolation_ = 0;

        std::unordered_map<std::string, core::PreparedStatementHandle> preparedCache_;
        std::unordered_map<std::uint64_t, std::string> preparedKeys_;
        std::list<std::string> preparedLru_;
        std::uint64_t preparedSeq_ = 0;
        int preparedLimit_ = 0;
    };

    class OdbcDriver : public IDriver {
    public:
        const char *name() const override { return "odbc"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<OdbcConnection>();
        }
    };

    void registerOdbcDriver();
}

#endif
