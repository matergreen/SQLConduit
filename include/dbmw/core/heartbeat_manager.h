#ifndef DBMW_CORE_HEARTBEAT_MANAGER_H
#define DBMW_CORE_HEARTBEAT_MANAGER_H

#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace dbmw::core {
    class ConnectionPool;

    class HeartbeatManager {
    public:
        explicit HeartbeatManager(std::chrono::milliseconds interval);

        ~HeartbeatManager();

        HeartbeatManager(const HeartbeatManager &) = delete;

        HeartbeatManager &operator=(const HeartbeatManager &) = delete;

        void addPool(const std::shared_ptr<ConnectionPool>& pool);

        void start();

        void stop();

        bool running() const { return running_.load(); }

    private:
        void loop();

        void sweepExpiredPools();

        std::chrono::milliseconds interval_;
        std::vector<std::weak_ptr<ConnectionPool> > pools_;
        std::mutex poolsMtx_;
        std::thread thread_;
        std::mutex waitMtx_;
        std::condition_variable waitCv_;
        std::atomic<bool> running_{false};
        std::atomic<bool> stopFlag_{false};
    };
}

#endif
