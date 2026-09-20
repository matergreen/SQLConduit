#include "dbmw/common/context.h"
#include "dbmw/common/observer.h"
#include "dbmw/config/datasource_config.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace dbmw::common;
using dbmw::config::ObservabilityConfig;

namespace dbmw::common {
    namespace {
        bool isLowerHex16(const std::string &s) {
            if (s.size() != 16) return false;
            for (char c: s) {
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            }
            return true;
        }
    }
}

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

static std::mutex g_capMtx;
static std::vector<OperationEvent> g_captured;

static void capturingObserver(const OperationEvent &e) {
    std::lock_guard<std::mutex> lk(g_capMtx);
    g_captured.push_back(e);
}

#define CLEAR_CAPTURED() do { std::lock_guard<std::mutex> _lk(g_capMtx); g_captured.clear(); } while (0)

int main() {
    std::cout << "== M2 追踪上下文：emitSql 无 ctx 时不发幽灵字段 ==\n";
    {
        CLEAR_CAPTURED();
        Observability::clearSlowSqlStats();
        Observability::setObserver(&capturingObserver);
        dbmw::config::ObservabilityConfig cfg;
        Observability::configure(cfg);

        OperationEvent e;
        e.dataSource = "ds-noctx";
        e.type = OperationType::Query;
        e.status = Status::OK();
        e.duration = std::chrono::microseconds(500);
        Observability::emitSql(e, "SELECT 1");

        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "无 ctx 时观察者仍触发一次");
        check(g_captured[0].traceId.empty(),
              "无 ctx 时 event.traceId 不被自动生成（不发幽灵 trace）");
        check(g_captured[0].spanId.empty(),
              "无 ctx 时 event.spanId 不被自动生成（不发幽灵 span）");
    }

    std::cout << "== M2 追踪上下文：emitSql 注入 trace + span ==\n";
    {
        CLEAR_CAPTURED();
        Observability::clearSlowSqlStats();

        SqlContext ctx;
        ctx.traceId = std::string(32, 'a');
        ctx.spanId = std::string(16, 'b');
        ContextScope scope(ctx);

        OperationEvent e;
        e.dataSource = "ds-trace";
        e.type = OperationType::Execute;
        e.status = Status::OK();
        e.duration = std::chrono::microseconds(1200);
        Observability::emitSql(e, "UPDATE t SET v=1");

        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "emitSql 在 ctx 下正常进入观察者");
        check(g_captured[0].traceId == ctx.traceId,
              "event.traceId 与栈顶 traceId 一致");
        check(g_captured[0].spanId == ctx.spanId,
              "调用方已填 spanId 时沿用，不自动改写");
    }

    std::cout << "== M2 追踪上下文：trace 单独、span 自动生成 ==\n";
    {
        CLEAR_CAPTURED();
        Observability::clearSlowSqlStats();

        SqlContext ctx;
        ctx.traceId = std::string(32, 'c');
        ContextScope scope(ctx);

        OperationEvent e;
        e.dataSource = "ds-autospan";
        e.status = Status::OK();
        e.duration = std::chrono::microseconds(800);
        Observability::emitSql(e, "SELECT 1 FROM a");

        std::string firstSpan;
        {
            std::lock_guard<std::mutex> lk(g_capMtx);
            check(g_captured.size() == 1, "trace-only 路径仍触发观察者");
            check(g_captured[0].traceId == ctx.traceId,
                  "trace-only 路径 event.traceId 等于栈顶");
            check(isLowerHex16(g_captured[0].spanId),
                  "trace-only 路径自动生成 16 hex spanId");
            firstSpan = g_captured[0].spanId;
        }

        Observability::emitSql(e, "SELECT 1 FROM a");
        std::lock_guard<std::mutex> lk2(g_capMtx);
        check(g_captured.size() == 2,
              "第二次 emitSql 也进观察者");
        check(g_captured[1].spanId != firstSpan && isLowerHex16(g_captured[1].spanId),
              "两次 emitSql 的 spanId 互不相同且都是 16 hex");
    }

    std::cout << "== M2 追踪上下文：嵌套 ContextScope 取栈顶 ==\n";
    {
        CLEAR_CAPTURED();
        Observability::clearSlowSqlStats();

        SqlContext outer;
        outer.traceId = std::string(32, 'o');
        outer.spanId = std::string(16, 'O');
        ContextScope sOuter(outer);

        SqlContext inner;
        inner.traceId = std::string(32, 'i');
        inner.spanId = std::string(16, 'I');
        ContextScope sInner(inner);

        OperationEvent e;
        e.dataSource = "ds-nested";
        e.status = Status::OK();
        e.duration = std::chrono::microseconds(700);
        Observability::emitSql(e, "SELECT * FROM t");

        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "嵌套路径 emitSql 仍进观察者");
        check(g_captured[0].traceId == inner.traceId,
              "嵌套 emitSql 取栈顶（inner）的 traceId");
        check(g_captured[0].spanId == inner.spanId,
              "嵌套 emitSql 取栈顶（inner）的 spanId");
    }

    std::cout << "== M2 追踪上下文：SlowSqlRecord 继承 trace 字段 ==\n";
    {
        CLEAR_CAPTURED();
        Observability::clearSlowSqlStats();

        dbmw::config::ObservabilityConfig cfg;
        cfg.slow_sql.enabled = true;
        cfg.slow_sql.threshold_ms = 1;
        cfg.slow_sql.recent_capacity = 10;
        Observability::configure(cfg);

        SqlContext ctx;
        ctx.traceId = std::string(32, 's');
        ctx.spanId = std::string(16, 'S');
        ContextScope scope(ctx);

        OperationEvent e;
        e.dataSource = "ds-slow";
        e.type = OperationType::Query;
        e.status = Status::OK();
        e.duration = std::chrono::milliseconds(20);
        Observability::emitSql(e, "SELECT COUNT(*) FROM bigtable");

        const auto recent = Observability::recentSlowSql(10);
        check(recent.size() == 1, "慢 SQL 窗口收到一条");
        if (!recent.empty()) {
            check(recent[0].traceId == ctx.traceId,
                  "SlowSqlRecord.traceId 与 event.traceId 同源");
            check(recent[0].spanId == ctx.spanId,
                  "SlowSqlRecord.spanId 与 event.spanId 同源");
            check(recent[0].dataSource == "ds-slow",
                  "SlowSqlRecord.dataSource 沿用事件值（回归保护）");
        }
    }

    std::cout << "== M2 追踪上下文：Observability::emit（非 SQL 路径）保留空 trace ==\n";
    {
        CLEAR_CAPTURED();
        Observability::setObserver(&capturingObserver);
        OperationEvent plain;
        plain.dataSource = "ds-emit";
        plain.type = OperationType::Begin;
        plain.status = Status::OK();
        plain.duration = std::chrono::microseconds(100);
        Observability::emit(plain);

        std::lock_guard<std::mutex> lk(g_capMtx);
        check(g_captured.size() == 1, "emit 单独路径触发观察者");
        check(g_captured[0].traceId.empty() && g_captured[0].spanId.empty(),
              "emit（非 emitSql）不留 trace 字段，归属清晰");
    }

    std::cout << "\n----------------------------------------\n";
    std::cout << "通过 " << g_passed << " 项，失败 " << g_failed << " 项\n";

    Observability::setObserver({});
    Observability::configure({});
    Observability::clearSlowSqlStats();
    return g_failed == 0 ? 0 : 1;
}
