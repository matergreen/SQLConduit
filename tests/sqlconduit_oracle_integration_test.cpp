#include "sqlconduit/common/oracle_types.h"
#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/util.h"
#include "sqlconduit/drivers/oracle.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static sqlconduit::Client g_client;

struct OraItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    double price = 0;
    std::optional<sqlconduit::common::Blob> payload;
    std::optional<std::string> doc;
    std::optional<sqlconduit::common::Decimal> amount;
    sqlconduit::common::Timestamp createdAt;
};

struct OraRequiredBlob {
    sqlconduit::common::Blob payload;
};

namespace sqlconduit::mapping {
    template<>
    struct RowMapper<OraItem> {
        static Mapping<OraItem> describe() {
            return Mapping<OraItem>()
                    .field(&OraItem::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                    .field(&OraItem::name, "name")
                    .field(&OraItem::qty, "qty")
                    .field(&OraItem::price, "price")
                    .field(&OraItem::payload, "payload")
                    .field(&OraItem::doc, "doc")
                    .field(&OraItem::amount, "amount")
                    .field(&OraItem::createdAt, "created_at");
        }
    };

    template<>
    struct RowMapper<OraRequiredBlob> {
        static Mapping<OraRequiredBlob> describe() {
            return Mapping<OraRequiredBlob>().field(&OraRequiredBlob::payload, "payload");
        }
    };
}

namespace {
    using sqlconduit::common::ErrorCode;
    using sqlconduit::common::Params;
    using sqlconduit::common::ResultSet;
    using sqlconduit::common::Status;
    using sqlconduit::common::Value;

    int gChecks = 0;

