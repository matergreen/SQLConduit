#include "dbmw/dbmw.h"
#include "dbmw/async/dbmw_async.h"
#include "dbmw/common/context.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/core/database_manager.h"
#include "dbmw/core/idatabase_connection.h"
#include "dbmw/core/interceptor.h"
#include "dbmw/core/query_cache.h"
#include "dbmw/driver/driver_registry.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>

using namespace dbmw;
using common::Status;
using common::ErrorCode;

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

static std::atomic<int> gMockQueryCount{0};

class MockRedactConnection : public core::IDatabaseConnection {
public:
    common::Status connect(const config::DataSourceConfig &) override {
        open_ = true;
        return Status::OK();
    }

    common::Status ping() override {
        return open_
                   ? Status::OK()
                   : Status::error(common::ErrorCode::NotConnected, "closed");
    }

    common::Status query(const std::string &, common::ResultSet &out) override {
        ++gMockQueryCount;
        out.setFields({"secret"});
        common::Row r;
        r.set("secret", std::string("SECRET-12345"));
        out.addRow(std::move(r));
        return Status::OK();
    }

    common::Status execute(const std::string &, std::int64_t &affected) override {
        affected = 1;
        return Status::OK();
    }

    common::Status begin() override { return Status::OK(); }
    common::Status commit() override { return Status::OK(); }
    common::Status rollback() override { return Status::OK(); }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

private:
    bool open_ = false;
};

class MockRedactDriver : public driver::IDriver {
public:
    const char *name() const override { return "mockr"; }

    std::unique_ptr<core::IDatabaseConnection> createConnection() override {
        return std::make_unique<MockRedactConnection>();
    }
};

struct RedactionInterceptor : public core::ISqlInterceptor {
    bool enableTransform = true;
    std::string maskedValue = "***";
    std::atomic<int> afterCount{0};
    std::atomic<int> rowCount{0};

    void onRoute(const std::string &, const std::string &,
                 common::OperationType, common::SqlContext &) override {
    }

    common::Status beforeExecution(const core::ExecutionView &) override {
        return Status::OK();
    }

    void afterExecution(const core::ExecutionView &view) override {
        ++afterCount;
        if (!enableTransform) return;
        if (!view.result) return;
        view.result->transformed = true;
        (void) view.result->rows();
    }

    void onRow(const core::ExecutionView &, common::Row &row) override {
        ++rowCount;
        if (enableTransform && row.has("secret")) row.set("secret", maskedValue);
    }

    void onCompletion(const core::ExecutionView &) override {
    }
};

static void test_sync_redaction_not_cached() {
    std::cout << "== M7.1 同步路径：脱敏读不进缓存（I10 核心）==\n";
    core::DatabaseManager mgr;
    config::DataSourceConfig ds;
    ds.name = "ds";
    ds.type = "mockr";
    ds.host = "localhost";
    check(mgr.addDataSource(ds).ok(), "addDataSource ok");

    config::QueryCacheConfig qc_on;
    qc_on.enabled = true;
    core::QueryCache::configure(qc_on);

    auto redact = std::make_shared<RedactionInterceptor>();
    redact->enableTransform = true;
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(true);
    core::InterceptorRegistry::add(redact);

    auto g = mgr.getDataSource("ds");
    gMockQueryCount = 0;
    redact->afterCount = 0;

    common::ResultSet rs1;
    check(g->query("SELECT secret", rs1).ok(), "首次读：成功");
    check(rs1.transformed, "首次读：rs.transformed=true（拦截器置位）");
    check(rs1.rowCount() == 1, "首次读：1 行");

    common::ResultSet rs2;
    check(g->query("SELECT secret", rs2).ok(), "第二次读：成功");
    check(rs2.transformed, "第二次读：rs.transformed=true（每次都重脱敏）");
    check(rs2.rowCount() == 1, "第二次读：1 行");

    check(gMockQueryCount.load() == 1,
          "I10：driver 只调用 1 次，缓存保存的是原始结果");
    check(redact->afterCount.load() == 2,
          "每个公开 query 恰好执行一次 afterExecution");

    std::uint64_t streamedRows = 0;
    std::string streamedSecret;
    check(g->queryEach("SELECT secret", {}, [&](const common::Row &row) {
              if (const auto *value = std::get_if<std::string>(&row.at("secret")))
                  streamedSecret = *value;
              return true;
          }, streamedRows).ok() && streamedRows == 1,
          "queryEach 正常逐行交付");
    check(streamedSecret == "***" && redact->rowCount.load() == 1,
          "queryEach 在业务回调前恰好执行一次 onRow 脱敏");

    core::QueryCache::configure({});
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(false);
    mgr.shutdown(std::chrono::milliseconds(0));
}

