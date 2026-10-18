#ifndef SQLCONDUIT_INTERNAL_OPERATION_STATE_H
#define SQLCONDUIT_INTERNAL_OPERATION_STATE_H
#include "sqlconduit/common/operation_options.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/async/async_types.h"
#include <functional>
#include <mutex>

namespace sqlconduit::core { class IDatabaseConnection; }
namespace sqlconduit::common::detail {
    class OperationState : public std::enable_shared_from_this<OperationState> {
    public:
        static std::shared_ptr<OperationState> create(const OperationOptions &options,
                                                     const std::shared_ptr<OperationState> &parent);
        ~OperationState();
        [[nodiscard]] Status status() const;
        [[nodiscard]] std::chrono::milliseconds remaining(std::chrono::milliseconds other) const;
        [[nodiscard]] std::chrono::steady_clock::time_point deadline() const;
        [[nodiscard]] bool native() const;
        [[nodiscard]] bool fallbackAllowed() const;
        std::shared_ptr<void> subscribe(std::function<void()> callback);
        std::shared_ptr<void> bind(core::IDatabaseConnection *connection, bool write, bool native);
        void finish();
        bool stop(ErrorCode code);
    private:
        struct Impl;
        explicit OperationState(const OperationOptions &options);
        std::shared_ptr<Impl> impl_;
    };
    inline std::shared_ptr<OperationState> currentOperation() { return ContextScope::current().operation; }
    inline Status operationStatus() {
        const auto op = currentOperation();
        return op ? op->status() : Status{};
    }

    template<class Result>
    std::function<void(Result)> controlledCompletion(std::function<void(Result)> completion) {
        auto operation = currentOperation();
        if (!operation) return completion;
        struct Pending {
            std::mutex mutex;
            bool done = false;
            std::shared_ptr<OperationState> operation;
            std::function<void(Result)> completion;
            std::shared_ptr<void> lease;
            void deliver(Result result) {
                std::function<void(Result)> target;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (done) return;
                    done = true;
                    target = std::move(completion);
                }
                if (auto stop = operation->status(); !stop.ok()) {
                    result.status = std::move(stop);
                    result.mode = operation->native() ? async::ExecutionMode::Native : async::ExecutionMode::CompatibilityFallback;
                }
                target(std::move(result));
            }
        };
        auto pending = std::make_shared<Pending>();
        pending->operation = operation;
        pending->completion = std::move(completion);
        std::weak_ptr<Pending> weak = pending;
        pending->lease = operation->subscribe([weak] {
            if (auto state = weak.lock()) { Result result; state->deliver(std::move(result)); }
        });
        return [pending](Result result) { pending->deliver(std::move(result)); };
    }
}
#endif
