#include "dbmw/dbmw.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
    void report(const char *tag, const dbmw::common::Status &st) {
        std::cout << (st.ok() ? "[OK]   " : "[NOTE] ")
                << tag << ": " << st.message
                << " (code=" << dbmw::common::errorCodeToString(st.code) << ")" << std::endl;
    }

    const char *opName(dbmw::common::OperationType t) {
        using T = dbmw::common::OperationType;
        switch (t) {
            case T::Query: return "query";
            case T::Execute: return "execute";
            case T::Begin: return "begin";
            case T::Commit: return "commit";
            case T::Rollback: return "rollback";
            case T::Cancel: return "cancel";
            case T::Stream: return "stream";
            case T::Batch: return "batch";
            case T::Savepoint: return "savepoint";
            case T::Select: return "select(cursor)";
            default: return "unknown";
        }
    }
}

int main(int argc, char **argv) {
    std::string configPath = (argc > 1) ? argv[1] : "config/datasources.json.example";

    auto st = dbmw::DBMW::init(configPath);
    if (!st.ok()) {
        std::cerr << "[FAIL] init: " << st.message << std::endl;
        return 1;
    }
    std::cout << "[OK]   init from " << configPath << std::endl;

    dbmw::DBMW::setObserver([](const dbmw::common::OperationEvent &ev) {
        if (!ev.status.ok() || ev.slow) {
            std::cout << "  [OBS] " << ev.dataSource << " " << opName(ev.type)
                    << " dur=" << ev.duration.count() << "us"
                    << " ok=" << ev.status.ok()
                    << (ev.slow ? " SLOW" : "") << std::endl;
        }
    });

    dbmw::common::ResultSet rs;
    report("query(default)", dbmw::DBMW::query("SELECT 1", rs));

    dbmw::common::ResultSet rsPg;
    report("query(pg)", dbmw::DBMW::query("pg", "SELECT 1", rsPg));

    dbmw::common::ResultSet rsGroup;
    report("query(group=app)", dbmw::DBMW::query("app", "SELECT 1", rsGroup));

    dbmw::common::Params params;
    params.emplace_back(std::string("O'Brien"));
    params.emplace_back(static_cast<std::int64_t>(42));
    dbmw::common::ResultSet prs;
    report("query(params)", dbmw::DBMW::query("SELECT id, name FROM users WHERE name = ? AND age > ?", params, prs));

    std::int64_t affected = 0;
    report("execute", dbmw::DBMW::execute("UPDATE accounts SET balance = balance - 100 WHERE id = ?",
               dbmw::common::Params{static_cast<std::int64_t>(1)}, affected));
    std::cout << "        affected=" << affected << std::endl;

    std::uint64_t streamed = 0;
    report("queryEach",
           dbmw::DBMW::queryEach(
               "SELECT id, name FROM users WHERE age > ?",
               dbmw::common::Params{static_cast<std::int64_t>(0)},
               [&streamed](const dbmw::common::Row &row) {
                   ++streamed;
                   std::cout << "        row#" << streamed << " id=" << dbmw::common::valueToString(row.at("id")) << std::endl;
                   return true;
               },
               streamed));

    dbmw::common::ParamBatch batch;
    batch.push_back(dbmw::common::Params{std::string("alice"), static_cast<std::int64_t>(20)});
    batch.push_back(dbmw::common::Params{std::string("bob"), static_cast<std::int64_t>(25)});
    dbmw::common::BatchResult bres;
    report("executeBatch",
           dbmw::DBMW::executeBatch(
               "INSERT INTO users(name, age) VALUES (?, ?)", batch, bres));
    std::cout << "        totalAffected=" << bres.totalAffected() << std::endl;

    {
        std::unique_ptr<dbmw::core::Cursor> cur;
        dbmw::core::CursorOptions opts;
        opts.batch_size = 100;
        opts.auto_transaction = true;
        auto co = dbmw::DBMW::openCursor(
            "SELECT id, name FROM users WHERE age > ?",
            dbmw::common::Params{static_cast<std::int64_t>(0)}, opts, cur);
        report("openCursor", co);
        if (co.ok() && cur) {
            std::uint64_t n = 0;
            dbmw::common::Row row;
            bool ok = false;
            while (true) {
                const auto fs = cur->fetchRow(row, ok);
                if (!ok) break;
                ++n;
                std::cout << "        cursor row#" << n << " name="
                        << dbmw::common::valueToString(row.at("name")) << std::endl;
            }
            std::cout << "        cursor rowsFetched=" << cur->rowsFetched()
                    << " isOpen=" << cur->isOpen() << std::endl;
            report("cursor.close", cur->close());
        }
    }

    report("transaction",
           dbmw::DBMW::transaction([](const dbmw::core::Session &s) {
               std::int64_t n = 0;
               if (auto r = s.execute(
                   "UPDATE accounts SET balance = balance - 100 WHERE id = ?",
                   dbmw::common::Params{static_cast<std::int64_t>(1)}, n); !r.ok())
                   return r;
               return s.execute(
                   "UPDATE accounts SET balance = balance + 100 WHERE id = ?",
                   dbmw::common::Params{static_cast<std::int64_t>(2)}, n);
           }));

    dbmw::common::TransactionOptions txOpts;
    txOpts.isolation = dbmw::common::IsolationLevel::Serializable;
    txOpts.readOnly = false;
    txOpts.timeout = std::chrono::milliseconds(5000);
    report("transaction(opts)",
           dbmw::DBMW::transaction(txOpts, [](const dbmw::core::Session &s) {
               std::int64_t n = 0;
               return s.execute("UPDATE accounts SET balance = balance + 1 WHERE id = ?",
                                dbmw::common::Params{static_cast<std::int64_t>(1)}, n);
           }));

    report("transaction(group=app)",
           dbmw::DBMW::transaction("app", [](const dbmw::core::Session &s) {
               std::int64_t n = 0;
               return s.execute("INSERT INTO t(k, v) VALUES (?, ?)",
                                dbmw::common::Params{std::string("k"), static_cast<std::int64_t>(1)}, n);
           }));

    report("withSession",
           dbmw::DBMW::withSession("pg", [](dbmw::core::Session &s) {
               if (auto r = s.begin(); !r.ok()) return r;
               std::int64_t n = 0;
               if (auto r = s.execute("INSERT INTO t(k,v) VALUES (?,?)",
                                      dbmw::common::Params{std::string("a"), static_cast<std::int64_t>(1)}, n);
                   !r.ok())
                   return r;
               if (auto r = s.savepoint("sp1"); !r.ok()) return r;
               if (auto r = s.execute("INSERT INTO t(k,v) VALUES (?,?)",
                                      dbmw::common::Params{std::string("b"), static_cast<std::int64_t>(2)}, n);
                   !r.ok())
                   return r;
               if (auto r = s.rollbackToSavepoint("sp1"); !r.ok()) return r;
               std::cout << "        inTransaction=" << s.inTransaction()
                       << " didWrite=" << s.didWrite() << std::endl;
               return s.commit();
           }));

    if (auto ds = dbmw::DBMW::dataSource("main")) {
        std::cout << "[OK]   dataSource(name=" << ds->name() << ")" << std::endl;
        dbmw::core::ConnectionPool::Stats s;
        if (ds->poolStats(s)) {
            std::cout << "        pool: total=" << s.total << " idle=" << s.idle
                    << " borrowed=" << s.borrowed << " waiting=" << s.waiting
                    << " util=" << s.utilization() << std::endl;
        }
    } else {
        std::cout << "[NOTE] dataSource(\"main\") not found" << std::endl;
    }

    dbmw::core::ConnectionPool::Stats def;
    if (dbmw::DBMW::poolStats(def)) {
        std::cout << "[OK]   default poolStats: total=" << def.total
                << " borrowed=" << def.borrowed << std::endl;
    }
    for (const auto &np: dbmw::DBMW::allPoolStats()) {
        std::cout << "        allPoolStats: " << np.dataSource
                << " total=" << np.stats.total
                << " borrowed=" << np.stats.borrowed << std::endl;
    }

    for (const auto &s: dbmw::DBMW::slowSqlStats(5)) {
        std::cout << "        slowSql[" << s.dataSource << "] "
                << s.sqlTemplate.substr(0, 60) << " count=" << s.count
                << " max=" << s.maxDuration.count() << "us" << std::endl;
    }
    for (const auto &r: dbmw::DBMW::recentSlowSql(5)) {
        std::cout << "        recentSlow[" << r.dataSource << "] dur="
                << r.duration.count() << "us code="
                << dbmw::common::errorCodeToString(r.errorCode) << std::endl;
    }
    dbmw::DBMW::clearSlowSqlStats();

    dbmw::common::Value v = std::string("hello");
    std::cout << "[OK]   valueToString(\"hello\") = "
            << dbmw::common::valueToString(v) << std::endl;
    dbmw::common::Timestamp ts;
    if (dbmw::common::tryParseTimestamp("2026-09-02 16:30:00", ts)) {
        std::cout << "[OK]   timestampToString = "
                << dbmw::common::timestampToString(ts) << std::endl;
    }

    std::cout << "[OK]   quoteIdentifier(\"order\") = "
            << dbmw::common::quoteIdentifier("order") << std::endl;
    std::cout << "[OK]   escapeLiteralGeneric(\"a'b\") = "
            << dbmw::common::escapeLiteralGeneric(std::string("a'b")) << std::endl;

    report("reload", dbmw::DBMW::reload(configPath));

    dbmw::DBMW::shutdown();
    std::cout << "[OK]   shutdown" << std::endl;
    return 0;
}
