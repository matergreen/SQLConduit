#ifndef SQLCONDUIT_CORE_RUNTIME_SERVICES_H
#define SQLCONDUIT_CORE_RUNTIME_SERVICES_H

#include "sqlconduit/core/query_cache.h"
#include "sqlconduit/core/sql_auditor.h"
#include "sqlconduit/core/interceptor.h"

#include <atomic>
#include <memory>

namespace sqlconduit::core::detail {
    struct RuntimeServices {
        explicit RuntimeServices(bool useProcessDefaultObservability = false);

        QueryCacheState queryCache;
        SqlAuditorState sqlAuditor;
        InterceptorRegistryState interceptors;
        common::detail::ObservabilityState observability;
        std::atomic<bool> preparedCacheEnabled{true};
        std::atomic<int> preparedCacheMaxPerConnection{0};
    };

    // Used by directly constructed low-level DataSource objects.
    std::shared_ptr<RuntimeServices> defaultRuntimeServices();
}

#endif
