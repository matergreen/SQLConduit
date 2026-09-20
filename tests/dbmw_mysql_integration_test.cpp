#include "dbmw/async/dbmw_async.h"
#include "dbmw/dbmw.h"
#include "dbmw/mapping.h"
#include "dbmw/util.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

struct MapItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    std::uint64_t unsignedValue = 0;
};
namespace dbmw::mapping {
    template <> struct RowMapper<MapItem> {
        static Mapping<MapItem> describe() {
            return Mapping<MapItem>()
                .field(&MapItem::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                .field(&MapItem::name, "name")
                .field(&MapItem::qty, "qty")
                .field(&MapItem::unsignedValue, "unsigned_value");
        }
    };
}

namespace {
using dbmw::common::ErrorCode;
using dbmw::common::Params;
using dbmw::common::ResultSet;
using dbmw::common::Status;
using dbmw::common::Value;
using dbmw::common::util::CallParam;
using dbmw::common::util::CallParams;
using dbmw::common::util::CallResult;
using dbmw::common::util::ParamDirection;
using dbmw::common::util::RoutineKind;
using dbmw::common::util::RoutineRef;

int checks = 0;
void require(bool condition, const std::string &message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void requireOk(const Status &status, const std::string &where) {
    require(status.ok(), where + " failed: [" + dbmw::common::errorCodeToString(status.code)
                         + "] " + status.message + " sqlstate=" + status.sqlState);
}
std::string env(const char *name, const std::string &fallback = {}) {
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}
std::string quoteJson(const std::string &value) {
    std::string out;
    for (const char c: value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
std::int64_t asInt(const Value &v) { return std::get<std::int64_t>(v); }

struct Fixture {
    std::string table;
    std::string configPath;
    bool initialized = false;
    Fixture() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        table = "dbmw_it_" + std::to_string(static_cast<unsigned long long>(stamp));
        configPath = "/tmp/" + table + ".json";
    }
    ~Fixture() {
        if (initialized) {
            std::int64_t affected = 0;
            (void)dbmw::DBMW::execute("DROP TABLE IF EXISTS " + table, affected);
            dbmw::DBMW::shutdown(std::chrono::milliseconds(3000));
        }
        std::remove(configPath.c_str());
    }
    void start() {
        const auto host = env("DBMW_TEST_MYSQL_HOST", "127.0.0.1");
        const auto port = env("DBMW_TEST_MYSQL_PORT", "3306");
        const auto user = env("DBMW_TEST_MYSQL_USER", "root");
        const auto database = env("DBMW_TEST_MYSQL_DATABASE", "dbmw_test");
        require(!env("DBMW_TEST_MYSQL_PASSWORD").empty(),
                "DBMW_TEST_MYSQL_PASSWORD must be set");
        std::ofstream file(configPath);
        file << "{\"default_datasource\":\"mysql\","
                "\"pool\":{\"min\":0,\"max\":4,\"borrow_timeout_ms\":2000},"
                "\"prepared_cache\":{\"enabled\":true,\"max_per_connection\":1},"
                "\"cursor\":{\"enabled\":true,\"default_batch_size\":2,"
                "\"max_open_cursors\":1},"
                "\"observability\":{\"slow_sql\":{\"enabled\":true,"
                "\"threshold_ms\":0,\"aggregate_capacity\":100,"
                "\"recent_capacity\":100},\"pool_metrics\":{\"enabled\":true}},"
                "\"async\":{\"enabled\":true,\"threads\":2,\"queue_size\":64},"
                "\"datasources\":[{\"name\":\"mysql\",\"type\":\"mysql\","
                "\"host\":\"" << quoteJson(host) << "\",\"port\":" << port
             << ",\"user\":\"" << quoteJson(user)
             << "\",\"password_env\":\"DBMW_TEST_MYSQL_PASSWORD\","
                "\"database\":\"" << quoteJson(database)
             << "\",\"connection_timeout_ms\":5000}]}";
        file.close();
        requireOk(dbmw::DBMW::init(configPath), "init");
        initialized = true;
        std::int64_t affected = 0;
        requireOk(dbmw::DBMW::execute(
            "CREATE TABLE " + table + " (id BIGINT AUTO_INCREMENT PRIMARY KEY, "
            "name VARCHAR(100) NOT NULL UNIQUE, qty BIGINT NOT NULL, unsigned_value BIGINT "
            "UNSIGNED NOT NULL, amount DECIMAL(30,9), due_date DATE, local_time TIME(6), "
            "metadata JSON, payload BLOB, created_at DATETIME(6)) ENGINE=InnoDB", affected),
            "create table");
    }
};

void testTypesAndKeys(Fixture &f) {
    auto ds = dbmw::DBMW::dataSource();
    require(ds != nullptr, "default datasource missing");
    dbmw::common::GeneratedKeys keys;
    std::int64_t affected = 0;
    const dbmw::common::Blob blob{0, 1, 127, 128, 255};
    requireOk(ds->execute(
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,amount,due_date,local_time,"
        "metadata,payload,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
        Params{std::string("alpha"), std::int64_t(7),
               std::uint64_t{18446744073709551615ULL},
               dbmw::common::Decimal{"123456789012345678901.123456789"},
               dbmw::common::Date{"2026-09-06"}, dbmw::common::Time{"11:50:00.123456"},
               dbmw::common::Json{"{\"ok\":true}"}, blob,
               dbmw::common::Timestamp{std::chrono::system_clock::now()}},
        affected, keys), "insert with generated key");
    require(affected == 1 && keys.lastInsertId() > 0, "generated key missing");

    ResultSet rows;
    requireOk(dbmw::DBMW::query(
        "SELECT qty,unsigned_value,amount,due_date,local_time,metadata,payload,created_at "
        "FROM " + f.table + " WHERE id=?", Params{keys.lastInsertId()}, rows), "type query");
    require(rows.rowCount() == 1, "type query row count");
    const auto &row = rows.rows().front();
    require(asInt(row.at("qty")) == 7, "signed bigint mismatch");
    require(std::get<std::uint64_t>(row.at("unsigned_value")) ==
                18446744073709551615ULL, "unsigned bigint mismatch");
    require(std::get<dbmw::common::Decimal>(row.at("amount")).value ==
                "123456789012345678901.123456789", "decimal precision lost");
    require(std::get<dbmw::common::Date>(row.at("due_date")).value == "2026-09-06",
            "date mismatch");
    require(std::get<dbmw::common::Time>(row.at("local_time")).value == "11:50:00.123456",
            "time mismatch");
    require(std::holds_alternative<dbmw::common::Json>(row.at("metadata")), "json type lost");
    require(std::get<dbmw::common::Blob>(row.at("payload")) == blob, "blob mismatch");
    require(std::holds_alternative<dbmw::common::Timestamp>(row.at("created_at")),
            "datetime type lost");
}

void testTransactionsBatchCursorAndAsync(Fixture &f) {
    dbmw::common::ParamBatch batch{{std::string("b1"), std::int64_t(1)},
                                   {std::string("b2"), std::int64_t(2)}};
    dbmw::common::BatchResult batchResult;
    requireOk(dbmw::DBMW::executeBatch(
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
        "VALUES (?,?,1,NOW(6))", batch, batchResult), "batch");
    require(batchResult.totalAffected() == 2, "batch affected mismatch");

    const auto rollback = dbmw::DBMW::transaction([&](dbmw::core::Session &session) {
        std::int64_t affected = 0;
        auto st = session.execute("INSERT INTO " + f.table
            + " (name,qty,unsigned_value,created_at) VALUES ('rollback',1,1,NOW(6))", affected);
        if (!st.ok()) return st;
        return Status::error(ErrorCode::TxError, "intentional rollback");
    });
    require(rollback.code == ErrorCode::TxError, "rollback status mismatch");
    ResultSet rolled;
    requireOk(dbmw::DBMW::query("SELECT COUNT(*) n FROM " + f.table
                                + " WHERE name='rollback'", rolled), "verify rollback");
    require(asInt(rolled.rows()[0].at("n")) == 0, "transaction was committed");

    requireOk(dbmw::DBMW::withSession([&](dbmw::core::Session &session) {
        dbmw::core::PreparedStatementHandle first, second;
        auto st = session.prepare("SELECT id FROM " + f.table + " WHERE name=?",
                                  {std::string()}, first);
        if (!st.ok()) return st;
        st = session.prepare("SELECT qty FROM " + f.table + " WHERE name=?",
                             {std::string()}, second);
        if (!st.ok()) return st;
        ResultSet out;
        const auto evicted = session.executePrepared(first, {std::string("alpha")}, out);
        return evicted.code == ErrorCode::QueryError ? Status::OK()
            : Status::error(ErrorCode::QueryError, "evicted handle unexpectedly executed");
    }), "prepared LRU safety");

    std::uint64_t delivered = 0;
    int callbacks = 0;
    requireOk(dbmw::DBMW::queryEach("SELECT id FROM " + f.table + " ORDER BY id", {},
        [&](const dbmw::common::Row &) { return ++callbacks < 2; }, delivered), "queryEach");
    require(delivered == 2, "queryEach early stop mismatch");

    dbmw::core::CursorOptions options;
    options.batch_size = 2;
    std::unique_ptr<dbmw::core::Cursor> cursor;
    requireOk(dbmw::DBMW::openCursor("SELECT id FROM " + f.table + " ORDER BY id", {},
                                     options, cursor), "open cursor");
    ResultSet streamed;
    while (cursor->hasNext()) requireOk(cursor->fetch(2, streamed), "cursor fetch");
    require(streamed.rowCount() >= 3, "cursor missed rows");
    requireOk(cursor->close(), "cursor close");

    auto future = dbmw::async::query("SELECT COUNT(*) n FROM " + f.table);
    const auto asyncRows = future.get();
    requireOk(asyncRows.status, "async query");
    require(asInt(asyncRows.rows.rows()[0].at("n")) >= 3, "async count mismatch");

    std::int64_t affected = 0;
    const auto duplicate = dbmw::DBMW::execute(
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
        "VALUES ('alpha',1,1,NOW(6))", affected);
    require(duplicate.code == ErrorCode::ConstraintViolation, "duplicate not classified");
    dbmw::core::ConnectionPool::Stats pool;
    require(dbmw::DBMW::poolStats(pool) && pool.borrowRequests > 0, "pool metrics empty");
    require(!dbmw::DBMW::slowSqlStats().empty(), "slow SQL metrics empty");
}

// ----------------------------------------------------------------------------
// v0.5.x 集成覆盖：实体映射（v0.5.0）+ 脚本执行/存储过程/call/多结果集/异步 util（v0.5.1）
// 这些功能此前只在单元测试里用合成 ResultSet 验证过，从未接真实驱动跑过。
// ----------------------------------------------------------------------------

void testEntityMapping(Fixture &f) {
    MapItem item;
    item.name = "map_item_1";
    item.qty = 11;
    item.unsignedValue = 42;
    auto ins = dbmw::insertAs<MapItem>(f.table, item);
    require(ins.status.ok(), "insertAs failed: " + ins.status.message);
    require(ins.affected == 1 && item.id > 0, "insertAs did not generate key");
    const std::int64_t id = item.id;

    auto got = dbmw::queryAs<MapItem>(
        "SELECT id,name,qty,unsigned_value FROM " + f.table + " WHERE id=?",
        Params{std::int64_t(id)});
    require(got.status.ok(), "queryAs failed: " + got.status.message);
    require(got.items.size() == 1, "queryAs wrong row count");
    if (!got.items.empty()) {
        require(got.items[0].name == "map_item_1", "mapped name mismatch");
        require(got.items[0].qty == 11, "mapped qty mismatch");
        require(got.items[0].unsignedValue == 42, "mapped unsigned_value mismatch");
    }

    item.qty = 99;
    auto upd = dbmw::updateAs<MapItem>(f.table, item);
    require(upd.status.ok() && upd.affected == 1, "updateAs failed");
    auto got2 = dbmw::queryAs<MapItem>(
        "SELECT id,name,qty,unsigned_value FROM " + f.table + " WHERE id=?",
        Params{std::int64_t(id)});
    require(got2.status.ok() && !got2.items.empty() && got2.items[0].qty == 99,
            "updateAs did not persist");

    MapItem b1; b1.name = "map_batch_1"; b1.qty = 1; b1.unsignedValue = 1;
    MapItem b2; b2.name = "map_batch_2"; b2.qty = 2; b2.unsignedValue = 2;
    auto batch = dbmw::insertBatchAs<MapItem>(f.table, {b1, b2});
    require(batch.status.ok(), "insertBatchAs failed: " + batch.status.message);
    require(batch.batch.totalAffected() == 2, "insertBatchAs affected mismatch");

    std::uint64_t mappedRows = 0;
    auto each = dbmw::queryEachAs<MapItem>(
        "SELECT id,name,qty,unsigned_value FROM " + f.table +
        " WHERE name LIKE 'map_%' ORDER BY id",
        Params{}, [&](MapItem &&) { return true; }, mappedRows);
    require(each.ok() && mappedRows >= 3, "queryEachAs mapped count mismatch");
}

void testScriptExecution(Fixture &f) {
    const std::string script =
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
        "VALUES ('script_a',1,1,NOW(6));\n"
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
        "VALUES ('script_b',2,2,NOW(6));";
    std::size_t executed = 0;
    auto st = dbmw::common::util::runScriptText(script, {}, &executed);
    require(st.ok(), "runScriptText failed: " + st.message);
    require(executed == 2, "runScriptText executed count mismatch");

    // 复合块里 BEGIN..END 内的 ';' 不得被拆分：整段作为单条语句下发到真实库。
    const std::string proc =
        "CREATE PROCEDURE dbmw_it_script_proc() BEGIN SELECT 1; SELECT 2; END";
    auto procSt = dbmw::common::util::createRoutine(proc);
    require(procSt.ok(), "createRoutine(procedure) failed: " + procSt.message);
    std::int64_t dropped = 0;
    requireOk(dbmw::DBMW::execute("DROP PROCEDURE IF EXISTS dbmw_it_script_proc", dropped),
              "drop script procedure");
}

void testRoutinesAndCall(Fixture &f) {
    const std::string addFn =
        "CREATE FUNCTION dbmw_it_add(a INT, b INT) RETURNS INT DETERMINISTIC RETURN a + b";
    require(dbmw::common::util::createRoutine(addFn).ok(), "createRoutine(function) failed");

    // MySQL 函数必须用 SELECT 路径调用（dbmw 对 MySQL RoutineRef 统一生成 CALL，
    // 而 CALL 仅适用于存储过程），故标量函数走 callQuery。
    ResultSet rs;
    auto st = dbmw::common::util::callQuery(
        "SELECT dbmw_it_add(?,?)",
        Params{std::int64_t(3), std::int64_t(4)}, rs, dbmw::common::util::CallOptions{});
    require(st.ok(), "callQuery(function) failed: " + st.message);
    require(rs.rowCount() == 1, "function result set missing");
    if (rs.rowCount() == 1)
        require(asInt(rs.rows().front().data().begin()->second) == 7, "function return mismatch");

    // INOUT 存储过程：需同连接，走 core::Session 重载（preSql SET + CALL + fetchSql）。
    const std::string swapProc =
        "CREATE PROCEDURE dbmw_it_swap(INOUT a INT, INOUT b INT) "
        "BEGIN SET a = a + b; SET b = a - b; SET a = a - b; END";
    require(dbmw::common::util::createRoutine(swapProc).ok(),
            "createRoutine(procedure INOUT) failed");
    RoutineRef swapRef{"dbmw_it_swap", RoutineKind::Procedure};
    CallParams io{CallParam(ParamDirection::InOut, std::int64_t(5)),
                  CallParam(ParamDirection::InOut, std::int64_t(9))};
    CallResult ioRes;
    auto ioSt = dbmw::DBMW::withSession([&](dbmw::core::Session &s) {
        return dbmw::common::util::call(s, swapRef, io, ioRes,
            dbmw::common::util::CallOptions{});
    });
    require(ioSt.ok(), "call(procedure INOUT) failed: " + ioSt.message);
    require(ioRes.outParams.size() == 2, "INOUT out params count mismatch");
    if (ioRes.outParams.size() == 2) {
        require(asInt(ioRes.outParams[0]) == 9, "INOUT a after swap mismatch");
        require(asInt(ioRes.outParams[1]) == 5, "INOUT b after swap mismatch");
    }

    // 多结果集：存储过程内多条 SELECT，驱动 queryAll 逐集 drain。
    const std::string multiProc =
        "CREATE PROCEDURE dbmw_it_multi() BEGIN SELECT 1 AS n; SELECT 2 AS n; END";
    require(dbmw::common::util::createRoutine(multiProc).ok(),
            "createRoutine(procedure multi) failed");
    RoutineRef multiRef{"dbmw_it_multi", RoutineKind::Procedure};
    CallResult multiRes;
    auto mSt = dbmw::common::util::call(multiRef, CallParams{}, multiRes,
        dbmw::common::util::CallOptions{});
    require(mSt.ok(), "call(procedure multi-resultset) failed: " + mSt.message);
    require(multiRes.sets.size() == 2,
            "multi-result set count mismatch: got " + std::to_string(multiRes.sets.size()));

    std::int64_t d = 0;
    requireOk(dbmw::DBMW::execute("DROP FUNCTION IF EXISTS dbmw_it_add", d), "drop function");
    requireOk(dbmw::DBMW::execute("DROP PROCEDURE IF EXISTS dbmw_it_swap", d), "drop proc swap");
    requireOk(dbmw::DBMW::execute("DROP PROCEDURE IF EXISTS dbmw_it_multi", d), "drop proc multi");
}

void testAsyncUtil(Fixture &f) {
    const std::string fn =
        "CREATE FUNCTION dbmw_it_aadd(a INT, b INT) RETURNS INT DETERMINISTIC RETURN a + b";
    auto fr = dbmw::async::util::createRoutine(fn).get();
    require(fr.status.ok(), "async createRoutine failed: " + fr.status.message);

    const std::string script =
        "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
        "VALUES ('async_script',1,1,NOW(6));";
    auto exec = dbmw::async::util::runScriptText(script).get();
    require(exec.status.ok(), "async runScriptText failed: " + exec.status.message);

    auto mr = dbmw::async::util::callAll(
        "SELECT dbmw_it_aadd(?,?)",
        dbmw::common::Params{std::int64_t(6), std::int64_t(7)}, {}).get();
    require(mr.status.ok(), "async callAll failed: " + mr.status.message);
    require(!mr.sets.empty() && !mr.sets.front().rows().empty(), "async callAll set missing");
    if (!mr.sets.empty() && !mr.sets.front().rows().empty()) {
        const auto &val = mr.sets.front().rows().front().data().begin()->second;
        require(std::get<std::int64_t>(val) == 13, "async function return mismatch");
    }

    std::int64_t d = 0;
    requireOk(dbmw::DBMW::execute("DROP FUNCTION IF EXISTS dbmw_it_aadd", d), "drop async function");
}

int main() {
    Fixture fixture;
    try {
        fixture.start();
        testTypesAndKeys(fixture);
        testTransactionsBatchCursorAndAsync(fixture);
        testEntityMapping(fixture);
        testScriptExecution(fixture);
        testRoutinesAndCall(fixture);
        testAsyncUtil(fixture);
        std::cout << "MySQL integration test passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "MySQL integration test failed after " << checks
                  << " checks: " << error.what() << '\n';
        return 1;
    }
}
