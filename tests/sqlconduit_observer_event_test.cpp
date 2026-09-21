#include "sqlconduit/common/context.h"
#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/config/datasource_config.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace sqlconduit;
using common::ContextScope;
using common::ErrorCode;
using common::OperationEvent;
using common::OperationType;
using common::Observability;
using common::ResultSet;
using common::Row;
using common::SqlContext;
using common::Status;

static int g_failed = 0;
static int g_passed = 0;

static void check(bool cond, const std::string& name)
{
    if (cond)
    {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    }
    else
    {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

static std::mutex g_capMtx;
static std::vector<OperationEvent> g_captured;

static void capObserver(const OperationEvent& e)
{
    std::lock_guard<std::mutex> lk(g_capMtx);
    g_captured.push_back(e);
}

static void clearCapture()
{
    std::lock_guard<std::mutex> lk(g_capMtx);
    g_captured.clear();
}

static void test_defaults_without_ctx_or_result()
{
    std::cout << "== M9.1 默认值：空 ctx + 空 result 时 shadow/transformed=false ==\n";
    clearCapture();
    Observability::setObserver(&capObserver);
    sqlconduit::config::ObservabilityConfig cfg;
    Observability::configure(cfg);

    OperationEvent e;
    e.dataSource = "ds";
    e.type = OperationType::Query;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(100);
    Observability::emitSql(e, "SELECT 1");
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "observer 已触发一次");
        if (!g_captured.empty())
        {
            check(!g_captured[0].shadow, "默认 ctx：event.shadow=false");
            check(!g_captured[0].transformed, "默认 result=nullptr：event.transformed=false");
        }
    }
}

static void test_shadow_propagates_from_ctx()
{
    std::cout << "== M9.2 shadow 由栈顶 ctx 注入 ==\n";
    clearCapture();
    SqlContext ctx;
    ctx.shadow = true;
    ContextScope scope(ctx);
    OperationEvent e;
    e.dataSource = "ds";
    e.type = OperationType::Query;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(100);
    Observability::emitSql(e, "SELECT * FROM probe_health");
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "shadow scope 内：observer 已触发");
        if (!g_captured.empty())
        {
            check(g_captured[0].shadow,
                  "shadow=true scope：event.shadow=true（注入成功）");
        }
    }
}

static void test_shadow_resets_after_scope()
{
    std::cout << "== M9.3 shadow 帧退出后恢复 false ==\n";
    clearCapture();
    {
        SqlContext ctx;
        ctx.shadow = true;
        ContextScope scope(ctx);
        OperationEvent e;
        e.dataSource = "ds";
        e.type = OperationType::Query;
        e.status = Status::OK();
        e.duration = std::chrono::microseconds(50);
        Observability::emitSql(e, "SELECT 1");
    }
    OperationEvent e2;
    e2.dataSource = "ds";
    e2.type = OperationType::Query;
    e2.status = Status::OK();
    e2.duration = std::chrono::microseconds(50);
    Observability::emitSql(e2, "SELECT 2");
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 2, "两条 emit 都触发 observer");
        if (g_captured.size() >= 2)
        {
            check(g_captured[0].shadow, "shadow 帧内：shadow=true");
            check(!g_captured[1].shadow, "shadow 帧外：shadow=false（栈帧隔离）");
        }
    }
}

static void test_transformed_from_result()
{
    std::cout << "== M9.4 transformed 由 result.transformed 注入 ==\n";
    clearCapture();
    ResultSet out;
    out.setFields({"secret"});
    Row row;
    row.set("secret", std::string("***"));
    out.addRow(std::move(row));
    out.transformed = true;

    OperationEvent e;
    e.dataSource = "ds";
    e.type = OperationType::Query;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(80);
    Observability::emitSql(e, "SELECT secret FROM t", {}, &out);
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "observer 已触发");
        if (!g_captured.empty())
        {
            check(g_captured[0].transformed,
                  "result.transformed=true → event.transformed=true");
        }
    }
}

static void test_not_transformed()
{
    std::cout << "== M9.5 非脱敏：result.transformed=false → event.transformed=false ==\n";
    clearCapture();
    ResultSet out;
    out.setFields({"id"});
    Row row;
    row.set("id", std::int64_t(42));
    out.addRow(std::move(row));

    OperationEvent e;
    e.dataSource = "ds";
    e.type = OperationType::Query;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(80);
    Observability::emitSql(e, "SELECT id FROM t", {}, &out);
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "observer 已触发");
        if (!g_captured.empty())
        {
            check(!g_captured[0].transformed,
                  "result.transformed=false → event.transformed=false");
        }
    }
}

static void test_write_path_keeps_transformed_false()
{
    std::cout << "== M9.6 写路径无 result：transformed 默认 false ==\n";
    clearCapture();
    OperationEvent e;
    e.dataSource = "ds";
    e.type = OperationType::Execute;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(40);
    Observability::emitSql(e, "UPDATE x SET y=1");
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "observer 已触发");
        if (!g_captured.empty())
        {
            check(!g_captured[0].transformed, "execute 路径：event.transformed=false（写没 result）");
            check(!g_captured[0].shadow, "execute 路径：event.shadow=false（默认）");
        }
    }
}

static void test_shadow_and_transformed_together()
{
    std::cout << "== M9.7 shadow + transformed 同一事件内并行 ==\n";
    clearCapture();
    SqlContext ctx;
    ctx.shadow = true;
    ContextScope scope(ctx);
    ResultSet out;
    out.transformed = true;
    OperationEvent e;
    e.dataSource = "shadow-ds";
    e.type = OperationType::Query;
    e.status = Status::OK();
    e.duration = std::chrono::microseconds(120);
    Observability::emitSql(e, "SELECT secret FROM probe", {}, &out);
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "observer 已触发");
        if (!g_captured.empty())
        {
            check(g_captured[0].shadow, "shadow=true 生效");
            check(g_captured[0].transformed, "transformed=true 生效");
        }
    }
}

static void test_error_event_carries_marks()
{
    std::cout << "== M9.8 错误事件同样带 shadow/transformed 标记（告警归因需要） ==\n";
    clearCapture();
    SqlContext ctx;
    ctx.shadow = true;
    ctx.tenantId = std::string("t-acme");
    ContextScope scope(ctx);
    ResultSet out;
    out.transformed = true;
    OperationEvent e;
    e.dataSource = "shadow-ds";
    e.type = OperationType::Query;
    e.status = Status::error(ErrorCode::NotConnected, "broken");
    e.duration = std::chrono::microseconds(5'000);
    Observability::emitSql(e, "SELECT secret FROM probe", {}, &out);
    {
        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "失败事件也触发 observer");
        if (!g_captured.empty())
        {
            check(g_captured[0].shadow,
                  "失败事件：event.shadow=true（影子流量发生的失败要单独计数）");
            check(g_captured[0].transformed,
                  "失败事件：event.transformed=true（脱敏路径上的失败也要计入合规）");
            check(!g_captured[0].status.ok(),
                  "失败事件：event.status.ok()=false");
        }
    }
}

int main()
{
    test_defaults_without_ctx_or_result();
    test_shadow_propagates_from_ctx();
    test_shadow_resets_after_scope();
    test_transformed_from_result();
    test_not_transformed();
    test_write_path_keeps_transformed_false();
    test_shadow_and_transformed_together();
    test_error_event_carries_marks();
    Observability::setObserver(nullptr);
    Observability::configure({});
    std::cout << "\n===== total PASS=" << g_passed << " FAIL=" << g_failed << " =====\n";
    return g_failed == 0 ? 0 : 1;
}
