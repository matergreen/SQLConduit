#include "sqlconduit/core/query_cache.h"
#include "sqlconduit/core/runtime_services.h"

#include <utility>
#include <variant>

namespace sqlconduit::core {
    namespace {
        std::string compositeKey(const std::string &dataSource, const std::string &key) {
            std::string ck;
            ck.reserve(dataSource.size() + 1 + key.size());
            ck += dataSource;
            ck.push_back('\0');
            ck += key;
            return ck;
        }

        std::size_t valueBytes(const common::Value &v) {
            if (const auto *p = std::get_if<std::string>(&v)) return p->size();
            if (const auto *p = std::get_if<common::Decimal>(&v)) return p->value.size();
            if (const auto *p = std::get_if<common::Date>(&v)) return p->value.size();
            if (const auto *p = std::get_if<common::Time>(&v)) return p->value.size();
            if (const auto *p = std::get_if<common::Uuid>(&v)) return p->value.size();
            if (const auto *p = std::get_if<common::Json>(&v)) return p->value.size();
            if (const auto *p = std::get_if<common::Blob>(&v)) return p->size();
            return sizeof(common::Value);
        }
    }

    std::size_t detail::QueryCacheState::approxBytes(const common::ResultSet &rs) {
        std::size_t bytes = 0;
        for (const auto &f: rs.fields()) bytes += f.size() + sizeof(std::string);
        for (const auto &row: rs.rows()) {
            for (const auto &[col, val]: row.data()) {
                bytes += col.size() + sizeof(std::string) + valueBytes(val);
            }
        }
        return bytes;
    }

    void detail::QueryCacheState::eraseLocked(std::unordered_map<std::string, Entry>::iterator it) {
        totalBytes_ -= (it->second.bytes <= totalBytes_ ? it->second.bytes : totalBytes_);
        lru_.erase(it->second.lru);
        store_.erase(it);
    }

    void detail::QueryCacheState::evictLocked(const std::size_t incomingBytes, const bool reserveSlot) {
        const auto maxEntries = cfg_.max_entries > 0
                                    ? static_cast<std::size_t>(cfg_.max_entries)
                                    : 0;
        const auto maxBytes = cfg_.max_memory_bytes > 0
                                  ? static_cast<std::size_t>(cfg_.max_memory_bytes)
                                  : 0;

        while (!lru_.empty()) {
            const bool tooMany = maxEntries > 0 &&
                                 (reserveSlot ? store_.size() >= maxEntries : store_.size() > maxEntries);
            const bool tooBig = maxBytes > 0 && totalBytes_ + incomingBytes > maxBytes;
            if (!tooMany && !tooBig) return;
            const std::string old = lru_.back();
            auto oit = store_.find(old);
            if (oit == store_.end()) {
                lru_.pop_back();
                continue;
            }
            eraseLocked(oit);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void detail::QueryCacheState::configure(const config::QueryCacheConfig &cfg) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            cfg_ = cfg;
            store_.clear();
            lru_.clear();
            totalBytes_ = 0;
        }
        enabled_.store(cfg.enabled && cfg.ttl_ms > 0, std::memory_order_release);
        replicaOnly_.store(cfg.cache_on_replica_only, std::memory_order_release);
    }

    bool detail::QueryCacheState::enabled() const {
        return enabled_.load(std::memory_order_acquire);
    }

    bool detail::QueryCacheState::replicaOnly() const {
        return replicaOnly_.load(std::memory_order_acquire);
    }

