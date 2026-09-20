#include "dbmw/core/interceptor.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace dbmw::core {
    namespace {
        std::atomic<bool> &enabledFlag() {
            static std::atomic<bool> v{false};
            return v;
        }

        std::mutex &registryMtx() {
            static std::mutex m;
            return m;
        }

        std::vector<std::shared_ptr<ISqlInterceptor> > &registry() {
            static std::vector<std::shared_ptr<ISqlInterceptor> > r;
            return r;
        }

        thread_local std::size_t g_executionDepth = 0;
        thread_local std::size_t g_callbackDepth = 0;

        class CallbackGuard {
        public:
            CallbackGuard() noexcept { ++g_callbackDepth; }
            ~CallbackGuard() noexcept { --g_callbackDepth; }
        };

        template<typename Fn>
        void safeCall(Fn &&fn) noexcept {
            try { std::forward<Fn>(fn)(); } catch (...) {
            }
        }
    }

    void InterceptorRegistry::add(std::shared_ptr<ISqlInterceptor> interceptor) {
        if (!interceptor) return;
        std::lock_guard<std::mutex> lk(registryMtx());
        registry().push_back(std::move(interceptor));
    }

    void InterceptorRegistry::clear() {
        std::lock_guard<std::mutex> lk(registryMtx());
        registry().clear();
    }

    InterceptorRegistry::Snapshot InterceptorRegistry::snapshot() {
        std::lock_guard<std::mutex> lk(registryMtx());
        return registry();
    }

    bool InterceptorRegistry::enabled() noexcept {
        return enabledFlag().load(std::memory_order_acquire);
    }

    void InterceptorRegistry::setEnabled(bool v) noexcept {
        enabledFlag().store(v, std::memory_order_release);
    }

    detail::InterceptorGuard::InterceptorGuard(const ExecutionView &view)
        : view_(view),
          active_(InterceptorRegistry::enabled() && g_executionDepth == 0 &&
                  g_callbackDepth == 0) {
        if (active_) ++g_executionDepth;
    }

    detail::InterceptorGuard::~InterceptorGuard() noexcept {
        if (active_) {
            CallbackGuard callbackGuard;
            try {
                for (auto &it: InterceptorRegistry::snapshot()) {
                    safeCall([&] { it->onCompletion(view_); });
                }
            } catch (...) {
            }
        }
        if (active_) --g_executionDepth;
    }

    namespace detail {
        void runOnRoute(const std::string &dataSource, const std::string &sql,
                        common::OperationType type, common::SqlContext &ctx) {
            if (!InterceptorRegistry::enabled() || g_executionDepth > 0 ||
                g_callbackDepth > 0)
                return;
            CallbackGuard callbackGuard;
            for (auto &it: InterceptorRegistry::snapshot()) {
                safeCall([&] { it->onRoute(dataSource, sql, type, ctx); });
            }
        }

        common::Status runBeforeExecution(const ExecutionView &view) {
            if (!InterceptorRegistry::enabled() || g_executionDepth > 1 ||
                g_callbackDepth > 0)
                return common::Status::OK();
            CallbackGuard callbackGuard;
            common::Status st;
            for (auto &it: InterceptorRegistry::snapshot()) {
                safeCall([&] { st = it->beforeExecution(view); });
                if (!st.ok()) return st;
            }
            return st;
        }

        void runAfterExecution(const ExecutionView &view) {
            if (!InterceptorRegistry::enabled() || g_executionDepth > 1 ||
                g_callbackDepth > 0)
                return;
            CallbackGuard callbackGuard;
            for (auto &it: InterceptorRegistry::snapshot()) {
                safeCall([&] { it->afterExecution(view); });
            }
        }

        void runOnRow(const ExecutionView &view, common::Row &row) {
            if (!InterceptorRegistry::enabled() || g_executionDepth > 1 ||
                g_callbackDepth > 0)
                return;
            CallbackGuard callbackGuard;
            for (auto &it: InterceptorRegistry::snapshot()) {
                safeCall([&] { it->onRow(view, row); });
            }
        }

        detail::InterceptorGuard makeInterceptorGuard(const ExecutionView &view) {
            return detail::InterceptorGuard(view);
        }

        std::size_t currentInterceptorDepth() noexcept {
            return g_executionDepth + g_callbackDepth;
        }
    }
}
