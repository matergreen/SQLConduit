#include "sqlconduit/config/config_loader.h"
#include "yaml_parser.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <iterator>
#include <initializer_list>
#include <utility>

namespace sqlconduit::config {
    using json = nlohmann::json;

    namespace {
        bool hasYamlExtension(const std::string &path) {
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return (lower.size() >= 5 &&
                    lower.compare(lower.size() - 5, 5, ".yaml") == 0) ||
                   (lower.size() >= 4 &&
                    lower.compare(lower.size() - 4, 4, ".yml") == 0);
        }

        bool hasJsonExtension(const std::string &path) {
            std::string lower = path;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return lower.size() >= 5 &&
                   lower.compare(lower.size() - 5, 5, ".json") == 0;
        }

        bool looksLikeYaml(const std::string &content) {
            const auto first = std::find_if_not(content.begin(), content.end(),
                                                [](const unsigned char c) {
                                                    return std::isspace(c) != 0;
                                                });
            return first != content.end() && *first != '{' && *first != '[';
        }

        bool parseInt64(const std::string &text, std::int64_t &out) {
            if (text.empty()) return false;
            errno = 0;
            char *end = nullptr;
            const long long value = std::strtoll(text.c_str(), &end, 10);
            if (errno == ERANGE || end != text.c_str() + text.size()) return false;
            out = static_cast<std::int64_t>(value);
            return true;
        }

        bool rejectUnknownFields(const json &object,
                                 const std::initializer_list<const char *> allowed,
                                 const std::string &path,
                                 std::string &error) {
            for (auto it = object.begin(); it != object.end(); ++it) {
                const bool known = std::any_of(
                    allowed.begin(), allowed.end(), [&](const char *field) {
                        return it.key() == field;
                    });
                if (!known) {
                    error = "config error [unknown_field] at " + path + "/" + it.key()
                            + ": field is not defined by sqlconduit.schema.json";
                    return false;
                }
            }
            return true;
        }
    }

