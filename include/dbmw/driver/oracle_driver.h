#ifndef DBMW_DRIVER_ORACLE_DRIVER_H
#define DBMW_DRIVER_ORACLE_DRIVER_H

#include "dbmw/core/idatabase_connection.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/driver/idriver.h"
#include "dbmw/common/types.h"
#include "dbmw/common/oracle_types.h"

#include <string>
#include <memory>
#include <mutex>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef DBMW_ENABLE_ORACLE
#include <oci.h>
#endif

namespace dbmw::driver {
    class OracleConnection : public core::IDatabaseConnection {
    public:
        OracleConnection() = default;

        ~OracleConnection() override { OracleConnection::close(); }

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

        [[nodiscard]] bool supportsMultipleResultSets() const override {
#if defined(DBMW_ENABLE_ORACLE) && defined(OCI_RESULT_TYPE_SELECT)
            return true;
#else
            return false;
#endif
        }

        common::Status queryAll(const std::string &sql,
                                std::vector<common::ResultSet> &out) override;

        common::Status queryAll(const std::string &sql, const common::Params &params,
                                std::vector<common::ResultSet> &out) override;

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                  const core::CursorOptions &opts,
                                  std::unique_ptr<core::ICursor> &out) override;

        std::string escapeLiteral(const common::Value &v) const override;

        bool supportsParams() const override {
#ifdef DBMW_ENABLE_ORACLE
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
        common::Status runStatement(const std::string &sql, const common::Params &params,
                                    bool isQuery, std::int64_t &affected,
                                    common::ResultSet &out, bool collectKeys,
                                    std::vector<std::string> &keyColumns,
                                    const common::RowCallback &callback,
                                    std::int64_t &streamedRows,
                                    const std::string &cacheKey = std::string());

        common::Status lastError(const common::ErrorCode fallback, const char *where) const;

        void dropCachedStatement(std::uint64_t id);

        common::Status connectString(const config::DataSourceConfig &cfg, std::string &out) const;

        bool open_ = false;
        bool txOpen_ = false;
        std::int64_t lobMaxBytes_ = 4194304;
        config::DataSourceConfig cfg_;
        std::unordered_map<std::string, core::PreparedStatementHandle> preparedCache_;
        std::unordered_map<std::uint64_t, std::string> preparedSql_;
        std::list<std::string> preparedLru_;
        std::uint64_t preparedSeq_ = 0;
        int preparedLimit_ = 0;
        std::string lastErr_;
        mutable std::mutex operationMtx_;
        bool operationActive_ = false;
#ifdef DBMW_ENABLE_ORACLE
        OCIEnv *env_ = nullptr;
        OCIError *err_ = nullptr;
        OCISvcCtx *svc_ = nullptr;
#endif
    };

    class OracleDriver : public IDriver {
    public:
        const char *name() const override { return "oracle"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<OracleConnection>();
        }
    };

    void registerOracleDriver();
}

#endif
