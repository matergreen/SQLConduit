#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/core/idatabase_connection.h"
#include "sqlconduit/core/rate_limiter.h"
#include "sqlconduit/config/config_loader.h"
#include "sqlconduit/driver/driver_registry.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace sqlconduit;
using common::Status;

static int g_failed = 0;
static int g_passed = 0;

static void check(bool cond, const std::string &name) {
    if (cond) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

class MockConnection : public core::IDatabaseConnection {
public:
    static std::atomic<int> alive;
    static std::atomic<bool> connectFails;
    static std::atomic<bool> pingFails;
    static std::atomic<bool> queryBreaks;
    static std::atomic<int> queryFailuresRemaining;
    static std::atomic<int> executeFailuresRemaining;
    static std::atomic<int> queryCalls;
    static std::atomic<int> executeCalls;
    static std::atomic<int> executeOkBeforeFail;
    static std::atomic<bool> cancelThrows;
    static std::atomic<bool> cancelUnsupported;
    static std::vector<std::string> log;

    static void resetLog() { log.clear(); }

    common::Status connect(const config::DataSourceConfig &cfg) override {
        (void) cfg;
        if (connectFails.load()) return Status::error(common::ErrorCode::ConnectionFailed, "mock connect failed");
        open_ = true;
        ++alive;
        log.push_back("connect");
        return Status::OK();
    }

    common::Status ping() override {
        if (!open_ || pingFails.load()) return Status::error(common::ErrorCode::PingFailed, "mock ping failed");
        return Status::OK();
    }

    common::Status query(const std::string &sql, common::ResultSet &out) override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        ++queryCalls;
        if (queryBreaks.load() || queryFailuresRemaining.fetch_sub(1) > 0)
            return Status::databaseError(common::ErrorCode::QueryError,
                                         "mock connection lost", "08006");
        log.push_back("query:" + sql);
        common::Row r;
        r.set("echo", std::string(sql));
        out.addRow(std::move(r));
        return Status::OK();
    }

    common::Status execute(const std::string &sql, std::int64_t &affected) override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        ++executeCalls;
        if (executeFailuresRemaining.fetch_sub(1) > 0)
            return Status::databaseError(common::ErrorCode::QueryError,
                                         "mock connection lost", "08006");
        if (executeOkBeforeFail.load() >= 0 && executeOkBeforeFail.fetch_sub(1) == 0)
            return Status::databaseError(common::ErrorCode::QueryError,
                                         "mock write failed midway", "08006");
        log.push_back("execute:" + sql);
        affected = 1;
        return Status::OK();
    }

    common::Status begin() override {
        if (tx_) return Status::error(common::ErrorCode::TxError, "already in tx");
        tx_ = true;
        log.push_back("begin");
        return Status::OK();
    }

    common::Status begin(const common::TransactionOptions &options) override {
        if (options.readOnly || options.isolation != common::IsolationLevel::Default) {
            log.push_back(std::string("options:")
                          + (options.readOnly ? "readonly" : "readwrite") + ":"
                          + std::to_string(static_cast<int>(options.isolation)));
        }
        return begin();
    }

    common::Status commit() override {
        if (!tx_) return Status::error(common::ErrorCode::TxError, "no tx");
        tx_ = false;
        log.push_back("commit");
        return Status::OK();
    }

    common::Status rollback() override {
        if (!tx_) return Status::error(common::ErrorCode::TxError, "no tx");
        tx_ = false;
        log.push_back("rollback");
        return Status::OK();
    }

    common::Status savepoint(const std::string &name) override {
        if (!tx_) return Status::error(common::ErrorCode::TxError, "no tx");
        log.push_back("savepoint:" + name);
        return Status::OK();
    }

    common::Status releaseSavepoint(const std::string &name) override {
        if (!tx_) return Status::error(common::ErrorCode::TxError, "no tx");
        log.push_back("release:" + name);
        return Status::OK();
    }

    common::Status rollbackToSavepoint(const std::string &name) override {
        if (!tx_) return Status::error(common::ErrorCode::TxError, "no tx");
        log.push_back("rollback_to:" + name);
        return Status::OK();
    }

    common::Status cancel() override {
        if (cancelUnsupported.load()) return IDatabaseConnection::cancel();
        log.push_back("cancel");
        if (cancelThrows.load()) throw std::runtime_error("mock cancel blew up");
        return Status::OK();
    }

    void close() override {
        if (open_) {
            open_ = false;
            --alive;
            log.push_back("close");
        }
    }

    bool isOpen() const override { return open_; }

    bool inTransaction() const override { return tx_; }

    bool allowsLiteralInterpolation() const override { return true; }

