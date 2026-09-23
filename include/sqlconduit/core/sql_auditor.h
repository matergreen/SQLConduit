#ifndef SQLCONDUIT_CORE_SQL_AUDITOR_H
#define SQLCONDUIT_CORE_SQL_AUDITOR_H

#include "sqlconduit/common/types.h"
#include "sqlconduit/common/observer.h"
#include "sqlconduit/config/datasource_config.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace sqlconduit::core {
    struct SqlAuditStats {
        std::uint64_t checked = 0;
        std::uint64_t warned = 0;
        std::uint64_t blocked = 0;
    };

    namespace detail {
        class SqlAuditorState {
        public:
            SqlAuditorState() = default;

            void configure(const config::SqlAuditConfig &cfg);

            common::Status check(const std::string &sql, common::OperationType type,
                                 bool readOnly = false);

            [[nodiscard]] SqlAuditStats stats() const;

        private:
            struct Policy {
                bool block = false;
                bool log_blocked = true;
                bool block_no_where_dml = false;
                bool require_limit_select = false;
                bool enforce_read_only = false;
                std::unordered_set<std::uint64_t> blacklist;
                std::unordered_set<std::uint64_t> whitelist;
                bool needsFingerprint = false;
                bool needsKind = false;
            };

            std::atomic<bool> enabled_{false};
            mutable std::mutex mtx_;
            std::shared_ptr<const Policy> policy_;
            std::atomic<std::uint64_t> checked_{0};
            std::atomic<std::uint64_t> warned_{0};
            std::atomic<std::uint64_t> blocked_{0};
        };
    }

    // Compatibility facade for the process-wide default runtime.
    class SqlAuditor {
    public:
        SqlAuditor() = delete;

        static void configure(const config::SqlAuditConfig &cfg);

        static common::Status check(const std::string &sql, common::OperationType type,
                                    bool readOnly = false);

        using Stats = SqlAuditStats;

        static Stats stats();
    };
}

#endif
