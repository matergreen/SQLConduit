#ifndef DBMW_ASYNC_EXECUTOR_H
#define DBMW_ASYNC_EXECUTOR_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace dbmw::async {

    struct ExecutorStats {
        std::size_t threads = 0;
        std::size_t queueDepth = 0;
        std::size_t active = 0;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t delayedPending = 0;
    };

    class IExecutor {
    public:
        virtual ~IExecutor() = default;

        using Task = std::function<void()>;

        virtual bool tryPost(Task task) = 0;

        virtual void postAfter(Task task, std::chrono::milliseconds delay) = 0;

        virtual void shutdown(std::chrono::milliseconds grace
                              = std::chrono::milliseconds(5000)) = 0;

        [[nodiscard]] virtual ExecutorStats stats() const = 0;
    };

    std::shared_ptr<IExecutor> makeThreadPoolExecutor(int threads, std::size_t queueSize);

}

#endif