private:
    bool open_ = false;
    bool tx_ = false;
};

std::atomic<int> MockConnection::alive{0};
std::atomic<bool> MockConnection::connectFails{false};
std::atomic<bool> MockConnection::pingFails{false};
std::atomic<bool> MockConnection::queryBreaks{false};
std::atomic<int> MockConnection::queryFailuresRemaining{0};
std::atomic<int> MockConnection::executeFailuresRemaining{0};
std::atomic<int> MockConnection::queryCalls{0};
std::atomic<int> MockConnection::executeCalls{0};
std::atomic<int> MockConnection::executeOkBeforeFail{-1};
std::atomic<bool> MockConnection::cancelThrows{false};
std::atomic<bool> MockConnection::cancelUnsupported{true};
std::vector<std::string> MockConnection::log;

class MockDriver : public driver::IDriver {
public:
    const char *name() const override { return "mock"; }

    std::unique_ptr<core::IDatabaseConnection> createConnection() override {
        return std::make_unique<MockConnection>();
    }
};

class CountingLimiter : public core::IRateLimiter {
public:
    std::atomic<int> acquires{0};

    bool acquire(std::uint64_t) override {
        acquires.fetch_add(1);
        return true;
    }
};

class DenyLimiter : public core::IRateLimiter {
public:
    bool acquire(std::uint64_t) override { return false; }
};

static config::DataSourceConfig mockLeafCfg(const std::string &name) {
    config::DataSourceConfig c;
    c.name = name;
    c.type = "mock";
    c.host = "localhost";
    c.connection_timeout_ms = 100;
    return c;
}

