#ifndef SQLCONDUIT_CORE_QUERY_CACHE_H
#define SQLCONDUIT_CORE_QUERY_CACHE_H

#include "sqlconduit/common/types.h"
#include "sqlconduit/config/datasource_config.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sqlconduit::core {
    struct QueryCacheStats {
        std::size_t entries = 0;
        std::size_t approxBytes = 0;
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t evictions = 0;
        std::uint64_t invalidations = 0;
    };

    namespace detail {
        class QueryCacheState {
        public:
            QueryCacheState() = default;

            void configure(const config::QueryCacheConfig &cfg);

            [[nodiscard]] bool enabled() const;

            [[nodiscard]] bool replicaOnly() const;

            bool get(const std::string &dataSource, const std::string &key,
                     common::ResultSet &out);

            void put(const std::string &dataSource, const std::string &key,
                     const common::ResultSet &rs);

            void invalidate(const std::string &dataSource);

            [[nodiscard]] QueryCacheStats stats() const;

        private:
            struct Entry {
                common::ResultSet rs;
                std::chrono::steady_clock::time_point expire;
                std::list<std::string>::iterator lru;
                std::size_t bytes = 0;
            };

            static std::size_t approxBytes(const common::ResultSet &rs);

            void evictLocked(std::size_t incomingBytes, bool reserveSlot);

            void eraseLocked(std::unordered_map<std::string, Entry>::iterator it);

            mutable std::mutex mtx_;
            config::QueryCacheConfig cfg_;
            std::unordered_map<std::string, Entry> store_;
            std::list<std::string> lru_;
            std::size_t totalBytes_ = 0;
            std::atomic<bool> enabled_{false};
            std::atomic<bool> replicaOnly_{false};
            std::atomic<std::uint64_t> hits_{0};
            std::atomic<std::uint64_t> misses_{0};
            std::atomic<std::uint64_t> evictions_{0};
            std::atomic<std::uint64_t> invalidations_{0};
        };
    }

    // Compatibility facade for the process-wide default runtime.
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

        using Stats = QueryCacheStats;

        static Stats stats();
    };
}

#endif
