#ifndef SQLCONDUIT_ASYNC_EXECUTOR_H
#define SQLCONDUIT_ASYNC_EXECUTOR_H

#include "sqlconduit/async/async_types.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace sqlconduit::async {
    class IExecutor {
    public:
        virtual ~IExecutor() = default;

        using Task = std::function<void()>;

        virtual bool tryPost(Task task) = 0;

        virtual void postAfter(Task task, std::chrono::milliseconds delay) = 0;

        virtual void shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(5000)) = 0;

        [[nodiscard]] virtual ExecutorStats stats() const = 0;
    };

    std::shared_ptr<IExecutor> makeThreadPoolExecutor(int threads, std::size_t queueSize);
}

#endif