    bool detail::QueryCacheState::get(const std::string &dataSource, const std::string &key,
                                      common::ResultSet &out) {
        if (!enabled_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lk(mtx_);
        if (!cfg_.enabled) return false;
        const std::string ck = compositeKey(dataSource, key);
        const auto it = store_.find(ck);
        if (it == store_.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (it->second.expire <= std::chrono::steady_clock::now()) {
            eraseLocked(it);
            misses_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        out = it->second.rs;
        lru_.splice(lru_.begin(), lru_, it->second.lru);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void detail::QueryCacheState::put(const std::string &dataSource, const std::string &key,
                                      const common::ResultSet &rs) {
        if (!enabled_.load(std::memory_order_acquire)) return;
        const std::size_t bytes = approxBytes(rs);

        std::lock_guard<std::mutex> lk(mtx_);
        if (!cfg_.enabled) return;
        if (cfg_.ttl_ms <= 0) return;
        if (cfg_.max_memory_bytes > 0 &&
            bytes > static_cast<std::size_t>(cfg_.max_memory_bytes))
            return;

        const std::string ck = compositeKey(dataSource, key);
        const auto now = std::chrono::steady_clock::now();
        const auto ttl = std::chrono::milliseconds(cfg_.ttl_ms);

        if (const auto it = store_.find(ck); it != store_.end()) {
            totalBytes_ -= (it->second.bytes <= totalBytes_ ? it->second.bytes : totalBytes_);
            it->second.rs = rs;
            it->second.bytes = bytes;
            it->second.expire = now + ttl;
            totalBytes_ += bytes;
            lru_.splice(lru_.begin(), lru_, it->second.lru);
            evictLocked(0, false);
            return;
        }

        evictLocked(bytes, true);
        Entry e;
        e.rs = rs;
        e.bytes = bytes;
        e.expire = now + ttl;
        lru_.push_front(ck);
        e.lru = lru_.begin();
        totalBytes_ += bytes;
        store_.emplace(ck, std::move(e));
    }

    void detail::QueryCacheState::invalidate(const std::string &dataSource) {
        if (!enabled_.load(std::memory_order_acquire)) return;
        std::uint64_t removed = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (store_.empty()) return;
            const std::string prefix = compositeKey(dataSource, std::string{});
            for (auto it = store_.begin(); it != store_.end();) {
                if (it->first.size() >= prefix.size() &&
                    it->first.compare(0, prefix.size(), prefix) == 0) {
                    totalBytes_ -= (it->second.bytes <= totalBytes_
                                        ? it->second.bytes
                                        : totalBytes_);
                    lru_.erase(it->second.lru);
                    it = store_.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
        }
        if (removed > 0) invalidations_.fetch_add(removed, std::memory_order_relaxed);
    }

    QueryCacheStats detail::QueryCacheState::stats() const {
        QueryCacheStats out;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            out.entries = store_.size();
            out.approxBytes = totalBytes_;
        }
        out.hits = hits_.load(std::memory_order_relaxed);
        out.misses = misses_.load(std::memory_order_relaxed);
        out.evictions = evictions_.load(std::memory_order_relaxed);
        out.invalidations = invalidations_.load(std::memory_order_relaxed);
        return out;
    }

    void QueryCache::configure(const config::QueryCacheConfig &cfg) {
        detail::defaultRuntimeServices()->queryCache.configure(cfg);
    }

    bool QueryCache::enabled() {
        return detail::defaultRuntimeServices()->queryCache.enabled();
    }

    bool QueryCache::replicaOnly() {
        return detail::defaultRuntimeServices()->queryCache.replicaOnly();
    }

    bool QueryCache::get(const std::string &dataSource, const std::string &key,
                         common::ResultSet &out) {
        return detail::defaultRuntimeServices()->queryCache.get(dataSource, key, out);
    }

    void QueryCache::put(const std::string &dataSource, const std::string &key,
                         const common::ResultSet &rs) {
        detail::defaultRuntimeServices()->queryCache.put(dataSource, key, rs);
    }

    void QueryCache::invalidate(const std::string &dataSource) {
        detail::defaultRuntimeServices()->queryCache.invalidate(dataSource);
    }

    QueryCache::Stats QueryCache::stats() {
        return detail::defaultRuntimeServices()->queryCache.stats();
    }
}
