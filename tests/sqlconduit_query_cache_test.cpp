#include "sqlconduit/core/query_cache.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
    using sqlconduit::common::ResultSet;
    using sqlconduit::core::detail::QueryCacheState;

    int failures = 0;

    void check(const bool condition, const std::string &name) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
        if (!condition) ++failures;
    }

    sqlconduit::config::QueryCacheConfig config(const int ttlMs = 60000,
                                                 const int maxEntries = 100,
                                                 const int maxBytes = 0) {
        sqlconduit::config::QueryCacheConfig value;
        value.enabled = true;
        value.ttl_ms = ttlMs;
        value.max_entries = maxEntries;
        value.max_memory_bytes = maxBytes;
        return value;
    }

    ResultSet result(const std::string &value) {
        ResultSet rows;
        rows.setFields({"value"});
        sqlconduit::common::Row row;
        row.set("value", value);
        rows.addRow(std::move(row));
        return rows;
    }

    std::string valueOf(const ResultSet &rows) {
        if (rows.empty()) return {};
        const auto *value = std::get_if<std::string>(&rows.rows().front().at("value"));
        return value ? *value : std::string{};
    }
}

int main() {
    {
        QueryCacheState cache;
        cache.configure({});
        ResultSet output;
        cache.put("main", "key", result("ignored"));
        check(!cache.enabled() && !cache.get("main", "key", output),
              "disabled cache short-circuits get and put");
        const auto stats = cache.stats();
        check(stats.entries == 0 && stats.hits == 0 && stats.misses == 0,
              "disabled cache does not mutate entries or counters");
    }

    {
        QueryCacheState cache;
        cache.configure(config(25));
        cache.put("main", "ttl", result("fresh"));
        ResultSet output;
        check(cache.get("main", "ttl", output) && valueOf(output) == "fresh",
              "entry is readable before its TTL");
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        check(!cache.get("main", "ttl", output), "expired entry is rejected");
        const auto stats = cache.stats();
        check(stats.entries == 0 && stats.hits == 1 && stats.misses == 1,
              "TTL expiry removes storage and updates hit/miss counters");
    }

    {
        QueryCacheState cache;
        cache.configure(config(60000, 2));
        cache.put("main", "a", result("A"));
        cache.put("main", "b", result("B"));
        ResultSet output;
        check(cache.get("main", "a", output), "LRU access refreshes recency");
        cache.put("main", "c", result("C"));
        check(!cache.get("main", "b", output) && cache.get("main", "a", output) &&
                  valueOf(output) == "A" && cache.get("main", "c", output),
              "entry limit evicts the least recently used key");
        const auto stats = cache.stats();
        check(stats.entries == 2 && stats.evictions == 1,
              "entry-limit eviction is reflected in cache statistics");
    }

    {
        QueryCacheState probe;
        probe.configure(config());
        probe.put("main", "one", result(std::string(128, 'x')));
        const auto oneEntryBytes = probe.stats().approxBytes;

        QueryCacheState bounded;
        bounded.configure(config(60000, 100, static_cast<int>(oneEntryBytes)));
        bounded.put("main", "one", result(std::string(128, 'x')));
        bounded.put("main", "two", result(std::string(128, 'y')));
        ResultSet output;
        check(!bounded.get("main", "one", output) && bounded.get("main", "two", output),
              "memory limit evicts old entries before insertion");
        check(bounded.stats().entries == 1 &&
                  bounded.stats().approxBytes <= oneEntryBytes,
              "reported cache memory remains within the configured limit");

        QueryCacheState tooSmall;
        tooSmall.configure(config(60000, 100, static_cast<int>(oneEntryBytes - 1)));
        tooSmall.put("main", "oversized", result(std::string(128, 'z')));
        check(tooSmall.stats().entries == 0,
              "an entry larger than the memory budget is not cached");
    }

    {
        QueryCacheState cache;
        cache.configure(config());
        cache.put("main", "same", result("main"));
        cache.put("main-replica", "same", result("replica"));
        cache.invalidate("main");
        ResultSet output;
        check(!cache.get("main", "same", output) &&
                  cache.get("main-replica", "same", output) &&
                  valueOf(output) == "replica",
              "invalidation is exact to one datasource and not a string-prefix match");
        check(cache.stats().invalidations == 1,
              "invalidation count reports the number of removed entries");
    }

    {
        QueryCacheState cache;
        cache.configure(config());
        cache.put("main", "copy", result("original"));
        ResultSet first;
        check(cache.get("main", "copy", first), "cached result can be read");
        first.mutableRows().front().set("value", std::string("mutated"));
        ResultSet second;
        check(cache.get("main", "copy", second) && valueOf(second) == "original",
              "callers receive an isolated result copy");

        auto reconfigured = config();
        reconfigured.cache_on_replica_only = true;
        cache.configure(reconfigured);
        check(cache.stats().entries == 0 && cache.replicaOnly(),
              "reconfiguration atomically clears old entries and publishes routing mode");
        check(cache.stats().hits == 2,
              "reconfiguration preserves lifetime counters for observability");
    }

    {
        QueryCacheState cache;
        cache.configure(config(60000, 64));
        constexpr int threadCount = 8;
        constexpr int operations = 3000;
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (int thread = 0; thread < threadCount; ++thread) {
            workers.emplace_back([&, thread] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (int i = 0; i < operations; ++i) {
                    const std::string dataSource = "ds-" + std::to_string(thread % 3);
                    const std::string key = "key-" + std::to_string((thread * 17 + i) % 96);
                    if (i % 11 == 0) {
                        cache.invalidate(dataSource);
                    } else if (i % 2 == 0) {
                        cache.put(dataSource, key, result(std::to_string(i)));
                    } else {
                        ResultSet output;
                        (void) cache.get(dataSource, key, output);
                    }
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto &worker: workers) worker.join();
        const auto stats = cache.stats();
        check(stats.entries <= 64,
              "concurrent get/put/invalidate preserves the configured entry bound");
        check(stats.hits + stats.misses > 0 && stats.invalidations > 0,
              "concurrent cache activity remains observable");
    }

    std::cout << "query cache targeted tests: "
              << (failures == 0 ? "passed" : "failed") << '\n';
    return failures == 0 ? 0 : 1;
}
