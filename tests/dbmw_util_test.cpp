#include "dbmw/dbmw.h"
#include "dbmw/util.h"
#include "dbmw/async/dbmw_async.h"
#include "dbmw/common/context.h"
#include "dbmw/common/sql_analyze.h"
#include "dbmw/core/idatabase_connection.h"
#include "dbmw/core/query_cache.h"
#include "dbmw/core/sql_auditor.h"
#include "dbmw/config/datasource_config.h"
#include "dbmw/driver/driver_registry.h"

#if defined(DBMW_ENABLE_ASYNC_CORO)
#include "dbmw/async/task.h"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace dbmw;
using common::Status;
using common::ErrorCode;
namespace util = dbmw::common::util;

static int g_failed = 0;
static int g_passed = 0;

static void check(const bool cond, const std::string &name) {
    if (cond) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

using RowData = std::vector<std::pair<std::string, common::Value> >;

static std::vector<RowData> gRows;
static std::vector<std::vector<RowData> > gSets;
static std::atomic<int> gMainExec{0};
static std::atomic<int> gMainQuery{0};
static std::atomic<int> gShadowExec{0};
static std::atomic<int> gBegin{0};
static std::atomic<int> gCommit{0};
static std::atomic<int> gRollback{0};
static std::atomic<int> gFailRemaining{0};
static std::atomic<int> gExecDelayMs{0};
static std::int64_t gAffected = 1;
static std::string gLastSql;

static common::ResultSet buildResultSet(const std::vector<RowData> &rows) {
    common::ResultSet rs;
    if (!rows.empty()) {
        std::vector<std::string> fields;
        for (const auto &kv: rows.front()) fields.push_back(kv.first);
        rs.setFields(std::move(fields));
    }
    for (const auto &rd: rows) {
        common::Row r;
        for (const auto &kv: rd) r.set(kv.first, kv.second);
        rs.addRow(std::move(r));
    }
    return rs;
}

class UtilMockConnection : public core::IDatabaseConnection {
public:
    explicit UtilMockConnection(std::atomic<int> *execCounter,
                                std::atomic<int> *queryCounter)
        : exec_(execCounter), query_(queryCounter) {
    }

    Status connect(const config::DataSourceConfig &) override {
        open_ = true;
        return Status::OK();
    }

    Status ping() override {
        return open_ ? Status::OK() : Status::error(ErrorCode::NotConnected, "closed");
    }

    Status query(const std::string &sql, common::ResultSet &out) override {
        gLastSql = sql;
        if (query_) ++(*query_);
        out = buildResultSet(gRows);
        return Status::OK();
    }

    Status query(const std::string &sql, const common::Params &,
                 common::ResultSet &out) override {
        return query(sql, out);
    }

    Status execute(const std::string &sql, std::int64_t &affected) override {
        gLastSql = sql;
        if (exec_) ++(*exec_);
        if (gExecDelayMs.load() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(gExecDelayMs.load()));
        if (gFailRemaining.load() > 0) {
            gFailRemaining.fetch_sub(1);
            Status st = Status::error(ErrorCode::QueryError, "mock injected failure");
            st.retryable = true;
            return st;
        }
        affected = gAffected;
        return Status::OK();
    }

    Status execute(const std::string &sql, const common::Params &,
                   std::int64_t &affected) override {
        return execute(sql, affected);
    }

    Status queryEach(const std::string &sql, const common::Params &,
                     const common::RowCallback &cb, std::uint64_t &rows) override {
        gLastSql = sql;
        if (query_) ++(*query_);
        rows = 0;
        for (const auto &rd: gRows) {
            const auto rs = buildResultSet({rd});
            if (!cb(rs.rows().front())) break;
            ++rows;
        }
        return Status::OK();
    }

    Status begin() override {
        ++gBegin;
        tx_ = true;
        return Status::OK();
    }

    Status begin(const common::TransactionOptions &) override { return begin(); }

    Status commit() override {
        ++gCommit;
        tx_ = false;
        return Status::OK();
    }

    Status rollback() override {
        ++gRollback;
        tx_ = false;
        return Status::OK();
    }

    void close() override { open_ = false; }

    bool isOpen() const override { return open_; }

    bool inTransaction() const override { return tx_; }

    bool supportsParams() const override { return true; }

    bool supportsMultipleResultSets() const override { return true; }

    Status queryAll(const std::string &sql, const common::Params &,
                    std::vector<common::ResultSet> &out) override {
        gLastSql = sql;
        if (query_) ++(*query_);
        out.clear();
        for (const auto &set: gSets) out.push_back(buildResultSet(set));
        return Status::OK();
    }

private:
    std::atomic<int> *exec_;
    std::atomic<int> *query_;
    bool open_ = false;
    bool tx_ = false;
};

class MainDriver : public driver::IDriver {
public:
    const char *name() const override { return "umock"; }

    std::unique_ptr<core::IDatabaseConnection> createConnection() override {
        return std::make_unique<UtilMockConnection>(&gMainExec, &gMainQuery);
    }
};

class ShadowDriver : public driver::IDriver {
public:
    const char *name() const override { return "ushadow"; }

    std::unique_ptr<core::IDatabaseConnection> createConnection() override {
        return std::make_unique<UtilMockConnection>(&gShadowExec, nullptr);
    }
};

static void resetCounters() {
    gMainExec = 0;
    gMainQuery = 0;
    gShadowExec = 0;
    gBegin = 0;
    gCommit = 0;
    gRollback = 0;
    gFailRemaining = 0;
    gExecDelayMs = 0;
    gAffected = 1;
    gLastSql.clear();
    gSets.clear();
}

static std::string g_configPath;

static void writeConfig() {
    const std::string cfg = R"({
  "default_datasource": "main",
  "heartbeat_interval_ms": 3600000,
  "pool": { "enabled": true, "min": 0, "max": 4, "borrow_timeout_ms": 2000 },
  "retry": { "max_attempts": 2, "initial_backoff_ms": 1, "max_backoff_ms": 1, "retry_writes": true },
  "query_cache": { "enabled": true, "ttl_ms": 60000, "max_entries": 100 },
  "interceptors": { "enabled": false },
  "async": { "enabled": true, "threads": 2, "queue_size": 64 },
  "datasources": [
    { "name": "main", "type": "umock", "host": "localhost" },
    { "name": "shadow_ds", "type": "ushadow", "host": "localhost" },
    { "name": "my", "type": "mysql", "host": "localhost" },
    { "name": "pg", "type": "postgres", "host": "localhost" },
    { "name": "ms", "type": "odbc", "host": "localhost" },
    { "name": "ora", "type": "oracle", "host": "localhost" },
    { "name": "pgm", "type": "postgres_mock", "host": "localhost" },
    { "name": "mymock", "type": "mysql_mock", "host": "localhost" }
  ],
  "groups": [
    { "name": "grp", "primary": "main", "shadow": "shadow_ds", "replicas": [] }
  ]
}
)";
    std::ofstream(g_configPath) << cfg;
}

