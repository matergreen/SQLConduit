#ifndef SQLCONDUIT_CORE_RUNTIME_SERVICES_H
#define SQLCONDUIT_CORE_RUNTIME_SERVICES_H

#include "sqlconduit/core/query_cache.h"
#include "sqlconduit/core/sql_auditor.h"
#include "sqlconduit/core/interceptor.h"
#include "sqlconduit/common/diagnostics.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace sqlconduit::core::detail {
    struct RuntimeServices {
        explicit RuntimeServices(bool useProcessDefaultObservability = false);

        void setPhaseObserver(common::ExecutionPhaseObserver observer);
        void phase(common::ExecutionPhase phase, const common::SqlContext &context,
                   std::chrono::microseconds duration = {},
                   common::ErrorCode error = common::ErrorCode::Ok,
                   common::FallbackReason reason = common::FallbackReason::None,
                   int attempt = 0) const noexcept;

        QueryCacheState queryCache;
        SqlAuditorState sqlAuditor;
        InterceptorRegistryState interceptors;
        common::detail::ObservabilityState observability;
        std::atomic<bool> preparedCacheEnabled{true};
        std::atomic<int> preparedCacheMaxPerConnection{0};
        std::atomic<bool> phasesEnabled{false};
        std::shared_ptr<common::ExecutionPhaseObserver> phaseObserver;
        std::mutex phaseObserverMutex;
    };

    // Used by directly constructed low-level DataSource objects.
    std::shared_ptr<RuntimeServices> defaultRuntimeServices();
}

#endif