    bool ConfigLoader::loadFromFile(const std::string &path, GlobalConfig &out, std::string &error) {
        out = GlobalConfig{};
        error.clear();
        std::ifstream f(path);
        if (!f) {
            error = "cannot open config file: " + path;
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        if (content.size() >= 3 &&
            static_cast<unsigned char>(content[0]) == 0xef &&
            static_cast<unsigned char>(content[1]) == 0xbb &&
            static_cast<unsigned char>(content[2]) == 0xbf) {
            content.erase(0, 3);
        }
        json j;
        if (hasYamlExtension(path) || (!hasJsonExtension(path) && looksLikeYaml(content))) {
            std::string parseError;
            if (!detail::parseYaml(content, j, parseError)) {
                error = "yaml parse error: " + parseError;
                return false;
            }
        } else {
            try {
                j = json::parse(content);
            } catch (const json::exception &e) {
                error = std::string("json parse error: ") + e.what();
                return false;
            }
        }

        try {
            if (!j.is_object()) {
                error = "config error [type] at /: root must be an object";
                return false;
            }
            if (!rejectUnknownFields(j, {
                    "$schema", "default_datasource", "heartbeat_interval_ms", "pool", "retry",
                    "circuit_breaker", "observability", "rate_limit", "sql_audit", "query_cache",
                    "cursor", "prepared_cache", "async", "interceptors", "datasources", "groups"
                }, "", error)) return false;
            out.default_datasource = j.value("default_datasource", std::string());
            out.heartbeat_interval_ms = j.value("heartbeat_interval_ms", 5000);
            if (out.heartbeat_interval_ms < 0) {
                error = "config error [minimum] at /heartbeat_interval_ms: must be >= 0";
                return false;
            }

            if (j.contains("pool")) {
                if (!j["pool"].is_object()) {
                    error = "config error [type] at /pool: must be an object";
                    return false;
                }
                if (!rejectUnknownFields(j["pool"], {
                        "enabled", "min", "max", "borrow_timeout_ms", "idle_timeout_ms",
                        "max_lifetime_ms", "leak_detection_threshold_ms", "validation_interval_ms"
                    }, "/pool", error)) return false;
                out.pool.enabled = j["pool"].value("enabled", out.pool.enabled);
                out.pool.min = j["pool"].value("min", out.pool.min);
                out.pool.max = j["pool"].value("max", out.pool.max);
                out.pool.borrow_timeout_ms = j["pool"].value("borrow_timeout_ms", out.pool.borrow_timeout_ms);
                out.pool.idle_timeout_ms = j["pool"].value("idle_timeout_ms", out.pool.idle_timeout_ms);
                out.pool.max_lifetime_ms = j["pool"].value("max_lifetime_ms", out.pool.max_lifetime_ms);
                out.pool.leak_detection_threshold_ms = j["pool"].value("leak_detection_threshold_ms",
                                                                       out.pool.leak_detection_threshold_ms);
                out.pool.validation_interval_ms = j["pool"].value("validation_interval_ms",
                                                                  out.pool.validation_interval_ms);
            }
            if (out.pool.min < 0 || out.pool.max < 1 || out.pool.min > out.pool.max ||
                out.pool.borrow_timeout_ms < 0 || out.pool.idle_timeout_ms < 0 ||
                out.pool.max_lifetime_ms < 0 || out.pool.leak_detection_threshold_ms < 0 ||
                out.pool.validation_interval_ms < 0) {
                error = "config error [invalid_value] at /pool: require 0 <= min <= max, max >= 1, "
                        "and non-negative timeout values";
                return false;
            }

            if (j.contains("retry")) {
                if (!j["retry"].is_object()) {
                    error = "retry must be an object";
                    return false;
                }
                const auto &retry = j["retry"];
                if (!rejectUnknownFields(retry, {
                        "max_attempts", "initial_backoff_ms", "max_backoff_ms", "retry_writes"
                    }, "/retry", error)) return false;
                out.retry.max_attempts = retry.value("max_attempts", out.retry.max_attempts);
                out.retry.initial_backoff_ms = retry.value(
                    "initial_backoff_ms", out.retry.initial_backoff_ms);
                out.retry.max_backoff_ms = retry.value("max_backoff_ms", out.retry.max_backoff_ms);
                out.retry.retry_writes = retry.value("retry_writes", out.retry.retry_writes);
                if (out.retry.max_attempts < 1 || out.retry.initial_backoff_ms < 0 ||
                    out.retry.max_backoff_ms < out.retry.initial_backoff_ms) {
                    error = "invalid retry configuration";
                    return false;
                }
            }
            if (j.contains("circuit_breaker")) {
                if (!j["circuit_breaker"].is_object()) {
                    error = "circuit_breaker must be an object";
                    return false;
                }
                const auto &breaker = j["circuit_breaker"];
                if (!rejectUnknownFields(breaker, {"failure_threshold", "open_interval_ms"},
                                         "/circuit_breaker", error)) return false;
                out.circuit_breaker.failure_threshold = breaker.value(
                    "failure_threshold", out.circuit_breaker.failure_threshold);
                out.circuit_breaker.open_interval_ms = breaker.value(
                    "open_interval_ms", out.circuit_breaker.open_interval_ms);
                if (out.circuit_breaker.failure_threshold < 0 ||
                    out.circuit_breaker.open_interval_ms < 1) {
                    error = "invalid circuit_breaker configuration";
                    return false;
                }
            }

            if (j.contains("observability")) {
                if (!j["observability"].is_object()) {
                    error = "observability must be an object";
                    return false;
                }
                const auto &observability = j["observability"];
                if (!rejectUnknownFields(observability, {
                        "sql_log", "slow_sql", "pool_metrics", "stats_report"
                    }, "/observability", error)) return false;
                if (observability.contains("sql_log")) {
                    if (!observability["sql_log"].is_object()) {
                        error = "observability.sql_log must be an object";
                        return false;
                    }
                    const auto &sqlLog = observability["sql_log"];
                    if (!rejectUnknownFields(sqlLog, {
                            "enabled", "mode", "level", "log_success", "log_errors", "slow_only",
                            "sample_rate", "max_sql_length", "max_param_length",
                            "include_string_values", "include_blob_values"
                        }, "/observability/sql_log", error)) return false;
                    auto &cfg = out.observability.sql_log;
                    cfg.enabled = sqlLog.value("enabled", cfg.enabled);
                    cfg.mode = sqlLog.value("mode", cfg.mode);
                    cfg.level = sqlLog.value("level", cfg.level);
                    cfg.log_success = sqlLog.value("log_success", cfg.log_success);
                    cfg.log_errors = sqlLog.value("log_errors", cfg.log_errors);
                    cfg.slow_only = sqlLog.value("slow_only", cfg.slow_only);
                    cfg.sample_rate = sqlLog.value("sample_rate", cfg.sample_rate);
                    cfg.max_sql_length = sqlLog.value("max_sql_length", cfg.max_sql_length);
                    cfg.max_param_length = sqlLog.value("max_param_length", cfg.max_param_length);
                    cfg.include_string_values = sqlLog.value(
                        "include_string_values", cfg.include_string_values);
                    cfg.include_blob_values = sqlLog.value(
                        "include_blob_values", cfg.include_blob_values);
                    if ((cfg.mode != "template" && cfg.mode != "full") ||
                        (cfg.level != "debug" && cfg.level != "info" &&
                         cfg.level != "warn" && cfg.level != "error") ||
                        cfg.sample_rate < 0.0 || cfg.sample_rate > 1.0 ||
                        cfg.max_sql_length < 64 || cfg.max_param_length < 1) {
                        error = "invalid observability.sql_log configuration";
                        return false;
                    }
                }
                if (observability.contains("slow_sql")) {
                    if (!observability["slow_sql"].is_object()) {
                        error = "observability.slow_sql must be an object";
                        return false;
                    }
                    const auto &slowSql = observability["slow_sql"];
                    if (!rejectUnknownFields(slowSql, {
                            "enabled", "threshold_ms", "aggregate_capacity", "recent_capacity",
                            "retain_rendered_sql", "max_sql_length", "histogram_buckets_ms"
                        }, "/observability/slow_sql", error)) return false;
                    auto &cfg = out.observability.slow_sql;
                    cfg.enabled = slowSql.value("enabled", cfg.enabled);
                    cfg.threshold_ms = slowSql.value("threshold_ms", cfg.threshold_ms);
                    cfg.aggregate_capacity = slowSql.value(
                        "aggregate_capacity", cfg.aggregate_capacity);
                    cfg.recent_capacity = slowSql.value("recent_capacity", cfg.recent_capacity);
                    cfg.retain_rendered_sql = slowSql.value(
                        "retain_rendered_sql", cfg.retain_rendered_sql);
                    cfg.max_sql_length = slowSql.value("max_sql_length", cfg.max_sql_length);
                    if (slowSql.contains("histogram_buckets_ms")) {
                        cfg.histogram_buckets_ms = slowSql["histogram_buckets_ms"]
                                .get<std::vector<int> >();
                    }
                    if (cfg.threshold_ms < 0 || cfg.aggregate_capacity < 1 ||
                        cfg.recent_capacity < 0 || cfg.max_sql_length < 64 ||
                        cfg.histogram_buckets_ms.empty()) {
                        error = "invalid observability.slow_sql configuration";
                        return false;
                    }
                    int previous = -1;
                    for (const int bucket: cfg.histogram_buckets_ms) {
                        if (bucket <= 0 || bucket <= previous) {
                            error = "slow_sql histogram buckets must be positive and increasing";
                            return false;
                        }
                        previous = bucket;
                    }
                }
                if (observability.contains("pool_metrics")) {
                    if (!observability["pool_metrics"].is_object()) {
                        error = "observability.pool_metrics must be an object";
                        return false;
                    }
                    if (!rejectUnknownFields(observability["pool_metrics"], {"enabled"},
                                             "/observability/pool_metrics", error)) return false;
                    out.observability.pool_metrics.enabled = observability["pool_metrics"].value(
                        "enabled", out.observability.pool_metrics.enabled);
                }
                if (observability.contains("stats_report")) {
                    if (!observability["stats_report"].is_object()) {
                        error = "observability.stats_report must be an object";
                        return false;
                    }
                    const auto &report = observability["stats_report"];
                    if (!rejectUnknownFields(report, {
                            "enabled", "interval_ms", "file", "format", "include_pool",
                            "include_slow_sql", "slow_sql_limit"
                        }, "/observability/stats_report", error)) return false;
                    auto &cfg = out.observability.stats_report;
                    cfg.enabled = report.value("enabled", cfg.enabled);
                    cfg.interval_ms = report.value("interval_ms", cfg.interval_ms);
                    cfg.file = report.value("file", cfg.file);
                    cfg.format = report.value("format", cfg.format);
                    cfg.include_pool = report.value("include_pool", cfg.include_pool);
                    cfg.include_slow_sql = report.value("include_slow_sql", cfg.include_slow_sql);
                    cfg.slow_sql_limit = report.value("slow_sql_limit", cfg.slow_sql_limit);
                    if (cfg.interval_ms < 1000) {
                        error = "config error [minimum] at /observability/stats_report/interval_ms: "
                                "must be >= 1000";
                        return false;
                    }
                    if (cfg.format != "text" && cfg.format != "json") {
                        error = "observability.stats_report.format must be 'text' or 'json'";
                        return false;
                    }
                    if (cfg.slow_sql_limit < 0) {
                        error = "invalid observability.stats_report configuration";
                        return false;
                    }
                }
            }

            if (j.contains("rate_limit")) {
                if (!j["rate_limit"].is_object()) {
                    error = "rate_limit must be an object";
                    return false;
                }
                const auto &rl = j["rate_limit"];
                if (!rejectUnknownFields(rl, {
                        "enabled", "global_qps", "per_fingerprint_qps", "burst", "fingerprint_mode"
                    }, "/rate_limit", error)) return false;
                out.rate_limit.enabled = rl.value("enabled", out.rate_limit.enabled);
                out.rate_limit.global_qps = rl.value("global_qps", out.rate_limit.global_qps);
                out.rate_limit.per_fingerprint_qps = rl.value(
                    "per_fingerprint_qps", out.rate_limit.per_fingerprint_qps);
                out.rate_limit.burst = rl.value("burst", out.rate_limit.burst);
                out.rate_limit.fingerprint_mode = rl.value(
                    "fingerprint_mode", out.rate_limit.fingerprint_mode);
                if (out.rate_limit.global_qps < 0 || out.rate_limit.per_fingerprint_qps < 0 ||
                    out.rate_limit.burst < 0) {
                    error = "invalid rate_limit configuration";
                    return false;
                }
                const auto &fm = out.rate_limit.fingerprint_mode;
                if (fm != "off" && fm != "template" && fm != "full") {
                    error = "rate_limit.fingerprint_mode must be off/template/full";
                    return false;
                }
            }

            if (j.contains("sql_audit")) {
                if (!j["sql_audit"].is_object()) {
                    error = "sql_audit must be an object";
                    return false;
                }
                const auto &audit = j["sql_audit"];
                if (!rejectUnknownFields(audit, {
                        "enabled", "action", "block_no_where_dml", "require_limit_select",
                        "enforce_read_only", "log_blocked", "blacklist_fingerprints",
                        "whitelist_fingerprints"
                    }, "/sql_audit", error)) return false;
                out.sql_audit.enabled = audit.value("enabled", out.sql_audit.enabled);
                out.sql_audit.action = audit.value("action", out.sql_audit.action);
                out.sql_audit.block_no_where_dml = audit.value(
                    "block_no_where_dml", out.sql_audit.block_no_where_dml);
                out.sql_audit.require_limit_select = audit.value(
                    "require_limit_select", out.sql_audit.require_limit_select);
                out.sql_audit.enforce_read_only = audit.value(
                    "enforce_read_only", out.sql_audit.enforce_read_only);
                out.sql_audit.log_blocked = audit.value(
                    "log_blocked", out.sql_audit.log_blocked);
                if (audit.contains("blacklist_fingerprints"))
                    out.sql_audit.blacklist_fingerprints =
                            audit["blacklist_fingerprints"].get<std::vector<std::uint64_t> >();
                if (audit.contains("whitelist_fingerprints"))
                    out.sql_audit.whitelist_fingerprints =
                            audit["whitelist_fingerprints"].get<std::vector<std::uint64_t> >();
                if (out.sql_audit.action != "block" && out.sql_audit.action != "warn") {
                    error = "sql_audit.action must be block/warn";
                    return false;
                }
            }

            if (j.contains("query_cache")) {
                if (!j["query_cache"].is_object()) {
                    error = "query_cache must be an object";
                    return false;
                }
                const auto &qc = j["query_cache"];
                if (!rejectUnknownFields(qc, {
                        "enabled", "ttl_ms", "max_entries", "max_memory_bytes",
                        "cache_on_replica_only"
                    }, "/query_cache", error)) return false;
                out.query_cache.enabled = qc.value("enabled", out.query_cache.enabled);
                out.query_cache.ttl_ms = qc.value("ttl_ms", out.query_cache.ttl_ms);
                out.query_cache.max_entries = qc.value("max_entries", out.query_cache.max_entries);
                out.query_cache.max_memory_bytes = qc.value(
                    "max_memory_bytes", out.query_cache.max_memory_bytes);
                out.query_cache.cache_on_replica_only = qc.value(
                    "cache_on_replica_only", out.query_cache.cache_on_replica_only);
                if (out.query_cache.ttl_ms < 0 || out.query_cache.max_entries < 0 ||
                    out.query_cache.max_memory_bytes < 0) {
                    error = "invalid query_cache configuration";
                    return false;
                }
            }

            if (j.contains("cursor")) {
                if (!j["cursor"].is_object()) {
                    error = "cursor must be an object";
                    return false;
                }
                const auto &cur = j["cursor"];
                if (!rejectUnknownFields(cur, {
                        "enabled", "default_batch_size", "max_open_cursors", "allow_scrollable"
                    }, "/cursor", error)) return false;
                out.cursor.enabled = cur.value("enabled", out.cursor.enabled);
                out.cursor.default_batch_size = cur.value(
                    "default_batch_size", out.cursor.default_batch_size);
                out.cursor.max_open_cursors = cur.value(
                    "max_open_cursors", out.cursor.max_open_cursors);
                out.cursor.allow_scrollable = cur.value(
                    "allow_scrollable", out.cursor.allow_scrollable);
                if (out.cursor.default_batch_size < 0 || out.cursor.max_open_cursors < 0) {
                    error = "invalid cursor configuration";
                    return false;
                }
            }

            if (j.contains("prepared_cache")) {
                if (!j["prepared_cache"].is_object()) {
                    error = "prepared_cache must be an object";
                    return false;
                }
                const auto &pc = j["prepared_cache"];
                if (!rejectUnknownFields(pc, {"enabled", "max_per_connection"},
                                         "/prepared_cache", error)) return false;
                out.prepared_cache.enabled = pc.value("enabled", out.prepared_cache.enabled);
                out.prepared_cache.max_per_connection = pc.value(
                    "max_per_connection", out.prepared_cache.max_per_connection);
                if (out.prepared_cache.max_per_connection < 0) {
                    error = "invalid prepared_cache configuration: max_per_connection must be >= 0";
                    return false;
                }
            }

            if (j.contains("interceptors")) {
                if (!j["interceptors"].is_object()) {
                    error = "interceptors must be an object";
                    return false;
                }
                const auto &ic = j["interceptors"];
                if (!rejectUnknownFields(ic, {"enabled"}, "/interceptors", error)) return false;
                out.interceptors.enabled = ic.value("enabled", out.interceptors.enabled);
            }

            if (j.contains("async")) {
                if (!j["async"].is_object()) {
                    error = "async must be an object";
                    return false;
                }
                const auto &as = j["async"];
                if (!rejectUnknownFields(as, {
                        "enabled", "threads", "queue_size", "statement_timeout_ms"
                    }, "/async", error)) return false;
                out.async.enabled = as.value("enabled", out.async.enabled);
                out.async.threads = as.value("threads", out.async.threads);
                out.async.queue_size = as.value("queue_size", out.async.queue_size);
                out.async.statement_timeout_ms = as.value(
                    "statement_timeout_ms", out.async.statement_timeout_ms);
                if (out.async.threads < 0 || out.async.queue_size < 1 ||
                    out.async.statement_timeout_ms < 0) {
                    error = "invalid async configuration";
                    return false;
                }
            }

            if (!j.contains("datasources") || !j["datasources"].is_array()) {
                error = "missing 'datasources' array";
                return false;
            }
            if (j["datasources"].empty()) {
                error = "config error [min_items] at /datasources: at least one datasource is required";
                return false;
            }

            std::size_t datasourceIndex = 0;
            for (const auto &d: j["datasources"]) {
                const std::string datasourcePath = "/datasources/" +
                                                   std::to_string(datasourceIndex++);
                if (!d.is_object()) {
                    error = "config error [type] at " + datasourcePath + ": must be an object";
                    return false;
                }
                if (!rejectUnknownFields(d, {
                        "name", "type", "host", "port", "user", "password", "password_env",
                        "database", "dsn", "connection_timeout_ms", "socket_timeout_ms",
                        "query_timeout_ms", "max_result_rows", "tls", "oracle", "extra"
                    }, datasourcePath, error)) return false;
                DataSourceConfig cfg;
                cfg.name = d.value("name", std::string());
                cfg.type = d.value("type", std::string());
                cfg.host = d.value("host", std::string());
                cfg.port = d.value("port", 0);
                cfg.user = d.value("user", std::string());
                cfg.password = d.value("password", std::string());
                cfg.password_env = d.value("password_env", std::string());
                cfg.database = d.value("database", std::string());
                cfg.dsn = d.value("dsn", std::string());
                cfg.connection_timeout_ms = d.value("connection_timeout_ms", 5000);
                cfg.socket_timeout_ms = d.value("socket_timeout_ms", 0);
                cfg.query_timeout_ms = d.value("query_timeout_ms", 0);
                cfg.max_result_rows = d.value("max_result_rows", 0);
                if (cfg.port < 0 || cfg.port > 65535) {
                    error = "config error [range] at " + datasourcePath
                            + "/port: must be in range 0..65535";
                    return false;
                }
                if (cfg.connection_timeout_ms < 0 || cfg.socket_timeout_ms < 0 ||
                    cfg.query_timeout_ms < 0 || cfg.max_result_rows < 0) {
                    error = "datasource '" + cfg.name
                            + "' timeout and max_result_rows values must be >= 0";
                    return false;
                }
                if (!cfg.password_env.empty()) {
                    const char *secret = std::getenv(cfg.password_env.c_str());
                    if (!secret) {
                        error = "datasource '" + cfg.name + "' references missing password_env '"
                                + cfg.password_env + "'";
                        return false;
                    }
                    cfg.password = secret;
                }
                if (d.contains("tls")) {
                    if (!d["tls"].is_object()) {
                        error = "datasource '" + cfg.name + "' tls must be an object";
                        return false;
                    }
                    const auto &tls = d["tls"];
                    if (!rejectUnknownFields(tls, {"enabled", "verify_peer", "ca", "cert", "key"},
                                             datasourcePath + "/tls", error)) return false;
                    cfg.tls_enabled = tls.value("enabled", true);
                    cfg.tls_verify_peer = tls.value("verify_peer", true);
                    cfg.tls_ca = tls.value("ca", std::string());
                    cfg.tls_cert = tls.value("cert", std::string());
                    cfg.tls_key = tls.value("key", std::string());
                }
                if (d.contains("extra")) {
                    if (!d["extra"].is_object()) {
                        error = "config error [type] at " + datasourcePath
                                + "/extra: must be an object";
                        return false;
                    }
                    for (auto it = d["extra"].begin(); it != d["extra"].end(); ++it)
                        cfg.extra[it.key()] = it.value().get<std::string>();
                }
                if (cfg.name.empty()) {
                    error = "datasource missing 'name'";
                    return false;
                }
                if (cfg.type.empty()) {
                    error = "datasource '" + cfg.name + "' missing 'type'";
                    return false;
                }
                if (d.contains("oracle") && cfg.type != "oracle") {
                    error = "datasource '" + cfg.name
                            + "' has an oracle block but type is not 'oracle'";
                    return false;
                }
                if (cfg.type == "oracle") {
                    bool hasService = false;
                    bool hasSid = false;
                    bool hasWallet = false;
                    bool hasServerDn = false;
                    bool hasCharset = false;
                    bool hasLobLimit = false;
                    bool hasBlobBind = false;
                    if (d.contains("oracle")) {
                        if (!d["oracle"].is_object()) {
                            error = "datasource '" + cfg.name + "' oracle must be an object";
                            return false;
                        }
                        const auto &oracle = d["oracle"];
                        if (!rejectUnknownFields(oracle, {
                                "service_name", "sid", "wallet_location", "server_cert_dn",
                                "charset_id", "lob_max_bytes", "blob_bind"
                            }, datasourcePath + "/oracle", error)) return false;
                        cfg.oracle.service_name = oracle.value("service_name", std::string());
                        cfg.oracle.sid = oracle.value("sid", std::string());
                        cfg.oracle.wallet_location = oracle.value(
                            "wallet_location", std::string());
                        cfg.oracle.server_cert_dn = oracle.value(
                            "server_cert_dn", std::string());
                        cfg.oracle.charset_id = oracle.value("charset_id", cfg.oracle.charset_id);
                        cfg.oracle.lob_max_bytes = oracle.value(
                            "lob_max_bytes", cfg.oracle.lob_max_bytes);
                        cfg.oracle.blob_bind = oracle.value("blob_bind", cfg.oracle.blob_bind);
                        hasService = oracle.contains("service_name");
                        hasSid = oracle.contains("sid");
                        hasWallet = oracle.contains("wallet_location");
                        hasServerDn = oracle.contains("server_cert_dn");
                        hasCharset = oracle.contains("charset_id");
                        hasLobLimit = oracle.contains("lob_max_bytes");
                        hasBlobBind = oracle.contains("blob_bind");
                    }
                    const auto legacy = [&cfg](const char *key) -> const std::string * {
                        const auto it = cfg.extra.find(key);
                        return it == cfg.extra.end() ? nullptr : &it->second;
                    };
                    if (!hasService)
                        if (const auto *v = legacy("service_name")) cfg.oracle.service_name = *v;
                    if (!hasSid)
                        if (const auto *v = legacy("sid")) cfg.oracle.sid = *v;
                    if (!hasWallet)
                        if (const auto *v = legacy("wallet_location"))
                            cfg.oracle.wallet_location = *v;
                    if (!hasServerDn)
                        if (const auto *v = legacy("server_cert_dn"))
                            cfg.oracle.server_cert_dn = *v;
                    if (!hasCharset) {
                        if (const auto *v = legacy("charset_id")) {
                            std::int64_t parsed = 0;
                            if (!parseInt64(*v, parsed) || parsed < 1 || parsed > 65535) {
                                error = "datasource '" + cfg.name
                                        + "' extra.charset_id must be in range 1..65535";
                                return false;
                            }
                            cfg.oracle.charset_id = static_cast<int>(parsed);
                        }
                    }
                    if (!hasLobLimit) {
                        if (const auto *v = legacy("lob_max_bytes")) {
                            std::int64_t parsed = 0;
                            if (!parseInt64(*v, parsed) || parsed < 1) {
                                error = "datasource '" + cfg.name
                                        + "' extra.lob_max_bytes must be > 0";
                                return false;
                            }
                            cfg.oracle.lob_max_bytes = parsed;
                        }
                    }
                    if (!hasBlobBind)
                        if (const auto *v = legacy("blob_bind")) cfg.oracle.blob_bind = *v;

                    if (cfg.oracle.charset_id < 1 || cfg.oracle.charset_id > 65535 ||
                        cfg.oracle.lob_max_bytes < 1 ||
                        (cfg.oracle.blob_bind != "auto" && cfg.oracle.blob_bind != "raw" &&
                         cfg.oracle.blob_bind != "lob")) {
                        error = "datasource '" + cfg.name + "' has invalid oracle configuration";
                        return false;
                    }
                    if (!cfg.oracle.service_name.empty() && !cfg.oracle.sid.empty()) {
                        error = "datasource '" + cfg.name
                                + "' oracle.service_name and oracle.sid are mutually exclusive";
                        return false;
                    }
                    if (!cfg.oracle.sid.empty() && !cfg.database.empty()) {
                        error = "datasource '" + cfg.name
                                + "' cannot combine database (service-name alias) with oracle.sid";
                        return false;
                    }
                    const bool rawConnect = cfg.extra.find("connection_string") != cfg.extra.end();
                    if (!rawConnect && cfg.dsn.empty() && cfg.oracle.service_name.empty() &&
                        cfg.oracle.sid.empty() && cfg.database.empty()) {
                        error = "datasource '" + cfg.name
                                + "' requires oracle.service_name, oracle.sid, database, dsn, or "
                                "extra.connection_string";
                        return false;
                    }
                    if ((rawConnect || !cfg.dsn.empty()) && cfg.tls_enabled) {
                        error = "datasource '" + cfg.name
                                + "' uses dsn/extra.connection_string; encode TCPS and certificate "
                                "verification in that value or Oracle Net configuration instead "
                                "of using tls";
                        return false;
                    }
                    if (!cfg.tls_ca.empty() && cfg.oracle.wallet_location.empty())
                        cfg.oracle.wallet_location = cfg.tls_ca;
                    if (!cfg.tls_enabled && (!cfg.oracle.wallet_location.empty() ||
                                             !cfg.oracle.server_cert_dn.empty())) {
                        error = "datasource '" + cfg.name
                                + "' oracle.wallet_location/server_cert_dn require tls.enabled";
                        return false;
                    }
                    if (!cfg.oracle.server_cert_dn.empty() && !cfg.tls_verify_peer) {
                        error = "datasource '" + cfg.name
                                + "' oracle.server_cert_dn requires tls.verify_peer=true";
                        return false;
                    }
                    if (cfg.tls_enabled && (!cfg.tls_cert.empty() || !cfg.tls_key.empty())) {
                        error = "datasource '" + cfg.name
                                + "' Oracle TLS does not consume tls.cert/tls.key directly; "
                                "configure oracle.wallet_location";
                        return false;
                    }
                } else if (cfg.tls_verify_peer && cfg.tls_enabled && cfg.tls_ca.empty()) {
                    error = "datasource '" + cfg.name
                            + "' enables TLS peer verification but tls.ca is empty";
                    return false;
                }
                out.datasources.push_back(std::move(cfg));
            }

            if (j.contains("groups")) {
                if (!j["groups"].is_array()) {
                    error = "groups must be an array";
                    return false;
                }
                std::size_t groupIndex = 0;
                for (const auto &g: j["groups"]) {
                    const std::string groupPath = "/groups/" + std::to_string(groupIndex++);
                    if (!g.is_object()) {
                        error = "config error [type] at " + groupPath + ": must be an object";
                        return false;
                    }
                    if (!rejectUnknownFields(g, {
                            "name", "primary", "replicas", "read_after_write_ms",
                            "fallback_to_primary", "read_only", "failover", "shadow"
                        }, groupPath, error)) return false;
                    DataSourceGroupConfig group;
                    group.name = g.value("name", std::string());
                    group.primary = g.value("primary", std::string());
                    group.read_after_write_ms = g.value("read_after_write_ms", 0);
                    group.fallback_to_primary = g.value("fallback_to_primary", true);
                    if (group.name.empty() || group.primary.empty() ||
                        group.read_after_write_ms < 0) {
                        error = "invalid datasource group";
                        return false;
                    }
                    if (g.contains("replicas")) {
                        if (!g["replicas"].is_array()) {
                            error = "group '" + group.name + "' replicas must be an array";
                            return false;
                        }
                        std::size_t replicaIndex = 0;
                        for (const auto &r: g["replicas"]) {
                            ReplicaConfig replica;
                            if (r.is_string()) {
                                replica.name = r.get<std::string>();
                            } else if (r.is_object()) {
                                if (!rejectUnknownFields(
                                        r, {"name", "weight"}, groupPath + "/replicas/" +
                                                               std::to_string(replicaIndex), error))
                                    return false;
                                replica.name = r.value("name", std::string());
                                replica.weight = r.value("weight", 1);
                            } else {
                                error = "invalid replica in group '" + group.name + "'";
                                return false;
                            }
                            if (replica.name.empty() || replica.weight < 1 || replica.weight > 100) {
                                error = "invalid replica in group '" + group.name + "'";
                                return false;
                            }
                            group.replicas.push_back(std::move(replica));
                            ++replicaIndex;
                        }
                    }
                    group.read_only = g.value("read_only", false);
                    if (!group.replicas.empty() && group.read_after_write_ms == 0) {
                        std::cerr
                                << "sqlconduit WARN: datasource group '" << group.name << "' has "
                                << group.replicas.size() << " replica(s) but "
                                << "read_after_write_ms=0; writes-then-reads may be served by "
                                << "replicas and return stale data. "
                                << "Set read_after_write_ms > 0 (e.g. 1000) to pin post-write "
                                << "reads to the primary.\n";
                    }
                    if (g.contains("failover") && !g["failover"].is_object()) {
                        error = "group '" + group.name + "' failover must be an object";
                        return false;
                    }
                    if (g.contains("failover")) {
                        const auto &fo = g["failover"];
                        if (!rejectUnknownFields(fo, {
                                "primaries", "acknowledge_external_fencing", "require_healthy",
                                "write_buffer"
                            }, groupPath + "/failover", error)) return false;
                        if (fo.contains("primaries") && !fo["primaries"].is_array()) {
                            error = "group '" + group.name + "' failover.primaries must be an array";
                            return false;
                        }
                        if (fo.contains("primaries")) {
                            for (const auto &p: fo["primaries"]) {
                                if (!p.is_string() || p.get<std::string>().empty()) {
                                    error = "group '" + group.name
                                            + "' failover.primaries must be non-empty strings";
                                    return false;
                                }
                                group.failover.primaries.push_back(p.get<std::string>());
                            }
                        }
                        group.failover.require_healthy = fo.value("require_healthy", false);
                        group.failover.acknowledge_external_fencing =
                                fo.value("acknowledge_external_fencing", false);
                        if (fo.contains("write_buffer") && !fo["write_buffer"].is_object()) {
                            error = "group '" + group.name + "' failover.write_buffer must be an object";
                            return false;
                        }
                        if (fo.contains("write_buffer")) {
                            const auto &wb = fo["write_buffer"];
                            if (!rejectUnknownFields(wb, {
                                    "enabled", "acknowledge_data_loss_and_duplicates", "max_queue",
                                    "ttl_ms", "flush_interval_ms"
                                }, groupPath + "/failover/write_buffer", error)) return false;
                            group.failover.write_buffer.enabled = wb.value("enabled", false);
                            group.failover.write_buffer.acknowledge_data_loss_and_duplicates =
                                    wb.value("acknowledge_data_loss_and_duplicates", false);
                            group.failover.write_buffer.max_queue = wb.value("max_queue", 1000);
                            group.failover.write_buffer.ttl_ms = wb.value("ttl_ms", 30000);
                            group.failover.write_buffer.flush_interval_ms =
                                    wb.value("flush_interval_ms", 1000);
                            if (group.failover.write_buffer.max_queue < 1 ||
                                group.failover.write_buffer.ttl_ms < 0 ||
                                group.failover.write_buffer.flush_interval_ms < 1) {
                                error = "invalid group '" + group.name + "' failover.write_buffer";
                                return false;
                            }
                        }
                        if (!group.failover.primaries.empty() &&
                            !group.failover.acknowledge_external_fencing) {
                            error = "group '" + group.name
                                    + "' configures automatic write failover without "
                                    "failover.acknowledge_external_fencing=true; sqlconduit does not "
                                    "perform leader election or fencing";
                            return false;
                        }
                        if (group.failover.write_buffer.enabled &&
                            !group.failover.write_buffer.acknowledge_data_loss_and_duplicates) {
                            error = "group '" + group.name
                                    + "' enables the volatile write buffer without "
                                    "failover.write_buffer.acknowledge_data_loss_and_duplicates=true";
                            return false;
                        }
                    }
                    if (g.contains("shadow") && !g["shadow"].is_string()) {
                        error = "group '" + group.name + "' shadow must be a string";
                        return false;
                    }
                    if (g.contains("shadow")) {
                        group.shadow = g["shadow"].get<std::string>();
                    }
                    out.groups.push_back(std::move(group));
                }
            }

            if (out.default_datasource.empty() && !out.datasources.empty()) {
                out.default_datasource = out.datasources.front().name;
            }
            return true;
        } catch (const json::exception &e) {
            out = GlobalConfig{};
            error = std::string("config validation error: ") + e.what();
            return false;
        }
    }
}
