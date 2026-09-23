#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/common/logger.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <list>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace sqlconduit::common {
    namespace {
        std::uint64_t nextStateIdentity() {
            static std::atomic<std::uint64_t> next{1};
            return next.fetch_add(1, std::memory_order_relaxed);
        }

        struct StateData {
            StateData() : identity(nextStateIdentity()) {
            }

            const std::uint64_t identity;
            std::mutex stateMutex;
            std::mutex statsMutex;
            OperationObserver observer;
            config::ObservabilityConfig config;
            PoolMetricsObserver poolObserver;
            PoolMetricsCollector poolCollector;
            const void *poolCollectorOwner = nullptr;
            std::atomic<std::uint64_t> stateVersion{1};
            std::unordered_map<std::uint64_t, SlowSqlStats> slowStats;
            std::deque<SlowSqlRecord> recentSlow;
            std::list<std::uint64_t> lru;
            std::unordered_map<std::uint64_t, std::list<std::uint64_t>::iterator> lruPos;
        };

        StateData &defaultStateData() {
            static StateData state;
            return state;
        }

        thread_local StateData *g_activeState = nullptr;

        StateData &currentStateData() {
            return g_activeState ? *g_activeState : defaultStateData();
        }

        class ActiveStateScope {
        public:
            explicit ActiveStateScope(StateData &state)
                : previous_(g_activeState) {
                g_activeState = &state;
            }

            ~ActiveStateScope() { g_activeState = previous_; }

        private:
            StateData *previous_;
        };

#define g_stateMutex currentStateData().stateMutex
#define g_statsMutex currentStateData().statsMutex
#define g_observer currentStateData().observer
#define g_config currentStateData().config
#define g_poolObserver currentStateData().poolObserver
#define g_poolCollector currentStateData().poolCollector
#define g_poolCollectorOwner currentStateData().poolCollectorOwner
#define g_stateVersion currentStateData().stateVersion
#define g_slowStats currentStateData().slowStats
#define g_recentSlow currentStateData().recentSlow
#define g_lru currentStateData().lru
#define g_lruPos currentStateData().lruPos

        struct Snapshot {
            config::ObservabilityConfig config;
            OperationObserver observer;
            PoolMetricsObserver poolObserver;
            PoolMetricsCollector poolCollector;
        };

        const Snapshot &currentSnapshot() {
            static thread_local StateData *tlsOwner = nullptr;
            static thread_local std::uint64_t tlsIdentity = 0;
            static thread_local std::uint64_t tlsVersion = 0;
            static thread_local Snapshot tlsSnapshot;
            auto *owner = &currentStateData();
            const auto version = g_stateVersion.load(std::memory_order_acquire);
            if (tlsOwner == owner && tlsIdentity == owner->identity && tlsVersion == version)
                return tlsSnapshot;
            std::lock_guard<std::mutex> lock(g_stateMutex);
            tlsSnapshot.config = g_config;
            tlsSnapshot.observer = g_observer;
            tlsSnapshot.poolObserver = g_poolObserver;
            tlsSnapshot.poolCollector = g_poolCollector;
            tlsVersion = g_stateVersion.load(std::memory_order_acquire);
            tlsOwner = owner;
            tlsIdentity = owner->identity;
            return tlsSnapshot;
        }

        std::uint64_t nextSampleSequence() {
            static thread_local std::uint64_t tlsSequence =
                    std::hash<std::thread::id>{}(std::this_thread::get_id());
            return ++tlsSequence;
        }

        void touchLru(std::uint64_t fp) {
            auto it = g_lruPos.find(fp);
            if (it == g_lruPos.end()) {
                g_lru.push_front(fp);
                g_lruPos.emplace(fp, g_lru.begin());
            } else {
                g_lru.splice(g_lru.begin(), g_lru, it->second);
            }
        }

        void evictLru() {
            if (g_lru.empty()) return;
            const auto fp = g_lru.back();
            g_lru.pop_back();
            g_lruPos.erase(fp);
            g_slowStats.erase(fp);
        }

        std::string truncate(std::string text, const std::size_t limit) {
            if (text.size() <= limit) return text;
            const auto original = text.size();
            bool inLiteral = false;
            bool escaped = false;
            for (std::size_t i = 0; i < limit; ++i) {
                const char c = text[i];
                if (escaped) {
                    escaped = false;
                    continue;
                }
                if (c == '\\') {
                    escaped = true;
                    continue;
                }
                if (c == '\'') {
                    if (i + 1 < limit && text[i + 1] == '\'') {
                        ++i;
                        continue;
                    }
                    inLiteral = !inLiteral;
                }
            }
            std::string head = text.substr(0, limit);
            if (inLiteral) head += '\'';
            head += "...[truncated, original_length=" + std::to_string(original) + "]";
            return head;
        }

        std::uint64_t fingerprint(const std::string &dataSource,
                                  const OperationType type,
                                  const std::string &sql) {
            std::uint64_t hash = 1469598103934665603ULL;
            auto add = [&](const unsigned char c) {
                hash ^= c;
                hash *= 1099511628211ULL;
            };
            for (const unsigned char c: dataSource) add(c);
            add(0);
            add(static_cast<unsigned char>(type));
            for (const unsigned char c: sql) add(c);
            return hash;
        }

        std::string structuralSql(const std::string &sql) {
            std::string out;
            out.reserve(sql.size());
            const std::size_t n = sql.size();
            const auto isIdent = [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            };
            bool pendingSpace = false;
            const auto flushSpace = [&] {
                if (pendingSpace && !out.empty() && out.back() != ' ') out.push_back(' ');
                pendingSpace = false;
            };
            std::size_t i = 0;
            while (i < n) {
                const auto c = static_cast<unsigned char>(sql[i]);

                if (std::isspace(c)) {
                    pendingSpace = !out.empty();
                    ++i;
                    continue;
                }

                if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
                    while (i < n && sql[i] != '\n') ++i;
                    pendingSpace = !out.empty();
                    continue;
                }
                if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
                    const bool semantic = i + 2 < n &&
                                          (sql[i + 2] == '!' || sql[i + 2] == '+');
                    const auto close = sql.find("*/", i + 2);
                    const auto end = close == std::string::npos ? n : close + 2;
                    if (semantic) {
                        flushSpace();
                        out.append(sql, i, end - i);
                    } else {
                        pendingSpace = !out.empty();
                    }
                    i = end;
                    continue;
                }

                if (c == '$') {
                    std::size_t tagEnd = i + 1;
                    while (tagEnd < n &&
                           (std::isalnum(static_cast<unsigned char>(sql[tagEnd])) ||
                            sql[tagEnd] == '_'))
                        ++tagEnd;
                    const bool validTag = tagEnd < n && sql[tagEnd] == '$' &&
                                          (tagEnd == i + 1 ||
                                           !std::isdigit(static_cast<unsigned char>(sql[i + 1])));
                    if (validTag) {
                        const std::string delimiter = sql.substr(i, tagEnd - i + 1);
                        const auto close = sql.find(delimiter, tagEnd + 1);
                        if (close != std::string::npos) {
                            flushSpace();
                            const auto end = close + delimiter.size();
                            std::uint64_t bodyHash = 1469598103934665603ULL;
                            for (auto p = tagEnd + 1; p < close; ++p) {
                                bodyHash ^= static_cast<unsigned char>(sql[p]);
                                bodyHash *= 1099511628211ULL;
                            }
                            out += delimiter + "<body_hash:" +
                                    std::to_string(bodyHash) + ">" + delimiter;
                            i = end;
                            continue;
                        }
                    }
                }

                if (c == '\'') {
                    flushSpace();
                    out += '?';
                    ++i;
                    while (i < n) {
                        if (sql[i] == '\\' && i + 1 < n) {
                            i += 2;
                            continue;
                        }
                        if (sql[i] == '\'') {
                            if (i + 1 < n && sql[i + 1] == '\'') {
                                i += 2;
                                continue;
                            }
                            ++i;
                            break;
                        }
                        ++i;
                    }
                    continue;
                }

                if (c == '"' || c == '`') {
                    flushSpace();
                    const char quote = static_cast<char>(c);
                    out.push_back(quote);
                    ++i;
                    while (i < n) {
                        out.push_back(sql[i]);
                        if (sql[i] == '\\' && i + 1 < n) {
                            out.push_back(sql[++i]);
                        } else if (sql[i] == quote) {
                            if (i + 1 < n && sql[i + 1] == quote)
                                out.push_back(sql[++i]);
                            else {
                                ++i;
                                break;
                            }
                        }
                        ++i;
                    }
                    continue;
                }

                const bool boundary = i == 0 ||
                                      !isIdent(static_cast<unsigned char>(sql[i - 1]));
                const bool signedNumber = (c == '+' || c == '-') && i + 1 < n &&
                                          (std::isdigit(static_cast<unsigned char>(sql[i + 1])) ||
                                           (sql[i + 1] == '.' && i + 2 < n &&
                                            std::isdigit(static_cast<unsigned char>(sql[i + 2]))));
                const bool plainNumber = std::isdigit(c) ||
                                         (c == '.' && i + 1 < n &&
                                          std::isdigit(static_cast<unsigned char>(sql[i + 1])));
                if (boundary && (plainNumber || signedNumber)) {
                    flushSpace();
                    out += '?';
                    if (signedNumber) ++i;
                    if (i + 1 < n && sql[i] == '0' &&
                        (sql[i + 1] == 'x' || sql[i + 1] == 'X')) {
                        i += 2;
                        while (i < n && (std::isxdigit(static_cast<unsigned char>(sql[i])) ||
                                         sql[i] == '_'))
                            ++i;
                        continue;
                    }
                    if (i + 1 < n && sql[i] == '0' &&
                        (sql[i + 1] == 'b' || sql[i + 1] == 'B')) {
                        i += 2;
                        while (i < n && (sql[i] == '0' || sql[i] == '1' || sql[i] == '_')) ++i;
                        continue;
                    }
                    while (i < n && (std::isdigit(static_cast<unsigned char>(sql[i])) ||
                                     sql[i] == '_'))
                        ++i;
                    if (i < n && sql[i] == '.') {
                        ++i;
                        while (i < n && (std::isdigit(static_cast<unsigned char>(sql[i])) ||
                                         sql[i] == '_'))
                            ++i;
                    }
                    if (i < n && (sql[i] == 'e' || sql[i] == 'E')) {
                        std::size_t exponent = i + 1;
                        if (exponent < n && (sql[exponent] == '+' || sql[exponent] == '-'))
                            ++exponent;
                        const auto digits = exponent;
                        while (exponent < n &&
                               (std::isdigit(static_cast<unsigned char>(sql[exponent])) ||
                                sql[exponent] == '_'))
                            ++exponent;
                        if (exponent > digits) i = exponent;
                    }
                    continue;
                }

                flushSpace();
                out += static_cast<char>(c);
                ++i;
            }
            return out;
        }

        LogLevel parseLevel(const std::string &level) {
            if (level == "info") return LogLevel::Info;
            if (level == "warn") return LogLevel::Warn;
            if (level == "error") return LogLevel::Error;
            return LogLevel::Debug;
        }

        const char *operationName(const OperationType type) {
            switch (type) {
                case OperationType::Query: return "query";
                case OperationType::Execute: return "execute";
                case OperationType::Stream: return "stream";
                case OperationType::Batch: return "batch";
                case OperationType::Select: return "select";
                case OperationType::Routine: return "routine";
                default: return "operation";
            }
        }
    }

    struct detail::ObservabilityState::Impl {
        explicit Impl(const bool useProcessDefault) {
            if (useProcessDefault) {
                data = &defaultStateData();
            } else {
                owned = std::make_unique<StateData>();
                data = owned.get();
            }
        }

        std::unique_ptr<StateData> owned;
        StateData *data = nullptr;
    };

    detail::ObservabilityState::ObservabilityState()
        : impl_(std::make_unique<Impl>(false)) {
    }

    detail::ObservabilityState::ObservabilityState(const bool useProcessDefault)
        : impl_(std::make_unique<Impl>(useProcessDefault)) {
    }

    detail::ObservabilityState::~ObservabilityState() = default;

    detail::ObservabilityState::ObservabilityState(ObservabilityState &&) noexcept = default;

    detail::ObservabilityState &detail::ObservabilityState::operator=(
        ObservabilityState &&) noexcept = default;

    void Observability::setObserver(OperationObserver observer) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_observer = std::move(observer);
        }
        g_stateVersion.fetch_add(1, std::memory_order_release);
    }

    void Observability::setPoolMetricsObserver(PoolMetricsObserver observer) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_poolObserver = std::move(observer);
        }
        g_stateVersion.fetch_add(1, std::memory_order_release);
    }

    void Observability::setPoolMetricsCollector(PoolMetricsCollector collector,
                                                const void *owner) {
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_poolCollector = std::move(collector);
            g_poolCollectorOwner = g_poolCollector ? owner : nullptr;
        }
        g_stateVersion.fetch_add(1, std::memory_order_release);
    }

    void Observability::clearPoolMetricsCollector(const void *owner) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            if (g_poolCollectorOwner == owner) {
                g_poolCollector = {};
                g_poolCollectorOwner = nullptr;
                changed = true;
            }
        }
        if (changed)
            g_stateVersion.fetch_add(1, std::memory_order_release);
    }

    PoolMetricsEvent Observability::samplePoolMetrics() noexcept {
        PoolMetricsEvent event;
        event.timestamp = std::chrono::system_clock::now();
        try {
            PoolMetricsCollector collector;
            PoolMetricsObserver observer;
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                collector = g_poolCollector;
                observer = g_poolObserver;
            }
            if (collector) {
                try {
                    event.pools = collector();
                } catch (...) {
                    event.pools.clear();
                }
            }
            if (observer) {
                try {
                    observer(event);
                } catch (...) {
                }
            }
        } catch (...) {
        }
        return event;
    }

    void Observability::emit(const OperationEvent &event) noexcept {
        try {
            const Snapshot &snapshot = currentSnapshot();
            if (!snapshot.observer) return;
            try {
                snapshot.observer(event);
            } catch (...) {
            }
        } catch (...) {
        }
    }

    void Observability::configure(const config::ObservabilityConfig &config) {
        config::ObservabilityConfig normalized = config;
        if (normalized.slow_sql.aggregate_capacity < 1)
            normalized.slow_sql.aggregate_capacity = 1;
        if (normalized.slow_sql.recent_capacity < 0)
            normalized.slow_sql.recent_capacity = 0;
        const auto capacity =
                static_cast<std::size_t>(normalized.slow_sql.aggregate_capacity);
        const auto recentCapacity =
                static_cast<std::size_t>(normalized.slow_sql.recent_capacity);
        const auto buckets = normalized.slow_sql.histogram_buckets_ms;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_config = std::move(normalized);
        }
        g_stateVersion.fetch_add(1, std::memory_order_release);

        std::lock_guard<std::mutex> lock(g_statsMutex);

        const bool bucketsChanged = std::any_of(
            g_slowStats.begin(), g_slowStats.end(), [&](const auto &entry) {
                return entry.second.histogramBucketsMs != buckets;
            });
        if (bucketsChanged) {
            g_slowStats.clear();
            g_lru.clear();
            g_lruPos.clear();
        }

        while (g_slowStats.size() > capacity) evictLru();
        while (g_recentSlow.size() > recentCapacity)
            g_recentSlow.pop_front();
    }

    void Observability::emitSql(OperationEvent event, const std::string &sql,
                                const SqlRenderer &renderer,
                                const common::ResultSet *result) noexcept {
        try {
            const Snapshot &snapshot = currentSnapshot();
            const config::ObservabilityConfig &config = snapshot.config;
            const OperationObserver &observer = snapshot.observer;

            const bool slowEnabled = config.slow_sql.enabled;
            const bool logEnabled = config.sql_log.enabled;

            {
                const SqlContext &ctx = ContextScope::current();
                if (!ctx.traceId.empty()) event.traceId = ctx.traceId;
                if (!ctx.spanId.empty()) {
                    event.spanId = ctx.spanId;
                } else if (!event.traceId.empty()) {
                    event.spanId = nextSpanId();
                }
                if (ctx.shadow) event.shadow = true;
            }
            if (result && result->transformed) event.transformed = true;

            if (!observer && !slowEnabled && !logEnabled) return;

            const std::string structure = (slowEnabled || logEnabled)
                                              ? structuralSql(sql)
                                              : std::string();
            if (slowEnabled || logEnabled)
                event.sqlFingerprint = fingerprint(event.dataSource, event.type, structure);

            const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                event.duration).count();
            event.slow = slowEnabled && durationMs >= config.slow_sql.threshold_ms;

            const bool logEligible = logEnabled &&
                                     (!config.sql_log.slow_only || durationMs >= config.slow_sql.threshold_ms) &&
                                     ((event.status.ok() && config.sql_log.log_success) ||
                                      (!event.status.ok() && config.sql_log.log_errors));
            const auto sequence = logEnabled ? nextSampleSequence() : 0;
            const bool sampled = !logEnabled ||
                                 config.sql_log.sample_rate >= 1.0 ||
                                 (config.sql_log.sample_rate > 0.0 &&
                                  static_cast<double>((event.sqlFingerprint ^ sequence) % 1000000ULL) /
                                  1000000.0 < config.sql_log.sample_rate);
            const bool needsRendered = renderer &&
                                       ((logEligible && sampled && config.sql_log.mode == "full") ||
                                        (event.slow && config.slow_sql.retain_rendered_sql));

            if (event.slow || (logEligible && sampled)) {
                event.sqlTemplate = truncate(sql, static_cast<std::size_t>(
                                                 event.slow
                                                     ? config.slow_sql.max_sql_length
                                                     : config.sql_log.max_sql_length));
            }
            if (needsRendered) {
                SqlRenderOptions options;
                options.includeStringValues = config.sql_log.include_string_values;
                options.includeBlobValues = config.sql_log.include_blob_values;
                options.maxParamLength = static_cast<std::size_t>(
                    config.sql_log.max_param_length);
                options.maxSqlLength = static_cast<std::size_t>(
                    std::max(config.sql_log.max_sql_length,
                             config.slow_sql.max_sql_length));
                std::string rendered;
                if (renderer(options, rendered).ok()) event.renderedSql = std::move(rendered);
            }

            if (event.slow) {
                const auto now = std::chrono::system_clock::now();
                const auto aggKey = event.sqlFingerprint;
                std::lock_guard<std::mutex> lock(g_statsMutex);
                const bool isNew = g_slowStats.find(aggKey) == g_slowStats.end();
                if (isNew) {
                    if (g_slowStats.size() >= static_cast<std::size_t>(
                            config.slow_sql.aggregate_capacity))
                        evictLru();
                    touchLru(aggKey);
                } else {
                    touchLru(aggKey);
                }
                auto &stats = g_slowStats[aggKey];
                if (isNew) {
                    stats.fingerprint = aggKey;
                    stats.dataSource = event.dataSource;
                    stats.type = event.type;
                    stats.sqlTemplate = truncate(structure, static_cast<std::size_t>(
                                                     config.slow_sql.max_sql_length));
                    stats.firstSeen = now;
                    stats.minDuration = event.duration;
                    stats.histogramBucketsMs = config.slow_sql.histogram_buckets_ms;
                    stats.histogram.assign(stats.histogramBucketsMs.size() + 1, 0);
                }
                ++stats.count;
                if (!event.status.ok()) ++stats.errorCount;
                if (event.status.code == ErrorCode::QueryTimeout) ++stats.timeoutCount;
                stats.totalDuration += event.duration;
                stats.minDuration = std::min(stats.minDuration, event.duration);
                stats.maxDuration = std::max(stats.maxDuration, event.duration);
                stats.lastSeen = now;
                std::size_t bucket = 0;
                while (bucket < stats.histogramBucketsMs.size() &&
                       event.duration.count() >
                       static_cast<std::int64_t>(stats.histogramBucketsMs[bucket]) * 1000)
                    ++bucket;
                ++stats.histogram[bucket];

                if (config.slow_sql.recent_capacity > 0) {
                    SlowSqlRecord record;
                    record.timestamp = now;
                    record.dataSource = event.dataSource;
                    record.type = event.type;
                    record.sqlTemplate = stats.sqlTemplate;
                    if (config.slow_sql.retain_rendered_sql)
                        record.renderedSql = truncate(event.renderedSql,
                                                      static_cast<std::size_t>(config.slow_sql.max_sql_length));
                    record.fingerprint = aggKey;
                    record.duration = event.duration;
                    record.errorCode = event.status.code;
                    record.sqlState = event.status.sqlState;
                    record.traceId = event.traceId;
                    record.spanId = event.spanId;
                    if (g_recentSlow.size() >= static_cast<std::size_t>(
                            config.slow_sql.recent_capacity))
                        g_recentSlow.pop_front();
                    g_recentSlow.push_back(std::move(record));
                }
            }

            if (logEligible && sampled) {
                const auto &displaySql = config.sql_log.mode == "full" &&
                                         !event.renderedSql.empty()
                                             ? event.renderedSql
                                             : event.sqlTemplate;
                std::ostringstream message;
                message << "sql datasource=" << event.dataSource
                        << " operation=" << operationName(event.type)
                        << " duration_ms=" << durationMs
                        << " rows=" << event.rowCount
                        << " status=" << errorCodeToString(event.status.code)
                        << " fingerprint=" << event.sqlFingerprint;
                if (!event.traceId.empty()) message << " trace=" << event.traceId;
                if (!event.spanId.empty()) message << " span=" << event.spanId;
                message << " statement=" << displaySql;
                Logger::log(parseLevel(config.sql_log.level), message.str());
            }

            if (observer) {
                try { observer(event); } catch (...) {
                }
            }
        } catch (...) {
        }
    }

    std::vector<SlowSqlStats> Observability::slowSqlStats(
        const std::size_t limit, const std::string &dataSource) {
        std::vector<SlowSqlStats> result;
        std::lock_guard<std::mutex> lock(g_statsMutex);
        result.reserve(g_slowStats.size());
        for (const auto &entry: g_slowStats) {
            if (dataSource.empty() || entry.second.dataSource == dataSource)
                result.push_back(entry.second);
        }
        std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
            const std::uint64_t ac = a.count == 0 ? 1 : a.count;
            const std::uint64_t bc = b.count == 0 ? 1 : b.count;
            const auto averageA = static_cast<long double>(a.totalDuration.count()) /
                                  static_cast<long double>(ac);
            const auto averageB = static_cast<long double>(b.totalDuration.count()) /
                                  static_cast<long double>(bc);
            return averageA > averageB;
        });
        if (result.size() > limit) result.resize(limit);
        return result;
    }

    std::vector<SlowSqlRecord> Observability::recentSlowSql(
        const std::size_t limit, const std::string &dataSource) {
        std::vector<SlowSqlRecord> result;
        if (limit == 0) return result;
        std::lock_guard<std::mutex> lock(g_statsMutex);
        for (auto it = g_recentSlow.rbegin(); it != g_recentSlow.rend(); ++it) {
            if (!dataSource.empty() && it->dataSource != dataSource) continue;
            result.push_back(*it);
            if (result.size() >= limit) break;
        }
        return result;
    }

    void Observability::clearSlowSqlStats() {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_slowStats.clear();
        g_lru.clear();
        g_lruPos.clear();
        g_recentSlow.clear();
    }

    void detail::ObservabilityState::setObserver(OperationObserver observer) {
        ActiveStateScope scope(*impl_->data);
        Observability::setObserver(std::move(observer));
    }

    void detail::ObservabilityState::emit(const OperationEvent &event) noexcept {
        ActiveStateScope scope(*impl_->data);
        Observability::emit(event);
    }

    void detail::ObservabilityState::setPoolMetricsObserver(PoolMetricsObserver observer) {
        ActiveStateScope scope(*impl_->data);
        Observability::setPoolMetricsObserver(std::move(observer));
    }

    void detail::ObservabilityState::setPoolMetricsCollector(PoolMetricsCollector collector,
                                                             const void *owner) {
        ActiveStateScope scope(*impl_->data);
        Observability::setPoolMetricsCollector(std::move(collector), owner);
    }

    void detail::ObservabilityState::clearPoolMetricsCollector(const void *owner) {
        ActiveStateScope scope(*impl_->data);
        Observability::clearPoolMetricsCollector(owner);
    }

    PoolMetricsEvent detail::ObservabilityState::samplePoolMetrics() noexcept {
        ActiveStateScope scope(*impl_->data);
        return Observability::samplePoolMetrics();
    }

    void detail::ObservabilityState::configure(const config::ObservabilityConfig &config) {
        ActiveStateScope scope(*impl_->data);
        Observability::configure(config);
    }

    void detail::ObservabilityState::emitSql(OperationEvent event, const std::string &sql,
                                             const SqlRenderer &renderer,
                                             const common::ResultSet *result) noexcept {
        ActiveStateScope scope(*impl_->data);
        Observability::emitSql(std::move(event), sql, renderer, result);
    }

    std::vector<SlowSqlStats> detail::ObservabilityState::slowSqlStats(
        const std::size_t limit, const std::string &dataSource) {
        ActiveStateScope scope(*impl_->data);
        return Observability::slowSqlStats(limit, dataSource);
    }

    std::vector<SlowSqlRecord> detail::ObservabilityState::recentSlowSql(
        const std::size_t limit, const std::string &dataSource) {
        ActiveStateScope scope(*impl_->data);
        return Observability::recentSlowSql(limit, dataSource);
    }

    void detail::ObservabilityState::clearSlowSqlStats() {
        ActiveStateScope scope(*impl_->data);
        Observability::clearSlowSqlStats();
    }

#undef g_stateMutex
#undef g_statsMutex
#undef g_observer
#undef g_config
#undef g_poolObserver
#undef g_poolCollector
#undef g_poolCollectorOwner
#undef g_stateVersion
#undef g_slowStats
#undef g_recentSlow
#undef g_lru
#undef g_lruPos
}
