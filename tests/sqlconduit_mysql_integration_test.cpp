#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/util.h"

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

static sqlconduit::Client g_client;

struct MapItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    std::uint64_t unsignedValue = 0;
};

namespace sqlconduit::mapping {
    template<>
    struct RowMapper<MapItem> {
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
    using sqlconduit::common::ErrorCode;
    using sqlconduit::common::Params;
    using sqlconduit::common::ResultSet;
    using sqlconduit::common::Status;
    using sqlconduit::common::Value;
    using sqlconduit::common::util::CallParam;
    using sqlconduit::common::util::CallParams;
    using sqlconduit::common::util::CallResult;
    using sqlconduit::common::util::ParamDirection;
    using sqlconduit::common::util::RoutineKind;
    using sqlconduit::common::util::RoutineRef;

    int checks = 0;

    void require(bool condition, const std::string &message) {
        ++checks;
        if (!condition) throw std::runtime_error(message);
    }

    void requireOk(const Status &status, const std::string &where) {
        require(status.ok(), where + " failed: [" + sqlconduit::common::errorCodeToString(status.code)
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

    const std::string &asString(const Value &v) {
        if (const auto *s = std::get_if<std::string>(&v)) return *s;
        throw std::runtime_error("expected string result value");
    }

    struct Fixture {
        std::string table;
        std::string configPath;
        bool initialized = false;

        Fixture() {
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            table = "sqlconduit_it_" + std::to_string(static_cast<unsigned long long>(stamp));
            configPath = "/tmp/" + table + ".json";
        }

        ~Fixture() {
            if (initialized) {
                std::int64_t affected = 0;
                (void) g_client.execute("DROP TABLE IF EXISTS " + table, affected);
                g_client.shutdown(std::chrono::milliseconds(3000));
            }
            std::remove(configPath.c_str());
        }

        void start() {
            const auto host = env("SQLCONDUIT_TEST_MYSQL_HOST", "127.0.0.1");
            const auto port = env("SQLCONDUIT_TEST_MYSQL_PORT", "3306");
            const auto user = env("SQLCONDUIT_TEST_MYSQL_USER", "root");
            const auto database = env("SQLCONDUIT_TEST_MYSQL_DATABASE", "sqlconduit_test");
            require(!env("SQLCONDUIT_TEST_MYSQL_PASSWORD").empty(),
                    "SQLCONDUIT_TEST_MYSQL_PASSWORD must be set");
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
                    << "\",\"password_env\":\"SQLCONDUIT_TEST_MYSQL_PASSWORD\","
                    "\"database\":\"" << quoteJson(database)
                    << "\",\"connection_timeout_ms\":5000}]}";
            file.close();
            requireOk(g_client.init(configPath), "init");
            initialized = true;
            std::int64_t affected = 0;
            requireOk(g_client.execute(
                          "CREATE TABLE " + table + " (id BIGINT AUTO_INCREMENT PRIMARY KEY, "
                          "name VARCHAR(100) NOT NULL UNIQUE, qty BIGINT NOT NULL, unsigned_value BIGINT "
                          "UNSIGNED NOT NULL, amount DECIMAL(30,9), due_date DATE, local_time TIME(6), "
                          "metadata JSON, payload BLOB, created_at DATETIME(6)) ENGINE=InnoDB", affected),
                      "create table");
        }
    };

    void testTypesAndKeys(Fixture &f) {
        auto ds = g_client.dataSource();
        require(ds != nullptr, "default datasource missing");
        sqlconduit::common::GeneratedKeys keys;
        std::int64_t affected = 0;
        const sqlconduit::common::Blob blob{0, 1, 127, 128, 255};
        requireOk(ds->execute(
                      "INSERT INTO " + f.table + " (name,qty,unsigned_value,amount,due_date,local_time,"
                      "metadata,payload,created_at) VALUES (?,?,?,?,?,?,?,?,?)",
                      Params{
                          std::string("alpha"), std::int64_t(7),
                          std::uint64_t{18446744073709551615ULL},
                          sqlconduit::common::Decimal{"123456789012345678901.123456789"},
                          sqlconduit::common::Date{"2026-09-06"}, sqlconduit::common::Time{"11:50:00.123456"},
                          sqlconduit::common::Json{"{\"ok\":true}"}, blob,
                          sqlconduit::common::Timestamp{std::chrono::system_clock::now()}
                      },
                      affected, keys), "insert with generated key");
        require(affected == 1 && keys.lastInsertId() > 0, "generated key missing");

        ResultSet rows;
        requireOk(g_client.query(
                      "SELECT qty,unsigned_value,amount,due_date,local_time,metadata,payload,created_at "
                      "FROM " + f.table + " WHERE id=?", Params{keys.lastInsertId()}, rows), "type query");
        require(rows.rowCount() == 1, "type query row count");
        const auto &row = rows.rows().front();
        require(asInt(row.at("qty")) == 7, "signed bigint mismatch");
        require(std::get<std::uint64_t>(row.at("unsigned_value")) ==
                18446744073709551615ULL, "unsigned bigint mismatch");
        require(std::get<sqlconduit::common::Decimal>(row.at("amount")).value ==
                "123456789012345678901.123456789", "decimal precision lost");
        require(std::get<sqlconduit::common::Date>(row.at("due_date")).value == "2026-09-06",
                "date mismatch");
        require(std::get<sqlconduit::common::Time>(row.at("local_time")).value == "11:50:00.123456",
                "time mismatch");
        require(std::holds_alternative<sqlconduit::common::Json>(row.at("metadata")), "json type lost");
        require(std::get<sqlconduit::common::Blob>(row.at("payload")) == blob, "blob mismatch");
        require(std::holds_alternative<sqlconduit::common::Timestamp>(row.at("created_at")),
                "datetime type lost");
    }

    void testTransactionsBatchCursorAndAsync(Fixture &f) {
        sqlconduit::common::ParamBatch batch{
            {std::string("b1"), std::int64_t(1)},
            {std::string("b2"), std::int64_t(2)}
        };
        sqlconduit::common::BatchResult batchResult;
        requireOk(g_client.executeBatch(
                      "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
                      "VALUES (?,?,1,NOW(6))", batch, batchResult), "batch");
        require(batchResult.totalAffected() == 2, "batch affected mismatch");

        const auto rollback = g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            auto st = session.execute("INSERT INTO " + f.table
                                      + " (name,qty,unsigned_value,created_at) VALUES ('rollback',1,1,NOW(6))",
                                      affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::TxError, "intentional rollback");
        });
        require(rollback.code == ErrorCode::TxError, "rollback status mismatch");
        ResultSet rolled;
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table
                                 + " WHERE name='rollback'", rolled), "verify rollback");
        require(asInt(rolled.rows()[0].at("n")) == 0, "transaction was committed");