static void disableAudit() {
    config::SqlAuditConfig cfg;
    cfg.enabled = false;
    core::SqlAuditor::configure(cfg);
}

static void enableBlockingAudit() {
    config::SqlAuditConfig cfg;
    cfg.enabled = true;
    cfg.action = "block";
    cfg.log_blocked = false;
    core::SqlAuditor::configure(cfg);
}

#if defined(DBMW_ENABLE_ASYNC_CORO)
static async::Task<void> coroCallBody(std::promise<async::ExecResult> pr) {
    common::Params p;
    p.push_back(common::Value(std::int64_t(1)));
    async::util::Options o;
    o.dataSource = "main";
    auto r = co_await async::util::callAsync("CALL cp(?)", p, o);
    pr.set_value(std::move(r));
}
#endif

#if defined(DBMW_ENABLE_ASYNC_CORO)
static async::Task<void> coroCallAllBody(std::promise<async::MultiQueryResult> pr) {
    common::Params p;
    p.push_back(common::Value(std::int64_t(1)));
    async::util::Options o;
    o.dataSource = "mymock";
    auto r = co_await async::util::callAllAsync("CALL `p`(?)", p, o);
    pr.set_value(std::move(r));
}

static async::Task<void> coroScriptBody(std::promise<async::ExecResult> pr) {
    common::util::ScriptOptions o;
    o.dataSource = "main";
    auto r = co_await async::util::runScriptTextAsync("SELECT 1; SELECT 2; SELECT 3", o);
    pr.set_value(std::move(r));
}
#endif

