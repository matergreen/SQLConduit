#include "sqlconduit/async/executor.h"
#include "sqlconduit/common/logger.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

namespace sqlconduit::async {
    namespace {
        class ThreadPoolExecutor final : public IExecutor {
        public:
            explicit ThreadPoolExecutor(const int threads, const std::size_t queueSize)
                : threadCount_(threads > 0
                                   ? static_cast<std::size_t>(threads)
                                   : std::thread::hardware_concurrency()),
                  queueLimit_(queueSize > 0 ? queueSize : 1) {
                if (threadCount_ == 0) threadCount_ = 1;
            }

            ~ThreadPoolExecutor() override {
                if (!stopping_.load(std::memory_order_acquire)) {
                    shutdown(std::chrono::milliseconds(0));
                }
            }

            bool tryPost(Task task) override {
                if (!task) return false;
                std::unique_lock<std::mutex> lk(mtx_);
                ensureStartedLocked();
                if (stopping_.load(std::memory_order_relaxed)) return false;
                if (queue_.size() >= queueLimit_) {
                    ++rejected_;
                    return false;
                }
                queue_.push_back(std::move(task));
                ++submitted_;
                cvWork_.notify_one();
                return true;
            }

            void postAfter(Task task, std::chrono::milliseconds delay) override {
                if (!task) return;
                std::unique_lock<std::mutex> lk(mtx_);
                ensureStartedLocked();
                if (stopping_.load(std::memory_order_relaxed)) return;
                delayed_.emplace(DelayedTask{
                    std::chrono::steady_clock::now()
                    + std::max<std::chrono::milliseconds>(delay, std::chrono::milliseconds(0)),
                    seq_++, std::move(task)
                });
                cvTimer_.notify_one();
            }

            void shutdown(const std::chrono::milliseconds grace) override {
                std::vector<std::thread> workers;
                std::thread timer;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (stopping_.exchange(true, std::memory_order_acq_rel)) return;
                    delayed_ = {};
                    cvWork_.notify_all();
                    cvTimer_.notify_all();
                    workers = std::move(workers_);
                    timer = std::move(timerThread_);
                }
                if (timer.joinable()) timer.join();
                for (auto &w: workers) {
                    if (w.joinable()) w.join();
                }
                (void) grace;
            }

            [[nodiscard]] ExecutorStats stats() const override {
                std::lock_guard<std::mutex> lk(mtx_);
                ExecutorStats out;
                out.threads = threadsStarted_ ? threadCount_ : 0;
                out.queueDepth = queue_.size();
                out.active = active_;
                out.submitted = submitted_;
                out.completed = completed_;
                out.rejected = rejected_;
                out.delayedPending = delayed_.size();
                return out;
            }

        private:
            struct DelayedTask {
                std::chrono::steady_clock::time_point deadline;
                std::uint64_t seq;
                Task task;

                bool operator<(const DelayedTask &other) const {
                    if (deadline != other.deadline) return deadline > other.deadline;
                    return seq > other.seq;
                }
            };

            void ensureStartedLocked() {
                if (threadsStarted_) return;
                threadsStarted_ = true;
                for (std::size_t i = 0; i < threadCount_; ++i) {
                    workers_.emplace_back([this] { workerLoop(); });
                }
                timerThread_ = std::thread([this] { timerLoop(); });
            }

            void workerLoop() {
                for (;;) {
                    Task task;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cvWork_.wait(lk, [this] {
                            return stopping_.load(std::memory_order_relaxed) || !queue_.empty();
                        });
                        if (queue_.empty()) {
                            if (stopping_.load(std::memory_order_relaxed)) return;
                            continue;
                        }
                        task = std::move(queue_.front());
                        queue_.pop_front();
                        ++active_;
                    }
                    runGuarded(task);
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        --active_;
                        ++completed_;
                    }
                }
            }

            void timerLoop() {
                for (;;) {
                    Task task;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        if (delayed_.empty()) {
                            cvTimer_.wait(lk, [this] {
                                return stopping_.load(std::memory_order_relaxed)
                                       || !delayed_.empty();
                            });
                            if (delayed_.empty()
                                && stopping_.load(std::memory_order_relaxed)) {
                                return;
                            }
                            continue;
                        }
                        const auto next = delayed_.top().deadline;
                        cvTimer_.wait_until(lk, next, [this, &next] {
                            return stopping_.load(std::memory_order_relaxed)
                                   || delayed_.empty() || delayed_.top().deadline != next;
                        });
                        if (stopping_.load(std::memory_order_relaxed) && delayed_.empty()) {
                            return;
                        }
                        if (delayed_.empty() || delayed_.top().deadline
                            > std::chrono::steady_clock::now()) {
                            continue;
                        }
                        task = std::move(const_cast<DelayedTask &>(delayed_.top()).task);
                        delayed_.pop();
                    }
                    runGuarded(task);
                }
            }

            static void runGuarded(const Task &task) {
                if (!task) return;
                try {
                    task();
                } catch (const std::exception &e) {
                    SQLCONDUIT_LOG_ERROR(std::string("async executor task threw: ") + e.what());
                } catch (...) {
                    SQLCONDUIT_LOG_ERROR("async executor task threw unknown exception");
                }
            }

            mutable std::mutex mtx_;
            std::condition_variable cvWork_;
            std::condition_variable cvTimer_;
            std::deque<Task> queue_;
            std::priority_queue<DelayedTask> delayed_;
            std::vector<std::thread> workers_;
            std::thread timerThread_;
            std::size_t threadCount_;
            std::size_t queueLimit_;
            std::uint64_t seq_ = 0;
            std::size_t active_ = 0;
            std::uint64_t submitted_ = 0;
            std::uint64_t completed_ = 0;
            std::uint64_t rejected_ = 0;
            bool threadsStarted_ = false;
            std::atomic<bool> stopping_{false};
        };
    }

    std::shared_ptr<IExecutor> makeThreadPoolExecutor(const int threads,
                                                      const std::size_t queueSize) {
        return std::make_shared<ThreadPoolExecutor>(threads, queueSize);
    }
}