        requireOk(g_client.withSession([&](sqlconduit::core::Session &session) {
            sqlconduit::core::PreparedStatementHandle first, second;
            auto st = session.prepare("SELECT id FROM " + f.table + " WHERE name=?",
                                      {std::string()}, first);
            if (!st.ok()) return st;
            st = session.prepare("SELECT qty FROM " + f.table + " WHERE name=?",
                                 {std::string()}, second);
            if (!st.ok()) return st;
            ResultSet out;
            const auto evicted = session.executePrepared(first, {std::string("alpha")}, out);
            return evicted.code == ErrorCode::QueryError
                       ? Status::OK()
                       : Status::error(ErrorCode::QueryError, "evicted handle unexpectedly executed");
        }), "prepared LRU safety");

        std::uint64_t delivered = 0;
        int callbacks = 0;
        requireOk(g_client.queryEach("SELECT id FROM " + f.table + " ORDER BY id", {},
                                     [&](const sqlconduit::common::Row &) { return ++callbacks < 2; },
                                     delivered),
                  "queryEach");
        require(delivered == 2, "queryEach early stop mismatch");

        sqlconduit::core::CursorOptions options;
        options.batch_size = 2;
        std::unique_ptr<sqlconduit::core::Cursor> cursor;
        requireOk(g_client.openCursor("SELECT id FROM " + f.table + " ORDER BY id", {},
                                      options, cursor), "open cursor");
        ResultSet streamed;
        while (cursor->hasNext()) requireOk(cursor->fetch(2, streamed), "cursor fetch");
        require(streamed.rowCount() >= 3, "cursor missed rows");
        requireOk(cursor->close(), "cursor close");

        auto future = g_client.queryAsync("SELECT COUNT(*) n FROM " + f.table);
        const auto asyncRows = future.get();
        requireOk(asyncRows.status, "async query");
        require(asInt(asyncRows.rows.rows()[0].at("n")) >= 3, "async count mismatch");

