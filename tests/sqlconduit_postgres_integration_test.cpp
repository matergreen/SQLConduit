#include "sqlconduit/common/pg_types.h"
#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/util.h"

#include <atomic>
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
#include <thread>
#include <vector>

static sqlconduit::Client g_client;

struct PgItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    double price = 0;
    bool active = false;
    sqlconduit::common::Timestamp createdAt;
};

struct PgTyped {
    std::int64_t id = 0;
    std::vector<std::string> tags;
    sqlconduit::common::PgPoint pt;
};

namespace sqlconduit::mapping {
    template<>
    struct RowMapper<PgTyped> {
        static Mapping<PgTyped> describe() {
            return Mapping<PgTyped>()
                    .field(&PgTyped::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                    .field(&PgTyped::tags, "tags")
                    .field(&PgTyped::pt, "pt");
        }
    };

    template<>
    struct RowMapper<PgItem> {
        static Mapping<PgItem> describe() {
            return Mapping<PgItem>()
                    .field(&PgItem::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                    .field(&PgItem::name, "name")
                    .field(&PgItem::qty, "qty")
                    .field(&PgItem::price, "price")
                    .field(&PgItem::active, "active")
                    .field(&PgItem::createdAt, "created_at");
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

    int gChecks = 0;

    void require(bool condition, const std::string &message) {
        ++gChecks;
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
        std::ostringstream out;
        for (const unsigned char c: value) {
            switch (c) {
                case '\\': out << "\\\\";
                    break;
                case '"': out << "\\\"";
                    break;
                case '\n': out << "\\n";
                    break;
                case '\r': out << "\\r";
                    break;
                case '\t': out << "\\t";
                    break;
                default:
                    if (c < 0x20) {
                        const char *hex = "0123456789abcdef";
                        out << "\\u00" << hex[c >> 4] << hex[c & 0x0f];
                    } else {
                        out << static_cast<char>(c);
                    }
            }
        }
        return out.str();
    }

    std::int64_t asInt(const Value &value) {
        if (const auto *v = std::get_if<std::int64_t>(&value)) return *v;
        throw std::runtime_error("expected int64 result value");
    }

    const std::string &asString(const Value &value) {
        if (const auto *v = std::get_if<std::string>(&value)) return *v;
        throw std::runtime_error("expected string result value");
    }

    bool existsByName(const std::string &table, const std::string &name) {
        ResultSet rows;
        requireOk(g_client.query("SELECT count(*) AS n FROM " + table + " WHERE name = ?",
                                 Params{std::string(name)}, rows),
                  "count by name");
        return asInt(rows.rows().at(0).at("n")) != 0;
    }

    struct Fixture {
        std::string schema;
        std::string table;
        std::string configPath;
        bool initialized = false;

        Fixture() {
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            schema = "sqlconduit_it_" + std::to_string(static_cast<unsigned long long>(stamp));
            table = schema + ".items";
            configPath = "/tmp/" + schema + ".json";
        }

        ~Fixture() {
            if (initialized) {
                std::int64_t affected = 0;
                (void) g_client.execute("DROP SCHEMA IF EXISTS " + schema + " CASCADE", affected);
                g_client.shutdown(std::chrono::milliseconds(3000));
            }
            std::remove(configPath.c_str());
        }

        void start() {
            const std::string host = env("SQLCONDUIT_TEST_PG_HOST", "127.0.0.1");
            const std::string port = env("SQLCONDUIT_TEST_PG_PORT", "5432");
            const std::string user = env("SQLCONDUIT_TEST_PG_USER", "postgres");
            const std::string database = env("SQLCONDUIT_TEST_PG_DATABASE", "postgres");
            require(!env("SQLCONDUIT_TEST_PG_PASSWORD").empty(),
                    "SQLCONDUIT_TEST_PG_PASSWORD must be set for the integration test");

            auto dataSource = [&](const std::string &name, int maxRows) {
                std::ostringstream out;
                out << "{\"name\":\"" << name << "\",\"type\":\"postgres\",";
                out << "\"host\":\"" << jsonEscape(host) << "\",\"port\":" << port << ',';
                out << "\"user\":\"" << jsonEscape(user) << "\",";
                out << "\"password_env\":\"SQLCONDUIT_TEST_PG_PASSWORD\",";
                out << "\"database\":\"" << jsonEscape(database) << "\",";
                out << "\"connection_timeout_ms\":3000,\"query_timeout_ms\":0,";
                out << "\"max_result_rows\":" << maxRows << '}';
                return out.str();
            };

            std::ofstream file(configPath);
            require(static_cast<bool>(file), "cannot create temporary integration config");
            file << "{\n"
                    << "\"default_datasource\":\"pg\",\n"
                    << "\"heartbeat_interval_ms\":1000,\n"
                    << "\"pool\":{\"enabled\":true,\"min\":0,\"max\":4,"
                    "\"borrow_timeout_ms\":1000,\"validation_interval_ms\":0},\n"
                    << "\"retry\":{\"max_attempts\":1,\"retry_writes\":false},\n"
                    << "\"circuit_breaker\":{\"failure_threshold\":0},\n"
                    << "\"prepared_cache\":{\"enabled\":true,\"max_per_connection\":8},\n"
                    << "\"cursor\":{\"enabled\":true,\"default_batch_size\":2,"
                    "\"max_open_cursors\":1,\"allow_scrollable\":false},\n"
                    << "\"query_cache\":{\"enabled\":true,\"ttl_ms\":60000,"
                    "\"max_entries\":100},\n"
                    << "\"observability\":{\"sql_log\":{\"enabled\":false},"
                    "\"slow_sql\":{\"enabled\":true,\"threshold_ms\":0,"
                    "\"aggregate_capacity\":100,\"recent_capacity\":100,"
                    "\"retain_rendered_sql\":false},"
                    "\"pool_metrics\":{\"enabled\":true}},\n"
                    << "\"async\":{\"enabled\":true,\"threads\":2,\"queue_size\":64},\n"
                    << "\"datasources\":[" << dataSource("pg", 0) << ','
                    << dataSource("limited", 2) << "],\n\"groups\":[]\n}\n";
            file.close();

            requireOk(g_client.init(configPath), "g_client.init");
            initialized = true;
            std::int64_t affected = 0;
            requireOk(g_client.execute("CREATE SCHEMA " + schema, affected), "create schema");
            requireOk(g_client.execute(
                          "CREATE TABLE " + table + " ("
                          "id BIGSERIAL PRIMARY KEY, name TEXT NOT NULL UNIQUE, qty BIGINT NOT NULL, "
                          "price DOUBLE PRECISION NOT NULL, active BOOLEAN NOT NULL, payload BYTEA, "
                          "amount NUMERIC(30,9), due_date DATE, local_time TIME, external_id UUID, "
                          "metadata JSONB, created_at TIMESTAMPTZ NOT NULL)", affected), "create table");
            requireOk(g_client.execute("CREATE TYPE " + schema +
                                       ".addr AS (city TEXT, zip TEXT)", affected),
                      "create composite type");
            requireOk(g_client.execute(
                          "CREATE TABLE " + schema + ".typed ("
                          "id BIGSERIAL PRIMARY KEY, tags TEXT[], nums INT[], addr "
                          + schema + ".addr, pt POINT, bx BOX)", affected),
                      "create typed table");
        }
    };

    void testConnectivityAndTypes(Fixture &f) {
        ResultSet version;
        requireOk(g_client.query("SELECT current_database() AS db, version() AS version", version),
                  "server identity query");
        require(version.rowCount() == 1, "server identity query returned wrong row count");
        require(!asString(version.rows()[0].at("version")).empty(), "PostgreSQL version is empty");

        auto ds = g_client.dataSource();
        require(ds != nullptr, "default datasource is missing");
        const sqlconduit::common::Timestamp timestamp = std::chrono::system_clock::now();
        const sqlconduit::common::Blob blob{0x00, 0x01, 0x7f, 0x80, 0xff};
        const sqlconduit::common::Decimal amount{"123456789012345678901.123456789"};
        const sqlconduit::common::Date dueDate{"2026-09-06"};
        const sqlconduit::common::Time localTime{"11:50:00.123456"};
        const sqlconduit::common::Uuid externalId{"550e8400-e29b-41d4-a716-446655440000"};
        const sqlconduit::common::Json metadata{"{\"source\":\"integration\",\"ok\":true}"};
        sqlconduit::common::GeneratedKeys keys;
        std::int64_t affected = 0;
        requireOk(ds->execute(
                      "INSERT INTO " + f.table
                      + " (name, qty, price, active, payload, amount, due_date, local_time, external_id, "
                      "metadata, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) RETURNING id",
                      Params{
                          std::string("alpha"), std::int64_t(7), 12.5, true, blob, amount, dueDate,
                          localTime, externalId, metadata, timestamp
                      },
                      affected, keys), "insert with RETURNING");
        require(affected == 1 && keys.lastInsertId() > 0, "generated key or affected rows is wrong");

        ResultSet rows;
        requireOk(g_client.query(
                      "SELECT name, qty, price, active, payload, amount, due_date, local_time, external_id, "
                      "metadata, created_at FROM " + f.table + " WHERE id = ?",
                      Params{keys.lastInsertId()}, rows), "type round trip");
        require(rows.rowCount() == 1, "type round trip returned wrong row count");
        const auto &row = rows.rows()[0];
        require(asString(row.at("name")) == "alpha", "text round trip failed");
        require(asInt(row.at("qty")) == 7, "bigint round trip failed");
        require(std::abs(std::get<double>(row.at("price")) - 12.5) < 0.0001,
                "double round trip failed");
        require(std::get<bool>(row.at("active")), "boolean round trip failed");
        require(std::get<sqlconduit::common::Blob>(row.at("payload")) == blob, "bytea round trip failed");
        require(std::get<sqlconduit::common::Decimal>(row.at("amount")) == amount,
                "numeric round trip lost precision");
        require(std::get<sqlconduit::common::Date>(row.at("due_date")) == dueDate,
                "date round trip failed");
        require(std::get<sqlconduit::common::Time>(row.at("local_time")) == localTime,
                "time round trip failed");
        require(std::get<sqlconduit::common::Uuid>(row.at("external_id")) == externalId,
                "uuid round trip failed");
        require(std::holds_alternative<sqlconduit::common::Json>(row.at("metadata")) &&
                std::get<sqlconduit::common::Json>(row.at("metadata")).value.find("integration") !=
                std::string::npos,
                "jsonb round trip failed");
        require(std::holds_alternative<sqlconduit::common::Timestamp>(row.at("created_at")),
                "timestamptz was not mapped to Timestamp");

        ResultSet nullRow;
        requireOk(g_client.query("SELECT ?::text AS value", Params{nullptr}, nullRow),
                  "NULL binding");
        require(std::holds_alternative<std::nullptr_t>(nullRow.rows()[0].at("value")),
                "NULL binding round trip failed");
    }

    void testBatchPreparedAndStreaming(Fixture &f) {
        sqlconduit::common::ParamBatch batch{
            Params{std::string("batch-1"), std::int64_t(1)},
            Params{std::string("batch-2"), std::int64_t(2)},
            Params{std::string("batch-3"), std::int64_t(3)}
        };
        sqlconduit::common::BatchResult result;
        requireOk(g_client.executeBatch(
                      "INSERT INTO " + f.table
                      + " (name, qty, price, active, created_at) VALUES (?, ?, 1.0, true, now())",
                      batch, result), "batch insert");
        require(result.affected.size() == 3 && result.totalAffected() == 3,
                "batch affected rows are wrong");

        requireOk(g_client.withSession([&](sqlconduit::core::Session &session) {
            sqlconduit::core::PreparedStatementHandle prepared;
            auto st = session.prepare("SELECT qty FROM " + f.table + " WHERE name = ?",
                                      Params{std::string("sample")}, prepared);
            if (!st.ok()) return st;
            if (!prepared.valid()) return Status::error(ErrorCode::QueryError, "invalid prepared handle");
            for (int i = 1; i <= 3; ++i) {
                ResultSet rows;
                st = session.executePrepared(prepared, Params{std::string("batch-") + std::to_string(i)}, rows);
                if (!st.ok()) return st;
                if (rows.rowCount() != 1 || asInt(rows.rows()[0].at("qty")) != i)
                    return Status::error(ErrorCode::QueryError, "prepared result mismatch");
            }
            return Status::OK();
        }), "explicit prepared statement");

        std::uint64_t streamedRows = 0;
        int callbacks = 0;
        requireOk(g_client.queryEach(
                      "SELECT id FROM " + f.table + " ORDER BY id", {},
                      [&](const sqlconduit::common::Row &) { return ++callbacks < 2; }, streamedRows),
                  "queryEach early stop");
        require(callbacks == 2 && streamedRows == 2, "queryEach early stop count is wrong");
    }

    void testTransactions(Fixture &f) {
        requireOk(g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            auto st = session.execute(
                "INSERT INTO " + f.table
                + " (name, qty, price, active, created_at) VALUES ('committed', 1, 1, true, now())",
                affected);
            if (!st.ok()) return st;
            st = session.savepoint("before_temp");
            if (!st.ok()) return st;
            st = session.execute(
                "INSERT INTO " + f.table
                + " (name, qty, price, active, created_at) VALUES ('savepoint-temp', 1, 1, true, now())",
                affected);
            if (!st.ok()) return st;
            st = session.rollbackToSavepoint("before_temp");
            if (!st.ok()) return st;
            return session.releaseSavepoint("before_temp");
        }), "transaction commit/savepoint");
        require(existsByName(f.table, "committed"), "committed row is missing");
        require(!existsByName(f.table, "savepoint-temp"), "savepoint rollback did not roll back row");

        const Status rollback = g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            const auto st = session.execute(
                "INSERT INTO " + f.table
                + " (name, qty, price, active, created_at) VALUES ('rolled-back', 1, 1, true, now())",
                affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::TxError, "intentional rollback");
        });
        require(rollback.code == ErrorCode::TxError, "transaction did not propagate callback error");
        require(!existsByName(f.table, "rolled-back"), "failed transaction was committed");

