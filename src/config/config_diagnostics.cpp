#include "sqlconduit/config/config_loader.h"

#include <nlohmann/json.hpp>
#include <set>

namespace sqlconduit::config {
    bool ConfigLoader::validateTopology(const GlobalConfig &c, std::string &error) {
        error.clear();
        auto fail = [&](const std::string &path, const std::string &message) {
            error = "config error [topology] at " + path + ": " + message;
            return false;
        };
        if (c.datasources.empty()) return fail("/datasources", "at least one datasource is required");
        std::set<std::string> names, leaves;
        for (const auto &d : c.datasources) {
            if (d.name.empty() || d.type.empty())
                return fail("/datasources", "name and type must not be empty");
            if (!names.insert(d.name).second)
                return fail("/datasources", "duplicate datasource name: " + d.name);
            leaves.insert(d.name);
        }
        for (const auto &g : c.groups) {
            if (g.name.empty() || !names.insert(g.name).second)
                return fail("/groups", "empty or duplicate datasource/group name: " + g.name);
        }
        for (std::size_t i = 0; i < c.groups.size(); ++i) {
            const auto &g = c.groups[i];
            const auto path = "/groups/" + std::to_string(i);
            auto leaf = [&](const std::string &name) { return leaves.count(name) != 0; };
            if (!leaf(g.primary)) return fail(path + "/primary", "must reference a plain datasource");
            for (const auto &r : g.replicas) {
                if (!leaf(r.name) || r.weight < 1)
                    return fail(path + "/replicas", "requires a plain datasource and positive weight");
            }
            for (const auto &p : g.failover.primaries) {
                if (!leaf(p)) return fail(path + "/failover/primaries", "must reference plain datasources");
            }
            if (!g.failover.primaries.empty() && !g.failover.acknowledge_external_fencing)
                return fail(path + "/failover", "external fencing acknowledgement is required");
            if (g.failover.write_buffer.enabled &&
                (!g.failover.write_buffer.acknowledge_data_loss_and_duplicates || g.read_only))
                return fail(path + "/failover/write_buffer",
                            "requires duplicate/data-loss acknowledgement and a writable group");
            if (!g.shadow.empty()) {
                if (!leaf(g.shadow) || g.shadow == g.primary)
                    return fail(path + "/shadow", "must reference a separate plain datasource");
                for (const auto &r : g.replicas) {
                    if (r.name == g.shadow)
                        return fail(path + "/shadow", "must not reference a replica of this group");
                }
            }
        }
        if (names.count(c.default_datasource) == 0)
            return fail("/default_datasource", "must reference an existing datasource or group");
        return true;
    }

