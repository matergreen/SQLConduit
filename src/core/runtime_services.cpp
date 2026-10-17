#include "sqlconduit/core/runtime_services.h"

namespace sqlconduit::core::detail {
    void RuntimeServices::setPhaseObserver(common::ExecutionPhaseObserver observer) {
        std::lock_guard<std::mutex> lock(phaseObserverMutex);
        const bool enabled = static_cast<bool>(observer);
        auto next = observer ? std::make_shared<common::ExecutionPhaseObserver>(std::move(observer)) : nullptr;
        std::atomic_store(&phaseObserver, std::move(next));
        phasesEnabled.store(enabled, std::memory_order_release);
    }

    void RuntimeServices::phase(common::ExecutionPhase phase, const common::SqlContext &context,
                                std::chrono::microseconds duration, common::ErrorCode error,
                                common::FallbackReason reason, int attempt) const noexcept {
        if (!phasesEnabled.load(std::memory_order_acquire)) return;
        try {
            const auto observer = std::atomic_load(&phaseObserver);
            if (observer) (*observer)(common::ExecutionPhaseEvent{phase, reason, duration, error, attempt, context});
        } catch (...) {
            // A diagnostic sink must not alter operation completion or ownership.
        }
    }

    RuntimeServices::RuntimeServices(const bool useProcessDefaultObservability)
        : observability(useProcessDefaultObservability) {
    }

    std::shared_ptr<RuntimeServices> defaultRuntimeServices() {
        static auto services = std::make_shared<RuntimeServices>(true);
        return services;
    }
}