static void test_non_redacted_caches_normally() {
    std::cout << "== M7.2 未脱敏的读照常进缓存（I10 不误伤）==\n";
    core::DatabaseManager mgr;
    config::DataSourceConfig ds;
    ds.name = "ds";
    ds.type = "mockr";
    ds.host = "localhost";
    mgr.addDataSource(ds);

    config::QueryCacheConfig qc_on;
    qc_on.enabled = true;
    core::QueryCache::configure(qc_on);

    auto noop = std::make_shared<RedactionInterceptor>();
    noop->enableTransform = false;
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(true);
    core::InterceptorRegistry::add(noop);

    auto g = mgr.getDataSource("ds");
    gMockQueryCount = 0;
    noop->afterCount = 0;

    common::ResultSet rs1;
    g->query("SELECT secret", rs1);
    check(!rs1.transformed, "首次读：rs.transformed=false（未脱敏）");
    check(rs1.rowCount() == 1, "首次读：1 行");

    common::ResultSet rs2;
    g->query("SELECT secret", rs2);
    check(!rs2.transformed, "第二次读：rs.transformed=false");
    check(gMockQueryCount.load() == 1,
          "非脱敏读：driver 只调 1 次（缓存命中）");
    check(noop->afterCount.load() == 2,
          "每个公开 query 恰好执行一次 afterExecution");

    core::QueryCache::configure({});
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(false);
    mgr.shutdown(std::chrono::milliseconds(0));
}

static std::atomic<int> gTransformedReads{0};

struct RedactionFlagInterceptor : public core::ISqlInterceptor {
    void onRoute(const std::string &, const std::string &,
                 common::OperationType, common::SqlContext &) override {
    }

    common::Status beforeExecution(const core::ExecutionView &) override {
        return Status::OK();
    }

    void afterExecution(const core::ExecutionView &view) override {
        if (!view.result) return;
        view.result->transformed = true;
        ++gTransformedReads;
    }

    void onCompletion(const core::ExecutionView &) override {
    }
};

static void test_transformed_flag_blocks_cache() {
    std::cout << "== M7.3 transformed=true 不污染原始缓存==\n";
    core::DatabaseManager mgr;
    config::DataSourceConfig ds;
    ds.name = "ds";
    ds.type = "mockr";
    ds.host = "localhost";
    mgr.addDataSource(ds);

    config::QueryCacheConfig qc_on;
    qc_on.enabled = true;
    core::QueryCache::configure(qc_on);

    auto re = std::make_shared<RedactionFlagInterceptor>();
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(true);
    core::InterceptorRegistry::add(re);

    auto g = mgr.getDataSource("ds");
    gMockQueryCount = 0;
    gTransformedReads = 0;

    common::ResultSet rs1;
    g->query("SELECT x", rs1);
    check(rs1.transformed, "首次：rs.transformed=true");
    check(gMockQueryCount.load() == 1, "首次：driver 1 次");
    check(gTransformedReads.load() >= 1, "首次：afterExecution 改写标记 ≥1 次");

    common::ResultSet rs2;
    g->query("SELECT x", rs2);
    check(rs2.transformed, "二次：rs.transformed=true（再次脱敏）");
    check(gMockQueryCount.load() == 1,
          "二次：命中未改写的原始缓存");
    check(gTransformedReads.load() == 2,
          "二次：afterExecution 改写标记累计 2 次");

    core::QueryCache::configure({});
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::setEnabled(false);
    mgr.shutdown(std::chrono::milliseconds(0));
}