    std::string ConfigLoader::explain(const GlobalConfig &c) {
        using json = nlohmann::json;
        // Serialize typed values rather than raw input so implicit defaults and
        // environment resolution are reflected without exposing secret strings.
        json j;
#define FIELD(obj, name) {#name, (obj).name}
        const auto &p = c.pool;
        j["pool"] = {FIELD(p, enabled), FIELD(p, min), FIELD(p, max),
                     FIELD(p, borrow_timeout_ms), FIELD(p, idle_timeout_ms),
                     FIELD(p, max_lifetime_ms), FIELD(p, leak_detection_threshold_ms),
                     FIELD(p, validation_interval_ms)};
        const auto &r = c.retry;
        j["retry"] = {FIELD(r, max_attempts), FIELD(r, initial_backoff_ms),
                      FIELD(r, max_backoff_ms), FIELD(r, retry_writes)};
        const auto &b = c.circuit_breaker;
        j["circuit_breaker"] = {FIELD(b, failure_threshold), FIELD(b, open_interval_ms)};
        const auto &a = c.async;
        j["async"] = {FIELD(a, enabled), FIELD(a, threads), FIELD(a, queue_size),
                      FIELD(a, statement_timeout_ms)};
        const auto &cu = c.cursor;
        j["cursor"] = {FIELD(cu, enabled), FIELD(cu, default_batch_size),
                       FIELD(cu, max_open_cursors), FIELD(cu, allow_scrollable)};
        j["prepared_cache"] = {FIELD(c.prepared_cache, enabled), FIELD(c.prepared_cache, max_per_connection)};
        j["interceptors"] = {FIELD(c.interceptors, enabled)};
        const auto &rl = c.rate_limit;
        j["rate_limit"] = {FIELD(rl, enabled), FIELD(rl, global_qps),
                           FIELD(rl, per_fingerprint_qps), FIELD(rl, burst), FIELD(rl, fingerprint_mode)};
        const auto &au = c.sql_audit;
        j["sql_audit"] = {FIELD(au, enabled), FIELD(au, action), FIELD(au, block_no_where_dml),
                          FIELD(au, require_limit_select), FIELD(au, enforce_read_only),
                          FIELD(au, log_blocked), FIELD(au, blacklist_fingerprints), FIELD(au, whitelist_fingerprints)};
        const auto &qc = c.query_cache;
        j["query_cache"] = {FIELD(qc, enabled), FIELD(qc, ttl_ms), FIELD(qc, max_entries),
                            FIELD(qc, max_memory_bytes), FIELD(qc, cache_on_replica_only)};
        const auto &sl = c.observability.sql_log;
        j["observability"]["sql_log"] = {FIELD(sl, enabled), FIELD(sl, mode), FIELD(sl, level),
            FIELD(sl, log_success), FIELD(sl, log_errors), FIELD(sl, slow_only), FIELD(sl, sample_rate),
            FIELD(sl, max_sql_length), FIELD(sl, max_param_length),
            FIELD(sl, include_string_values), FIELD(sl, include_blob_values)};
        const auto &ss = c.observability.slow_sql;
        j["observability"]["slow_sql"] = {FIELD(ss, enabled), FIELD(ss, threshold_ms),
            FIELD(ss, aggregate_capacity), FIELD(ss, recent_capacity), FIELD(ss, retain_rendered_sql),
            FIELD(ss, max_sql_length), FIELD(ss, histogram_buckets_ms)};
        j["observability"]["pool_metrics"] = {FIELD(c.observability.pool_metrics, enabled)};
        const auto &sr = c.observability.stats_report;
        j["observability"]["stats_report"] = {FIELD(sr, enabled), FIELD(sr, interval_ms),
            FIELD(sr, file), FIELD(sr, format), FIELD(sr, include_pool), FIELD(sr, include_slow_sql), FIELD(sr, slow_sql_limit)};
        j["default_datasource"] = c.default_datasource;
        j["heartbeat_interval_ms"] = c.heartbeat_interval_ms;
        j["datasources"] = json::array();
        for (const auto &d : c.datasources) {
            json extra = json::object();
            for (const auto &e : d.extra) extra[e.first] = "[redacted]";
            const auto &o = d.oracle;
            j["datasources"].push_back({FIELD(d, name), FIELD(d, type), FIELD(d, host),
                FIELD(d, port), FIELD(d, user), FIELD(d, database),
                {"password", "[redacted]"}, FIELD(d, password_env),
                {"dsn", d.dsn.empty() ? "" : "[redacted]"},
                FIELD(d, connection_timeout_ms), FIELD(d, socket_timeout_ms),
                FIELD(d, query_timeout_ms), FIELD(d, max_result_rows), {"extra", extra},
                {"tls", {{"enabled", d.tls_enabled}, {"verify_peer", d.tls_verify_peer},
                         {"ca", d.tls_ca}, {"cert", d.tls_cert}, {"key", "[redacted]"}}},
                {"oracle", {FIELD(o, service_name), FIELD(o, sid),
                    {"wallet_location", o.wallet_location.empty() ? "" : "[redacted]"},
                    FIELD(o, server_cert_dn), FIELD(o, charset_id), FIELD(o, lob_max_bytes), FIELD(o, blob_bind)}}});
        }
        j["groups"] = json::array();
        for (const auto &g : c.groups) {
            json replicas = json::array();
            for (const auto &v : g.replicas) replicas.push_back({FIELD(v, name), FIELD(v, weight)});
            const auto &f = g.failover;
            const auto &w = f.write_buffer;
            j["groups"].push_back({FIELD(g, name), FIELD(g, primary), {"replicas", replicas},
                FIELD(g, read_after_write_ms), FIELD(g, fallback_to_primary), FIELD(g, read_only), FIELD(g, shadow),
                {"failover", {FIELD(f, primaries), FIELD(f, acknowledge_external_fencing), FIELD(f, require_healthy),
                    {"write_buffer", {FIELD(w, enabled), FIELD(w, acknowledge_data_loss_and_duplicates),
                        FIELD(w, max_queue), FIELD(w, ttl_ms), FIELD(w, flush_interval_ms)}}}}});
        }
#undef FIELD
        return j.dump(2);
    }
}
