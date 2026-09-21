#ifndef SQLCONDUIT_CORE_STATS_REPORTER_H
#define SQLCONDUIT_CORE_STATS_REPORTER_H

#include "sqlconduit/common/observer.h"
#include "sqlconduit/config/datasource_config.h"
#include "sqlconduit/core/database_manager.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sqlconduit::core
{
    class StatsReporter
    {
    public:
        using PoolStatsCollector = std::function<std::vector<NamedPoolStats>()>;

        StatsReporter() = default;

        ~StatsReporter();

        StatsReporter(const StatsReporter&) = delete;

        StatsReporter& operator=(const StatsReporter&) = delete;

        void start(const config::StatsReportConfig& cfg, PoolStatsCollector collector);

        void stop();

        [[nodiscard]] bool running() const;

    private:
        void run();

        void writeOnce();

        [[nodiscard]] std::string renderText(
            const std::chrono::system_clock::time_point& now,
            const std::vector<NamedPoolStats>& pools,
            const std::vector<common::SlowSqlStats>& slowSql) const;

        [[nodiscard]] std::string renderJson(
            const std::chrono::system_clock::time_point& now,
            const std::vector<NamedPoolStats>& pools,
            const std::vector<common::SlowSqlStats>& slowSql) const;

        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::thread thread_;
        bool running_ = false;
        config::StatsReportConfig cfg_;
        PoolStatsCollector collector_;
    };
}

#endif
