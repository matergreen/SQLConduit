#include "dbmw/common/oracle_types.h"
#include "dbmw/dbmw.h"
#include "dbmw/mapping.h"
#include "dbmw/util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

struct OraItem {
    std::int64_t id = 0;
    std::string name;
    std::int64_t qty = 0;
    double price = 0;
    dbmw::common::Blob payload;
    std::string doc;
    dbmw::common::Decimal amount{"0"};
    dbmw::common::Timestamp createdAt;
};

namespace dbmw::mapping {
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
}

namespace {
    using dbmw::common::ErrorCode;
    using dbmw::common::Params;
    using dbmw::common::ResultSet;
    using dbmw::common::Status;
    using dbmw::common::Value;

    int gChecks = 0;

    void require(const bool condition, const std::string &message) {
        ++gChecks;
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
        if (const auto *v = std::get_if<dbmw::common::Decimal>(&value))
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
            table = "DBMW_IT_" + std::to_string(static_cast<unsigned long long>(stamp) % 1000000000);
            configPath = "/tmp/" + table + ".json";
        }

        ~Fixture() {
            if (initialized) {
                std::int64_t affected = 0;
                (void) dbmw::DBMW::execute("DROP TABLE " + table + " PURGE", affected);
                dbmw::DBMW::shutdown(std::chrono::milliseconds(3000));
            }
            std::remove(configPath.c_str());
        }

