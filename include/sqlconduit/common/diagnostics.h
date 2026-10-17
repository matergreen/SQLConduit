#ifndef SQLCONDUIT_COMMON_DIAGNOSTICS_H
#define SQLCONDUIT_COMMON_DIAGNOSTICS_H

#include "sqlconduit/async/async_types.h"
#include "sqlconduit/common/connection_pool_stats.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/common/context.h"
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace sqlconduit::common {
    enum class ExecutionPhase {
        Queue = 0, PoolWait = 1, Native = 2, Fallback = 3, Retry = 4,
        ReplicaFallback = 5, Rejected = 6, Cancel = 7, CacheHit = 8
    };

    enum class FallbackReason {
        None = 0, DriverUnsupported = 1, DriverDeclinedStatement = 2,
        CompositeOperation = 3, CacheHit = 4
    };

    struct ExecutionPhaseEvent {
        ExecutionPhase phase = ExecutionPhase::Queue;
        FallbackReason fallbackReason = FallbackReason::None;
        std::chrono::microseconds duration{0};
        ErrorCode errorCode = ErrorCode::Ok;
        int attempt = 0;
        SqlContext context;
    };

    using ExecutionPhaseObserver = std::function<void(const ExecutionPhaseEvent &)>;

    struct ConnectionCapabilities {
        // Empty versions mean the driver did not supply metadata.
        std::string clientVersion;
        std::string serverVersion;
        bool observed = false;
        bool nativeAsync = false;
        bool parameters = false;
        bool prepared = false;
        bool multipleResults = false;
        std::string asyncConstraints;
    };

    struct DataSourceDiagnostics {
        std::string name;
        std::string driver;
        bool group = false;
        bool readOnly = false;
        std::string primary;
        std::vector<std::string> replicas;
        std::vector<std::string> failoverPrimaries;
        std::string shadow;
        int maxAttempts = 1;
        std::string circuitState = "closed";
        int consecutiveFailures = 0;
        ErrorCode lastError = ErrorCode::Ok;
        ErrorCode lastCancellationError = ErrorCode::Ok;
        std::size_t bufferedWrites = 0;
        ConnectionPoolStats pool;
        ConnectionCapabilities capabilities;
    };

    struct CapabilityReport {
        Status status;
        // Groups report every participating leaf, not a guessed aggregate mode.
        std::vector<DataSourceDiagnostics> sources;
    };

    struct DiagnosticsReport {
        bool running = false;
        std::string version;
        std::string configSource;
        async::ExecutorStats executor;
        std::vector<DataSourceDiagnostics> sources;
        std::size_t cacheEntries = 0;
        std::uint64_t cacheHits = 0;
        std::uint64_t cacheMisses = 0;
        bool sqlAuditEnabled = false;
        bool rateLimitEnabled = false;
    };
}
#endif
