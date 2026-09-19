#ifndef DBMW_CORE_DATABASE_MANAGER_H
#define DBMW_CORE_DATABASE_MANAGER_H

#include "dbmw/config/datasource_config.h"
#include "dbmw/core/connection_pool.h"
#include "dbmw/core/heartbeat_manager.h"
#include "dbmw/core/rate_limiter.h"
#include "dbmw/core/write_buffer.h"
#include "dbmw/common/observer.h"
#include "dbmw/common/types.h"
#include "dbmw/core/cursor.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dbmw {

    namespace async::detail {
        class AsyncEngine;
    }

    namespace core {

    class Cursor;

    class Session {
    public:
        struct AuditContext {
            bool enabled = false;
            bool readOnly = false;

            AuditContext() = default;
            AuditContext(bool e, bool ro) : enabled(e), readOnly(ro) {}
        };

        Session(const Session &) = delete;

        Session &operator=(const Session &) = delete;

        ~Session();

        Session(Session &&other) noexcept
            : h_(std::move(other.h_)), dataSource_(std::move(other.dataSource_)),
              audit_(other.audit_), txOpen_(other.txOpen_),
              didWrite_(other.didWrite_.load()) {
            other.txOpen_ = false;
        }

        Session &operator=(Session &&other) noexcept {
            if (this != &other) {
                cleanupOpenTransaction();
                h_ = std::move(other.h_);
                dataSource_ = std::move(other.dataSource_);
                audit_ = other.audit_;
                txOpen_ = other.txOpen_;
                other.txOpen_ = false;
                didWrite_.store(other.didWrite_.load());
            }
            return *this;
        }

        common::Status query(const std::string &sql, common::ResultSet &out) const;

        common::Status query(const std::string &sql, const common::Params &params,
                             common::ResultSet &out) const;

        common::Status queryAll(const std::string &sql,
                                std::vector<common::ResultSet> &out) const;

        common::Status queryAll(const std::string &sql, const common::Params &params,
                                std::vector<common::ResultSet> &out) const;

        common::Status execute(const std::string &sql, std::int64_t &affected) const;

        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected) const;

        common::Status queryEach(const std::string &sql, const common::Params &params,
                                 const common::RowCallback &callback,
                                 std::uint64_t &rows) const;

        common::Status executeBatch(const std::string &sql,
                                    const common::ParamBatch &batch,
                                    common::BatchResult &out) const;

        common::Status execute(const std::string &sql, std::int64_t &affected,
                               common::GeneratedKeys &out) const;

        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected, common::GeneratedKeys &out) const;

        common::Status query(const std::string &sql, const common::StreamParams &params,
                             common::ResultSet &out) const;

        common::Status execute(const std::string &sql, const common::StreamParams &params,
                               std::int64_t &affected, common::GeneratedKeys &out) const;

        common::Status executeBatch(const std::string &sql,
                                    const common::StreamParamBatch &batch,
                                    common::BatchResult &out) const;

        common::Status prepare(const std::string &sql, const common::Params &typesSample,
                               PreparedStatementHandle &out) const;

        common::Status executePrepared(const PreparedStatementHandle &h,
                                       const common::Params &params,
                                       common::ResultSet &out) const;

        common::Status executePrepared(const PreparedStatementHandle &h,
                                       const common::Params &params,
                                       std::int64_t &affected) const;

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                  const CursorOptions &opts,
                                  std::unique_ptr<Cursor> &out) const;

        common::Status begin();

        common::Status begin(const common::TransactionOptions &options);

        common::Status commit();

        common::Status rollback();

        common::Status savepoint(const std::string &name);
        common::Status releaseSavepoint(const std::string &name);
        common::Status rollbackToSavepoint(const std::string &name);

        [[nodiscard]] common::Status cancel() const;

        [[nodiscard]] bool inTransaction() const { return txOpen_; }

        [[nodiscard]] bool didWrite() const { return didWrite_.load(); }

    private:
        friend class DataSource;

        explicit Session(std::unique_ptr<ConnectionPool::Handle> h, std::string dataSource)
            : h_(std::move(h)), dataSource_(std::move(dataSource)), audit_() {}

        explicit Session(std::unique_ptr<ConnectionPool::Handle> h, std::string dataSource,
                         AuditContext audit)
            : h_(std::move(h)), dataSource_(std::move(dataSource)), audit_(std::move(audit)) {}

        [[nodiscard]] common::Status auditStatement(const std::string &sql,
                                                    common::OperationType type) const;

        [[nodiscard]] common::Status runPreparedQuery(const std::string &sql,
                                                      const common::Params &params,
                                                      common::ResultSet &out) const;

        [[nodiscard]] common::Status runPreparedExec(const std::string &sql,
                                                     const common::Params &params,
                                                     std::int64_t &affected,
                                                     common::GeneratedKeys *keys) const;

        void cleanupOpenTransaction() noexcept;

        std::unique_ptr<ConnectionPool::Handle> h_;
        std::string dataSource_;
        AuditContext audit_;
        bool txOpen_ = false;
        mutable std::atomic<bool> didWrite_{false};
    };

    class Cursor {
    public:
        enum class Binding { OwnsHandle, BorrowedInSession };
        using RowTransform = std::function<void(common::Row &)>;

        Cursor(std::unique_ptr<ConnectionPool::Handle> h,
               std::unique_ptr<ICursor> impl,
               Session::AuditContext audit,
               Binding binding,
               std::shared_ptr<void> cursorLease = {},
               RowTransform rowTransform = {})
            : handle_(std::move(h)), impl_(std::move(impl)),
              audit_(std::move(audit)), binding_(binding),
              cursorLease_(std::move(cursorLease)),
              rowTransform_(std::move(rowTransform)) {}

        Cursor(const Cursor &) = delete;
        Cursor &operator=(const Cursor &) = delete;
        Cursor(Cursor &&) noexcept = default;
        Cursor &operator=(Cursor &&) noexcept = default;

        common::Status fetch(std::size_t n, common::ResultSet &out) {
            if (!impl_) return common::Status::error(common::ErrorCode::CursorClosed,
                                                    "cursor already closed or moved-from");
            const auto firstNewRow = out.rowCount();
            const auto status = impl_->fetch(n, out);
            if (status.ok() && rowTransform_) {
                auto &rows = out.mutableRows();
                for (std::size_t i = firstNewRow; i < rows.size(); ++i)
                    rowTransform_(rows[i]);
            }
            return status;
        }

        common::Status fetchRow(common::Row &out, bool &ok) {
            ok = false;
            if (!impl_) return common::Status::error(common::ErrorCode::CursorClosed,
                                                    "cursor already closed or moved-from");
            const auto status = impl_->fetchRow(out, ok);
            if (status.ok() && ok && rowTransform_) rowTransform_(out);
            return status;
        }

        common::Status close() {
            if (!impl_) {
                if (binding_ == Binding::OwnsHandle) handle_.reset();
                return common::Status::OK();
            }
            const auto st = impl_->close();
            impl_.reset();
            if (binding_ == Binding::OwnsHandle) handle_.reset();
            cursorLease_.reset();
            return st;
        }

        [[nodiscard]] bool isOpen() const { return impl_ && impl_->isOpen(); }
        [[nodiscard]] bool hasNext() const { return impl_ && impl_->hasNext(); }
        [[nodiscard]] std::uint64_t rowsFetched() const {
            return impl_ ? impl_->rowsFetched() : 0;
        }

        ~Cursor() noexcept;

    private:
        std::unique_ptr<ConnectionPool::Handle> handle_;
        std::unique_ptr<ICursor> impl_;
        Session::AuditContext audit_;
        Binding binding_;
        std::shared_ptr<void> cursorLease_;
        RowTransform rowTransform_;
    };

    using SessionFn = std::function<common::Status(Session &)>;

    using NamedPoolStats = common::NamedPoolStats;

    struct DataSourceOptions {
        config::RetryConfig retry = {};
        config::CircuitBreakerConfig circuit_breaker = {};
        std::shared_ptr<IRateLimiter> rate_limiter = nullptr;
        bool read_only = false;
        config::CursorConfig cursor = {};
        bool attach_heartbeat = true;
    };

    struct GroupOptions {
        std::shared_ptr<IRateLimiter> rate_limiter = nullptr;
        config::CursorConfig cursor = {};
        bool acknowledge_external_fencing = false;
        bool acknowledge_data_loss_and_duplicates = false;
    };

    class DataSource {
    public:
        DataSource(std::weak_ptr<ConnectionPool> pool, std::string name,
                   config::RetryConfig retry = {},
                   config::CircuitBreakerConfig circuitBreaker = {},
                   std::shared_ptr<IRateLimiter> rateLimiter = nullptr,
                   bool readOnly = false, bool readReplica = false)
            : pool_(std::move(pool)), name_(std::move(name)), retry_(retry),
              circuitBreaker_(circuitBreaker), rateLimiter_(std::move(rateLimiter)),
              readOnly_(readOnly), readReplica_(readReplica) {}

        DataSource(std::string name, std::shared_ptr<DataSource> primary,
                   std::vector<std::shared_ptr<DataSource>> weightedReplicas,
                   std::chrono::milliseconds readAfterWrite,
                   bool fallbackToPrimary,
                   std::shared_ptr<IRateLimiter> rateLimiter = nullptr,
                   bool readOnly = false,
                   std::vector<std::shared_ptr<DataSource>> failoverPrimaries = {},
                   bool requireHealthy = false,
                   std::shared_ptr<WriteBuffer> writeBuffer = nullptr)
            : name_(std::move(name)), primary_(std::move(primary)),
              replicas_(std::move(weightedReplicas)),
              readAfterWrite_(readAfterWrite), fallbackToPrimary_(fallbackToPrimary),
              rateLimiter_(std::move(rateLimiter)), readOnly_(readOnly),
              failoverPrimaries_(std::move(failoverPrimaries)),
              requireHealthy_(requireHealthy),
              writeBuffer_(std::move(writeBuffer)) {}

        common::Status query(const std::string &sql, common::ResultSet &out) const;

        common::Status query(const std::string &sql, const common::Params &params,
                             common::ResultSet &out) const;

        common::Status queryAll(const std::string &sql,
                                std::vector<common::ResultSet> &out) const;

        common::Status queryAll(const std::string &sql, const common::Params &params,
                                std::vector<common::ResultSet> &out) const;

        common::Status execute(const std::string &sql, std::int64_t &affected) const;

        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected) const;

        common::Status queryEach(const std::string &sql, const common::Params &params,
                                 const common::RowCallback &callback,
                                 std::uint64_t &rows) const;

        common::Status executeBatch(const std::string &sql,
                                    const common::ParamBatch &batch,
                                    common::BatchResult &out) const;

        common::Status execute(const std::string &sql, std::int64_t &affected,
                               common::GeneratedKeys &out) const;

        common::Status execute(const std::string &sql, const common::Params &params,
                               std::int64_t &affected, common::GeneratedKeys &out) const;

        common::Status query(const std::string &sql, const common::StreamParams &params,
                             common::ResultSet &out) const;

        common::Status execute(const std::string &sql, const common::StreamParams &params,
                               std::int64_t &affected, common::GeneratedKeys &out) const;

        common::Status executeBatch(const std::string &sql,
                                    const common::StreamParamBatch &batch,
                                    common::BatchResult &out) const;

        common::Status openCursor(const std::string &sql, const common::Params &params,
                                  const CursorOptions &opts,
                                  std::unique_ptr<Cursor> &out) const;

        common::Status withSession(const SessionFn &fn) const;

        common::Status withSession(const SessionFn &fn, std::chrono::milliseconds borrowTimeout) const;

        common::Status transaction(const SessionFn &fn) const;

        common::Status transaction(const SessionFn &fn, std::chrono::milliseconds borrowTimeout) const;

        common::Status transaction(const common::TransactionOptions &options,
                                   const SessionFn &fn) const;

        common::Status transaction(const common::TransactionOptions &options,
                                   const SessionFn &fn,
                                   std::chrono::milliseconds borrowTimeout) const;

        [[nodiscard]] const std::string &name() const { return name_; }

        [[nodiscard]] const std::string &driverType() const { return driverType_; }

        bool poolStats(ConnectionPool::Stats &out) const;

        void applyCursorConfig(const config::CursorConfig &cfg) {
            cursorEnabled_ = cfg.enabled;
            defaultBatchSize_ = cfg.default_batch_size > 0 ? cfg.default_batch_size : 256;
            cursorScrollable_ = cfg.allow_scrollable;
            cursorBudget_->limit.store(cfg.max_open_cursors);
        }

    private:
        friend class DatabaseManager;
        friend class async::detail::AsyncEngine;

        common::Status beforeAttempt() const;
        void afterAttempt(const common::Status &status) const;
        [[nodiscard]] std::chrono::milliseconds retryDelay(int attempt) const;
        [[nodiscard]] std::shared_ptr<DataSource> readTarget() const;
        void markWrite() const;

        common::Status preGate(const std::string &sql, common::OperationType type) const;

        common::Status gateSession() const;

        common::Status queryUngated(const std::string &sql, common::ResultSet &out) const;

        common::Status queryUngated(const std::string &sql, const common::Params &params,
                                    common::ResultSet &out) const;

        common::Status executeUngated(const std::string &sql, std::int64_t &affected) const;

        common::Status executeUngated(const std::string &sql, const common::Params &params,
                                      std::int64_t &affected) const;

        common::Status queryEachUngated(const std::string &sql, const common::Params &params,
                                        const common::RowCallback &callback,
                                        std::uint64_t &rows) const;

        common::Status executeBatchUngated(const std::string &sql,
                                           const common::ParamBatch &batch,
                                           common::BatchResult &out) const;

        common::Status executeUngated(const std::string &sql, std::int64_t &affected,
                                      common::GeneratedKeys &out) const;

        common::Status executeUngated(const std::string &sql, const common::Params &params,
                                      std::int64_t &affected,
                                      common::GeneratedKeys &out) const;

        common::Status queryUngated(const std::string &sql, const common::StreamParams &params,
                                    common::ResultSet &out) const;

        common::Status executeUngated(const std::string &sql, const common::StreamParams &params,
                                      std::int64_t &affected,
                                      common::GeneratedKeys &out) const;

        common::Status executeBatchUngated(const std::string &sql,
                                           const common::StreamParamBatch &batch,
                                           common::BatchResult &out) const;

        common::Status openCursorUngated(const std::string &sql, const common::Params &params,
                                         const CursorOptions &opts,
                                         std::unique_ptr<Cursor> &out) const;

        bool cursorBudgetAcquire(std::shared_ptr<void> &lease) const;

        common::Status dispatchWrite(
            const std::function<common::Status(const std::shared_ptr<DataSource> &)> &attempt,
            const std::function<common::Status()> &buffered) const;

        [[nodiscard]] bool isCircuitOpen() const;

        [[nodiscard]] std::vector<std::shared_ptr<DataSource>> writeTargets() const;

        [[nodiscard]] static bool safeToFailoverWrite(const common::Status &status);

        common::Status borrowSession(std::unique_ptr<ConnectionPool::Handle> &out,
                                     std::chrono::milliseconds timeout) const;

        std::unique_ptr<Session> makeSession(
            std::unique_ptr<ConnectionPool::Handle> h) const {
            return std::unique_ptr<Session>(new Session(std::move(h), name_));
        }

        [[nodiscard]] std::shared_ptr<ConnectionPool> pool() const { return pool_.lock(); }

        [[nodiscard]] bool cacheEligible() const;
        bool cacheLookup(const std::string &sql, const common::Params &params,
                         common::ResultSet &out, std::string &key) const;
        void cacheStore(const std::string &key, const common::ResultSet &rows) const;

        common::Status withSessionInternal(const SessionFn &fn,
                                           std::chrono::milliseconds borrowTimeout,
                                           bool *wroteOut,
                                           bool enforceReadOnly) const;

        common::Status transactionInternal(const common::TransactionOptions &options,
                                           const SessionFn &fn,
                                           std::chrono::milliseconds borrowTimeout,
                                           bool enforceReadOnly) const;

        std::weak_ptr<ConnectionPool> pool_;
        std::string name_;
        config::RetryConfig retry_;
        config::CircuitBreakerConfig circuitBreaker_;
        mutable std::atomic<int> consecutiveFailures_{0};
        mutable std::atomic<bool> halfOpenInFlight_{false};
        mutable std::atomic<std::chrono::steady_clock::time_point> circuitOpenUntil_{};
        std::shared_ptr<DataSource> primary_;
        std::vector<std::shared_ptr<DataSource>> replicas_;
        std::chrono::milliseconds readAfterWrite_{0};
        bool fallbackToPrimary_ = true;
        mutable std::atomic<std::int64_t> lastWriteNs_{0};
        std::shared_ptr<IRateLimiter> rateLimiter_;
        bool readOnly_ = false;
        std::atomic<bool> readReplica_{false};
        std::vector<std::shared_ptr<DataSource>> failoverPrimaries_;
        bool requireHealthy_ = false;
        std::string shadowName_;
        std::shared_ptr<DataSource> shadow_;
        std::string driverType_;
        std::shared_ptr<WriteBuffer> writeBuffer_;
        struct CursorBudgetState {
            std::atomic<int> limit{0};
            std::atomic<int> open{0};
        };
        std::shared_ptr<CursorBudgetState> cursorBudget_ =
            std::make_shared<CursorBudgetState>();
        bool cursorEnabled_ = true;
        int defaultBatchSize_ = 256;
        bool cursorScrollable_ = false;
    };

    int currentTransactionDepth() noexcept;

    class StatsReporter;
    struct PoolCollectorLease;

    class DatabaseManager {
    public:
        DatabaseManager();

        ~DatabaseManager();

        DatabaseManager(const DatabaseManager &) = delete;

        DatabaseManager &operator=(const DatabaseManager &) = delete;

        common::Status init(const config::GlobalConfig &cfg,
                            std::chrono::milliseconds replacementGrace =
                                std::chrono::milliseconds(0));

        std::shared_ptr<DataSource> getDataSource(const std::string &name);

        std::shared_ptr<DataSource> getDefault();

        common::Status addDataSource(const config::DataSourceConfig &cfg,
                                     const DataSourceOptions &opts = DataSourceOptions{});

        common::Status removeDataSource(const std::string &name,
                                        std::chrono::milliseconds grace =
                                            std::chrono::milliseconds(5000));

        common::Status addGroup(const config::DataSourceGroupConfig &cfg,
                                const GroupOptions &opts = GroupOptions{});

        common::Status removeGroup(const std::string &name,
                                   std::chrono::milliseconds grace =
                                       std::chrono::milliseconds(5000));

        void shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(5000));

        static void setDefaultRateLimiter(std::shared_ptr<IRateLimiter> limiter) noexcept;

        inline static std::shared_ptr<IRateLimiter> defaultRateLimiter_;

        size_t dataSourceCount() const;

        std::vector<NamedPoolStats> allPoolStats() const;

    private:

        [[nodiscard]] common::Status validateGroupRefs(
            const config::DataSourceGroupConfig &cfg,
            const std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > &candidates,
            const std::unordered_set<std::string> &replicaNames) const;

        [[nodiscard]] common::Status checkLeafNotInUse_Unused(
            const std::string &leafName) const;

        [[nodiscard]] common::Status buildSingleDataSource(
            const config::DataSourceConfig &dsc,
            const config::PoolConfig &poolCfg,
            const config::RetryConfig &retry,
            const config::CircuitBreakerConfig &circuit,
            const config::CursorConfig &cursor,
            std::shared_ptr<IRateLimiter> rateLimiter,
            const std::unordered_set<std::string> &replicaNames,
            bool attachHeartbeat,
            std::shared_ptr<ConnectionPool> &outPool,
            std::shared_ptr<DataSource> &outSource);

        [[nodiscard]] common::Status buildSingleDataSourceGroup(
            const config::DataSourceGroupConfig &group,
            const config::PoolConfig &poolCfg,
            const GroupOptions &opts,
            const std::unordered_map<std::string, std::shared_ptr<DataSource> > &sources,
            const std::unordered_set<std::string> &replicaNames,
            std::vector<std::shared_ptr<WriteBuffer> > &outBuffers,
            std::shared_ptr<DataSource> &outSource);

        [[nodiscard]] common::Status resolveShadows();

        mutable std::mutex mtx_;
        std::unordered_map<std::string, std::shared_ptr<ConnectionPool> > pools_;
        std::unordered_map<std::string, std::shared_ptr<DataSource> > datasources_;
        std::unique_ptr<HeartbeatManager> heartbeat_;
        std::vector<std::shared_ptr<WriteBuffer>> writeBuffers_;
        std::string defaultName_;
        std::unique_ptr<StatsReporter> statsReporter_;
        std::shared_ptr<PoolCollectorLease> poolCollectorLease_;
    };

}
}

#endif