        sqlconduit::common::TransactionOptions readOnly;
        readOnly.readOnly = true;
        requireOk(g_client.transaction(readOnly, [&](sqlconduit::core::Session &session) {
            ResultSet rows;
            return session.query("SELECT count(*) AS n FROM " + f.table, rows);
        }), "read-only transaction");

        sqlconduit::common::TransactionOptions timeout;
        timeout.timeout = std::chrono::milliseconds(100);
        const auto started = std::chrono::steady_clock::now();
        const Status timed = g_client.transaction(timeout, [](sqlconduit::core::Session &session) {
            ResultSet rows;
            return session.query("SELECT pg_sleep(2)", rows);
        });
        require(timed.code == ErrorCode::QueryTimeout,
                "transaction timeout was not classified as QueryTimeout: " + timed.message);
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
                "transaction timeout did not interrupt pg_sleep promptly");
    }

    void testErrorsLimitsAndCursor(Fixture &f) {
        std::int64_t affected = 0;
        const Status duplicate = g_client.execute(
            "INSERT INTO " + f.table
            + " (name, qty, price, active, created_at) VALUES (?, 1, 1, true, now())",
            Params{std::string("alpha")}, affected);
        require(duplicate.code == ErrorCode::ConstraintViolation,
                "unique violation was not classified as ConstraintViolation");
        require(duplicate.sqlState == "23505", "unique violation SQLSTATE was not preserved");

        ResultSet limited;
        const Status limit = g_client.query(
            "limited", "SELECT id FROM " + f.table + " ORDER BY id", {}, limited);
        require(limit.code == ErrorCode::QueryError, "max_result_rows did not reject oversized result");

        sqlconduit::core::CursorOptions options;
        options.batch_size = 2;
        std::unique_ptr<sqlconduit::core::Cursor> cursor;
        requireOk(g_client.openCursor("SELECT id FROM " + f.table + " ORDER BY id", {}, options, cursor),
                  "open cursor");
        std::unique_ptr<sqlconduit::core::Cursor> second;
        const Status cursorLimit = g_client.openCursor(
            "SELECT id FROM " + f.table + " ORDER BY id", {}, options, second);
        require(cursorLimit.code == ErrorCode::CursorLimit, "max_open_cursors was not enforced");

        ResultSet streamed;
        while (cursor->hasNext()) requireOk(cursor->fetch(2, streamed), "cursor fetch");
        require(streamed.rowCount() >= 5, "cursor did not stream all rows");
        requireOk(cursor->close(), "cursor close");
        sqlconduit::core::ConnectionPool::Stats stats;
        require(g_client.poolStats(stats), "pool stats unavailable");
        require(stats.borrowed == 0, "cursor close did not immediately return its connection");

        std::unique_ptr<sqlconduit::core::Cursor> reopened;
        requireOk(g_client.openCursor(
                      "SELECT id FROM " + f.table + " ORDER BY id", {}, options, reopened),
                  "reopen cursor after close");
        requireOk(reopened->close(), "close reopened cursor");
    }

    void testCacheAsyncAndObservability(Fixture &f) {
        ResultSet before;
        requireOk(g_client.query("SELECT qty FROM " + f.table + " WHERE name = ?",
                                 Params{std::string("alpha")}, before), "cached read before write");
        std::int64_t affected = 0;
        requireOk(g_client.execute("UPDATE " + f.table + " SET qty = ? WHERE name = ?",
                                   Params{std::int64_t(99), std::string("alpha")}, affected),
                  "cache invalidating write");
        ResultSet after;
        requireOk(g_client.query("SELECT qty FROM " + f.table + " WHERE name = ?",
                                 Params{std::string("alpha")}, after), "cached read after write");
        require(asInt(after.rows()[0].at("qty")) == 99, "write did not invalidate query cache");

        auto future = g_client.queryAsync("SELECT count(*) AS n FROM " + f.table);
        auto asyncRows = future.get();
        requireOk(asyncRows.status, "async future query");
        require(asInt(asyncRows.rows.rows()[0].at("n")) >= 5, "async query returned wrong count");

        sqlconduit::core::ConnectionPool::Stats stats;
        require(g_client.poolStats(stats), "pool stats unavailable after async test");
        require(stats.borrowRequests > 0 && stats.borrowSuccesses > 0 && stats.connectionsCreated > 0,
                "pool counters were not populated");
        require(!g_client.slowSqlStats(100).empty(), "slow SQL aggregates are empty");
        require(!g_client.recentSlowSql(100).empty(), "recent slow SQL records are empty");
    }


    void testEntityMapping(Fixture &f) {
        const std::string ent = f.schema + "_entity";
        std::int64_t aff = 0;
        (void) g_client.execute("DROP TABLE IF EXISTS " + ent, aff);
        requireOk(g_client.execute(
                      "CREATE TABLE " + ent + " ("
                      "id BIGSERIAL PRIMARY KEY, name TEXT NOT NULL UNIQUE, qty BIGINT NOT NULL, "
                      "price DOUBLE PRECISION NOT NULL, active BOOLEAN NOT NULL, "
                      "created_at TIMESTAMPTZ NOT NULL)", aff), "create entity table");

        PgItem item;
        item.name = "map_item_1";
        item.qty = 11;
        item.price = 3.5;
        item.active = true;
        item.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        auto ins = sqlconduit::insertAs<PgItem>(g_client, ent, item);
        require(ins.status.ok(), "insertAs failed: " + ins.status.message);
        require(ins.affected == 1, "insertAs affected mismatch");
        require(item.id > 0, "insertAs did not backfill generated id (PG auto-key gap?)");
        const std::int64_t id = item.id;

        auto got = sqlconduit::queryAs<PgItem>(g_client,
                                               "SELECT id,name,qty,price,active,created_at FROM " + ent + " WHERE id=?",
                                               Params{std::int64_t(id)});
        require(got.status.ok(), "queryAs failed: " + got.status.message);
        require(got.items.size() == 1, "queryAs wrong row count");
        if (!got.items.empty()) {
            require(got.items[0].name == "map_item_1", "mapped name mismatch");
            require(got.items[0].qty == 11, "mapped qty mismatch");
            require(std::abs(got.items[0].price - 3.5) < 1e-9, "mapped price mismatch");
            require(got.items[0].active, "mapped active mismatch");
        }

        item.qty = 99;
        auto upd = sqlconduit::updateAs<PgItem>(g_client, ent, item);
        require(upd.status.ok() && upd.affected == 1, "updateAs failed");
        auto got2 = sqlconduit::queryAs<PgItem>(g_client,
                                                "SELECT id,name,qty,price,active,created_at FROM " + ent +
                                                " WHERE id=?",
                                                Params{std::int64_t(id)});
        require(got2.status.ok() && !got2.items.empty() && got2.items[0].qty == 99,
                "updateAs did not persist");

        PgItem b1;
        b1.name = "map_batch_1";
        b1.qty = 1;
        b1.price = 1.0;
        b1.active = true;
        b1.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        PgItem b2;
        b2.name = "map_batch_2";
        b2.qty = 2;
        b2.price = 2.0;
        b2.active = false;
        b2.createdAt = sqlconduit::common::Timestamp{std::chrono::system_clock::now()};
        std::vector<PgItem> bv{b1, b2};
        auto batch = sqlconduit::insertBatchAs<PgItem>(g_client, ent, bv);
        require(batch.status.ok(), "insertBatchAs failed: " + batch.status.message);
        require(batch.batch.totalAffected() == 2, "insertBatchAs affected mismatch");
        require(bv[0].id > 0 && bv[1].id > 0, "insertBatchAs did not backfill generated ids");
        require(bv[0].id != bv[1].id, "insertBatchAs backfilled the same id twice");

        std::uint64_t mappedRows = 0;
        auto each = sqlconduit::queryEachAs<PgItem>(g_client,
                                                    "SELECT id,name,qty,price,active,created_at FROM " + ent +
                                                    " WHERE name LIKE 'map_%' ORDER BY id",
                                                    Params{}, [&](PgItem &&) { return true; }, mappedRows);
        require(each.ok() && mappedRows >= 3, "queryEachAs mapped count mismatch");

        std::int64_t d = 0;
        requireOk(g_client.execute("DROP TABLE IF EXISTS " + ent, d), "drop entity table");
    }

    void testScriptExecution(Fixture &f) {
        const std::string script =
                "INSERT INTO " + f.table + " (name, qty, price, active, created_at) "
                "VALUES ('script_a',1,1.0,true,now());\n"
                "INSERT INTO " + f.table + " (name, qty, price, active, created_at) "
                "VALUES ('script_b',2,2.0,true,now());";
        std::size_t executed = 0;
        auto st = sqlconduit::common::util::runScriptText(g_client, script, {}, &executed);
        require(st.ok(), "runScriptText failed: " + st.message);
        require(executed == 2, "runScriptText executed count mismatch");

        const std::string proc =
                "CREATE PROCEDURE sqlconduit_it_script_proc() AS $$ BEGIN PERFORM 1; END; $$ LANGUAGE plpgsql";
        auto procSt = sqlconduit::common::util::createRoutine(g_client, proc);
        require(procSt.ok(), "createRoutine(procedure) failed: " + procSt.message);
        std::int64_t dropped = 0;
        requireOk(g_client.execute("DROP PROCEDURE IF EXISTS sqlconduit_it_script_proc", dropped),
                  "drop script procedure");
    }

    void testRoutinesAndCall(Fixture &f) {
        require(sqlconduit::common::util::createRoutine(g_client,
                                                        "CREATE FUNCTION sqlconduit_it_add(a INT, b INT) RETURNS INT AS $$ SELECT a + b $$ "
                                                        "LANGUAGE sql").ok(), "createRoutine(function) failed");
        RoutineRef addRef{"sqlconduit_it_add", RoutineKind::Function};
        CallParams addParams{
            CallParam(ParamDirection::In, std::int64_t(3)),
            CallParam(ParamDirection::In, std::int64_t(4))
        };
        CallResult addRes;
        sqlconduit::common::util::CallOptions addOpts;
        addOpts.dialect = Dialect::Postgres;
        addOpts.returnsRows = true;
        auto addSt = sqlconduit::common::util::call(g_client, addRef, addParams, addRes, addOpts);
        require(addSt.ok(), "call(function) failed: " + addSt.message);
        require(!addRes.sets.empty() && !addRes.sets.front().rows().empty(),
                "function result set missing");
        if (!addRes.sets.empty() && !addRes.sets.front().rows().empty()) {
            const auto &v = addRes.sets.front().rows().front().data().begin()->second;
            require(std::get<std::int64_t>(v) == 7, "function return mismatch");
        }

        require(sqlconduit::common::util::createRoutine(g_client,
                                                        "CREATE FUNCTION sqlconduit_it_swap(INOUT a INT, INOUT b INT) RETURNS RECORD AS $$ "
                                                        "BEGIN a := a + b; b := a - b; a := a - b; END; $$ LANGUAGE plpgsql")
                .ok(),
                "createRoutine(function INOUT) failed");
        RoutineRef swapRef{"sqlconduit_it_swap", RoutineKind::Function};
        CallParams io{
            CallParam(ParamDirection::InOut, std::int64_t(5)),
            CallParam(ParamDirection::InOut, std::int64_t(9))
        };
        CallResult ioRes;
        sqlconduit::common::util::CallOptions ioOpts;
        ioOpts.dialect = Dialect::Postgres;
        ioOpts.returnsRows = true;
        auto ioSt = g_client.withSession([&](sqlconduit::core::Session &s) {
            return sqlconduit::common::util::call(g_client, s, swapRef, io, ioRes, ioOpts);
        });
        require(ioSt.ok(), "call(function INOUT) failed: " + ioSt.message);
        require(ioRes.outParams.size() == 2, "INOUT out params count mismatch");
        if (ioRes.outParams.size() == 2) {
            require(asInt(ioRes.outParams[0]) == 9, "INOUT a after swap mismatch");
            require(asInt(ioRes.outParams[1]) == 5, "INOUT b after swap mismatch");
        }

        std::vector<ResultSet> sets;
        auto mSt = g_client.queryAll("SELECT 1 AS n; SELECT 2 AS n", Params{}, sets);
        require(mSt.ok(), "queryAll failed on PG: " + mSt.message);
        require(!sets.empty(), "queryAll returned no result set on PG");

        std::int64_t d = 0;
        requireOk(g_client.execute("DROP FUNCTION IF EXISTS sqlconduit_it_add", d), "drop fn add");
        requireOk(g_client.execute("DROP FUNCTION IF EXISTS sqlconduit_it_swap", d), "drop fn swap");
    }

    void testArrayCompositeGeometry(Fixture &f) {
        const std::string typed = f.schema + ".typed";
        std::int64_t affected = 0;

        sqlconduit::common::Array tags;
        tags.items.push_back(Value{std::string("red")});
        tags.items.push_back(Value{std::string("blue")});

        sqlconduit::common::Array nums;
        nums.items.push_back(Value{std::int64_t(1)});
        nums.items.push_back(Value{std::int64_t(2)});
        nums.items.push_back(Value{std::int64_t(3)});

        sqlconduit::common::Composite addr;
        addr.fields.emplace_back("city", Value{std::string("Shanghai")});
        addr.fields.emplace_back("zip", Value{std::string("200000")});

        const std::string pointText = sqlconduit::common::pgFormatPoint(sqlconduit::common::PgPoint{1, 2});
        const std::string boxText = sqlconduit::common::pgFormatBox(sqlconduit::common::PgBox{{3, 4}, {1, 2}});

        requireOk(g_client.execute(
                      "INSERT INTO " + typed + " (tags, nums, addr, pt, bx) VALUES (?, ?, ?, ?, ?)",
                      Params{
                          Value{tags}, Value{nums}, Value{addr},
                          Value{sqlconduit::common::Json{pointText}},
                          Value{sqlconduit::common::Json{boxText}}
                      },
                      affected), "insert array/composite/geometry row");
        require(affected == 1, "typed insert affected rows mismatch");

        ResultSet rows;
        requireOk(g_client.query("SELECT tags, nums, addr, pt, bx FROM " + typed, rows),
                  "read typed row");
        require(rows.rowCount() == 1, "typed row count mismatch");
        if (rows.rowCount() != 1) return;
        const auto &row = rows.rows()[0];

        const auto *readTags = std::get_if<sqlconduit::common::Array>(&row.at("tags"));
        require(readTags != nullptr && readTags->items.size() == 2, "TEXT[] surfaced as Array");
        if (readTags && readTags->items.size() == 2)
            require(std::get<std::string>(readTags->items[0]) == "red", "tags[0] value mismatch");

        const auto *readNums = std::get_if<sqlconduit::common::Array>(&row.at("nums"));
        require(readNums != nullptr && readNums->items.size() == 3 &&
                std::get<std::int64_t>(readNums->items[0]) == 1, "INT[] surfaced as Array of int64");

        const auto *readAddr = std::get_if<sqlconduit::common::Composite>(&row.at("addr"));
        require(readAddr != nullptr && readAddr->fields.size() == 2, "composite surfaced as Composite");
        if (readAddr) {
            const auto *city = readAddr->find("city");
            require(city != nullptr && std::get<std::string>(*city) == "Shanghai",
                    "composite field city mismatch");
        }

        const auto *readPoint = std::get_if<sqlconduit::common::Json>(&row.at("pt"));
        require(readPoint != nullptr && readPoint->value == pointText, "POINT surfaced as Json text");
        sqlconduit::common::PgPoint parsed{};
        require(readPoint && sqlconduit::common::pgParsePoint(readPoint->value, parsed) &&
                parsed.x == 1 && parsed.y == 2, "POINT text parses back to PgPoint");

        ResultSet nested;
        requireOk(g_client.query("SELECT ARRAY[[1,2],[3,4]] AS m", nested), "nested array query");
        require(nested.rowCount() == 1, "nested array row missing");
        if (nested.rowCount() == 1) {
            const auto *outer = std::get_if<sqlconduit::common::Array>(&nested.rows()[0].at("m"));
            require(outer && outer->items.size() == 2 &&
                    std::holds_alternative<sqlconduit::common::Array>(outer->items[0]),
                    "nested array parsed as Array of Array");
        }

        const auto entities = sqlconduit::queryAs<PgTyped>(g_client, "SELECT id, tags, pt FROM " + typed, Params{});
        requireOk(entities.status, "queryAs<PgTyped>");
        require(entities.items.size() == 1, "queryAs<PgTyped> row count mismatch");
        if (entities.items.size() == 1) {
            require(entities.items[0].tags.size() == 2 && entities.items[0].tags[0] == "red",
                    "mapping bound std::vector<std::string> from Array");
            require(entities.items[0].pt == sqlconduit::common::PgPoint{1, 2},
                    "mapping bound PgPoint from Json text");
        }

        requireOk(g_client.execute("DELETE FROM " + typed, affected), "clean typed table");
    }

    void testSymmetryGaps(Fixture &f) {
        // Non-ASCII TEXT round trip (UTF-8 is the default PG server encoding).
        const std::string cjk = "中文往返测试";
        std::int64_t affected = 0;
        requireOk(g_client.execute(
                          "INSERT INTO " + f.table
                          + " (name,qty,price,active,created_at) VALUES (?,1,1,true,now())",
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
    }
}

int main() {
    Fixture fixture;
    try {
        fixture.start();
        testConnectivityAndTypes(fixture);
        testBatchPreparedAndStreaming(fixture);
        testTransactions(fixture);
        testErrorsLimitsAndCursor(fixture);
        testCacheAsyncAndObservability(fixture);
        testEntityMapping(fixture);
        testScriptExecution(fixture);
        testRoutinesAndCall(fixture);
        testArrayCompositeGeometry(fixture);
        testSymmetryGaps(fixture);
        std::cout << "PostgreSQL integration test passed (" << gChecks << " checks)\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "PostgreSQL integration test failed after " << gChecks
                << " checks: " << error.what() << '\n';
        return 1;
    }
}