int main() {
    g_configPath = (std::filesystem::temp_directory_path() / "dbmw_util_test.json").string();

    driver::DriverRegistry::instance().registerDriver(
        "umock", [] { return std::make_unique<MainDriver>(); });
    driver::DriverRegistry::instance().registerDriver(
        "ushadow", [] { return std::make_unique<ShadowDriver>(); });
    driver::DriverRegistry::instance().registerDriver(
        "postgres_mock", [] { return std::make_unique<MainDriver>(); });
    driver::DriverRegistry::instance().registerDriver(
        "mysql_mock", [] { return std::make_unique<MainDriver>(); });

    writeConfig();
    if (!DBMW::init(g_configPath).ok()) {
        std::cout << "init failed\n";
        return 1;
    }
    disableAudit();

    std::cout << "== U1. makeCallSql 三方言调用协议 ==\n";
    {
        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "main";
        util::RoutineRef fn;
        fn.name = "public.f";
        fn.kind = util::RoutineKind::Function;
        fn.dataSource = "main";
        std::string s;
        check(util::makeCallSql(proc, 2, util::Dialect::MySQL, false, s).ok() &&
              s == "CALL `p`(?, ?)", "MySQL: CALL `p`(?, ?)");
        check(util::makeCallSql(proc, 0, util::Dialect::MySQL, false, s).ok() &&
              s == "CALL `p`()", "MySQL: 无参 → CALL `p`()");
        check(util::makeCallSql(proc, 2, util::Dialect::Postgres, false, s).ok() &&
              s == "CALL \"p\"(?, ?)", "PG 存储过程: CALL \"p\"(?, ?)");
        check(util::makeCallSql(fn, 2, util::Dialect::Postgres, true, s).ok() &&
              s == "SELECT * FROM \"public\".\"f\"(?, ?)", "PG 函数: SELECT * FROM f(?, ?)");
        check(util::makeCallSql(fn, 1, util::Dialect::Postgres, false, s).ok() &&
              s == "SELECT \"public\".\"f\"(?)", "PG 函数无结果集: SELECT f(?)");
        check(util::makeCallSql(proc, 2, util::Dialect::SqlServer, false, s).ok() &&
              s == "EXEC \"p\" ?, ?", "SQLServer 无结果集: EXEC \"p\" ?, ?");
        check(util::makeCallSql(proc, 1, util::Dialect::SqlServer, true, s).ok() &&
              s == "{CALL \"p\"(?)}", "SQLServer 有结果集: {CALL \"p\"(?)}");
        check(!util::makeCallSql(proc, 1, util::Dialect::Auto, false, s).ok() &&
              util::makeCallSql(proc, 1, util::Dialect::Auto, false, s).code ==
              ErrorCode::NotSupported, "Auto 方言 → NotSupported（不猜）");
        check(util::makeCallSql(proc, 1, util::Dialect::Postgres, true, s).code ==
              ErrorCode::NotSupported, "PG 存储过程要结果集 → NotSupported");
        check(util::makeCallSql(proc, 2, util::Dialect::Oracle, false, s).ok() &&
              s == "BEGIN \"p\"(?, ?); END;", "Oracle 存储过程: BEGIN \"p\"(?, ?); END;");
        check(util::makeCallSql(fn, 2, util::Dialect::Oracle, true, s).ok() &&
              s == "SELECT * FROM TABLE(\"public\".\"f\"(?, ?))", "Oracle 表函数: TABLE(f(?, ?))");
        check(util::makeCallSql(fn, 1, util::Dialect::Oracle, false, s).ok() &&
              s == "SELECT \"public\".\"f\"(?) FROM DUAL", "Oracle 标量函数: SELECT f(?) FROM DUAL");
        check(util::makeCallSql(proc, 1, util::Dialect::Oracle, true, s).code ==
              ErrorCode::NotSupported, "Oracle 存储过程要结果集 → 需 REF CURSOR，NotSupported");
    }

    std::cout << "== U2. detectDialect ==\n";
    {
        check(util::detectDialect("my") == util::Dialect::MySQL, "type=mysql → MySQL");
        check(util::detectDialect("pg") == util::Dialect::Postgres, "type=postgres → Postgres");
        check(util::detectDialect("ms") == util::Dialect::SqlServer, "type=odbc → SqlServer");
        check(util::detectDialect("ora") == util::Dialect::Oracle, "type=oracle → Oracle");
        check(util::detectDialect("pgm") == util::Dialect::Postgres,
              "type 含 postgres → Postgres");
        check(util::detectDialect("main") == util::Dialect::Auto,
              "未知驱动 → Auto（要求显式传方言）");
    }

    std::cout << "== U3. DELIMITER 剥离 ==\n";
    {
        std::string s = "DELIMITER //\nCREATE PROCEDURE p() BEGIN SELECT 1; END //\nDELIMITER ;\n";
        const std::size_t n = util::stripDelimiterDirectives(s);
        check(n == 2, "剥离 2 条 DELIMITER 指令");
        check(s == "CREATE PROCEDURE p() BEGIN SELECT 1; END //\n", "例程体原样保留");

        resetCounters();
        const std::string raw =
                "DELIMITER //\nCREATE PROCEDURE p() BEGIN SELECT 1; END //\nDELIMITER ;\n";
        util::CreateRoutineOptions on;
        on.dataSource = "main";
        check(util::createRoutine(raw, on).ok(), "默认 stripDelimiter=true → 执行成功");
        check(gLastSql.find("DELIMITER") == std::string::npos, "发送的 SQL 已无 DELIMITER");

        util::CreateRoutineOptions off;
        off.dataSource = "main";
        off.stripDelimiter = false;
        check(util::createRoutine(raw, off).ok(), "stripDelimiter=false → 原样执行");
        check(gLastSql.find("DELIMITER") != std::string::npos, "原样发送（保留 DELIMITER）");
    }

    std::cout << "== U4. 多语句判定回归 ==\n";
    {
        using common::sql::hasMultipleStatements;
        using common::sql::isRoutineDdl;
        const std::string two = "SELECT 1; DROP TABLE t";
        check(hasMultipleStatements(two) && hasMultipleStatements(two, true),
              "普通双语句仍判 true");
        const std::string myProc = "CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END";
        check(hasMultipleStatements(myProc), "默认参数下例程体判 true（既有行为不变）");
        check(!hasMultipleStatements(myProc, true), "allowRoutineBody 下例程体判 false");
        const std::string pgFn =
                "CREATE FUNCTION f() RETURNS int AS $$ BEGIN RETURN 1; END; $$ LANGUAGE plpgsql";
        check(!hasMultipleStatements(pgFn) && !hasMultipleStatements(pgFn, true),
              "PG $$ 体本来就被 mask");
        check(isRoutineDdl(myProc) && isRoutineDdl(pgFn), "isRoutineDdl 识别两种方言");
        check(!isRoutineDdl("CREATE INDEX idx ON t (a)"), "CREATE INDEX 不是例程 DDL");
        check(!isRoutineDdl("SELECT 1"), "SELECT 不是例程 DDL");
        const std::string smuggle = "CREATE PROCEDURE p() BEGIN SELECT 1; END; DROP TABLE t";
        check(hasMultipleStatements(smuggle, true), "例程体后紧跟的独立语句仍被判 true");
        const std::string nested =
                "CREATE PROCEDURE p() BEGIN IF 1 THEN SELECT 1; END IF; "
                "CASE WHEN 1 THEN SELECT 2; ELSE SELECT 3; END CASE; END";
        check(!hasMultipleStatements(nested, true), "嵌套 IF/CASE 块仍正确配对");
    }

    std::cout << "== U5/U6/U7. 审计 ==\n";
    {
        enableBlockingAudit();
        std::int64_t aff = 0;
        const auto a = DBMW::execute("CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END", aff);
        check(a.ok(), "U5 例程 DDL 不被多语句规则拦下");
        check(a.code != ErrorCode::SqlBlocked, "U5 错误码不是 SqlBlocked");

        const auto b = DBMW::execute("SELECT 1; DROP TABLE t", aff);
        check(!b.ok() && b.code == ErrorCode::SqlBlocked, "U6 真多语句仍被拦下");

        config::SqlAuditConfig cfg;
        cfg.enabled = true;
        cfg.action = "block";
        cfg.log_blocked = false;
        cfg.whitelist_fingerprints.push_back(
            common::sql::fingerprintTemplate("SELECT 42"));
        core::SqlAuditor::configure(cfg);
        const auto c = DBMW::execute("CREATE PROCEDURE q() BEGIN SELECT 1; END", aff);
        check(!c.ok() && c.code == ErrorCode::SqlBlocked, "U7 不在白名单的例程 DDL 仍被拦下");
        disableAudit();
    }

    std::cout << "== U8. 影子库：DDL 绝不能落到影子 ==\n";
    {
        resetCounters();
        std::int64_t aff = 0;
        {
            common::SqlContext ctx;
            ctx.shadow = true;
            common::ContextScope scope(ctx);
            DBMW::execute("grp", "UPDATE t SET a = 1", aff);
        }
        check(gShadowExec.load() == 1 && gMainExec.load() == 0,
              "对照组：普通写在 shadow 上下文落到影子库");

        resetCounters();
        util::CreateRoutineOptions o;
        o.dataSource = "grp";
        Status st;
        {
            common::SqlContext ctx;
            ctx.shadow = true;
            common::ContextScope scope(ctx);
            st = util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", o);
        }
        check(st.ok(), "U8 util DDL 在 shadow 上下文执行成功");
        check(gMainExec.load() == 1 && gShadowExec.load() == 0,
              "U8 DDL 落到主库，影子库 0 次");
    }

    std::cout << "== U9. 幂等：DDL 默认不重试 ==\n";
    {
        resetCounters();
        gFailRemaining = 10;
        util::CreateRoutineOptions o;
        o.dataSource = "main";
        const auto a = util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", o);
        check(!a.ok(), "DDL 失败");
        check(gMainExec.load() == 1, "U9 默认 NonIdempotent → 只尝试 1 次");

        resetCounters();
        gFailRemaining = 10;
        util::CreateRoutineOptions idm;
        idm.dataSource = "main";
        idm.idempotency = common::Idempotency::Idempotent;
        util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", idm);
        check(gMainExec.load() == 2, "U9 显式 Idempotent → 按 max_attempts=2 重试");
        resetCounters();
    }

    std::cout << "== U10. 缓存失效 ==\n";
    {
        common::ResultSet seed = buildResultSet({
            {
                std::make_pair(
                    "a", common::Value(std::int64_t(1)))
            }
        });
        core::QueryCache::put("main", "k1", seed);
        common::ResultSet probe;
        check(core::QueryCache::get("main", "k1", probe), "前置：缓存里已有条目");

        util::CreateRoutineOptions o;
        o.dataSource = "main";
        check(util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", o).ok(),
              "创建例程成功");
        check(!core::QueryCache::get("main", "k1", probe), "U10 结构变更后该源缓存失效");
        resetCounters();
    }

    std::cout << "== U11/U12. 方言能力：不支持就显式报错 ==\n";
    {
        util::CreateRoutineOptions my;
        my.dataSource = "my";
        my.replace = true;
        check(util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", my).code ==
              ErrorCode::NotSupported, "U11 MySQL 不支持 CREATE OR REPLACE");
        util::CreateRoutineOptions ine;
        ine.dataSource = "pg";
        ine.ifNotExists = true;
        check(util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END", ine).code ==
              ErrorCode::NotSupported, "U11 IF NOT EXISTS 三个方言都不支持");

        util::RoutineRef ref;
        ref.name = "p";
        ref.dataSource = "my";
        util::DropRoutineOptions cas;
        cas.dataSource = "my";
        cas.cascade = true;
        check(util::dropRoutine(ref, cas).code == ErrorCode::NotSupported,
              "U12 CASCADE 仅 PG");

        std::string s;
        util::DropRoutineOptions pgOpt;
        util::RoutineRef pgRef;
        pgRef.name = "public.p";
        pgRef.dataSource = "pg";
        pgOpt.cascade = true;
        check(util::makeDropRoutineSql(pgRef, pgOpt, util::Dialect::Postgres, s).ok() &&
              s == "DROP PROCEDURE IF EXISTS \"public\".\"p\" CASCADE",
              "U12 PG: DROP PROCEDURE IF EXISTS ... CASCADE");
        check(util::makeDropRoutineSql(pgRef, util::DropRoutineOptions{},
                                       util::Dialect::MySQL, s).ok() &&
              s == "DROP PROCEDURE IF EXISTS `public`.`p`", "U12 MySQL DROP 语法");
        util::RoutineRef fnRef;
        fnRef.name = "f";
        fnRef.kind = util::RoutineKind::Function;
        check(util::makeDropRoutineSql(fnRef, util::DropRoutineOptions{},
                                       util::Dialect::SqlServer, s).ok() &&
              s == "DROP FUNCTION IF EXISTS \"f\"", "U12 SQLServer DROP FUNCTION");
    }

    std::cout << "== U13/U14. 索引生成 ==\n";
    {
        std::string s;
        util::IndexSpec base;
        base.table = "t";
        base.name = "idx";
        base.columns = {"a", "b"};
        check(util::makeCreateIndexSql(base, util::Dialect::MySQL, s).ok() &&
              s == "CREATE INDEX `idx` ON `t` (a, b)", "MySQL: CREATE INDEX `idx` ON `t` (a, b)");

        util::IndexSpec u = base;
        u.unique = true;
        u.columns = {"a"};
        u.usingMethod = "BTREE";
        check(util::makeCreateIndexSql(u, util::Dialect::MySQL, s).ok() &&
              s == "CREATE UNIQUE INDEX `idx` ON `t` (a) USING BTREE", "MySQL: USING 在列清单后");

        util::IndexSpec pgSpec = base;
        pgSpec.unique = true;
        pgSpec.columns = {"a"};
        pgSpec.concurrent = true;
        pgSpec.ifNotExists = true;
        pgSpec.usingMethod = "gin";
        check(util::makeCreateIndexSql(pgSpec, util::Dialect::Postgres, s).ok() &&
              s == "CREATE UNIQUE INDEX CONCURRENTLY IF NOT EXISTS \"idx\" ON \"t\" USING gin (a)",
              "PG: CONCURRENTLY + IF NOT EXISTS + USING gin");

        check(util::makeCreateIndexSql(base, util::Dialect::SqlServer, s).ok() &&
              s == "CREATE INDEX \"idx\" ON \"t\" (a, b)", "SQLServer: 无 USING");
        util::IndexSpec msU = base;
        msU.usingMethod = "BTREE";
        check(util::makeCreateIndexSql(msU, util::Dialect::SqlServer, s).code ==
              ErrorCode::NotSupported, "SQLServer USING → NotSupported");

        util::IndexSpec ine = base;
        ine.ifNotExists = true;
        check(util::makeCreateIndexSql(ine, util::Dialect::MySQL, s).code ==
              ErrorCode::NotSupported, "U14 MySQL 索引 IF NOT EXISTS → NotSupported");
        check(util::makeCreateIndexSql(ine, util::Dialect::Postgres, s).ok(),
              "U14 PG 索引 IF NOT EXISTS 通过");

        util::IndexSpec conc = base;
        conc.concurrent = true;
        check(util::makeCreateIndexSql(conc, util::Dialect::MySQL, s).code ==
              ErrorCode::NotSupported, "U14 MySQL CONCURRENTLY → NotSupported");

        check(util::makeDropIndexSql("t", "idx", true, util::Dialect::MySQL, s).ok() &&
              s == "DROP INDEX IF EXISTS `idx` ON `t`", "MySQL: DROP INDEX ... ON ...");
        check(util::makeDropIndexSql("t", "idx", true, util::Dialect::Postgres, s).ok() &&
              s == "DROP INDEX IF EXISTS \"idx\"", "PG: DROP INDEX（不带 ON）");
        check(util::makeDropIndexSql("t", "idx", false, util::Dialect::SqlServer, s).ok() &&
              s == "DROP INDEX \"idx\" ON \"t\"", "SQLServer: DROP INDEX ... ON ...");
    }

    std::cout << "== U15. CONCURRENTLY 不得在事务内 ==\n";
    {
        util::IndexSpec spec;
        spec.table = "t";
        spec.name = "idx";
        spec.columns = {"a"};
        spec.concurrent = true;
        util::CreateIndexOptions o;
        o.dataSource = "pgm";
        const auto outside = util::createIndex(spec, o);
        check(outside.ok(), "事务外 CONCURRENTLY 通过");

        bool insideFailed = false;
        ErrorCode insideCode = ErrorCode::Ok;
        DBMW::transaction("pgm", [&](core::Session &) {
            const auto st = util::createIndex(spec, o);
            insideFailed = !st.ok();
            insideCode = st.code;
            return Status::OK();
        });
        check(insideFailed && insideCode == ErrorCode::TxError,
              "U15 事务内 CONCURRENTLY → TxError");
    }

    std::cout << "== U16/U17/U18. 调用 ==\n";
    {
        resetCounters();
        gAffected = 7;
        std::int64_t aff = 0;
        common::Params p;
        p.push_back(common::Value(std::int64_t(1)));
        util::CallOptions o;
        o.dataSource = "main";
        check(util::call("CALL p(?)", p, aff, o).ok() && aff == 7, "U16 affected 正确返回");
        check(gLastSql == "CALL p(?)", "U16 SQL 原样送达");

        gRows = {
            {{"id", common::Value(std::int64_t(1))}, {"n", common::Value(std::string("a"))}},
            {{"id", common::Value(std::int64_t(2))}, {"n", common::Value(std::string("b"))}}
        };
        common::ResultSet rs;
        check(util::callQuery("SELECT * FROM f(?)", p, rs, o).ok() && rs.rowCount() == 2,
              "U17 callQuery 结果集正确");

        std::uint64_t rows = 0;
        std::vector<std::int64_t> seen;
        check(util::callEach("SELECT * FROM f(?)", p,
                             [&seen](const common::Row &r) {
                                 seen.push_back(std::get<std::int64_t>(r.at("id")));
                                 return true;
                             }, rows, o).ok() && rows == 2 && seen.size() == 2,
              "U18 callEach 流式行数正确");

        std::uint64_t stopped = 0;
        int visited = 0;
        util::callEach("SELECT * FROM f(?)", p,
                       [&visited](const common::Row &) {
                           ++visited;
                           return false;
                       }, stopped, o);
        check(visited == 1 && stopped == 0, "U18 回调返回 false 立即终止（不再消费后续行）");
        resetCounters();
    }

    std::cout << "== U19. 事务内 call ==\n";
    {
        resetCounters();
        common::Params p;
        p.push_back(common::Value(std::int64_t(1)));
        std::int64_t aff = 0;
        bool inTx = false;
        const auto st = DBMW::transaction("main", [&](core::Session &s) {
            inTx = s.inTransaction();
            return util::call(s, "CALL p(?)", p, aff);
        });
        check(st.ok() && aff == 1 && inTx, "U19 事务内 call 与 Session::execute 语义一致");
        check(gBegin.load() == 1 && gCommit.load() == 1, "U19 begin/commit 各 1 次");
        check(core::currentTransactionDepth() == 0, "U19 事务结束后深度归零");
        resetCounters();
    }

    std::cout << "== U20. 异步三形态 ==\n";
    {
        resetCounters();
        gAffected = 3;
        common::Params p;
        p.push_back(common::Value(std::int64_t(1)));
        async::util::Options o;
        o.dataSource = "main";

        std::promise<async::ExecResult> pr1;
        auto f1 = pr1.get_future();
        async::util::call("CALL p(?)", p,
                          [&pr1](async::ExecResult &&r) { pr1.set_value(std::move(r)); }, o);
        const auto r1 = f1.get();
        check(r1.status.ok() && r1.affected == 3, "U20 回调形态与同步同结果");

        auto f2 = async::util::callQuery("SELECT * FROM f(?)", p, o);
        const auto r2 = f2.get();
        check(r2.status.ok() && r2.rows.rowCount() == 2, "U20 future 形态与同步同结果");

        resetCounters();
        std::promise<async::OpResult> pr3;
        auto f3 = pr3.get_future();
        async::util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END",
                                   [&pr3](async::OpResult &&r) { pr3.set_value(std::move(r)); },
                                   o);
        check(f3.get().status.ok(), "U20 异步 createRoutine 成功");

        resetCounters();
        std::promise<async::OpResult> pr4;
        auto f4 = pr4.get_future();
        async::util::Options grp;
        grp.dataSource = "grp";
        {
            common::SqlContext ctx;
            ctx.shadow = true;
            common::ContextScope scope(ctx);
            async::util::createRoutine("CREATE PROCEDURE p() BEGIN SELECT 1; END",
                                       [&pr4](async::OpResult &&r) { pr4.set_value(std::move(r)); },
                                       grp);
        }
        check(f4.get().status.ok(), "U20 异步 DDL 成功");
        check(gMainExec.load() == 1 && gShadowExec.load() == 0,
              "U20 异步同样强制主库（I7 治理一致）");

#if defined(DBMW_ENABLE_ASYNC_CORO)
        resetCounters();
        gAffected = 5;
        std::promise<async::ExecResult> pr5;
        auto f5 = pr5.get_future();
        async::run(coroCallBody(std::move(pr5)));
        const auto r5 = f5.get();
        check(r5.status.ok() && r5.affected == 5, "U20 协程形态与同步同结果");
#endif
        resetCounters();
    }

    std::cout << "== U21. makeCallPlan：OUT/INOUT 的 SQL 编排 ==\n";
    {
        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::RoutineRef fn;
        fn.name = "public.f";
        fn.kind = util::RoutineKind::Function;
        fn.dataSource = "pgm";

        util::CallParams inOnly;
        inOnly.emplace_back(common::Value(std::int64_t(7)));
        util::CallPlan plan;
        check(util::makeCallPlan(proc, inOnly, util::Dialect::MySQL, true, plan).ok() &&
              plan.callSql == "CALL `p`(?)" && plan.preSql.empty() && plan.fetchSql.empty(),
              "U21 MySQL 纯 IN 不需要前后置语句");

        util::CallParams withOut;
        withOut.emplace_back(common::Value(std::int64_t(7)));
        withOut.emplace_back(util::CallParam{
            util::ParamDirection::Out,
            common::Value(std::int64_t(0))
        });
        util::CallPlan outPlan;
        check(util::makeCallPlan(proc, withOut, util::Dialect::MySQL, true, outPlan).ok() &&
              outPlan.callSql == "CALL `p`(?, @dbmw_out_1)" &&
              outPlan.fetchSql == "SELECT @dbmw_out_1 AS dbmw_out_1" &&
              outPlan.needsSameConnection && outPlan.callParams.size() == 1,
              "U21 MySQL OUT → 会话变量 + 回读 SELECT");

        util::CallParams inOut;
        inOut.emplace_back(util::CallParam{
            util::ParamDirection::InOut,
            common::Value(std::int64_t(3))
        });
        util::CallPlan ioPlan;
        check(util::makeCallPlan(proc, inOut, util::Dialect::MySQL, true, ioPlan).ok() &&
              ioPlan.preSql == "SET @dbmw_out_0 = ?" &&
              ioPlan.callSql == "CALL `p`(@dbmw_out_0)" &&
              ioPlan.fetchSql == "SELECT @dbmw_out_0 AS dbmw_out_0" &&
              ioPlan.preParams.size() == 1 && ioPlan.callParams.empty(),
              "U21 MySQL INOUT → SET + CALL + 回读");

        util::CallPlan pgPlan;
        check(util::makeCallPlan(fn, withOut, util::Dialect::Postgres, true, pgPlan).ok() &&
              pgPlan.callSql == "SELECT * FROM \"public\".\"f\"(?)" &&
              pgPlan.outFromRowCount == 1 && !pgPlan.needsSameConnection,
              "U21 PG 函数 OUT 不占调用位、走结果列");

        util::CallPlan bad;
        check(util::makeCallPlan(proc, withOut, util::Dialect::Postgres, false, bad).code ==
              ErrorCode::NotSupported, "U21 PG 存储过程 OUT → NotSupported");
        check(util::makeCallPlan(proc, withOut, util::Dialect::SqlServer, false, bad).code ==
              ErrorCode::NotSupported, "U21 SqlServer OUT → NotSupported（缺变量类型）");
        check(util::makeCallPlan(fn, withOut, util::Dialect::Postgres, false, bad).code ==
              ErrorCode::NotSupported, "U21 PG OUT 参数要求 returnsRows=true");
    }

    std::cout << "== U22. 结构化 call：一次调用拿全部结果集 ==\n";
    {
        resetCounters();
        RowData r1a{{"id", common::Value(std::int64_t(1))}, {"n", common::Value(std::string("a"))}};
        RowData r1b{{"id", common::Value(std::int64_t(2))}, {"n", common::Value(std::string("b"))}};
        RowData r2a{{"total", common::Value(std::int64_t(9))}};
        gSets = {{r1a, r1b}, {r2a}};

        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams params;
        params.emplace_back(common::Value(std::int64_t(1)));
        util::CallOptions o;
        o.dataSource = "mymock";

        util::CallResult r;
        const auto st = util::call(proc, params, r, o);
        check(st.ok() && r.sets.size() == 2, "U22 一次调用收集到 2 个结果集");
        check(r.rowCount() == 3 && r.sets[0].rowCount() == 2 && r.sets[1].rowCount() == 1,
              "U22 行分布在各结果集内正确");
        check(r.sets[1].rows().front().at("total") == common::Value(std::int64_t(9)),
              "U22 第二个结果集内容可读");
        check(gLastSql == "CALL `p`(?)", "U22 按方言生成调用语句");
    }

    std::cout << "== U23. 池路径拒绝 OUT（需要连接亲和）==\n";
    {
        resetCounters();
        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams withOut;
        withOut.emplace_back(common::Value(std::int64_t(7)));
        withOut.emplace_back(util::CallParam{
            util::ParamDirection::Out,
            common::Value(std::int64_t(0))
        });
        util::CallOptions o;
        o.dataSource = "mymock";
        util::CallResult r;
        const auto st = util::call(proc, withOut, r, o);
        check(st.code == ErrorCode::NotSupported && r.outParams.empty(),
              "U23 池路径 + OUT → NotSupported（提示改用 Session）");
    }

    std::cout << "== U24. Session 路径：OUT 回读 ==\n";
    {
        resetCounters();
        RowData r1a{{"id", common::Value(std::int64_t(1))}};
        RowData r1b{{"id", common::Value(std::int64_t(2))}};
        gSets = {{r1a, r1b}};
        RowData outRow{{"dbmw_out_1", common::Value(std::int64_t(42))}};
        gRows = {outRow};

        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams withOut;
        withOut.emplace_back(common::Value(std::int64_t(7)));
        withOut.emplace_back(util::CallParam{
            util::ParamDirection::Out,
            common::Value(std::int64_t(0))
        });

        util::CallResult r;
        const auto st = DBMW::transaction("mymock", [&](core::Session &s) {
            return util::call(s, proc, withOut, r);
        });
        check(st.ok() && r.outParams.size() == 1 &&
              r.outParams[0] == common::Value(std::int64_t(42)), "U24 Session 路径读回 OUT=42");
        check(r.sets.size() == 1 && r.sets[0].rowCount() == 2, "U24 同时拿到过程返回的结果集");
    }

    std::cout << "== U25. Session 路径：INOUT 先 SET 再 CALL ==\n";
    {
        resetCounters();
        RowData outRow{{"dbmw_out_0", common::Value(std::int64_t(11))}};
        gRows = {outRow};
        gSets.clear();

        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams inOut;
        inOut.emplace_back(util::CallParam{
            util::ParamDirection::InOut,
            common::Value(std::int64_t(3))
        });

        util::CallResult r;
        const auto st = DBMW::transaction("mymock", [&](core::Session &s) {
            return util::call(s, proc, inOut, r);
        });
        check(st.ok() && gMainExec.load() == 1, "U25 INOUT 先执行 SET 赋值（1 次 execute）");
        check(r.outParams.size() == 1 && r.outParams[0] == common::Value(std::int64_t(11)),
              "U25 INOUT 回读新值 11");
    }

    std::cout << "== U26. PG 函数 OUT：从结果列取值 ==\n";
    {
        resetCounters();
        RowData pgRow{
            {"o", common::Value(std::int64_t(7))},
            {"v", common::Value(std::string("x"))}
        };
        gSets = {{pgRow}};

        util::RoutineRef fn;
        fn.name = "f";
        fn.kind = util::RoutineKind::Function;
        fn.dataSource = "pgm";
        util::CallParams params;
        params.emplace_back(common::Value(std::int64_t(1)));
        params.emplace_back(util::CallParam{
            util::ParamDirection::Out,
            common::Value(std::int64_t(0))
        });
        util::CallOptions o;
        o.dataSource = "pgm";

        util::CallResult r;
        const auto st = util::call(fn, params, r, o);
        check(st.ok() && r.outParams.size() == 1 &&
              r.outParams[0] == common::Value(std::int64_t(7)),
              "U26 PG OUT 取结果首行首列=7");
        check(gLastSql == "SELECT * FROM \"f\"(?)", "U26 PG 生成 SELECT * FROM f(?)");
    }

    std::cout << "== U27. OUT 回读的失败路径 ==\n";
    {
        std::vector<common::Value> outs;
        common::ResultSet noRow;
        check(util::readOutParams(noRow, {"dbmw_out_0"}, outs).code == ErrorCode::QueryError,
              "U27 回读无行 → QueryError");

        common::ResultSet empty;
        util::CallParams params;
        const auto st = util::readOutParamsFromRow(empty, 1, outs);
        check(st.code == ErrorCode::QueryError, "U27 结果集为空时读 OUT → QueryError");

        RowData only1{{"a", common::Value(std::int64_t(1))}};
        common::ResultSet one;
        one.setFields({"a"});
        common::Row row;
        row.set("a", common::Value(std::int64_t(1)));
        one.addRow(std::move(row));
        check(util::readOutParamsFromRow(one, 2, outs).code == ErrorCode::QueryError,
              "U27 列数不足 → QueryError");
    }

    std::cout << "== U28. 异步多结果集 ==\n";
    {
        resetCounters();
        RowData r1a{{"id", common::Value(std::int64_t(1))}};
        RowData r1b{{"id", common::Value(std::int64_t(2))}};
        RowData r2a{{"total", common::Value(std::int64_t(9))}};
        gSets = {{r1a, r1b}, {r2a}};

        async::util::Options ao;
        ao.dataSource = "mymock";
        common::Params p;
        p.push_back(common::Value(std::int64_t(1)));
        const auto ar = async::util::callAll("CALL `p`(?)", p, ao).get();
        check(ar.status.ok() && ar.sets.size() == 2 && ar.rowCount() == 3,
              "U28 future 形态收集 2 个结果集");

        std::promise<async::MultiQueryResult> pr;
        auto fut = pr.get_future();
        async::util::callAll("CALL `p`(?)", p,
                             [&pr](async::MultiQueryResult &&r) { pr.set_value(std::move(r)); },
                             ao);
        check(fut.get().sets.size() == 2, "U28 回调形态收集 2 个结果集");

        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams params;
        params.emplace_back(common::Value(std::int64_t(1)));
        std::promise<async::MultiQueryResult> pr2;
        auto fut2 = pr2.get_future();
        async::util::call(proc, params,
                          [&pr2](async::MultiQueryResult &&r) { pr2.set_value(std::move(r)); },
                          ao);
        check(fut2.get().status.ok(), "U28 结构化异步调用");

        util::CallParams withOut;
        withOut.emplace_back(common::Value(std::int64_t(7)));
        withOut.emplace_back(util::CallParam{
            util::ParamDirection::Out,
            common::Value(std::int64_t(0))
        });
        std::promise<async::MultiQueryResult> pr3;
        auto fut3 = pr3.get_future();
        async::util::call(proc, withOut,
                          [&pr3](async::MultiQueryResult &&r) { pr3.set_value(std::move(r)); },
                          ao);
        check(fut3.get().status.code == ErrorCode::NotSupported,
              "U28 异步路径拒绝 OUT/INOUT");

#if defined(DBMW_ENABLE_ASYNC_CORO)
        std::promise<async::MultiQueryResult> pr4;
        auto fut4 = pr4.get_future();
        async::run(coroCallAllBody(std::move(pr4)));
        const auto cr = fut4.get();
        check(cr.status.ok() && cr.sets.size() == 2, "U28 协程形态收集 2 个结果集");
#endif
        resetCounters();
    }

    std::cout << "== U29. returnsRows=false 走 execute ==\n";
    {
        resetCounters();
        gAffected = 4;
        gSets.clear();
        util::RoutineRef proc;
        proc.name = "p";
        proc.dataSource = "mymock";
        util::CallParams params;
        params.emplace_back(common::Value(std::int64_t(1)));
        util::CallOptions o;
        o.dataSource = "mymock";
        o.returnsRows = false;
        util::CallResult r;
        const auto st = util::call(proc, params, r, o);
        check(st.ok() && r.sets.empty() && r.affected == 4,
              "U29 returnsRows=false → 只报 affected");
    }

    std::cout << "== U30. splitSqlScript 拆分 ==\n";
    {
        std::vector<std::string> out;
        util::splitSqlScript("SELECT 1; SELECT 2; SELECT 3", out);
        check(out.size() == 3, "U30 普通三条语句拆成 3 段");

        out.clear();
        util::splitSqlScript("SELECT 'a;b' ; SELECT 2", out);
        check(out.size() == 2 && out[0] == "SELECT 'a;b'", "U30 字符串内的分号不被拆");

        out.clear();
        util::splitSqlScript("SELECT 1 -- c;d\n; SELECT 2", out);
        check(out.size() == 2, "U30 行注释内的分号不被拆");

        out.clear();
        util::splitSqlScript("SELECT 1 /* x;y */ ; SELECT 2", out);
        check(out.size() == 2, "U30 块注释内的分号不被拆");

        out.clear();
        util::splitSqlScript("CREATE PROCEDURE p() BEGIN SELECT 1; SELECT 2; END", out);
        check(out.size() == 1, "U30 BEGIN..END 复合块整段不拆");

        out.clear();
        util::splitSqlScript("SELECT 1 ; ; SELECT 2", out);
        check(out.size() == 2, "U30 空语句被丢弃");
    }

    std::cout << "== U31. runScriptText 内存脚本 ==\n";
    {
        resetCounters();
        util::ScriptOptions o;
        o.dataSource = "main";
        std::size_t ex = 0;
        const auto st = util::runScriptText("SELECT 1; SELECT 2; SELECT 3", o, &ex);
        check(st.ok() && ex == 3, "U31 三条语句全部执行");
        check(gMainExec.load() == 3, "U31 主库 execute 计数 = 3");
        resetCounters();
    }

    std::cout << "== U32. stopOnError=false 继续执行 ==\n";
    {
        resetCounters();
        gFailRemaining = 1;
        util::ScriptOptions o;
        o.dataSource = "main";
        o.stopOnError = false;
        std::size_t ex = 0;
        const auto st = util::runScriptText("SELECT 1; SELECT 2; SELECT 3", o, &ex);
        check(!st.ok() && st.code == ErrorCode::QueryError, "U32 失败后返回错误");
        check(ex == 2, "U32 后续语句仍执行（executed=2）");
        check(gMainExec.load() == 3, "U32 三条语句都尝试过");
        resetCounters();
        gFailRemaining = 0;
    }

    std::cout << "== U33/U34. runScripts / runScriptsInDir ==\n";
    {
        const std::string base = (std::filesystem::temp_directory_path() /
                                  "dbmw_script_test").string();
        std::error_code rec;
        std::filesystem::remove_all(base, rec);
        std::filesystem::create_directories(base + "/sub", rec);
        auto writef = [](const std::string &p, const std::string &c) {
            std::ofstream(p) << c;
        };
        writef(base + "/01.sql", "SELECT 1; SELECT 2");
        writef(base + "/02.sql", "SELECT 3");
        writef(base + "/sub/03.sql", "SELECT 4; SELECT 5; SELECT 6");
        writef(base + "/notes.txt", "not sql");

        resetCounters();
        util::ScriptOptions o;
        o.dataSource = "main";
        std::vector<util::ScriptResult> per;
        const auto s1 = util::runScripts({base + "/01.sql", base + "/02.sql"}, o, &per);
        check(s1.ok() && per.size() == 2, "U33 两个文件均执行");
        check(per[0].statements == 2 && per[0].executed == 2, "U33 01.sql 2 条全执行");
        check(per[1].statements == 1 && per[1].executed == 1, "U33 02.sql 1 条全执行");
        check(gMainExec.load() == 3, "U33 主库 execute = 3");
        resetCounters();

        std::vector<util::ScriptResult> per2;
        const auto s2 = util::runScriptsInDir(base, o, &per2);
        check(s2.ok() && per2.size() == 3, "U34 递归发现 3 个 .sql（notes.txt 被过滤）");
        std::size_t totalExec = 0;
        for (const auto &r: per2) totalExec += r.executed;
        check(totalExec == 6, "U34 共执行 6 条语句");
        check(gMainExec.load() == 6, "U34 主库 execute = 6");

        resetCounters();
        util::ScriptOptions flat = o;
        flat.recursive = false;
        std::vector<util::ScriptResult> per3;
        const auto s3 = util::runScriptsInDir(base, flat, &per3);
        check(s3.ok() && per3.size() == 2, "U34 非递归仅顶层 2 个 .sql");
        resetCounters();
        std::filesystem::remove_all(base, rec);
    }

    std::cout << "== U35. 目录不存在 → IoError ==\n";
    {
        util::ScriptOptions o;
        o.dataSource = "main";
        const auto st = util::runScriptsInDir("/no/such/dbmw_dir_xyz", o);
        check(!st.ok() && st.code == ErrorCode::IoError, "U35 目录不存在返回 IoError");
    }

    std::cout << "== U36. 文件不可读 / stopOnError 行为 ==\n";
    {
        util::ScriptOptions o;
        o.dataSource = "main";
        std::vector<util::ScriptResult> per;
        const auto st = util::runScripts({"/no/such/dbmw_file.sql"}, o, &per);
        check(!st.ok() && st.code == ErrorCode::IoError, "U36 缺文件返回 IoError");
        check(!per.empty() && per[0].status.code == ErrorCode::IoError, "U36 perFile 记录 IoError");

        const std::string base = (std::filesystem::temp_directory_path() /
                                  "dbmw_script_test2").string();
        std::error_code rec;
        std::filesystem::remove_all(base, rec);
        std::filesystem::create_directories(base, rec);
        auto writef = [](const std::string &p, const std::string &c) {
            std::ofstream(p) << c;
        };
        writef(base + "/ok.sql", "SELECT 1");
        util::ScriptOptions no = o;
        no.stopOnError = false;
        std::vector<util::ScriptResult> per2;
        const auto st2 = util::runScripts({base + "/ok.sql", "/no/such/dbmw_file.sql"}, no, &per2);
        check(!st2.ok() && per2.size() == 2, "U36 不停止时两文件都记录");
        std::filesystem::remove_all(base, rec);
    }

    std::cout << "== U37. 异步三形态 ==\n";
    {
        util::ScriptOptions o;
        o.dataSource = "main";

        const auto fr = async::util::runScriptText("SELECT 1; SELECT 2; SELECT 3", o).get();
        check(fr.status.ok(), "U37 future 形态执行成功");

        std::promise<async::ExecResult> pr;
        auto fut = pr.get_future();
        const auto handle = async::util::runScriptText(
            "SELECT 1; SELECT 2",
            [&pr](async::ExecResult &&r) { pr.set_value(std::move(r)); }, o);
        check(handle.valid(), "U37 回调形态返回可跟踪句柄");
        check(fut.get().status.ok(), "U37 回调形态执行成功");

        resetCounters();
        gFailRemaining = 1;
        util::ScriptOptions continueOnError = o;
        continueOnError.stopOnError = false;
        const auto failed = async::util::runScriptText(
            "SELECT 1; SELECT 2; SELECT 3", continueOnError).get();
        check(!failed.status.ok() && failed.status.code == ErrorCode::QueryError,
              "U37 stopOnError=false 继续执行后仍返回错误");
        check(gMainExec.load() == 3, "U37 异步脚本严格按顺序尝试全部语句");
        resetCounters();
        gFailRemaining = 0;

        std::promise<async::ExecResult> cancelledPromise;
        auto cancelledFuture = cancelledPromise.get_future();
        gExecDelayMs = 100;
        const auto cancellable = async::util::runScriptText(
            "SELECT 1; SELECT 2; SELECT 3",
            [&cancelledPromise](async::ExecResult &&r) {
                cancelledPromise.set_value(std::move(r));
            }, o);
        for (int i = 0; i < 100 && gMainExec.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancellable.cancel();
        const auto cancelled = cancelledFuture.get();
        check(cancelled.status.code == ErrorCode::Cancelled,
              "U37 取消脚本后以 Cancelled 完成");
        check(gMainExec.load() == 1, "U37 取消后不再调度后续语句");
        resetCounters();

        const std::string base = (std::filesystem::temp_directory_path() /
                                  "dbmw_script_test3").string();
        std::error_code rec;
        std::filesystem::remove_all(base, rec);
        std::filesystem::create_directories(base, rec);
        std::ofstream(base + "/a.sql") << "SELECT 1; SELECT 2";
        std::ofstream(base + "/b.sql") << "SELECT 3";
        const auto dr = async::util::runScriptsInDir(base, o).get();
        check(dr.status.ok(), "U37 runScriptsInDir future 成功");
        std::filesystem::remove_all(base, rec);

#if defined(DBMW_ENABLE_ASYNC_CORO)
        std::promise<async::ExecResult> prc;
        auto futc = prc.get_future();
        async::run(coroScriptBody(std::move(prc)));
        check(futc.get().status.ok(), "U37 协程形态执行成功");
#endif
    }

    std::cout << "\npassed=" << g_passed << " failed=" << g_failed << "\n";
    DBMW::shutdown(std::chrono::milliseconds(0));
    return g_failed == 0 ? 0 : 1;
}
