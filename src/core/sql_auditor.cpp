#include "dbmw/core/sql_auditor.h"

#include "dbmw/common/sql_analyze.h"
#include "dbmw/common/logger.h"

#include <memory>
#include <mutex>

namespace dbmw::core {
    std::atomic<bool> SqlAuditor::enabled_{false};
    std::mutex SqlAuditor::mtx_;
    std::shared_ptr<const SqlAuditor::Policy> SqlAuditor::policy_;
    std::atomic<std::uint64_t> SqlAuditor::checked_{0};
    std::atomic<std::uint64_t> SqlAuditor::warned_{0};
    std::atomic<std::uint64_t> SqlAuditor::blocked_{0};

    namespace {
        common::Status verdict(const bool block, const bool logIt,
                              const char *reason, const std::string &sql,
                              std::atomic<std::uint64_t> &warned,
                              std::atomic<std::uint64_t> &blocked) {
            if (logIt) {
                DBMW_LOG_WARN(std::string("sql audit ") + (block ? "blocked" : "warn") + " ("
                              + reason + "): " + sql);
            }
            if (block) {
                blocked.fetch_add(1, std::memory_order_relaxed);
                return common::Status::error(common::ErrorCode::SqlBlocked, reason);
            }
            warned.fetch_add(1, std::memory_order_relaxed);
            return common::Status::OK();
        }
    }

    void SqlAuditor::configure(const config::SqlAuditConfig &cfg) {
        auto policy = std::make_shared<Policy>();
        policy->block = cfg.action == "block";
        policy->log_blocked = cfg.log_blocked;
        policy->block_no_where_dml = cfg.block_no_where_dml;
        policy->require_limit_select = cfg.require_limit_select;
        policy->enforce_read_only = cfg.enforce_read_only;
        policy->blacklist.insert(cfg.blacklist_fingerprints.begin(),
                                 cfg.blacklist_fingerprints.end());
        policy->whitelist.insert(cfg.whitelist_fingerprints.begin(),
                                 cfg.whitelist_fingerprints.end());
        policy->needsFingerprint = !policy->blacklist.empty() || !policy->whitelist.empty();
        policy->needsKind = policy->enforce_read_only || policy->block_no_where_dml ||
                            policy->require_limit_select;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            policy_ = std::move(policy);
        }
        enabled_.store(cfg.enabled, std::memory_order_release);
    }

    common::Status SqlAuditor::check(const std::string &sql, const common::OperationType type,
                                     const bool readOnly) {
        if (!enabled_.load(std::memory_order_acquire)) return common::Status::OK();

        std::shared_ptr<const Policy> policy;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            policy = policy_;
        }
        if (!policy) return common::Status::OK();

        checked_.fetch_add(1, std::memory_order_relaxed);
        using namespace common::sql;

        if (hasMultipleStatements(sql, true)) {
            return verdict(policy->block, policy->log_blocked,
                           "multiple SQL statements are not allowed", sql,
                           warned_, blocked_);
        }

        if (policy->needsFingerprint) {
            const std::uint64_t fp = fingerprintTemplate(sql);

            if (!policy->whitelist.empty() &&
                policy->whitelist.find(fp) == policy->whitelist.end()) {
                return verdict(true, policy->log_blocked,
                               "SQL not in audit whitelist", sql, warned_, blocked_);
            }
            if (policy->blacklist.find(fp) != policy->blacklist.end()) {
                return verdict(true, policy->log_blocked,
                               "SQL in audit blacklist", sql, warned_, blocked_);
            }
        }

        if (!policy->needsKind) {
            (void) type;
            return common::Status::OK();
        }

        const StatementKind kind = classifyStatement(sql);

        if (policy->enforce_read_only && readOnly && isWrite(kind)) {
            return verdict(policy->block, policy->log_blocked,
                           "write on read-only datasource", sql, warned_, blocked_);
        }

        if (policy->block_no_where_dml &&
            (kind == StatementKind::Update || kind == StatementKind::Delete) &&
            !hasWhereClause(sql)) {
            return verdict(policy->block, policy->log_blocked,
                           "UPDATE/DELETE without WHERE clause", sql, warned_, blocked_);
        }

        const bool isCursor = (type == common::OperationType::Select);
        if (policy->require_limit_select && !isCursor && kind == StatementKind::Select &&
            !hasLimitClause(sql)) {
            return verdict(policy->block, policy->log_blocked,
                           "SELECT without LIMIT clause", sql, warned_, blocked_);
        }

        return common::Status::OK();
    }

    SqlAuditor::Stats SqlAuditor::stats() {
        Stats out;
        out.checked = checked_.load(std::memory_order_relaxed);
        out.warned = warned_.load(std::memory_order_relaxed);
        out.blocked = blocked_.load(std::memory_order_relaxed);
        return out;
    }
}
