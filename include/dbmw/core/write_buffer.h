#ifndef DBMW_CORE_WRITE_BUFFER_H
#define DBMW_CORE_WRITE_BUFFER_H

#include "dbmw/common/types.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace dbmw::core {
    class WriteBuffer {
    public:
        struct Config {
            bool enabled = false;
            int max_queue = 1000;
            int ttl_ms = 30000;
            int flush_interval_ms = 1000;
        };

        explicit WriteBuffer(Config cfg) : cfg_(cfg), enabled_(cfg.enabled) {
            if (cfg_.max_queue < 1) cfg_.max_queue = 1;
            if (cfg_.flush_interval_ms < 10) cfg_.flush_interval_ms = 10;
        }

        ~WriteBuffer() { stop(); }

        bool enqueue(std::function<common::Status()> task);

        void start();
        void stop();
        [[nodiscard]] bool enabled() const { return enabled_; }

        [[nodiscard]] std::size_t pending() const;

    private:
        using Task = std::function<common::Status()>;
        using Item = std::pair<Task, std::chrono::steady_clock::time_point>;

        void run();

        Config cfg_;
        bool enabled_;
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::queue<Item> q_;
        std::thread worker_;
        bool stop_ = false;
        bool running_ = false;
    };
}

#endif
