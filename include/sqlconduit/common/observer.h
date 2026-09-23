#ifndef SQLCONDUIT_COMMON_OBSERVER_H
#define SQLCONDUIT_COMMON_OBSERVER_H

#include "sqlconduit/common/connection_pool_stats.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/config/datasource_config.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sqlconduit::core::detail {
    struct RuntimeServices;
}

namespace sqlconduit::common {
    enum class OperationType {
        Query = 0,
        Execute = 1,
        Begin = 2,
        Commit = 3,
        Rollback = 4,
        Cancel = 5,
        Stream = 6,
        Batch = 7,
        Savepoint = 8,
        Select = 9,
        Routine = 10
    };

    struct OperationEvent {
        std::string dataSource;
        OperationType type = OperationType::Query;
        std::chrono::microseconds duration{0};
        Status status;
        std::uint64_t rowCount = 0;
        std::string sqlTemplate;
        std::string renderedSql;
        std::uint64_t sqlFingerprint = 0;
        std::string traceId;
        std::string spanId;
        bool slow = false;
        bool shadow = false;
        bool transformed = false;
    };

    struct SlowSqlStats {
        std::uint64_t fingerprint = 0;
        std::string dataSource;
        OperationType type = OperationType::Query;
        std::string sqlTemplate;
        std::uint64_t count = 0;
        std::uint64_t errorCount = 0;
        std::uint64_t timeoutCount = 0;
        std::chrono::microseconds totalDuration{0};
        std::chrono::microseconds minDuration{0};
        std::chrono::microseconds maxDuration{0};
        std::chrono::system_clock::time_point firstSeen;
        std::chrono::system_clock::time_point lastSeen;
        std::vector<int> histogramBucketsMs;
        std::vector<std::uint64_t> histogram;
    };

    struct SlowSqlRecord {
        std::chrono::system_clock::time_point timestamp;
        std::string dataSource;
        OperationType type = OperationType::Query;
        std::string sqlTemplate;
        std::string renderedSql;
        std::uint64_t fingerprint = 0;
        std::chrono::microseconds duration{0};
        ErrorCode errorCode = ErrorCode::Ok;
        std::string sqlState;
        std::string traceId;
        std::string spanId;
    };

    using OperationObserver = std::function<void(const OperationEvent &)>;
    using SqlRenderer = std::function<Status(const SqlRenderOptions &, std::string &)>;

    struct PoolMetricsEvent {
        std::chrono::system_clock::time_point timestamp;
        std::vector<NamedPoolStats> pools;
    };

    using PoolMetricsObserver = std::function<void(const PoolMetricsEvent &)>;
    using PoolMetricsCollector = std::function<std::vector<NamedPoolStats>()>;

    namespace detail {
        class ObservabilityState {
        public:
            ObservabilityState();

            ~ObservabilityState();

            ObservabilityState(ObservabilityState &&) noexcept;

            ObservabilityState &operator=(ObservabilityState &&) noexcept;

            ObservabilityState(const ObservabilityState &) = delete;

            ObservabilityState &operator=(const ObservabilityState &) = delete;

            void setObserver(OperationObserver observer);

            void emit(const OperationEvent &event) noexcept;

            void setPoolMetricsObserver(PoolMetricsObserver observer);

            void setPoolMetricsCollector(PoolMetricsCollector collector,
                                         const void *owner = nullptr);

            void clearPoolMetricsCollector(const void *owner);

            PoolMetricsEvent samplePoolMetrics() noexcept;

            void configure(const config::ObservabilityConfig &config);

            void emitSql(OperationEvent event, const std::string &sql,
                         const SqlRenderer &renderer = {},
                         const common::ResultSet *result = nullptr) noexcept;

            std::vector<SlowSqlStats> slowSqlStats(
                std::size_t limit = 100, const std::string &dataSource = {});

            std::vector<SlowSqlRecord> recentSlowSql(
                std::size_t limit = 100, const std::string &dataSource = {});

            void clearSlowSqlStats();

        private:
            explicit ObservabilityState(bool useProcessDefault);

            friend struct sqlconduit::core::detail::RuntimeServices;

            struct Impl;
            std::unique_ptr<Impl> impl_;
        };
    }

    class Observability {
    public:
        Observability() = delete;

        static void setObserver(OperationObserver observer);

        static void emit(const OperationEvent &event) noexcept;

        static void setPoolMetricsObserver(PoolMetricsObserver observer);

        static void setPoolMetricsCollector(PoolMetricsCollector collector,
                                            const void *owner = nullptr);

        static void clearPoolMetricsCollector(const void *owner);

        static PoolMetricsEvent samplePoolMetrics() noexcept;

        static void configure(const config::ObservabilityConfig &config);

        static void emitSql(OperationEvent event, const std::string &sql,
                            const SqlRenderer &renderer = {},
                            const common::ResultSet *result = nullptr) noexcept;

        static std::vector<SlowSqlStats> slowSqlStats(
            std::size_t limit = 100, const std::string &dataSource = {});

        static std::vector<SlowSqlRecord> recentSlowSql(
            std::size_t limit = 100, const std::string &dataSource = {});

        static void clearSlowSqlStats();
    };
}

#endif
