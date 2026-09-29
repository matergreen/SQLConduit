#ifndef SQLCONDUIT_DRIVER_POLLING_ASYNC_REACTOR_H
#define SQLCONDUIT_DRIVER_POLLING_ASYNC_REACTOR_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sqlconduit::driver::detail {
    class PollingAsyncReactor final {
    public:
        using Step = std::function<bool()>;

        explicit PollingAsyncReactor(const std::size_t workers = 2) {
            const auto count = workers == 0 ? std::size_t{1} : workers;
            workers_.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                workers_.emplace_back([this] { run(); });
        }

        ~PollingAsyncReactor() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto &worker: workers_)
                if (worker.joinable()) worker.join();
        }

        PollingAsyncReactor(const PollingAsyncReactor &) = delete;
        PollingAsyncReactor &operator=(const PollingAsyncReactor &) = delete;

        bool enqueue(Step step) {
            if (!step) return false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return false;
                tasks_.push_back(std::move(step));
            }
            ready_.notify_one();
            return true;
        }

    private:
        void run() {
            for (;;) {
                Step step;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                    if (stopping_ && tasks_.empty()) return;
                    step = std::move(tasks_.front());
                    tasks_.pop_front();
                }

                bool complete = true;
                try {
                    complete = step();
                } catch (...) {
                    complete = true;
                }

                if (!complete) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!stopping_) tasks_.push_back(std::move(step));
                    }
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait_for(lock, std::chrono::milliseconds(1),
                                    [this] { return stopping_; });
                }
            }
        }

        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<Step> tasks_;
        bool stopping_ = false;
        std::vector<std::thread> workers_;
    };
}

#endif
