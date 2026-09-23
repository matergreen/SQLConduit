#include "sqlconduit/core/write_buffer.h"

#include "sqlconduit/common/logger.h"

#include <chrono>
#include <string>
#include <utility>

namespace sqlconduit::core {
    bool WriteBuffer::enqueue(std::function<common::Status()> task) {
        if (!enabled_ || !task) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_ || !running_) return false;
        if (static_cast<int>(q_.size()) >= cfg_.max_queue) return false;
        q_.emplace(std::move(task), std::chrono::steady_clock::now());
        cv_.notify_one();
        return true;
    }

    std::size_t WriteBuffer::pending() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    void WriteBuffer::start() {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (running_) return;
        running_ = true;
        stop_ = false;
        worker_ = std::thread(&WriteBuffer::run, this);
    }

    void WriteBuffer::stop() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!running_) return;
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(mtx_);
        running_ = false;
    }

    void WriteBuffer::run() {
        using clock = std::chrono::steady_clock;
        const bool ttlEnabled = cfg_.ttl_ms > 0;
        const auto ttl = std::chrono::milliseconds(cfg_.ttl_ms);

        while (true) {
            std::queue<Item> batch;
            bool finalPass = false;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                if (!stop_) {
                    cv_.wait_for(lk, std::chrono::milliseconds(cfg_.flush_interval_ms),
                                 [this] { return stop_; });
                }
                finalPass = stop_;
                batch.swap(q_);
            }

            std::queue<Item> retry;
            while (!batch.empty()) {
                Item item = std::move(batch.front());
                batch.pop();

                if (ttlEnabled && clock::now() - item.second > ttl) {
                    SQLCONDUIT_LOG_WARN("write buffer: dropping expired buffered write");
                    continue;
                }

                const auto st = item.first();
                if (st.ok()) {
                    SQLCONDUIT_LOG_INFO("write buffer: flushed buffered write");
                    continue;
                }
                if (!finalPass && (st.retryable || st.connectionBroken)) {
                    retry.push(std::move(item));
                } else {
                    SQLCONDUIT_LOG_WARN("write buffer: buffered write failed, dropped: " + st.message);
                }
            }

            if (!retry.empty()) {
                std::size_t dropped = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    while (!q_.empty()) {
                        retry.push(std::move(q_.front()));
                        q_.pop();
                    }
                    while (!retry.empty()) {
                        if (static_cast<int>(q_.size()) >= cfg_.max_queue) {
                            ++dropped;
                        } else {
                            q_.push(std::move(retry.front()));
                        }
                        retry.pop();
                    }
                }
                if (dropped > 0) {
                    SQLCONDUIT_LOG_WARN("write buffer: queue full, dropped "
                        + std::to_string(dropped) + " buffered write(s)");
                }
            }

            if (finalPass) {
                std::size_t abandoned = 0;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    abandoned = q_.size();
                    std::queue<Item>().swap(q_);
                }
                if (abandoned > 0) {
                    SQLCONDUIT_LOG_WARN("write buffer: abandoning " + std::to_string(abandoned)
                        + " buffered write(s) on shutdown");
                }
                return;
            }
        }
    }
}
