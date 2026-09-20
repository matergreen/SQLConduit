#ifndef DBMW_CORE_SQL_AUDITOR_H
#define DBMW_CORE_SQL_AUDITOR_H

#include "dbmw/common/types.h"
#include "dbmw/common/observer.h"
#include "dbmw/config/datasource_config.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

namespace dbmw::core {
    class SqlAuditor {
    public:
        SqlAuditor() = delete;

        static void configure(const config::SqlAuditConfig &cfg);

        static common::Status check(const std::string &sql, common::OperationType type,
                                    bool readOnly = false);

        struct Stats {
            std::uint64_t checked = 0;
            std::uint64_t warned = 0;
            std::uint64_t blocked = 0;
        };

        static Stats stats();

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

        static std::atomic<bool> enabled_;
        static std::mutex mtx_;
        static std::shared_ptr<const Policy> policy_;
        static std::atomic<std::uint64_t> checked_;
        static std::atomic<std::uint64_t> warned_;
        static std::atomic<std::uint64_t> blocked_;
    };
}

#endif
