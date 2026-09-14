#ifndef DBMW_DRIVER_MYSQL_DRIVER_H
#define DBMW_DRIVER_MYSQL_DRIVER_H

#include "dbmw/core/idatabase_connection.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/driver/idriver.h"
#include "dbmw/common/types.h"

#include <string>
#include <memory>
#include <mutex>
#include <list>
#include <unordered_map>

#ifdef DBMW_ENABLE_MYSQL
#include <mysql.h>

#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000
using MysqlBool = bool;
#else
using MysqlBool = my_bool;
#endif

using MysqlBoolArray = std::unique_ptr<MysqlBool[]>;
#endif

namespace dbmw::driver {
    class MySQLConnection : public core::IDatabaseConnection {
    public:
        MySQLConnection() = default;

        ~MySQLConnection() override { MySQLConnection::close(); }

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

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                 const core::CursorOptions &opts,
                                 std::unique_ptr<core::ICursor> &out) override;

        [[nodiscard]] std::string escapeLiteral(const common::Value &v) const override;

        [[nodiscard]] bool supportsParams() const override {
#ifdef DBMW_ENABLE_MYSQL
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

        [[nodiscard]] bool isOpen() const override { return open_; }

        [[nodiscard]] bool inTransaction() const override { return txOpen_; }

        common::Status cancel() override;

    private:
        common::Status lastError(const char *where);

        friend class MyCursor;

#ifdef DBMW_ENABLE_MYSQL
        common::Status stmtError(const char *where, MYSQL_STMT *stmt);
#endif

        bool open_ = false;
        bool txOpen_ = false;
        config::DataSourceConfig cfg_;

        std::unordered_map<std::string, core::PreparedStatementHandle> preparedCache_;
        std::unordered_map<std::uint64_t, std::string> preparedKeys_;
        std::list<std::string> preparedLru_;
        std::uint64_t preparedSeq_ = 0;
        int preparedLimit_ = 0;
#ifdef DBMW_ENABLE_MYSQL
        MYSQL *m_ = nullptr;
        mutable std::mutex operationMtx_;
        unsigned long activeThreadId_ = 0;
#endif
    };

    class MySQLDriver : public IDriver {
    public:
        [[nodiscard]] const char *name() const override { return "mysql"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<MySQLConnection>();
        }
    };

    void registerMySQLDriver();
}

#endif
