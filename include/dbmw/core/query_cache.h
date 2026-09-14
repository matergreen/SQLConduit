#ifndef DBMW_CORE_QUERY_CACHE_H
#define DBMW_CORE_QUERY_CACHE_H

#include "dbmw/common/types.h"
#include "dbmw/config/datasource_config.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dbmw::core {
    class QueryCache {
    public:
        QueryCache() = delete;

        static void configure(const config::QueryCacheConfig &cfg);

        static bool enabled();
        static bool replicaOnly();

        static bool get(const std::string &dataSource, const std::string &key,
                        common::ResultSet &out);
        static void put(const std::string &dataSource, const std::string &key,
                        const common::ResultSet &rs);
        static void invalidate(const std::string &dataSource);

        struct Stats {
            std::size_t entries = 0;
            std::size_t approxBytes = 0;
            std::uint64_t hits = 0;
            std::uint64_t misses = 0;
            std::uint64_t evictions = 0;
            std::uint64_t invalidations = 0;
        };

        static Stats stats();

    private:
        struct Entry {
            common::ResultSet rs;
            std::chrono::steady_clock::time_point expire;
            std::list<std::string>::iterator lru;
            std::size_t bytes = 0;
        };

        static std::size_t approxBytes(const common::ResultSet &rs);

        static void evictLocked(std::size_t incomingBytes, bool reserveSlot);

        static void eraseLocked(std::unordered_map<std::string, Entry>::iterator it);

        static std::mutex mtx_;
        static config::QueryCacheConfig cfg_;
        static std::unordered_map<std::string, Entry> store_;
        static std::list<std::string> lru_;
        static std::size_t totalBytes_;
        static std::atomic<bool> enabled_;
        static std::atomic<bool> replicaOnly_;
        static std::atomic<std::uint64_t> hits_;
        static std::atomic<std::uint64_t> misses_;
        static std::atomic<std::uint64_t> evictions_;
        static std::atomic<std::uint64_t> invalidations_;
    };
}

#endif