int main() {
    driver::DriverRegistry::instance().registerDriver(
        "mock", [] { return std::make_unique<MockDriver>(); });

    std::cout << "== P1 自定义限流器经全局默认挂载并被闸门调用 ==\n";
    {
        core::DatabaseManager mgr;
        auto counting = std::make_shared<CountingLimiter>();
        mgr.setDefaultRateLimiter(counting);
        check(mgr.addDataSource(mockLeafCfg("p1"), core::DataSourceOptions{}).ok(), "addDataSource(p1)");
        auto ds = mgr.getDataSource("p1");
        common::ResultSet rs;
        check(ds->query("select 1", rs).ok(), "query ok（全局默认限流器放行）");
        check(rs.rowCount() == 1, "返回 1 行");
        check(counting->acquires.load() > 0, "自定义限流器被 preGate/gateSession 调用");
        mgr.shutdown(std::chrono::milliseconds(0));
        mgr.setDefaultRateLimiter(nullptr);
    }

    std::cout << "== P2 按数据源挂载的拒绝型限流器使 query 返回 RateLimited 且不可重试 ==\n";
    {
        core::DatabaseManager mgr;
        core::DataSourceOptions opts;
        opts.rate_limiter = std::make_shared<DenyLimiter>();
        check(mgr.addDataSource(mockLeafCfg("p2"), opts).ok(), "addDataSource(p2)");
        auto ds = mgr.getDataSource("p2");
        common::ResultSet rs;
        auto st = ds->query("select 1", rs);
        check(!st.ok(), "query 被限流拒绝");
        check(st.code == common::ErrorCode::RateLimited, "错误码为 RateLimited");
        check(st.retryable == false, "被限流不可重试");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== P3 默认 TokenBucket 实现仍生效且兼容 IRateLimiter ==\n";
    {
        auto rl = std::make_shared<core::RateLimiter>(1.0, 0.0, 1, "off");
        std::shared_ptr<core::IRateLimiter> base = rl;
        check(base != nullptr, "RateLimiter 可赋值给 IRateLimiter（非破坏性）");
        check(rl->acquire(0) == true, "默认桶首获通过");
        check(rl->acquire(0) == false, "默认桶次获被限流");
        check(rl->usesFingerprint() == false, "fingerprint_mode=off 时 usesFingerprint=false");
    }

    std::cout << "== P4 数据源显式挂载的限流器优先于全局默认 ==\n";
    {
        core::DatabaseManager mgr;
        auto deny = std::make_shared<DenyLimiter>();
        mgr.setDefaultRateLimiter(deny);
        core::DataSourceOptions opts;
        opts.rate_limiter = std::make_shared<core::RateLimiter>(100000.0, 0.0, 100000, "off");
        check(mgr.addDataSource(mockLeafCfg("p4"), opts).ok(), "addDataSource(p4)");
        auto ds = mgr.getDataSource("p4");
        common::ResultSet rs;
        check(ds->query("select 1", rs).ok(), "显式数据源限流器优先生效（全局 deny 不触发）");
        mgr.setDefaultRateLimiter(std::make_shared<DenyLimiter>());
        check(ds->query("select 2", rs).ok(), "更新全局默认不会覆盖数据源显式限流器");
        mgr.shutdown(std::chrono::milliseconds(0));
        mgr.setDefaultRateLimiter(nullptr);
    }

    std::cout << "== P5 仅配置按指纹限流时仍然生效 ==\n";
    {
        core::RateLimiter limiter(0.0, 1.0, 1, "template");
        check(limiter.acquire(101), "第一个指纹首次请求通过");
        check(!limiter.acquire(101), "同一指纹第二次请求被限流");
        check(limiter.acquire(202), "不同指纹使用独立令牌桶");
    }

    std::cout << "== P6 后设置的默认限流器按调用顺序更新已有数据源与组 ==\n";
    {
        core::DatabaseManager mgr;
        check(mgr.addDataSource(mockLeafCfg("p6_leaf")).ok(), "先添加叶子数据源");
        config::DataSourceGroupConfig group;
        group.name = "p6_group";
        group.primary = "p6_leaf";
        check(mgr.addGroup(group).ok(), "再添加数据源组");

        auto counting = std::make_shared<CountingLimiter>();
        mgr.setDefaultRateLimiter(counting);
        auto ds = mgr.getDataSource("p6_group");
        common::ResultSet rs;
        check(ds->query("select 1", rs).ok(), "最后设置默认限流器后组查询成功");
        auto leaf = mgr.getDataSource("p6_leaf");
        check(leaf->query("select 1", rs).ok(), "最后设置默认限流器后叶子查询成功");
        check(counting->acquires.load() >= 2, "组和叶子数据源均按顺序继承新默认限流器");

        mgr.setDefaultRateLimiter(std::make_shared<DenyLimiter>());
        check(ds->query("select 2", rs).code == common::ErrorCode::RateLimited,
              "再次更新默认限流器会立即作用于已有组");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== P7 配置仅启用按指纹限流时数据源组仍创建限流器 ==\n";
    {
        core::DatabaseManager mgr;
        config::GlobalConfig cfg;
        cfg.default_datasource = "p7_group";
        cfg.pool.min = 0;
        cfg.pool.max = 1;
        cfg.datasources.push_back(mockLeafCfg("p7_leaf"));
        config::DataSourceGroupConfig group;
        group.name = "p7_group";
        group.primary = "p7_leaf";
        cfg.groups.push_back(group);
        cfg.rate_limit.enabled = true;
        cfg.rate_limit.global_qps = 0;
        cfg.rate_limit.per_fingerprint_qps = 1;
        cfg.rate_limit.burst = 1;
        cfg.rate_limit.fingerprint_mode = "template";
        check(mgr.init(cfg).ok(), "仅 per_fingerprint_qps 配置可初始化");
        auto ds = mgr.getDefault();
        common::ResultSet rs;
        check(ds && ds->query("select 1", rs).ok(), "组内同一 SQL 首次请求通过");
        check(ds && ds->query("select 1", rs).code == common::ErrorCode::RateLimited,
              "组内同一 SQL 第二次请求被按指纹限流");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << (g_failed == 0 ? "ALL PASSED\n" : "SOME FAILED\n");
    std::cout << "passed=" << g_passed << " failed=" << g_failed << "\n";
    return g_failed == 0 ? 0 : 1;
}
