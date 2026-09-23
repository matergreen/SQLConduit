#include "sqlconduit/core/interceptor.h"

#include "sqlconduit/core/runtime_services.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace sqlconduit::core {
    using detail::InterceptorRegistryState;

    namespace {
        thread_local std::vector<const InterceptorRegistryState *> g_executionStack;
        thread_local std::vector<const InterceptorRegistryState *> g_callbackStack;

        std::size_t depth(const std::vector<const InterceptorRegistryState *> &stack,
                          const InterceptorRegistryState &registry) {
            return static_cast<std::size_t>(
                std::count(stack.begin(), stack.end(), &registry));
        }

        class CallbackGuard {
        public:
            explicit CallbackGuard(const InterceptorRegistryState &registry)
                : registry_(&registry) {
                g_callbackStack.push_back(registry_);
            }

            ~CallbackGuard() {
                if (!g_callbackStack.empty()) g_callbackStack.pop_back();
            }

        private:
            const InterceptorRegistryState *registry_;
        };

        template<typename Fn>
        void safeCall(Fn &&fn) noexcept {
            try { std::forward<Fn>(fn)(); } catch (...) {
            }
        }

        InterceptorRegistryState &defaultRegistry() {
            return detail::defaultRuntimeServices()->interceptors;
        }
    }

    void detail::InterceptorRegistryState::add(std::shared_ptr<ISqlInterceptor> interceptor) {
        if (!interceptor) return;
        std::lock_guard<std::mutex> lock(mutex_);
        interceptors_.push_back(std::move(interceptor));
    }

    void detail::InterceptorRegistryState::clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        interceptors_.clear();
    }

    detail::InterceptorRegistryState::Snapshot
    detail::InterceptorRegistryState::snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return interceptors_;
    }

    bool detail::InterceptorRegistryState::enabled() const noexcept {
        return enabled_.load(std::memory_order_acquire);
    }

    void detail::InterceptorRegistryState::setEnabled(const bool value) noexcept {
        enabled_.store(value, std::memory_order_release);
    }

    void InterceptorRegistry::add(std::shared_ptr<ISqlInterceptor> interceptor) {
        defaultRegistry().add(std::move(interceptor));
    }

    void InterceptorRegistry::clear() {
        defaultRegistry().clear();
    }

    InterceptorRegistry::Snapshot InterceptorRegistry::snapshot() {
        return defaultRegistry().snapshot();
    }

    bool InterceptorRegistry::enabled() noexcept {
        return defaultRegistry().enabled();
    }

    void InterceptorRegistry::setEnabled(const bool value) noexcept {
        defaultRegistry().setEnabled(value);
    }

    detail::InterceptorGuard::InterceptorGuard(const ExecutionView &view)
        : InterceptorGuard(defaultRegistry(), view) {
    }

    detail::InterceptorGuard::InterceptorGuard(InterceptorRegistryState &registry,
                                               const ExecutionView &view)
        : view_(view), registry_(&registry),
          active_(registry.enabled() && depth(g_executionStack, registry) == 0 &&
                  depth(g_callbackStack, registry) == 0) {
        if (active_) g_executionStack.push_back(registry_);
    }

    detail::InterceptorGuard::~InterceptorGuard() noexcept {
        if (!active_) return;
        {
            CallbackGuard callbackGuard(*registry_);
            for (auto &interceptor: registry_->snapshot())
                safeCall([&] { interceptor->onCompletion(view_); });
        }
        if (!g_executionStack.empty()) g_executionStack.pop_back();
    }

    namespace detail {
        void runOnRoute(InterceptorRegistryState &registry,
                        const std::string &dataSource, const std::string &sql,
                        const common::OperationType type, common::SqlContext &ctx) {
            if (!registry.enabled() || depth(g_executionStack, registry) > 0 ||
                depth(g_callbackStack, registry) > 0)
                return;
            CallbackGuard callbackGuard(registry);
            for (auto &interceptor: registry.snapshot())
                safeCall([&] { interceptor->onRoute(dataSource, sql, type, ctx); });
        }

        void runOnRoute(const std::string &dataSource, const std::string &sql,
                        const common::OperationType type, common::SqlContext &ctx) {
            runOnRoute(defaultRegistry(), dataSource, sql, type, ctx);
        }

        common::Status runBeforeExecution(InterceptorRegistryState &registry,
                                          const ExecutionView &view) {
            if (!registry.enabled() || depth(g_executionStack, registry) > 1 ||
                depth(g_callbackStack, registry) > 0)
                return common::Status::OK();
            CallbackGuard callbackGuard(registry);
            common::Status status;
            for (auto &interceptor: registry.snapshot()) {
                safeCall([&] { status = interceptor->beforeExecution(view); });
                if (!status.ok()) return status;
            }
            return status;
        }

        common::Status runBeforeExecution(const ExecutionView &view) {
            return runBeforeExecution(defaultRegistry(), view);
        }

        void runAfterExecution(InterceptorRegistryState &registry,
                               const ExecutionView &view) {
            if (!registry.enabled() || depth(g_executionStack, registry) > 1 ||
                depth(g_callbackStack, registry) > 0)
                return;
            CallbackGuard callbackGuard(registry);
            for (auto &interceptor: registry.snapshot())
                safeCall([&] { interceptor->afterExecution(view); });
        }

        void runAfterExecution(const ExecutionView &view) {
            runAfterExecution(defaultRegistry(), view);
        }

        void runOnRow(InterceptorRegistryState &registry,
                      const ExecutionView &view, common::Row &row) {
            if (!registry.enabled() || depth(g_executionStack, registry) > 1 ||
                depth(g_callbackStack, registry) > 0)
                return;
            CallbackGuard callbackGuard(registry);
            for (auto &interceptor: registry.snapshot())
                safeCall([&] { interceptor->onRow(view, row); });
        }

        void runOnRow(const ExecutionView &view, common::Row &row) {
            runOnRow(defaultRegistry(), view, row);
        }

        InterceptorGuard makeInterceptorGuard(InterceptorRegistryState &registry,
                                              const ExecutionView &view) {
            return InterceptorGuard(registry, view);
        }

        InterceptorGuard makeInterceptorGuard(const ExecutionView &view) {
            return InterceptorGuard(defaultRegistry(), view);
        }

        std::size_t currentInterceptorDepth() noexcept {
            return g_executionStack.size() + g_callbackStack.size();
        }
    }
}