    void require(const bool condition, const std::string &message) {
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
                default: out << static_cast<char>(c);
            }
        }
        return out.str();
    }

    std::int64_t asInt(const Value &value) {
        if (const auto *v = std::get_if<std::int64_t>(&value)) return *v;
        if (const auto *v = std::get_if<sqlconduit::common::Decimal>(&value))
            return static_cast<std::int64_t>(std::strtoll(v->value.c_str(), nullptr, 10));
        throw std::runtime_error("expected integer result value");
    }

    const std::string &asString(const Value &value) {
        if (const auto *v = std::get_if<std::string>(&value)) return *v;
        throw std::runtime_error("expected string result value");
    }

    struct Fixture {
        std::string table;
        std::string configPath;
        bool initialized = false;

        Fixture() {
            const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            table = "SQLCONDUIT_IT_" + std::to_string(static_cast<unsigned long long>(stamp) % 1000000000);
            configPath = "/tmp/" + table + ".json";
        }

        ~Fixture() {
            if (initialized) {
                std::int64_t affected = 0;
                (void) g_client.execute("DROP TABLE " + table + " PURGE", affected);
                g_client.shutdown(std::chrono::milliseconds(3000));
            }
            std::remove(configPath.c_str());
        }

        void start() {
            requireOk(g_client.addDriver(sqlconduit::drivers::oracle()),
                      "register Oracle driver");
            const std::string host = env("SQLCONDUIT_TEST_ORACLE_HOST", "127.0.0.1");
            const std::string port = env("SQLCONDUIT_TEST_ORACLE_PORT", "1521");
            const std::string user = env("SQLCONDUIT_TEST_ORACLE_USER", "system");
            const std::string service = env("SQLCONDUIT_TEST_ORACLE_SERVICE", "XEPDB1");
            require(!env("SQLCONDUIT_TEST_ORACLE_PASSWORD").empty(),
                    "SQLCONDUIT_TEST_ORACLE_PASSWORD must be set for the integration test");

            std::ostringstream ds;
            ds << "{\"name\":\"ora\",\"type\":\"oracle\","
                    << "\"host\":\"" << jsonEscape(host) << "\",\"port\":" << port << ','
                    << "\"user\":\"" << jsonEscape(user) << "\","
                    << "\"password_env\":\"SQLCONDUIT_TEST_ORACLE_PASSWORD\","
                    << "\"oracle\":{\"service_name\":\"" << jsonEscape(service) << "\"},"
                    << "\"connection_timeout_ms\":5000,\"query_timeout_ms\":0}";

            std::ofstream file(configPath);
            require(static_cast<bool>(file), "cannot create temporary integration config");
            file << "{\n"
                    << "\"default_datasource\":\"ora\",\n"
                    << "\"heartbeat_interval_ms\":1000,\n"
                    << "\"pool\":{\"enabled\":true,\"min\":0,\"max\":4,"
                    "\"borrow_timeout_ms\":5000,\"validation_interval_ms\":0},\n"
                    << "\"retry\":{\"max_attempts\":1,\"retry_writes\":false},\n"
                    << "\"circuit_breaker\":{\"failure_threshold\":0},\n"
                    << "\"prepared_cache\":{\"enabled\":true,\"max_per_connection\":8},\n"
                    << "\"query_cache\":{\"enabled\":false},\n"
                    << "\"observability\":{\"sql_log\":{\"enabled\":false},"
                    "\"slow_sql\":{\"enabled\":false}},\n"
                    << "\"async\":{\"enabled\":true,\"threads\":2,\"queue_size\":64},\n"
                    << "\"datasources\":[" << ds.str() << "],\n\"groups\":[]\n}\n";
            file.close();

            requireOk(g_client.init(configPath), "g_client.init");
            initialized = true;

            std::int64_t affected = 0;
            requireOk(g_client.execute(
                          "CREATE TABLE " + table + " ("
                          "\"id\" NUMBER GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, "
                          "\"name\" VARCHAR2(200) NOT NULL UNIQUE, "
                          "\"qty\" NUMBER(19) NOT NULL, "
                          "\"price\" BINARY_DOUBLE NOT NULL, "
                          "\"payload\" RAW(64), "
                          "\"doc\" CLOB, "
                          "\"big_blob\" BLOB, "
                          "\"amount\" NUMBER(30,9), "
                          "\"created_at\" TIMESTAMP NOT NULL)", affected), "create table");
        }
    };

    void testConnectivityAndTypes(Fixture &f) {
        ResultSet identity;
        requireOk(g_client.query("SELECT USER AS CURRENT_USER, 1 AS ONE FROM DUAL", identity),
                  "SELECT ... FROM DUAL");
        require(identity.rowCount() == 1, "DUAL query returned wrong row count");
        require(!asString(identity.rows()[0].at("CURRENT_USER")).empty(), "USER is empty");

        const sqlconduit::common::Timestamp created = std::chrono::system_clock::now();
        const sqlconduit::common::Blob payload{0x00, 0x01, 0x7f, 0x80, 0xff};
        sqlconduit::common::Blob bigBlob(5000);
        for (std::size_t i = 0; i < bigBlob.size(); ++i)
            bigBlob[i] = static_cast<unsigned char>((i * 7 + 3) & 0xff);
        const std::string clobText = "Oracle CLOB 中文往返验证";
        const sqlconduit::common::Decimal amount{"12345.678901234"};
        std::int64_t affected = 0;
        requireOk(g_client.execute(
                      "INSERT INTO " + f.table +
                      " (\"name\", \"qty\", \"price\", \"payload\", \"doc\", \"big_blob\", "
                      "\"amount\", \"created_at\") VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                      Params{
                          std::string("raw-types"), std::int64_t(7), 12.5, payload,
                          clobText, bigBlob, amount, created
                      },
                      affected), "insert typed row");
        require(affected == 1, "typed insert affected mismatch");

        ResultSet ids;
        requireOk(g_client.query("SELECT \"id\" FROM " + f.table + " WHERE \"name\" = ?",
                                 Params{std::string("raw-types")}, ids), "read generated id");
        require(ids.rowCount() == 1, "typed row count mismatch");

        ResultSet rows;
        requireOk(g_client.query(
                      "SELECT \"id\", \"qty\", \"price\", \"payload\", \"doc\", \"big_blob\", "
                      "\"amount\", \"created_at\" FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("raw-types")}, rows), "read raw row back");
        require(rows.rowCount() == 1, "raw row count mismatch");
        const auto &row = rows.rows()[0];
        require(asInt(row.at("qty")) == 7, "NUMBER(19) round trip");
        const auto *price = std::get_if<double>(&row.at("price"));
        require(price != nullptr && *price == 12.5, "BINARY_DOUBLE round trip");
        const auto *blob = std::get_if<sqlconduit::common::Blob>(&row.at("payload"));
        require(blob != nullptr && *blob == payload, "RAW round trip to Blob");
        require(asString(row.at("doc")) == clobText, "UTF-8 CLOB round trip to string");
        const auto *big = std::get_if<sqlconduit::common::Blob>(&row.at("big_blob"));
        require(big != nullptr && big->size() == bigBlob.size(),
                "BLOB round trip length (temporary LOB bind)");
        if (big && big->size() == bigBlob.size())
            require(*big == bigBlob, "BLOB round trip content");
        const auto *dec = std::get_if<sqlconduit::common::Decimal>(&row.at("amount"));
        require(dec != nullptr && dec->value == amount.value, "NUMBER(30,9) round trip to Decimal");
        const auto *ts = std::get_if<sqlconduit::common::Timestamp>(&row.at("created_at"));
        require(ts != nullptr, "TIMESTAMP round trip to Timestamp");
        if (ts)
            require(sqlconduit::common::timestampToString(*ts) == sqlconduit::common::timestampToString(created),
                    "TIMESTAMP value preserved");
    }

    void testMappingRoundTrip(Fixture &f) {
        OraItem item;
        item.name = "mapping-row";
        item.qty = 3;
        item.price = 9.25;
        item.payload = sqlconduit::common::Blob{0x0a, 0x0b};
        item.doc = "doc body";
        item.amount = sqlconduit::common::Decimal{"42.5"};
        item.createdAt = std::chrono::system_clock::now();

        const auto inserted = sqlconduit::insertAs<OraItem>(g_client, f.table, item);
        require(inserted.status.ok(), "insertAs failed: " + inserted.status.message);
        require(inserted.affected == 1, "insertAs affected mismatch");
        require(item.id > 0, "insertAs did not backfill the generated id");

        const auto fetched = sqlconduit::queryAs<OraItem>(g_client,
                                                          "SELECT \"id\", \"name\", \"qty\", \"price\", \"payload\", \"doc\", \"amount\", "
                                                          "\"created_at\" FROM " + f.table + " WHERE \"id\" = ?",
                                                          Params{item.id});
        require(fetched.status.ok(), "queryAs failed: " + fetched.status.message);
        require(fetched.items.size() == 1, "queryAs row count mismatch");
        require(fetched.items[0].name == "mapping-row", "queryAs name mismatch");
        require(fetched.items[0].qty == 3, "queryAs qty mismatch");
        require(fetched.items[0].payload == item.payload, "queryAs payload mismatch");
        require(fetched.items[0].doc == "doc body", "queryAs doc mismatch");

        OraItem toUpdate = fetched.items[0];
        toUpdate.qty = 11;
        const auto updated = sqlconduit::updateAs<OraItem>(g_client, f.table, toUpdate);
        require(updated.status.ok() && updated.affected == 1, "updateAs failed");

        std::vector<OraItem> batch;
        for (int i = 0; i < 3; ++i) {
            OraItem b;
            b.name = "batch-" + std::to_string(i);
            b.qty = i;
            b.price = 1.5;
            b.amount = sqlconduit::common::Decimal{"1"};
            b.createdAt = std::chrono::system_clock::now();
            batch.push_back(b);
        }
        const auto batched = sqlconduit::insertBatchAs<OraItem>(g_client, f.table, batch);
        require(batched.status.ok(), "insertBatchAs failed: " + batched.status.message);
        require(batched.batch.totalAffected() == 3, "insertBatchAs affected mismatch");
        for (const OraItem &b: batch)
            require(b.id > 0, "insertBatchAs did not backfill a batch row id");
    }

    void testTransactionsAndSavepoints(Fixture &f) {
        requireOk(g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            Status st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{
                    std::string("tx-keep"), std::int64_t(1), 1.0,
                    std::chrono::system_clock::now()
                }, affected);
            if (!st.ok()) return st;
            st = session.savepoint("before_temp");
            if (!st.ok()) return st;
            st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{
                    std::string("tx-temp"), std::int64_t(1), 1.0,
                    std::chrono::system_clock::now()
                }, affected);
            if (!st.ok()) return st;
            return session.rollbackToSavepoint("before_temp");
        }), "transaction commit/savepoint");

        ResultSet rows;
        requireOk(g_client.query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-keep")}, rows), "count kept row");
        require(asInt(rows.rows()[0].at("N")) == 1, "committed row is missing");
        requireOk(g_client.query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-temp")}, rows), "count rolled back row");
        require(asInt(rows.rows()[0].at("N")) == 0, "savepoint rollback did not undo the row");

        const Status rolledBack = g_client.transaction([&](sqlconduit::core::Session &session) {
            std::int64_t affected = 0;
            const Status st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{
                    std::string("tx-abort"), std::int64_t(1), 1.0,
                    std::chrono::system_clock::now()
                }, affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::QueryError, "deliberate rollback");
        });
        require(!rolledBack.ok(), "deliberate failure did not abort the transaction");
        requireOk(g_client.query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-abort")}, rows), "count aborted row");
        require(asInt(rows.rows()[0].at("N")) == 0, "aborted transaction left a row behind");
    }

    void testStreamingAndErrors(Fixture &f) {
        std::int64_t affected = 0;
        for (int i = 0; i < 5; ++i) {
            requireOk(g_client.execute(
                          "INSERT INTO " + f.table +
                          " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                          Params{
                              std::string("stream-" + std::to_string(i)), std::int64_t(i), 1.0,
                              std::chrono::system_clock::now()
                          }, affected), "seed stream row");
        }
        std::uint64_t streamed = 0;
        requireOk(g_client.queryEach(
                      "SELECT \"name\" FROM " + f.table + " WHERE \"name\" LIKE 'stream-%'",
                      Params{}, [](const sqlconduit::common::Row &) { return true; }, streamed),
                  "queryEach");
        require(streamed == 5, "queryEach streamed the wrong number of rows");

        sqlconduit::common::ParamBatch batch{
            Params{
                std::string("array-0"), std::int64_t(10), 2.0,
                std::chrono::system_clock::now()
            },
            Params{
                std::string("array-1"), std::int64_t(11), 2.0,
                std::chrono::system_clock::now()
            },
            Params{
                std::string("array-2"), std::int64_t(12), 2.0,
                std::chrono::system_clock::now()
            }
        };
        sqlconduit::common::BatchResult batchResult;
        requireOk(g_client.executeBatch(
                      "INSERT INTO " + f.table +
                      " (\"name\", \"qty\", \"price\", \"created_at\") "
                      "VALUES (?, ?, ?, ?)", batch, batchResult), "OCI array DML");
        require(batchResult.affected.size() == 3 && batchResult.totalAffected() == 3,
                "OCI array DML row counts");

        sqlconduit::core::CursorOptions cursorOptions;
        cursorOptions.batch_size = 2;
        std::unique_ptr<sqlconduit::core::Cursor> cursor;
        requireOk(g_client.openCursor(
                      "SELECT \"name\", \"qty\" FROM " + f.table +
                      " WHERE \"name\" LIKE ? ORDER BY \"name\"",
                      Params{std::string("array-%")}, cursorOptions, cursor),
                  "open OCI statement cursor");
        ResultSet cursorRows;
        while (cursor->hasNext()) requireOk(cursor->fetch(2, cursorRows), "OCI cursor fetch");
        require(cursorRows.rowCount() == 3 && cursor->rowsFetched() == 3,
                "OCI cursor incremental row count");
        requireOk(cursor->close(), "close OCI statement cursor");

        std::vector<ResultSet> implicitSets;
        requireOk(g_client.queryAll(
                      "DECLARE c1 SYS_REFCURSOR; c2 SYS_REFCURSOR; BEGIN "
                      "OPEN c1 FOR SELECT \"name\" FROM " + f.table +
                      " WHERE \"name\" LIKE ? ORDER BY \"name\"; "
                      "DBMS_SQL.RETURN_RESULT(c1); "
                      "OPEN c2 FOR SELECT \"name\" FROM " + f.table +
                      " WHERE \"name\" LIKE ? ORDER BY \"name\"; "
                      "DBMS_SQL.RETURN_RESULT(c2); END;",
                      Params{std::string("array-%"), std::string("stream-%")}, implicitSets),
                  "Oracle implicit result sets");
        require(implicitSets.size() == 2 && implicitSets[0].rowCount() == 3 &&
                implicitSets[1].rowCount() == 5,
                "OCIStmtGetNextResult returned both result sets in order");

        const Status duplicate = g_client.execute(
            "INSERT INTO " + f.table +
            " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
            Params{
                std::string("raw-types"), std::int64_t(1), 1.0,
                std::chrono::system_clock::now()
            }, affected);
        require(!duplicate.ok(), "duplicate insert unexpectedly succeeded");
        require(duplicate.code == ErrorCode::ConstraintViolation,
                "ORA-00001 should map to ConstraintViolation, got " +
                std::string(sqlconduit::common::errorCodeToString(duplicate.code)));
        require(duplicate.nativeCode == 1, "ORA-00001 should be recorded as nativeCode=1");
        require(duplicate.sqlState == "23000",
                "ORA-00001 should map to SQLSTATE 23000, got '" + duplicate.sqlState + "'");
    }

    void testCallableApi(Fixture &f) {
        using sqlconduit::common::CallOutput;
        using sqlconduit::common::CallParam;
        using sqlconduit::common::CallParams;
        using sqlconduit::common::ParamDirection;
        using sqlconduit::common::TypedArray;
        using sqlconduit::common::ValueType;

        CallOutput output;
        CallParams params{
            CallParam::out(ValueType::Int64),
            CallParam{Value{std::int64_t(41)}},
            CallParam::refCursor()
        };
        requireOk(g_client.call(
                      "BEGIN ? := ? + 1; OPEN ? FOR SELECT COUNT(*) AS N FROM " + f.table +
                      "; END;", params, output), "scalar OUT and REF CURSOR call");
        require(output.outParams.size() == 1 && asInt(output.outParams[0]) == 42,
                "scalar OUT bind value");
        require(output.sets.size() == 1 && output.sets[0].rowCount() == 1,
                "REF CURSOR result set");

        TypedArray numbers{
            "SYS.ODCINUMBERLIST",
            {
                Value{std::int64_t(2)}, Value{std::int64_t(3)},
                Value{std::int64_t(5)}
            }
        };
        CallParams collectionParams{
            CallParam::out(ValueType::Int64),
            CallParam{ParamDirection::In, Value{numbers}}
        };
        requireOk(g_client.call(
                      "BEGIN SELECT SUM(COLUMN_VALUE) INTO ? FROM TABLE(?); END;",
                      collectionParams, output), "named collection constructor call");
        require(output.outParams.size() == 1 && asInt(output.outParams[0]) == 10,
                "named collection members were bound in order");
    }

    void testSymmetryGaps(Fixture &f) {
        // Seed rows for the mapped streaming + prepared-reuse checks.
        std::int64_t affected = 0;
        for (int i = 0; i < 4; ++i) {
            requireOk(g_client.execute(
                          "INSERT INTO " + f.table +
                          " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                          Params{
                              std::string("each-" + std::to_string(i)), std::int64_t(i), 1.0,
                              std::chrono::system_clock::now()
                          }, affected), "seed each row");
        }

        // queryEachAs: mapped entity streaming (Oracle previously only covered queryEach).
        std::uint64_t mapped = 0;
        auto each = sqlconduit::queryEachAs<OraItem>(g_client,
                                                     "SELECT \"id\", \"name\", \"qty\", \"price\", "
                                                     "\"payload\", \"doc\", \"amount\", \"created_at\" FROM "
                                                     + f.table + " WHERE \"name\" LIKE ?",
                                                     Params{std::string("each-%")},
                                                     [](OraItem &&) { return true; }, mapped);
        ResultSet eachCount;
        requireOk(g_client.query("SELECT COUNT(*) AS N FROM " + f.table + " WHERE \"name\" LIKE ?",
                                 Params{std::string("each-%")}, eachCount), "each count");
        const auto eachRows = asInt(eachCount.rows()[0].at("N"));
        require(each.ok() && mapped == 4,
                "queryEachAs mapped count mismatch (mapped=" + std::to_string(mapped)
                + " rows=" + std::to_string(eachRows) + ")");
        std::uint64_t requiredMapped = 0;
        const auto required = sqlconduit::queryEachAs<OraRequiredBlob>(
            g_client,
            "SELECT \"payload\" FROM " + f.table + " WHERE \"name\" = ?",
            Params{std::string("each-0")}, [](OraRequiredBlob &&) { return true; }, requiredMapped);
        require(!required.ok() && required.code == ErrorCode::MappingError && requiredMapped == 0,
                "NULL should fail mapping into a non-optional Blob");

        // Async path: queryAsync is configured on but was never invoked.
        auto future = g_client.queryAsync("SELECT COUNT(*) AS N FROM " + f.table);
        const auto asyncRows = future.get();
        requireOk(asyncRows.status, "async query");
        require(asInt(asyncRows.rows.rows()[0].at("N")) >= 4, "async count mismatch");

        // Explicit prepared-statement reuse (config has prepared_cache enabled).
        requireOk(g_client.withSession([&](sqlconduit::core::Session &session) {
            sqlconduit::core::PreparedStatementHandle prepared;
            auto st = session.prepare("SELECT \"qty\" FROM " + f.table + " WHERE \"name\" = ?",
                                      Params{std::string()}, prepared);
            if (!st.ok()) return st;
            if (!prepared.valid()) return Status::error(ErrorCode::QueryError, "invalid prepared handle");
            for (int i = 0; i < 4; ++i) {
                ResultSet rows;
                st = session.executePrepared(prepared,
                                             Params{std::string("each-" + std::to_string(i))}, rows);
                if (!st.ok()) return st;
                if (rows.rowCount() != 1 || asInt(rows.rows()[0].at("qty")) != i)
                    return Status::error(ErrorCode::QueryError, "prepared result mismatch");
            }
            return Status::OK();
        }), "explicit prepared reuse");

        // NULL binding round trip.
        ResultSet nullRow;
        requireOk(g_client.query("SELECT ? AS v FROM DUAL", Params{nullptr}, nullRow), "NULL bind");
        require(std::holds_alternative<std::nullptr_t>(nullRow.rows()[0].at("v")),
                "NULL binding round trip failed");

        // Bad SQL must be classified as QueryError (not silently swallowed).
        ResultSet ignored;
        const Status bad = g_client.query("SELECT * FROM sqlconduit_it_no_such_table", ignored);
        require(!bad.ok() && bad.code == ErrorCode::QueryError,
                "bad SQL should be classified as QueryError, got " +
                std::string(sqlconduit::common::errorCodeToString(bad.code)));
    }
}

int main() {
    try {
        Fixture f;
        f.start();
        testConnectivityAndTypes(f);
        testMappingRoundTrip(f);
        testTransactionsAndSavepoints(f);
        testStreamingAndErrors(f);
        testCallableApi(f);
        testSymmetryGaps(f);
        std::cout << "Oracle integration test passed (" << gChecks << " checks)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cout << "Oracle integration test failed after " << gChecks << " checks: "
                << e.what() << "\n";
        return 1;
    }
}
