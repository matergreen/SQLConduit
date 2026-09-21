#include "dbmw/async/dbmw_async.h"
#include "dbmw/dbmw.h"
#include "dbmw/mapping.h"
#include "dbmw/util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>

struct OdbcItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    double amount = 0;
    dbmw::common::Timestamp createdAt;
};

namespace dbmw::mapping {
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
    using dbmw::common::ErrorCode;
    using dbmw::common::Params;
    using dbmw::common::ResultSet;
    using dbmw::common::Status;
    using dbmw::common::Value;
    using dbmw::common::util::CallParam;
    using dbmw::common::util::CallParams;
    using dbmw::common::util::CallResult;
    using dbmw::common::util::Dialect;
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

    std::string jsonEscape(const std::string &value) {
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
                (void) dbmw::DBMW::execute("DROP TABLE IF EXISTS " + table, affected);
                dbmw::DBMW::shutdown(std::chrono::milliseconds(3000));
            }
            std::remove(configPath.c_str());
        }

        void start() {
            const auto host = env("DBMW_TEST_ODBC_HOST", "127.0.0.1");
            const auto port = env("DBMW_TEST_ODBC_PORT", "1433");
            const auto user = env("DBMW_TEST_ODBC_USER", "sa");
            const auto password = env("DBMW_TEST_ODBC_PASSWORD");
            const auto database = env("DBMW_TEST_ODBC_DATABASE", "master");
            const auto driver = env("DBMW_TEST_ODBC_DRIVER", "FreeTDS");
            require(!password.empty(), "DBMW_TEST_ODBC_PASSWORD must be set");
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
            requireOk(dbmw::DBMW::init(configPath), "init");
            initialized = true;
            std::int64_t affected = 0;
            requireOk(dbmw::DBMW::execute(
                          "CREATE TABLE " + table + " (id BIGINT IDENTITY(1,1) PRIMARY KEY,"
                          "name VARCHAR(100) NOT NULL UNIQUE,qty BIGINT NOT NULL,amount DECIMAL(30,9),"
                          "due_date DATE,local_time TIME(6),external_id UNIQUEIDENTIFIER,"
                          "payload VARBINARY(MAX),created_at DATETIME2(6))", affected), "create table");
        }
    };

    void testTypesKeysAndErrors(Fixture &f) {
        auto ds = dbmw::DBMW::dataSource();
        require(ds != nullptr, "default datasource missing");
        dbmw::common::GeneratedKeys keys;
        std::int64_t affected = 0;
        const dbmw::common::Blob blob{0, 1, 127, 128, 255};
        requireOk(ds->execute(
                      "INSERT INTO " + f.table + " (name,qty,amount,due_date,local_time,external_id,"
                      "payload,created_at) OUTPUT INSERTED.id VALUES (?,?,?,?,?,?,?,?)",
                      Params{
                          std::string("alpha"), std::int64_t(7),
                          dbmw::common::Decimal{"123456789012345678901.123456789"},
                          dbmw::common::Date{"2026-09-06"}, dbmw::common::Time{"11:50:00.123456"},
                          dbmw::common::Uuid{"550e8400-e29b-41d4-a716-446655440000"}, blob,
                          dbmw::common::Timestamp{std::chrono::system_clock::now()}
                      },
                      affected, keys), "insert output key");
        require(!keys.empty() && keys.lastInsertId() > 0, "OUTPUT generated key missing");

        ResultSet rows;
        requireOk(dbmw::DBMW::query(
                      "SELECT qty,amount,due_date,local_time,external_id,payload,created_at FROM "
                      + f.table + " WHERE id=?", Params{keys.lastInsertId()}, rows), "type query");
        require(rows.rowCount() == 1 && asInt(rows.rows()[0].at("qty")) == 7,
                "bigint round trip failed");
        const auto &row = rows.rows()[0];
        require(std::get<dbmw::common::Decimal>(row.at("amount")).value ==
                "123456789012345678901.123456789", "decimal precision lost");
        require(std::get<dbmw::common::Date>(row.at("due_date")).value == "2026-09-06",
                "date round trip failed");
        require(std::get<dbmw::common::Time>(row.at("local_time")).value.find("11:50:00.123456") == 0,
                "time round trip failed");
        require(std::holds_alternative<dbmw::common::Uuid>(row.at("external_id")),
                "GUID type lost");
        require(std::get<dbmw::common::Blob>(row.at("payload")) == blob, "binary mismatch");
        require(std::holds_alternative<dbmw::common::Timestamp>(row.at("created_at")),
                "datetime2 type lost");

        const auto duplicate = dbmw::DBMW::execute(
            "INSERT INTO " + f.table + " (name,qty) VALUES ('alpha',1)", affected);
        require(duplicate.code == ErrorCode::ConstraintViolation,
                "unique violation not classified: " + duplicate.sqlState);
    }

    void testTransactionsPreparedBatchCursorAsync(Fixture &f) {
        dbmw::common::ParamBatch batch{
            {std::string("b1"), std::int64_t(1)},
            {std::string("b2"), std::int64_t(2)}
        };
        dbmw::common::BatchResult result;
        requireOk(dbmw::DBMW::executeBatch(
                      "INSERT INTO " + f.table + " (name,qty) VALUES (?,?)", batch, result), "batch");
        require(result.totalAffected() == 2, "batch affected mismatch");

        const auto rolled = dbmw::DBMW::transaction([&](dbmw::core::Session &session) {
            std::int64_t affected = 0;
            auto st = session.execute("INSERT INTO " + f.table
                                      + " (name,qty) VALUES ('rollback',1)", affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::TxError, "intentional rollback");
        });
        require(rolled.code == ErrorCode::TxError, "transaction status mismatch");
        ResultSet count;
        requireOk(dbmw::DBMW::query("SELECT COUNT(*) n FROM " + f.table
                                    + " WHERE name='rollback'", count), "verify rollback");
        require(asInt(count.rows()[0].at("n")) == 0, "rollback row persisted");

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
            return evicted.code == ErrorCode::QueryError
                       ? Status::OK()
                       : Status::error(ErrorCode::QueryError, "evicted handle unexpectedly executed");
        }), "prepared LRU safety");

        std::uint64_t delivered = 0;
        int callbacks = 0;
        requireOk(dbmw::DBMW::queryEach("SELECT id FROM " + f.table + " ORDER BY id", {},
                                        [&](const dbmw::common::Row &) { return ++callbacks < 2; }, delivered),
                  "queryEach");
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

        const auto asyncRows = dbmw::async::query("SELECT COUNT(*) n FROM " + f.table).get();
        requireOk(asyncRows.status, "async query");
        require(asInt(asyncRows.rows.rows()[0].at("n")) >= 3, "async count mismatch");
        dbmw::core::ConnectionPool::Stats pool;
        require(dbmw::DBMW::poolStats(pool) && pool.borrowRequests > 0, "pool metrics empty");
        require(!dbmw::DBMW::slowSqlStats().empty(), "slow SQL metrics empty");
    }

    void testEntityMapping(Fixture &f) {
        OdbcItem item;
        item.name = "map_item_1";
        item.qty = 11;
        item.amount = 1.0;
        item.createdAt = dbmw::common::Timestamp{std::chrono::system_clock::now()};
        auto ins = dbmw::insertAs<OdbcItem>(f.table, item);
        require(ins.status.ok(), "insertAs failed: " + ins.status.message);
        require(ins.affected == 1, "insertAs affected mismatch");
        require(item.id > 0, "insertAs did not backfill generated id (SQL Server OUTPUT INSERTED gap?)");
        const std::int64_t id = item.id;

        auto got = dbmw::queryAs<OdbcItem>(
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
        auto upd = dbmw::updateAs<OdbcItem>(f.table, item);
        require(upd.status.ok() && upd.affected == 1, "updateAs failed");
        auto got2 = dbmw::queryAs<OdbcItem>(
            "SELECT id,name,qty,amount,created_at FROM " + f.table + " WHERE id=?",
            Params{std::int64_t(id)});
        require(got2.status.ok() && !got2.items.empty() && got2.items[0].qty == 99,
                "updateAs did not persist");

        OdbcItem b1;
        b1.name = "map_batch_1";
        b1.qty = 1;
        b1.amount = 1.0;
        b1.createdAt = dbmw::common::Timestamp{std::chrono::system_clock::now()};
        OdbcItem b2;
        b2.name = "map_batch_2";
        b2.qty = 2;
        b2.amount = 2.0;
        b2.createdAt = dbmw::common::Timestamp{std::chrono::system_clock::now()};
        auto batch = dbmw::insertBatchAs<OdbcItem>(f.table, {b1, b2});
        require(batch.status.ok(), "insertBatchAs failed: " + batch.status.message);
        require(batch.batch.totalAffected() == 2, "insertBatchAs affected mismatch");

        std::uint64_t mappedRows = 0;
        auto each = dbmw::queryEachAs<OdbcItem>(
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
        auto st = dbmw::common::util::runScriptText(script, {}, &executed);
        require(st.ok(), "runScriptText failed: " + st.message);
        require(executed == 2, "runScriptText executed count mismatch");

        const std::string proc = "CREATE PROCEDURE dbmw_it_script_proc AS SELECT 1 AS n";
        auto procSt = dbmw::common::util::createRoutine(proc);
        require(procSt.ok(), "createRoutine(procedure) failed: " + procSt.message);
        std::int64_t dropped = 0;
        requireOk(dbmw::DBMW::execute("DROP PROCEDURE dbmw_it_script_proc", dropped),
                  "drop script procedure");
    }

    void testRoutinesAndCall(Fixture &f) {
        require(dbmw::common::util::createRoutine(
                    "CREATE FUNCTION dbmw_it_add(@a INT, @b INT) RETURNS INT AS BEGIN RETURN @a + @b END").ok(),
                "createRoutine(function) failed");
        RoutineRef addRef{"dbmw_it_add", RoutineKind::Function};
        CallParams addParams{
            CallParam(ParamDirection::In, std::int64_t(3)),
            CallParam(ParamDirection::In, std::int64_t(4))
        };
        CallResult addRes;
        dbmw::common::util::CallOptions addOpts;
        addOpts.dialect = Dialect::SqlServer;
        addOpts.returnsRows = true;
        auto addSt = dbmw::common::util::call(addRef, addParams, addRes, addOpts);
        require(addSt.ok(), "call(function) failed: " + addSt.message);
        require(!addRes.sets.empty() && !addRes.sets.front().rows().empty(),
                "function result set missing");
        if (!addRes.sets.empty() && !addRes.sets.front().rows().empty()) {
            const auto &v = addRes.sets.front().rows().front().data().begin()->second;
            require(asInt(v) == 7, "function return mismatch");
        }

        std::vector<ResultSet> sets;
        auto mSt = dbmw::DBMW::queryAll("SELECT 1 AS n; SELECT 2 AS n", Params{}, sets);
        require(mSt.ok(), "queryAll failed on ODBC: " + mSt.message);
        require(!sets.empty(), "queryAll returned no result set on ODBC");

        require(dbmw::common::util::createRoutine(
                    "CREATE PROCEDURE dbmw_it_rows AS SELECT 1 AS n").ok(),
                "createRoutine(procedure) failed");
        RoutineRef rowsRef{"dbmw_it_rows", RoutineKind::Procedure};
        CallResult rowsRes;
        dbmw::common::util::CallOptions rowsOpts;
        rowsOpts.dialect = Dialect::SqlServer;
        rowsOpts.returnsRows = true;
        auto rowsSt = dbmw::common::util::call(rowsRef, CallParams{}, rowsRes, rowsOpts);
        require(rowsSt.ok(), "call(procedure) failed: " + rowsSt.message);
        require(!rowsRes.sets.empty() && !rowsRes.sets.front().rows().empty(),
                "procedure result set missing");

        std::int64_t d = 0;
        requireOk(dbmw::DBMW::execute("DROP FUNCTION dbmw_it_add", d), "drop fn add");
        requireOk(dbmw::DBMW::execute("DROP PROCEDURE dbmw_it_rows", d), "drop procedure rows");
    }

    void testAsyncUtil(Fixture &f) {
        const std::string fn =
                "CREATE FUNCTION dbmw_it_aadd(@a INT, @b INT) RETURNS INT AS BEGIN RETURN @a + @b END";
        auto fr = dbmw::async::util::createRoutine(fn).get();
        require(fr.status.ok(), "async createRoutine failed: " + fr.status.message);

        const std::string script =
                "INSERT INTO " + f.table + " (name, qty, amount, created_at) "
                "VALUES ('async_script',1,1.0,GETDATE());";
        auto exec = dbmw::async::util::runScriptText(script).get();
        require(exec.status.ok(), "async runScriptText failed: " + exec.status.message);

        dbmw::async::util::Options callAllOpts;
        auto mr = dbmw::async::util::callAll(
            "SELECT dbo.dbmw_it_aadd(?, ?)",
            dbmw::common::Params{std::int64_t(6), std::int64_t(7)}, callAllOpts).get();
        require(mr.status.ok(), "async callAll failed: " + mr.status.message);
        require(!mr.sets.empty() && !mr.sets.front().rows().empty(), "async callAll set missing");
        if (!mr.sets.empty() && !mr.sets.front().rows().empty()) {
            const auto &val = mr.sets.front().rows().front().data().begin()->second;
            require(asInt(val) == 13, "async function return mismatch");
        }

        std::int64_t d = 0;
        requireOk(dbmw::DBMW::execute("DROP FUNCTION dbmw_it_aadd", d), "drop async function");
    }
}

int main() {
    Fixture fixture;
    try {
        fixture.start();
        testTypesKeysAndErrors(fixture);
        testTransactionsPreparedBatchCursorAsync(fixture);
        testEntityMapping(fixture);
        testScriptExecution(fixture);
        testRoutinesAndCall(fixture);
        testAsyncUtil(fixture);
        std::cout << "ODBC SQL Server integration test passed (" << checks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ODBC SQL Server integration test failed after " << checks
                << " checks: " << error.what() << '\n';
        return 1;
    }
}