        std::int64_t affected = 0;
        const auto duplicate = g_client.execute(
            "INSERT INTO " + f.table + " (name,qty,unsigned_value,created_at) "
            "VALUES ('alpha',1,1,NOW(6))", affected);
        require(duplicate.code == ErrorCode::ConstraintViolation, "duplicate not classified");
        sqlconduit::core::ConnectionPool::Stats pool;
        require(g_client.poolStats(pool) && pool.borrowRequests > 0, "pool metrics empty");
        require(!g_client.slowSqlStats().empty(), "slow SQL metrics empty");
    }


    void testEntityMapping(Fixture &f) {
        MapItem item;
        item.name = "map_item_1";
        item.qty = 11;
        item.unsignedValue = 42;
        auto ins = sqlconduit::insertAs<MapItem>(g_client, f.table, item);
        require(ins.status.ok(), "insertAs failed: " + ins.status.message);
        require(ins.affected == 1 && item.id > 0, "insertAs did not generate key");
        const std::int64_t id = item.id;

        auto got = sqlconduit::queryAs<MapItem>(g_client,
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
        auto upd = sqlconduit::updateAs<MapItem>(g_client, f.table, item);
        require(upd.status.ok() && upd.affected == 1, "updateAs failed");
        auto got2 = sqlconduit::queryAs<MapItem>(g_client,
                                                 "SELECT id,name,qty,unsigned_value FROM " + f.table + " WHERE id=?",
                                                 Params{std::int64_t(id)});
        require(got2.status.ok() && !got2.items.empty() && got2.items[0].qty == 99,
                "updateAs did not persist");

        MapItem b1;
        b1.name = "map_batch_1";
        b1.qty = 1;
        b1.unsignedValue = 1;
        MapItem b2;
        b2.name = "map_batch_2";
        b2.qty = 2;
        b2.unsignedValue = 2;
        std::vector<MapItem> bv{b1, b2};
        auto batch = sqlconduit::insertBatchAs<MapItem>(g_client, f.table, bv);
        require(batch.status.ok(), "insertBatchAs failed: " + batch.status.message);
        require(batch.batch.totalAffected() == 2, "insertBatchAs affected mismatch");
        require(bv[0].id > 0 && bv[1].id > 0, "insertBatchAs did not backfill generated ids");
        require(bv[0].id != bv[1].id, "insertBatchAs backfilled the same id twice");

        std::uint64_t mappedRows = 0;
        auto each = sqlconduit::queryEachAs<MapItem>(g_client,
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
        auto st = sqlconduit::common::util::runScriptText(g_client, script, {}, &executed);
        require(st.ok(), "runScriptText failed: " + st.message);
        require(executed == 2, "runScriptText executed count mismatch");

        const std::string proc =
                "CREATE PROCEDURE sqlconduit_it_script_proc() BEGIN SELECT 1; SELECT 2; END";
        auto procSt = sqlconduit::common::util::createRoutine(g_client, proc);
        require(procSt.ok(), "createRoutine(procedure) failed: " + procSt.message);
        std::int64_t dropped = 0;
        requireOk(g_client.execute("DROP PROCEDURE IF EXISTS sqlconduit_it_script_proc", dropped),
                  "drop script procedure");
    }

    void testRoutinesAndCall(Fixture &f) {
        const std::string addFn =
                "CREATE FUNCTION sqlconduit_it_add(a INT, b INT) RETURNS INT DETERMINISTIC RETURN a + b";
        require(sqlconduit::common::util::createRoutine(g_client, addFn).ok(), "createRoutine(function) failed");

        ResultSet rs;
        auto st = sqlconduit::common::util::callQuery(g_client,
                                                      "SELECT sqlconduit_it_add(?,?)",
                                                      Params{std::int64_t(3), std::int64_t(4)}, rs,
                                                      sqlconduit::common::util::CallOptions{});
        require(st.ok(), "callQuery(function) failed: " + st.message);
        require(rs.rowCount() == 1, "function result set missing");
        if (rs.rowCount() == 1)
            require(asInt(rs.rows().front().data().begin()->second) == 7, "function return mismatch");

        const std::string swapProc =
                "CREATE PROCEDURE sqlconduit_it_swap(INOUT a INT, INOUT b INT) "
                "BEGIN SET a = a + b; SET b = a - b; SET a = a - b; END";
        require(sqlconduit::common::util::createRoutine(g_client, swapProc).ok(),
                "createRoutine(procedure INOUT) failed");
        RoutineRef swapRef{"sqlconduit_it_swap", RoutineKind::Procedure};
        CallParams io{
            CallParam(ParamDirection::InOut, std::int64_t(5)),
            CallParam(ParamDirection::InOut, std::int64_t(9))
        };
        CallResult ioRes;
        auto ioSt = g_client.withSession([&](sqlconduit::core::Session &s) {
            return sqlconduit::common::util::call(g_client, s, swapRef, io, ioRes,
                                                  sqlconduit::common::util::CallOptions{});
        });
        require(ioSt.ok(), "call(procedure INOUT) failed: " + ioSt.message);
        require(ioRes.outParams.size() == 2, "INOUT out params count mismatch");
        if (ioRes.outParams.size() == 2) {
            require(asInt(ioRes.outParams[0]) == 9, "INOUT a after swap mismatch");
            require(asInt(ioRes.outParams[1]) == 5, "INOUT b after swap mismatch");
        }

        const std::string multiProc =
                "CREATE PROCEDURE sqlconduit_it_multi() BEGIN SELECT 1 AS n; SELECT 2 AS n; END";
        require(sqlconduit::common::util::createRoutine(g_client, multiProc).ok(),
                "createRoutine(procedure multi) failed");
        RoutineRef multiRef{"sqlconduit_it_multi", RoutineKind::Procedure};
        CallResult multiRes;
        auto mSt = sqlconduit::common::util::call(g_client, multiRef, CallParams{}, multiRes,
                                                  sqlconduit::common::util::CallOptions{});
        require(mSt.ok(), "call(procedure multi-resultset) failed: " + mSt.message);
        require(multiRes.sets.size() == 2,
                "multi-result set count mismatch: got " + std::to_string(multiRes.sets.size()));

        std::int64_t d = 0;
        requireOk(g_client.execute("DROP FUNCTION IF EXISTS sqlconduit_it_add", d), "drop function");
        requireOk(g_client.execute("DROP PROCEDURE IF EXISTS sqlconduit_it_swap", d), "drop proc swap");
        requireOk(g_client.execute("DROP PROCEDURE IF EXISTS sqlconduit_it_multi", d),
                  "drop proc multi");
    }

    void testMissingCoverage(Fixture &f) {
        // Non-ASCII UTF-8 round trip (MySQL default charset is utf8mb4).
        const std::string cjk = "中文往返测试";
        std::int64_t affected = 0;
        requireOk(g_client.execute(
                          "INSERT INTO " + f.table
                          + " (name,qty,unsigned_value,created_at) VALUES (?,1,1,NOW(6))",
                          Params{std::string(cjk)}, affected), "insert non-ascii name");
        ResultSet cjkRow;
        requireOk(g_client.query("SELECT name FROM " + f.table + " WHERE name=?",
                                 Params{std::string(cjk)}, cjkRow), "select non-ascii name");
        require(cjkRow.rowCount() == 1 && asString(cjkRow.rows()[0].at("name")) == cjk,
                "UTF-8 round trip failed");

        // Bad SQL must be classified as QueryError (not silently swallowed).
        ResultSet ignored;
        const Status bad = g_client.query("SELECT * FROM sqlconduit_it_no_such_table_xyz", ignored);
        require(!bad.ok() && bad.code == ErrorCode::QueryError,
                "bad SQL should be classified as QueryError, got " +
                std::string(sqlconduit::common::errorCodeToString(bad.code)));

        requireOk(g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t changed = 0;
            auto st = session.execute(
                "INSERT INTO " + f.table
                + " (name,qty,unsigned_value,created_at) VALUES ('sp-keep',1,1,NOW(6))", changed);
            if (!st.ok()) return st;
            st = session.savepoint("sp1");
            if (!st.ok()) return st;
            st = session.execute(
                "INSERT INTO " + f.table
                + " (name,qty,unsigned_value,created_at) VALUES ('sp-temp',1,1,NOW(6))", changed);
            if (!st.ok()) return st;
            st = session.rollbackToSavepoint("sp1");
            if (!st.ok()) return st;
            return session.releaseSavepoint("sp1");
        }), "savepoint commit");
        ResultSet savepointRows;
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table
                                 + " WHERE name='sp-keep'", savepointRows), "sp keep count");
        require(asInt(savepointRows.rows()[0].at("n")) == 1, "savepoint kept row missing");
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table
                                 + " WHERE name='sp-temp'", savepointRows), "sp temp count");
        require(asInt(savepointRows.rows()[0].at("n")) == 0,
                "savepoint rollback did not undo the row");
    }
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
        testMissingCoverage(fixture);
        std::cout << "MySQL integration test passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "MySQL integration test failed after " << checks
                << " checks: " << error.what() << '\n';
        return 1;
    }
}
