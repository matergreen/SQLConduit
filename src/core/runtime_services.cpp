#include "sqlconduit/core/runtime_services.h"

namespace sqlconduit::core::detail {
    RuntimeServices::RuntimeServices(const bool useProcessDefaultObservability)
        : observability(useProcessDefaultObservability) {
    }

    std::shared_ptr<RuntimeServices> defaultRuntimeServices() {
        static auto services = std::make_shared<RuntimeServices>(true);
        return services;
    }
}
