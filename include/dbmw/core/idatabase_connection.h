#ifndef DBMW_CORE_IDATABASE_CONNECTION_H
#define DBMW_CORE_IDATABASE_CONNECTION_H

#include "dbmw/common/types.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/core/cursor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dbmw::core {
    class PreparedStatementHandle {
    public:
        PreparedStatementHandle() = default;

        [[nodiscard]] bool valid() const { return id_ != 0; }
        [[nodiscard]] std::uint64_t id() const { return id_; }
        [[nodiscard]] void *native() const { return native_; }

        static PreparedStatementHandle make(std::uint64_t id, void *native) {
            PreparedStatementHandle h;
            h.id_ = id;
            h.native_ = native;
            return h;
        }

    private:
        std::uint64_t id_ = 0;
        void *native_ = nullptr;
    };

    class IDatabaseConnection {
    public:
        virtual ~IDatabaseConnection() = default;

        virtual common::Status connect(const config::DataSourceConfig &cfg) = 0;

        virtual common::Status ping() = 0;

        virtual common::Status query(const std::string &sql, common::ResultSet &out) = 0;

        virtual common::Status execute(const std::string &sql, std::int64_t &affected) = 0;

        virtual common::Status begin() = 0;

        virtual common::Status begin(const common::TransactionOptions &options);

        virtual common::Status commit() = 0;

        virtual common::Status rollback() = 0;

        virtual common::Status savepoint(const std::string &name);

        virtual common::Status releaseSavepoint(const std::string &name);

        virtual common::Status rollbackToSavepoint(const std::string &name);

        virtual void close() = 0;

        [[nodiscard]] virtual bool isOpen() const = 0;

        virtual common::Status cancel();

        virtual common::Status queryEach(const std::string &sql,
                                         const common::Params &params,
                                         const common::RowCallback &callback,
                                         std::uint64_t &rows);

        [[nodiscard]] virtual bool supportsMultipleResultSets() const { return false; }

        virtual common::Status queryAll(const std::string &sql,
                                        std::vector<common::ResultSet> &out);

        virtual common::Status queryAll(const std::string &sql,
                                        const common::Params &params,
                                        std::vector<common::ResultSet> &out);

        virtual common::Status openCursor(const std::string &sql,
                                          const common::Params &params,
                                          const CursorOptions &opts,
                                          std::unique_ptr<ICursor> &out);

        [[nodiscard]] virtual bool inTransaction() const { return false; }

        virtual common::Status executeBatch(const std::string &sql,
                                            const common::ParamBatch &batch,
                                            common::BatchResult &out);

        virtual common::Status query(const std::string &sql,
                                     const common::Params &params,
                                     common::ResultSet &out);

        virtual common::Status execute(const std::string &sql,
                                       const common::Params &params,
                                       std::int64_t &affected);

        [[nodiscard]] virtual bool supportsParams() const { return false; }

        [[nodiscard]] virtual bool allowsLiteralInterpolation() const { return false; }

        virtual common::Status prepare(const std::string &sql,
                                       const common::Params &typesSample,
                                       PreparedStatementHandle &out);

        virtual common::Status executePrepared(const PreparedStatementHandle &h,
                                               const common::Params &params,
                                               common::ResultSet &out);

        virtual common::Status executePrepared(const PreparedStatementHandle &h,
                                               const common::Params &params,
                                               std::int64_t &affected);

        virtual void closeAllPrepared();

        [[nodiscard]] virtual bool supportsPrepared() const { return false; }

        virtual void setPreparedCacheLimit(int maxPerConnection) { (void) maxPerConnection; }

        [[nodiscard]] virtual std::string escapeLiteral(const common::Value &v) const;

        common::Status renderSqlForLogging(const std::string &sql,
                                           const common::Params &params,
                                           const common::SqlRenderOptions &options,
                                           std::string &out) const;

        virtual common::Status execute(const std::string &sql, std::int64_t &affected,
                                       common::GeneratedKeys &out);

        virtual common::Status execute(const std::string &sql, const common::Params &params,
                                       std::int64_t &affected, common::GeneratedKeys &out);

        virtual common::Status query(const std::string &sql,
                                     const common::StreamParams &params,
                                     common::ResultSet &out);

        virtual common::Status execute(const std::string &sql,
                                       const common::StreamParams &params,
                                       std::int64_t &affected,
                                       common::GeneratedKeys &out);

        virtual common::Status executeBatch(const std::string &sql,
                                            const common::StreamParamBatch &batch,
                                            common::BatchResult &out);

    protected:
        using PlaceholderVisitor = std::function<std::string(std::size_t)>;

        static std::string replacePlaceholders(const std::string &sql,
                                               const PlaceholderVisitor &visitor,
                                               std::size_t &found);

        common::Status buildSql(const std::string &sql, const common::Params &params,
                                std::string &out) const;
    };
}

#endif
