#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/util.h"
#include "sqlconduit/drivers/odbc.h"
#include "sql_builder_integration.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

static sqlconduit::Client g_client;

struct OdbcItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    double amount = 0;
    sqlconduit::common::Timestamp createdAt;
};

namespace sqlconduit::mapping {
    template<>
    struct RowMapper<OdbcItem> {
        static Mapping<OdbcItem> describe() {
            return Mapping<OdbcItem>()
                    .field(&OdbcItem::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                    .field(&OdbcItem::name, "name")
                    .field(&OdbcItem::qty, "qty")
                    .field(&OdbcItem::amount, "amount", FieldFlags::Lossy)
                    .field(&OdbcItem::createdAt, "created_at");
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
    using sqlconduit::common::util::Dialect;
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

    std::string jsonEscape(const std::string &value) {
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
            requireOk(g_client.addDriver(sqlconduit::drivers::odbc()), "register ODBC driver");
            const auto host = env("SQLCONDUIT_TEST_ODBC_HOST", "127.0.0.1");
            const auto port = env("SQLCONDUIT_TEST_ODBC_PORT", "1433");
            const auto user = env("SQLCONDUIT_TEST_ODBC_USER", "sa");
            const auto password = env("SQLCONDUIT_TEST_ODBC_PASSWORD");
            const auto database = env("SQLCONDUIT_TEST_ODBC_DATABASE", "master");
            const auto driver = env("SQLCONDUIT_TEST_ODBC_DRIVER", "FreeTDS");
            require(!password.empty(), "SQLCONDUIT_TEST_ODBC_PASSWORD must be set");
            const std::string connection = "DRIVER={" + driver + "};SERVER=" + host
                                           + ";PORT=" + port + ";DATABASE=" + database + ";UID=" + user + ";PWD="
                                           + password + ";TDS_Version=7.4;";
            std::ofstream file(configPath);
            file << "{\"default_datasource\":\"odbc\","
                    "\"pool\":{\"min\":0,\"max\":4,\"borrow_timeout_ms\":3000},"
                    "\"prepared_cache\":{\"enabled\":true,\"max_per_connection\":1},"
                    "\"cursor\":{\"enabled\":true,\"default_batch_size\":2,"
                    "\"max_open_cursors\":1,\"allow_scrollable\":false},"
                    "\"observability\":{\"slow_sql\":{\"enabled\":true,"
                    "\"threshold_ms\":0,\"aggregate_capacity\":100,"
                    "\"recent_capacity\":100},\"pool_metrics\":{\"enabled\":true}},"
                    "\"async\":{\"enabled\":true,\"threads\":2,\"queue_size\":64},"
                    "\"datasources\":[{\"name\":\"odbc\",\"type\":\"odbc\","
                    "\"database\":\"" << jsonEscape(database) << "\","
                    "\"connection_timeout_ms\":5000,\"extra\":{"
                    "\"connection_string\":\"" << jsonEscape(connection) << "\","
                    "\"savepoint_style\":\"sqlserver\"}}]}";
            file.close();
            requireOk(g_client.init(configPath), "init");
            initialized = true;
            std::int64_t affected = 0;
            requireOk(g_client.execute(
                          "CREATE TABLE " + table + " (id BIGINT IDENTITY(1,1) PRIMARY KEY,"
                          "name VARCHAR(100) NOT NULL UNIQUE,qty BIGINT NOT NULL,amount DECIMAL(30,9),"
                          "due_date DATE,local_time TIME(6),external_id UNIQUEIDENTIFIER,"
                          "payload VARBINARY(MAX),created_at DATETIME2(6))", affected), "create table");
        }
    };

    void testTypesKeysAndErrors(Fixture &f) {
        auto ds = g_client.dataSource();
        require(ds != nullptr, "default datasource missing");
        sqlconduit::common::GeneratedKeys keys;
        std::int64_t affected = 0;
        const sqlconduit::common::Blob blob{0, 1, 127, 128, 255};
        requireOk(ds->execute(
                      "INSERT INTO " + f.table + " (name,qty,amount,due_date,local_time,external_id,"
                      "payload,created_at) OUTPUT INSERTED.id VALUES (?,?,?,?,?,?,?,?)",
                      Params{
                          std::string("alpha"), std::int64_t(7),
                          sqlconduit::common::Decimal{"123456789012345678901.123456789"},
                          sqlconduit::common::Date{"2026-09-06"}, sqlconduit::common::Time{"11:50:00.123456"},
                          sqlconduit::common::Uuid{"550e8400-e29b-41d4-a716-446655440000"}, blob,
                          sqlconduit::common::Timestamp{std::chrono::system_clock::now()}
                      },
                      affected, keys), "insert output key");
        require(!keys.empty() && keys.lastInsertId() > 0, "OUTPUT generated key missing");

        ResultSet rows;
        requireOk(g_client.query(
                      "SELECT qty,amount,due_date,local_time,external_id,payload,created_at FROM "
                      + f.table + " WHERE id=?", Params{keys.lastInsertId()}, rows), "type query");
        require(rows.rowCount() == 1 && asInt(rows.rows()[0].at("qty")) == 7,
                "bigint round trip failed");
        const auto &row = rows.rows()[0];
        require(std::get<sqlconduit::common::Decimal>(row.at("amount")).value ==
                "123456789012345678901.123456789", "decimal precision lost");
        require(std::get<sqlconduit::common::Date>(row.at("due_date")).value == "2026-09-06",
                "date round trip failed");
        require(std::get<sqlconduit::common::Time>(row.at("local_time")).value.find("11:50:00.123456") == 0,
                "time round trip failed");
        require(std::holds_alternative<sqlconduit::common::Uuid>(row.at("external_id")),
                "GUID type lost");
        require(std::get<sqlconduit::common::Blob>(row.at("payload")) == blob, "binary mismatch");
        require(std::holds_alternative<sqlconduit::common::Timestamp>(row.at("created_at")),
                "datetime2 type lost");

        const auto duplicate = g_client.execute(
            "INSERT INTO " + f.table + " (name,qty) VALUES ('alpha',1)", affected);
        require(duplicate.code == ErrorCode::ConstraintViolation,
                "unique violation not classified: " + duplicate.sqlState);
    }

    void testTransactionsPreparedBatchCursorAsync(Fixture &f) {
        sqlconduit::common::ParamBatch batch{
            {std::string("b1"), std::int64_t(1)},
            {std::string("b2"), std::int64_t(2)}
        };
        sqlconduit::common::BatchResult result;
        requireOk(g_client.executeBatch(
                      "INSERT INTO " + f.table + " (name,qty) VALUES (?,?)", batch, result), "batch");
        require(result.totalAffected() == 2, "batch affected mismatch");

        const auto rolled = g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            auto st = session.execute("INSERT INTO " + f.table
                                      + " (name,qty) VALUES ('rollback',1)", affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::TxError, "intentional rollback");
        });
        require(rolled.code == ErrorCode::TxError, "transaction status mismatch");
        ResultSet count;
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table
                                 + " WHERE name='rollback'", count), "verify rollback");
        require(asInt(count.rows()[0].at("n")) == 0, "rollback row persisted");

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

        const auto asyncRows = g_client.queryAsync("SELECT COUNT(*) n FROM " + f.table).get();
        requireOk(asyncRows.status, "async query");
        require(asInt(asyncRows.rows.rows()[0].at("n")) >= 3, "async count mismatch");
        sqlconduit::core::ConnectionPool::Stats pool;
        require(g_client.poolStats(pool) && pool.borrowRequests > 0, "pool metrics empty");
        require(!g_client.slowSqlStats().empty(), "slow SQL metrics empty");
    }

    void testEntityMapping(Fixture &f) {
        OdbcItem item;
        item.name = "map_item_1";
        item.qty = 11;
        item.amount = 1.0;
        item.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        auto ins = sqlconduit::insertAs<OdbcItem>(g_client, f.table, item);
        require(ins.status.ok(), "insertAs failed: " + ins.status.message);
        require(ins.affected == 1, "insertAs affected mismatch");
        require(item.id > 0, "insertAs did not backfill generated id (SQL Server OUTPUT INSERTED gap?)");
        const std::int64_t id = item.id;

        auto got = sqlconduit::queryAs<OdbcItem>(g_client,
                                                 "SELECT id,name,qty,amount,created_at FROM " + f.table + " WHERE id=?",
                                                 Params{std::int64_t(id)});
        require(got.status.ok(), "queryAs failed: " + got.status.message);
        require(got.items.size() == 1, "queryAs wrong row count");
        if (!got.items.empty()) {
            require(got.items[0].name == "map_item_1", "mapped name mismatch");
            require(got.items[0].qty == 11, "mapped qty mismatch");
            require(got.items[0].amount == 1.0, "mapped amount mismatch");
        }

        item.qty = 99;
        auto upd = sqlconduit::updateAs<OdbcItem>(g_client, f.table, item);
        require(upd.status.ok() && upd.affected == 1, "updateAs failed");
        auto got2 = sqlconduit::queryAs<OdbcItem>(g_client,
                                                  "SELECT id,name,qty,amount,created_at FROM " + f.table +
                                                  " WHERE id=?",
                                                  Params{std::int64_t(id)});
        require(got2.status.ok() && !got2.items.empty() && got2.items[0].qty == 99,
                "updateAs did not persist");

        OdbcItem b1;
        b1.name = "map_batch_1";
        b1.qty = 1;
        b1.amount = 1.0;
        b1.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        OdbcItem b2;
        b2.name = "map_batch_2";
        b2.qty = 2;
        b2.amount = 2.0;
        b2.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        auto batch = sqlconduit::insertBatchAs<OdbcItem>(g_client, f.table, {b1, b2});
        require(batch.status.ok(), "insertBatchAs failed: " + batch.status.message);
        require(batch.batch.totalAffected() == 2, "insertBatchAs affected mismatch");

        std::uint64_t mappedRows = 0;
        auto each = sqlconduit::queryEachAs<OdbcItem>(g_client,
                                                      "SELECT id,name,qty,amount,created_at FROM " + f.table +
                                                      " WHERE name LIKE 'map_%' ORDER BY id",
                                                      Params{}, [&](OdbcItem &&) { return true; }, mappedRows);
        require(each.ok() && mappedRows >= 3, "queryEachAs mapped count mismatch");
    }

    void testScriptExecution(Fixture &f) {
        const std::string script =
                "INSERT INTO " + f.table + " (name, qty, amount, created_at) "
                "VALUES ('script_a',1,1.0,GETDATE());\n"
                "INSERT INTO " + f.table + " (name, qty, amount, created_at) "
                "VALUES ('script_b',2,2.0,GETDATE());";
        std::size_t executed = 0;
        auto st = sqlconduit::common::util::runScriptText(g_client, script, {}, &executed);
        require(st.ok(), "runScriptText failed: " + st.message);
        require(executed == 2, "runScriptText executed count mismatch");

        const std::string proc = "CREATE PROCEDURE sqlconduit_it_script_proc AS SELECT 1 AS n";
        auto procSt = sqlconduit::common::util::createRoutine(g_client, proc);
        require(procSt.ok(), "createRoutine(procedure) failed: " + procSt.message);
        std::int64_t dropped = 0;
        requireOk(g_client.execute("DROP PROCEDURE sqlconduit_it_script_proc", dropped),
                  "drop script procedure");
    }

    void testRoutinesAndCall(Fixture &f) {
        require(sqlconduit::common::util::createRoutine(g_client,
                                                        "CREATE FUNCTION sqlconduit_it_add(@a INT, @b INT) RETURNS INT AS BEGIN RETURN @a + @b END")
                .ok(),
                "createRoutine(function) failed");
        RoutineRef addRef{"sqlconduit_it_add", RoutineKind::Function};
        CallParams addParams{
            CallParam(ParamDirection::In, std::int64_t(3)),
            CallParam(ParamDirection::In, std::int64_t(4))
        };
        CallResult addRes;
        sqlconduit::common::util::CallOptions addOpts;
        addOpts.dialect = Dialect::SqlServer;
        addOpts.returnsRows = true;
        auto addSt = sqlconduit::common::util::call(g_client, addRef, addParams, addRes, addOpts);
        require(addSt.ok(), "call(function) failed: " + addSt.message);
        require(!addRes.sets.empty() && !addRes.sets.front().rows().empty(),
                "function result set missing");
        if (!addRes.sets.empty() && !addRes.sets.front().rows().empty()) {
            const auto &v = addRes.sets.front().rows().front().data().begin()->second;
            require(asInt(v) == 7, "function return mismatch");
        }

        std::vector<ResultSet> sets;
        auto mSt = g_client.queryAll("SELECT 1 AS n; SELECT 2 AS n", Params{}, sets);
        require(mSt.ok(), "queryAll failed on ODBC: " + mSt.message);
        require(!sets.empty(), "queryAll returned no result set on ODBC");

        require(sqlconduit::common::util::createRoutine(g_client,
                                                        "CREATE PROCEDURE sqlconduit_it_rows AS SELECT 1 AS n").ok(),
                "createRoutine(procedure) failed");
        RoutineRef rowsRef{"sqlconduit_it_rows", RoutineKind::Procedure};
        CallResult rowsRes;
        sqlconduit::common::util::CallOptions rowsOpts;
        rowsOpts.dialect = Dialect::SqlServer;
        rowsOpts.returnsRows = true;
        auto rowsSt = sqlconduit::common::util::call(g_client, rowsRef, CallParams{}, rowsRes, rowsOpts);
        require(rowsSt.ok(), "call(procedure) failed: " + rowsSt.message);
        require(!rowsRes.sets.empty() && !rowsRes.sets.front().rows().empty(),
                "procedure result set missing");

        std::int64_t d = 0;
        requireOk(g_client.execute("DROP FUNCTION sqlconduit_it_add", d), "drop fn add");
        requireOk(g_client.execute("DROP PROCEDURE sqlconduit_it_rows", d), "drop procedure rows");
    }

    void testSymmetryGaps(Fixture &f) {
        // Non-ASCII NVARCHAR round trip on a dedicated table (VARCHAR depends on collation).
        const std::string uni = f.table + "_uni";
        std::int64_t affected = 0;
        requireOk(g_client.execute(
                          "CREATE TABLE " + uni + " (id BIGINT IDENTITY(1,1) PRIMARY KEY, "
                          "name NVARCHAR(100) NOT NULL)", affected), "create unicode table");
        const std::string cjk = "中文往返测试";
        requireOk(g_client.execute(
                          "INSERT INTO " + uni + " (name) VALUES (?)", Params{cjk}, affected),
                  "insert unicode name");
        const Status rejected = g_client.execute(
            "INSERT INTO " + uni + " (name) VALUES (?)", Params{std::string(1000, 'x')}, affected);
        require(!rejected.ok() && rejected.code == ErrorCode::QueryError,
                "oversized unicode parameter should fail as QueryError");
        const std::string recovered = "prepared-恢复";
        requireOk(g_client.execute(
                          "INSERT INTO " + uni + " (name) VALUES (?)", Params{recovered}, affected),
                  "reuse prepared insert after failure");
        ResultSet cjkRow;
        requireOk(g_client.query("SELECT name FROM " + uni + " ORDER BY id", cjkRow),
                  "select unicode names");
        const std::string firstName = cjkRow.rowCount() > 0
                                          ? asString(cjkRow.rows()[0].at("name")) : "<missing>";
        const std::string secondName = cjkRow.rowCount() > 1
                                           ? asString(cjkRow.rows()[1].at("name")) : "<missing>";
        require(cjkRow.rowCount() == 2 && firstName == cjk && secondName == recovered,
                "NVARCHAR UTF-16 round trip failed: rows=" +
                std::to_string(cjkRow.rowCount()) + " first=[" + firstName +
                "] second=[" + secondName + "]");
        requireOk(g_client.execute("DROP TABLE IF EXISTS " + uni, affected), "drop unicode table");

        // Transaction savepoint (SQL Server savepoint style configured on the datasource).
        requireOk(g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t aff = 0;
            ResultSet sessionBefore;
            auto st = session.query("SELECT @@SPID spid, @@TRANCOUNT trancount", sessionBefore);
            if (!st.ok()) return st;
            const auto spid = asInt(sessionBefore.rows()[0].at("spid"));
            if (asInt(sessionBefore.rows()[0].at("trancount")) != 1)
                return Status::error(ErrorCode::TxError, "transaction was not active on session connection");
            st = session.execute("INSERT INTO " + f.table + " (name,qty) VALUES ('sp-keep',1)", aff);
            if (!st.ok()) return st;
            st = session.savepoint("sp1");
            if (!st.ok()) return st;
            st = session.execute("INSERT INTO " + f.table + " (name,qty) VALUES ('sp-temp',1)", aff);
            if (!st.ok()) return st;
            st = session.rollbackToSavepoint("sp1");
            if (!st.ok()) return st;
            ResultSet sessionAfter;
            st = session.query("SELECT @@SPID spid, @@TRANCOUNT trancount", sessionAfter);
            if (!st.ok()) return st;
            if (asInt(sessionAfter.rows()[0].at("spid")) != spid ||
                asInt(sessionAfter.rows()[0].at("trancount")) != 1)
                return Status::error(ErrorCode::TxError,
                                     "session query did not stay on the transaction connection");
            return session.releaseSavepoint("sp1");
        }), "savepoint commit");
        ResultSet sp;
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table + " WHERE name='sp-keep'", sp),
                  "sp keep count");
        require(asInt(sp.rows()[0].at("n")) == 1, "savepoint kept row missing");
        requireOk(g_client.query("SELECT COUNT(*) n FROM " + f.table + " WHERE name='sp-temp'", sp),
                  "sp temp count");
        require(asInt(sp.rows()[0].at("n")) == 0, "savepoint rollback did not undo the row");

        // Bad SQL must be classified as QueryError (not silently swallowed).
        ResultSet ignored;
        const Status bad = g_client.query("SELECT * FROM sqlconduit_it_no_such_table_xyz", ignored);
        require(!bad.ok() && bad.code == ErrorCode::QueryError,
                "bad SQL should be classified as QueryError, got " +
                std::string(sqlconduit::common::errorCodeToString(bad.code)));
    }
}

int main() {
    Fixture fixture;
    try {
        fixture.start();
        testTypesKeysAndErrors(fixture);
        requireOk(runSqlBuilderCrud(
                      g_client, fixture.table, sqlconduit::common::util::Dialect::SqlServer,
                      {
                          {"name", std::string("builder-row")},
                          {"qty", std::int64_t{5}}
                      }),
                  "SQL Builder CRUD");
        testTransactionsPreparedBatchCursorAsync(fixture);
        testEntityMapping(fixture);
        testScriptExecution(fixture);
        testRoutinesAndCall(fixture);
        testSymmetryGaps(fixture);
        std::cout << "ODBC SQL Server integration test passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ODBC SQL Server integration test failed after " << checks
                << " checks: " << error.what() << '\n';
        return 1;
    }
}
