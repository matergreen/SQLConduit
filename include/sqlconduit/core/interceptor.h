#ifndef SQLCONDUIT_CORE_INTERCEPTOR_H
#define SQLCONDUIT_CORE_INTERCEPTOR_H

#include "sqlconduit/common/context.h"
#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/types.h"

#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sqlconduit::core {
    struct ExecutionView {
        const std::string &dataSource;
        const std::string &sql;
        common::OperationType type;
        const common::Params *params;
        common::ResultSet *result;
        std::int64_t affected = 0;
        std::chrono::microseconds duration{0};
        common::Status status;
        bool cached = false;
        std::size_t depth = 0;
        common::SqlContext &ctx;
    };

    class ISqlInterceptor {
    public:
        virtual ~ISqlInterceptor() = default;

        virtual void onRoute(const std::string &dataSource, const std::string &sql,
                             common::OperationType type, common::SqlContext &ctx) = 0;

        virtual common::Status beforeExecution(const ExecutionView &view) = 0;

        virtual void afterExecution(const ExecutionView &view) = 0;

        virtual void onRow(const ExecutionView &, common::Row &) {
        }

        virtual void onCompletion(const ExecutionView &view) = 0;
    };

    namespace detail {
        class InterceptorRegistryState {
        public:
            using Snapshot = std::vector<std::shared_ptr<ISqlInterceptor> >;

            void add(std::shared_ptr<ISqlInterceptor> interceptor);

            void clear();

            [[nodiscard]] Snapshot snapshot() const;

            [[nodiscard]] bool enabled() const noexcept;

            void setEnabled(bool value) noexcept;

        private:
            std::atomic<bool> enabled_{false};
            mutable std::mutex mutex_;
            std::vector<std::shared_ptr<ISqlInterceptor> > interceptors_;
        };
    }

    class InterceptorRegistry {
    public:
        InterceptorRegistry() = delete;

        static void add(std::shared_ptr<ISqlInterceptor> interceptor);

        static void clear();

        using Snapshot = std::vector<std::shared_ptr<ISqlInterceptor> >;

        static Snapshot snapshot();

        static bool enabled() noexcept;

        static void setEnabled(bool v) noexcept;
    };

    namespace detail {
        void runOnRoute(InterceptorRegistryState &registry,
                        const std::string &dataSource, const std::string &sql,
                        common::OperationType type, common::SqlContext &ctx);

        void runOnRoute(const std::string &dataSource, const std::string &sql,
                        common::OperationType type, common::SqlContext &ctx);

        common::Status runBeforeExecution(InterceptorRegistryState &registry,
                                          const ExecutionView &view);

        common::Status runBeforeExecution(const ExecutionView &view);

        void runAfterExecution(InterceptorRegistryState &registry,
                               const ExecutionView &view);

        void runAfterExecution(const ExecutionView &view);

        void runOnRow(InterceptorRegistryState &registry,
                      const ExecutionView &view, common::Row &row);

        void runOnRow(const ExecutionView &view, common::Row &row);

        class InterceptorGuard {
        public:
            explicit InterceptorGuard(const ExecutionView &view);

            InterceptorGuard(InterceptorRegistryState &registry,
                             const ExecutionView &view);

            ~InterceptorGuard() noexcept;

            InterceptorGuard(const InterceptorGuard &) = delete;

            InterceptorGuard &operator=(const InterceptorGuard &) = delete;

            InterceptorGuard(InterceptorGuard &&) = delete;

            InterceptorGuard &operator=(InterceptorGuard &&) = delete;

            [[nodiscard]] bool active() const noexcept { return active_; }

        private:
            const ExecutionView &view_;
            InterceptorRegistryState *registry_ = nullptr;
            bool active_ = false;
        };

        InterceptorGuard makeInterceptorGuard(InterceptorRegistryState &registry,
                                              const ExecutionView &view);

        InterceptorGuard makeInterceptorGuard(const ExecutionView &view);

        std::size_t currentInterceptorDepth() noexcept;
    }
}

#endif
