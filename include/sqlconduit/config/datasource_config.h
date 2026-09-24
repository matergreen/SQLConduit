#ifndef SQLCONDUIT_CONFIG_DATASOURCE_CONFIG_H
#define SQLCONDUIT_CONFIG_DATASOURCE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace sqlconduit::config {
    struct OracleConfig {
        std::string service_name;
        std::string sid;
        std::string wallet_location;
        std::string server_cert_dn;
        int charset_id = 873;
        std::int64_t lob_max_bytes = 4194304;
        std::string blob_bind = "auto";
    };

    struct DataSourceConfig {
        std::string name;
        std::string type;
        std::string host;
        int port = 0;
        std::string user;
        std::string password;
        std::string password_env;
        std::string database;
        std::string dsn;
        int connection_timeout_ms = 5000;
        int socket_timeout_ms = 0;
        int query_timeout_ms = 0;
        int max_result_rows = 0;
        bool tls_enabled = false;
        bool tls_verify_peer = true;
        std::string tls_ca;
        std::string tls_cert;
        std::string tls_key;
        OracleConfig oracle;
        std::map<std::string, std::string> extra;

        [[nodiscard]] std::string describe() const;

        [[nodiscard]] std::string redact(std::string text) const;
    };

    struct PoolConfig {
        bool enabled = true;
        int min = 1;
        int max = 8;
        int borrow_timeout_ms = 30000;
        int idle_timeout_ms = 600000;
        int max_lifetime_ms = 1800000;
        int leak_detection_threshold_ms = 30000;
        int validation_interval_ms = 30000;
    };

    struct RetryConfig {
        int max_attempts = 1;
        int initial_backoff_ms = 50;
        int max_backoff_ms = 1000;
        bool retry_writes = false;
    };

    struct CircuitBreakerConfig {
        int failure_threshold = 0;
        int open_interval_ms = 30000;
    };

    struct SqlLogConfig {
        bool enabled = false;
        std::string mode = "template";
        std::string level = "debug";
        bool log_success = true;
        bool log_errors = true;
        bool slow_only = false;
        double sample_rate = 1.0;
        int max_sql_length = 8192;
        int max_param_length = 256;
        bool include_string_values = false;
        bool include_blob_values = false;
    };

    struct SlowSqlConfig {
        bool enabled = false;
        int threshold_ms = 500;
        int aggregate_capacity = 1000;
        int recent_capacity = 200;
        bool retain_rendered_sql = false;
        int max_sql_length = 4096;
        std::vector<int> histogram_buckets_ms{10, 50, 100, 200, 500, 1000, 3000, 10000};
    };

    struct PoolMetricsConfig {
        bool enabled = true;
    };

    struct StatsReportConfig {
        bool enabled = false;
        int interval_ms = 60000;
        std::string file;
        std::string format = "text";
        bool include_pool = true;
        bool include_slow_sql = true;
        int slow_sql_limit = 10;
    };

    struct RateLimitConfig {
        bool enabled = false;
        int global_qps = 0;
        int per_fingerprint_qps = 0;
        int burst = 0;
        std::string fingerprint_mode = "off";
    };

    struct SqlAuditConfig {
        bool enabled = false;
        std::string action = "warn";
        bool block_no_where_dml = false;
        bool require_limit_select = false;
        bool enforce_read_only = false;
        bool log_blocked = true;
        std::vector<std::uint64_t> blacklist_fingerprints;
        std::vector<std::uint64_t> whitelist_fingerprints;
    };

    struct QueryCacheConfig {
        bool enabled = false;
        int ttl_ms = 60000;
        int max_entries = 1000;
        int max_memory_bytes = 0;
        bool cache_on_replica_only = false;
    };

    struct WriteBufferConfig {
        bool enabled = false;
        bool acknowledge_data_loss_and_duplicates = false;
        int max_queue = 1000;
        int ttl_ms = 30000;
        int flush_interval_ms = 1000;
    };

    struct FailoverConfig {
        std::vector<std::string> primaries;
        bool acknowledge_external_fencing = false;
        bool require_healthy = false;
        WriteBufferConfig write_buffer;
    };

    struct ObservabilityConfig {
        SqlLogConfig sql_log;
        SlowSqlConfig slow_sql;
        PoolMetricsConfig pool_metrics;
        StatsReportConfig stats_report;
    };

    struct CursorConfig {
        bool enabled = true;
        int default_batch_size = 256;
        int max_open_cursors = 0;
        bool allow_scrollable = false;
    };

    struct PreparedCacheConfig {
        bool enabled = true;
        int max_per_connection = 128;
    };

    struct ReplicaConfig {
        std::string name;
        int weight = 1;
    };

    struct AsyncConfig {
        bool enabled = true;
        int threads = 0;
        int queue_size = 4096;
        int statement_timeout_ms = 0;
    };

    struct InterceptorsConfig {
        bool enabled = false;
    };

    struct DataSourceGroupConfig {
        std::string name;
        std::string primary;
        std::vector<ReplicaConfig> replicas;
        int read_after_write_ms = 0;
        bool fallback_to_primary = true;
        bool read_only = false;
        FailoverConfig failover;
        std::string shadow;
    };

    struct GlobalConfig {
        std::string default_datasource;
        int heartbeat_interval_ms = 5000;
        PoolConfig pool;
        RetryConfig retry;
        CircuitBreakerConfig circuit_breaker;
        ObservabilityConfig observability;
        RateLimitConfig rate_limit;
        SqlAuditConfig sql_audit;
        QueryCacheConfig query_cache;
        CursorConfig cursor;
        PreparedCacheConfig prepared_cache;
        AsyncConfig async;
        InterceptorsConfig interceptors;
        std::vector<DataSourceConfig> datasources;
        std::vector<DataSourceGroupConfig> groups;
    };
}

#endif