        void start() {
            const std::string host = env("DBMW_TEST_ORACLE_HOST", "127.0.0.1");
            const std::string port = env("DBMW_TEST_ORACLE_PORT", "1521");
            const std::string user = env("DBMW_TEST_ORACLE_USER", "system");
            const std::string service = env("DBMW_TEST_ORACLE_SERVICE", "XEPDB1");
            require(!env("DBMW_TEST_ORACLE_PASSWORD").empty(),
                    "DBMW_TEST_ORACLE_PASSWORD must be set for the integration test");

            std::ostringstream ds;
            ds << "{\"name\":\"ora\",\"type\":\"oracle\","
               << "\"host\":\"" << jsonEscape(host) << "\",\"port\":" << port << ','
               << "\"user\":\"" << jsonEscape(user) << "\","
               << "\"password_env\":\"DBMW_TEST_ORACLE_PASSWORD\","
               << "\"extra\":{\"service_name\":\"" << jsonEscape(service) << "\"},"
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

            requireOk(dbmw::DBMW::init(configPath), "DBMW::init");
            initialized = true;

            std::int64_t affected = 0;
            requireOk(dbmw::DBMW::execute(
                          "CREATE TABLE " + table + " ("
                          "\"id\" NUMBER GENERATED BY DEFAULT AS IDENTITY PRIMARY KEY, "
                          "\"name\" VARCHAR2(200) NOT NULL UNIQUE, "
                          "\"qty\" NUMBER(19) NOT NULL, "
                          "\"price\" BINARY_DOUBLE NOT NULL, "
                          "\"payload\" RAW(64), "
                          "\"doc\" CLOB, "
                          "\"amount\" NUMBER(30,9), "
                          "\"created_at\" TIMESTAMP NOT NULL)", affected), "create table");
        }
    };

    void testConnectivityAndTypes(Fixture &f) {
        ResultSet identity;
        requireOk(dbmw::DBMW::query("SELECT USER AS CURRENT_USER, 1 AS ONE FROM DUAL", identity),
                  "SELECT ... FROM DUAL");
        require(identity.rowCount() == 1, "DUAL query returned wrong row count");
        require(!asString(identity.rows()[0].at("CURRENT_USER")).empty(), "USER is empty");

        const dbmw::common::Timestamp created = std::chrono::system_clock::now();
        const dbmw::common::Blob payload{0x00, 0x01, 0x7f, 0x80, 0xff};
        const dbmw::common::Decimal amount{"12345.678901234"};
        std::int64_t affected = 0;
        requireOk(dbmw::DBMW::execute(
                      "INSERT INTO " + f.table +
                      " (\"name\", \"qty\", \"price\", \"payload\", \"doc\", \"amount\", "
                      "\"created_at\") VALUES (?, ?, ?, ?, ?, ?, ?)",
                      Params{std::string("raw-types"), std::int64_t(7), 12.5, payload,
                             std::string("clob body"), amount, created},
                      affected), "insert typed row");
        require(affected == 1, "typed insert affected mismatch");

        ResultSet ids;
        requireOk(dbmw::DBMW::query("SELECT \"id\" FROM " + f.table + " WHERE \"name\" = ?",
                                    Params{std::string("raw-types")}, ids), "read generated id");
        require(ids.rowCount() == 1, "typed row count mismatch");

        ResultSet rows;
        requireOk(dbmw::DBMW::query(
                      "SELECT \"id\", \"qty\", \"price\", \"payload\", \"doc\", \"amount\", "
                      "\"created_at\" FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("raw-types")}, rows), "read raw row back");
        require(rows.rowCount() == 1, "raw row count mismatch");
        const auto &row = rows.rows()[0];
        require(asInt(row.at("qty")) == 7, "NUMBER(19) round trip");
        const auto *price = std::get_if<double>(&row.at("price"));
        require(price != nullptr && *price == 12.5, "BINARY_DOUBLE round trip");
        const auto *blob = std::get_if<dbmw::common::Blob>(&row.at("payload"));
        require(blob != nullptr && *blob == payload, "RAW round trip to Blob");
        require(asString(row.at("doc")) == "clob body", "CLOB round trip to string");
        const auto *dec = std::get_if<dbmw::common::Decimal>(&row.at("amount"));
        require(dec != nullptr && dec->value == amount.value, "NUMBER(30,9) round trip to Decimal");
        const auto *ts = std::get_if<dbmw::common::Timestamp>(&row.at("created_at"));
        require(ts != nullptr, "TIMESTAMP round trip to Timestamp");
        if (ts)
            require(dbmw::common::timestampToString(*ts) == dbmw::common::timestampToString(created),
                    "TIMESTAMP value preserved");
    }

    void testMappingRoundTrip(Fixture &f) {
        OraItem item;
        item.name = "mapping-row";
        item.qty = 3;
        item.price = 9.25;
        item.payload = dbmw::common::Blob{0x0a, 0x0b};
        item.doc = "doc body";
        item.amount = dbmw::common::Decimal{"42.5"};
        item.createdAt = std::chrono::system_clock::now();

        const auto inserted = dbmw::insertAs<OraItem>(f.table, item);
        require(inserted.status.ok(), "insertAs failed: " + inserted.status.message);
        require(inserted.affected == 1, "insertAs affected mismatch");
        require(item.id > 0, "insertAs did not backfill the generated id");

        const auto fetched = dbmw::queryAs<OraItem>(
            "SELECT \"id\", \"name\", \"qty\", \"price\", \"payload\", \"doc\", \"amount\", "
            "\"created_at\" FROM " + f.table + " WHERE \"id\" = ?", Params{item.id});
        require(fetched.status.ok(), "queryAs failed: " + fetched.status.message);
        require(fetched.items.size() == 1, "queryAs row count mismatch");
        require(fetched.items[0].name == "mapping-row", "queryAs name mismatch");
        require(fetched.items[0].qty == 3, "queryAs qty mismatch");
        require(fetched.items[0].payload == item.payload, "queryAs payload mismatch");
        require(fetched.items[0].doc == "doc body", "queryAs doc mismatch");

        OraItem toUpdate = fetched.items[0];
        toUpdate.qty = 11;
        const auto updated = dbmw::updateAs<OraItem>(f.table, toUpdate);
        require(updated.status.ok() && updated.affected == 1, "updateAs failed");

        std::vector<OraItem> batch;
        for (int i = 0; i < 3; ++i) {
            OraItem b;
            b.name = "batch-" + std::to_string(i);
            b.qty = i;
            b.price = 1.5;
            b.amount = dbmw::common::Decimal{"1"};
            b.createdAt = std::chrono::system_clock::now();
            batch.push_back(b);
        }
        const auto batched = dbmw::insertBatchAs<OraItem>(f.table, batch);
        require(batched.status.ok(), "insertBatchAs failed: " + batched.status.message);
        require(batched.batch.totalAffected() == 3, "insertBatchAs affected mismatch");
        for (const OraItem &b: batch)
            require(b.id > 0, "insertBatchAs did not backfill a batch row id");
    }

    void testTransactionsAndSavepoints(Fixture &f) {
        requireOk(dbmw::DBMW::transaction([&](dbmw::core::Session &session) {
            std::int64_t affected = 0;
            Status st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{std::string("tx-keep"), std::int64_t(1), 1.0,
                       std::chrono::system_clock::now()}, affected);
            if (!st.ok()) return st;
            st = session.savepoint("before_temp");
            if (!st.ok()) return st;
            st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{std::string("tx-temp"), std::int64_t(1), 1.0,
                       std::chrono::system_clock::now()}, affected);
            if (!st.ok()) return st;
            return session.rollbackToSavepoint("before_temp");
        }), "transaction commit/savepoint");

        ResultSet rows;
        requireOk(dbmw::DBMW::query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-keep")}, rows), "count kept row");
        require(asInt(rows.rows()[0].at("N")) == 1, "committed row is missing");
        requireOk(dbmw::DBMW::query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-temp")}, rows), "count rolled back row");
        require(asInt(rows.rows()[0].at("N")) == 0, "savepoint rollback did not undo the row");

        const Status rolledBack = dbmw::DBMW::transaction([&](dbmw::core::Session &session) {
            std::int64_t affected = 0;
            const Status st = session.execute(
                "INSERT INTO " + f.table +
                " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                Params{std::string("tx-abort"), std::int64_t(1), 1.0,
                       std::chrono::system_clock::now()}, affected);
            if (!st.ok()) return st;
            return Status::error(ErrorCode::QueryError, "deliberate rollback");
        });
        require(!rolledBack.ok(), "deliberate failure did not abort the transaction");
        requireOk(dbmw::DBMW::query(
                      "SELECT count(*) AS N FROM " + f.table + " WHERE \"name\" = ?",
                      Params{std::string("tx-abort")}, rows), "count aborted row");
        require(asInt(rows.rows()[0].at("N")) == 0, "aborted transaction left a row behind");
    }

    void testStreamingAndErrors(Fixture &f) {
        std::int64_t affected = 0;
        for (int i = 0; i < 5; ++i) {
            requireOk(dbmw::DBMW::execute(
                          "INSERT INTO " + f.table +
                          " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
                          Params{std::string("stream-" + std::to_string(i)), std::int64_t(i), 1.0,
                                 std::chrono::system_clock::now()}, affected), "seed stream row");
        }
        std::uint64_t streamed = 0;
        requireOk(dbmw::DBMW::queryEach(
                      "SELECT \"name\" FROM " + f.table + " WHERE \"name\" LIKE 'stream-%'",
                      Params{}, [](const dbmw::common::Row &) { return true; }, streamed),
                  "queryEach");
        require(streamed == 5, "queryEach streamed the wrong number of rows");

        const Status duplicate = dbmw::DBMW::execute(
            "INSERT INTO " + f.table +
            " (\"name\", \"qty\", \"price\", \"created_at\") VALUES (?, ?, ?, ?)",
            Params{std::string("raw-types"), std::int64_t(1), 1.0,
                   std::chrono::system_clock::now()}, affected);
        require(!duplicate.ok(), "duplicate insert unexpectedly succeeded");
        require(duplicate.code == ErrorCode::ConstraintViolation,
                "ORA-00001 should map to ConstraintViolation, got " +
                std::string(dbmw::common::errorCodeToString(duplicate.code)));
        require(duplicate.nativeCode == 1, "ORA-00001 should be recorded as nativeCode=1");
        std::cout << "  unique violation sqlstate=" << duplicate.sqlState << "\n";
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
        std::cout << "Oracle integration test passed (" << gChecks << " checks)\n";
        return 0;
    } catch (const std::exception &e) {
        std::cout << "Oracle integration test failed after " << gChecks << " checks: "
            << e.what() << "\n";
        return 1;
    }
}
