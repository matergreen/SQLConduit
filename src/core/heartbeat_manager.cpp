#include "sqlconduit/core/heartbeat_manager.h"
#include "sqlconduit/core/connection_pool.h"
#include "sqlconduit/common/logger.h"
#include <thread>
#include <string>
#include <utility>

namespace sqlconduit::core
{
    HeartbeatManager::HeartbeatManager(std::chrono::milliseconds interval)
        : interval_(interval)
    {
    }

    HeartbeatManager::~HeartbeatManager()
    {
        stop();
    }

    void HeartbeatManager::addPool(const std::shared_ptr<ConnectionPool>& pool)
    {
        if (!pool) return;
        std::lock_guard<std::mutex> lk(poolsMtx_);
        pools_.push_back(pool);
    }

    void HeartbeatManager::start()
    {
        if (running_.exchange(true)) return;
        stopFlag_ = false;
        thread_ = std::thread([this]() { loop(); });
    }

    void HeartbeatManager::stop()
    {
        if (!running_.exchange(false)) return;
        stopFlag_ = true;
        waitCv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void HeartbeatManager::sweepExpiredPools()
    {
        std::lock_guard<std::mutex> lk(poolsMtx_);
        std::vector<std::weak_ptr<ConnectionPool>> alive;
        alive.reserve(pools_.size());
        for (auto& w : pools_)
        {
            if (!w.expired()) alive.push_back(std::move(w));
        }
        pools_.swap(alive);
    }

    void HeartbeatManager::loop()
    {
        SQLCONDUIT_LOG_INFO("heartbeat manager started, interval="
            + std::to_string(interval_.count()) + "ms");
        while (!stopFlag_)
        {
            {
                std::unique_lock<std::mutex> lock(waitMtx_);
                if (waitCv_.wait_for(lock, interval_, [this] { return stopFlag_.load(); }))
                    break;
            }

            std::vector<std::shared_ptr<ConnectionPool>> snapshot;
            {
                std::lock_guard<std::mutex> lk(poolsMtx_);
                snapshot.reserve(pools_.size());
                for (const auto& w : pools_)
                {
                    if (auto p = w.lock()) snapshot.push_back(std::move(p));
                }
            }
            for (auto& p : snapshot)
            {
                if (stopFlag_) break;
                try
                {
                    p->healthCheck();
                }
                catch (const std::exception& e)
                {
                    SQLCONDUIT_LOG_WARN("heartbeat healthCheck threw: " + std::string(e.what()));
                }
                catch (...)
                {
                    SQLCONDUIT_LOG_WARN("heartbeat healthCheck threw an unknown exception");
                }
            }
            sweepExpiredPools();
        }
        SQLCONDUIT_LOG_INFO("heartbeat manager stopped");
    }
}