static void test_async_redaction_not_cached() {
    std::cout << "== M7.4 异步路径：脱敏读不进缓存（I10 + 异步桥接）==\n";
    const auto path = (std::filesystem::temp_directory_path() /
                       "dbmw_redaction_async.json").string();
    std::ofstream(path) << R"({
  "default_datasource": "ds",
  "heartbeat_interval_ms": 5000,
  "pool": { "enabled": true, "min": 0, "max": 4, "borrow_timeout_ms": 2000 },
  "retry": { "max_attempts": 1, "retry_writes": true },
  "circuit_breaker": { "failure_threshold": 0 },
  "rate_limit": { "enabled": false },
  "sql_audit": { "enabled": false },
  "query_cache": { "enabled": true },
  "async": { "enabled": true, "threads": 1, "queue_size": 64 },
  "interceptors": { "enabled": true },
  "datasources": [
    { "name": "ds", "type": "mockr", "host": "localhost" }
  ]
})";
    check(DBMW::init(path).ok(), "DBMW::init(async redaction cfg) ok");

    auto redact = std::make_shared<RedactionInterceptor>();
    redact->enableTransform = true;
    core::InterceptorRegistry::clear();
    core::InterceptorRegistry::add(redact);

    gMockQueryCount = 0;
    redact->afterCount = 0;

    {
        std::promise<async::QueryResult> pr;
        auto fut = pr.get_future();
        async::query("ds", "SELECT secret",
                     [&](async::QueryResult &&r) { pr.set_value(std::move(r)); });
        const auto out = fut.get();
        check(out.status.ok(), "异步首次：成功");
        check(out.rows.rowCount() == 1, "异步首次：1 行");
        check(out.rows.transformed, "异步首次：rows.transformed=true");
    }

    {
        std::promise<async::QueryResult> pr;
        auto fut = pr.get_future();
        async::query("ds", "SELECT secret",
                     [&](async::QueryResult &&r) { pr.set_value(std::move(r)); });
        const auto out = fut.get();
        check(out.status.ok(), "异步第二次：成功");
        check(out.rows.transformed, "异步第二次：rows.transformed=true");
    }

    check(gMockQueryCount.load() == 2,
          "异步 I10：driver 调用 2 次（缓存被 I10 守住）");
    check(redact->afterCount.load() >= 2,
          "异步 afterExecution 至少 2 次（缓存命中也跑）");

    DBMW::shutdown(std::chrono::milliseconds(0));
}

static void test_async_cache_hit_triggers_after() {
    std::cout << "== M7.5 异步缓存命中仍触发 afterExecution（§9.4 风险行）==\n";
    const auto path = (std::filesystem::temp_directory_path() /
                       "dbmw_redaction_async_hit.json").string();
    std::ofstream(path) << R"({
  "default_datasource": "ds",
  "heartbeat_interval_ms": 5000,
  "pool": { "enabled": true, "min": 0, "max": 4, "borrow_timeout_ms": 2000 },
  "retry": { "max_attempts": 1, "retry_writes": true },
  "circuit_breaker": { "failure_threshold": 0 },
  "rate_limit": { "enabled": false },
  "sql_audit": { "enabled": false },
  "query_cache": { "enabled": true },
  "async": { "enabled": true, "threads": 1, "queue_size": 64 },
  "interceptors": { "enabled": false },
  "datasources": [
    { "name": "ds", "type": "mockr", "host": "localhost" }
  ]
})";
    check(DBMW::init(path).ok(), "DBMW::init(async cache hit cfg) ok");

    gMockQueryCount = 0;
    {
        std::promise<async::QueryResult> pr;
        auto fut = pr.get_future();
        async::query("ds", "SELECT secret",
                     [&](async::QueryResult &&r) { pr.set_value(std::move(r)); });
        const auto out = fut.get();
        check(out.status.ok(), "首次异步：成功（缓存被填，无脱敏）");
        check(!out.rows.transformed, "首次异步：rows.transformed=false");
        check(out.rows.rowCount() == 1, "首次异步：1 行");
    }
    check(gMockQueryCount.load() == 1, "首次异步：driver 1 次（缓存填了）");

    auto redact2 = std::make_shared<RedactionInterceptor>();
    redact2->enableTransform = true;
    redact2->afterCount = 0;
    core::InterceptorRegistry::setEnabled(true);
    core::InterceptorRegistry::add(redact2);

    {
        std::promise<async::QueryResult> pr;
        auto fut = pr.get_future();
        async::query("ds", "SELECT secret",
                     [&](async::QueryResult &&r) { pr.set_value(std::move(r)); });
        const auto out = fut.get();
        check(out.status.ok(), "二次异步（缓存命中 + 脱敏）：成功");
        check(gMockQueryCount.load() == 1,
              "缓存命中：driver 仍只调 1 次（验证 §9.4 修复：缓存命中走 afterExecution）");
        check(redact2->afterCount.load() == 1,
              "afterExecution 调 1 次（缓存命中路径补调，§9.4 修复）");
        check(out.rows.transformed,
              "缓存命中 + afterExecution：rows.transformed=true");
        check(out.rows.rowCount() == 1, "缓存命中 + afterExecution：1 行");
    }

    DBMW::shutdown(std::chrono::milliseconds(0));
}

int main() {
    driver::DriverRegistry::instance().registerDriver(
        "mockr", [] { return std::make_unique<MockRedactDriver>(); });

    test_sync_redaction_not_cached();
    test_non_redacted_caches_normally();
    test_transformed_flag_blocks_cache();
    test_async_redaction_not_cached();
    test_async_cache_hit_triggers_after();

    std::cout << "\n========== M7 结果脱敏 总计: " << g_passed << " 通过 / "
            << g_failed << " 失败 ==========\n";
    return g_failed == 0 ? 0 : 1;
}
