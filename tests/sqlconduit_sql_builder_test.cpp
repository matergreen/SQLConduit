#include "sqlconduit/sql_builder.h"
#include "sql_builder_integration.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {
    int passed = 0;
    int failed = 0;

    void check(const bool condition, const std::string &message) {
        if (condition) {
            ++passed;
            std::cout << "[PASS] " << message << '\n';
        } else {
            ++failed;
            std::cerr << "[FAIL] " << message << '\n';
        }
    }
}

int main() {
    using namespace sqlconduit;

    std::cout << "== SQL Builder: SELECT and predicates ==\n";
    {
        const auto result = sql::Builder::select("app.users")
            .columns({"id", "name"})
            .where(sql::eq("status", std::string("active")))
            .where(sql::any({
                sql::ge("age", 18),
                sql::in("role", {std::string("admin"), std::string("owner")})
            }))
            .orderBy("id", sql::SortDirection::Descending)
            .build();
        check(result.ok(), "structured SELECT builds successfully");
        check(result.statement.sql ==
              "SELECT \"id\", \"name\" FROM \"app\".\"users\" WHERE \"status\" = ? AND "
              "(\"age\" >= ? OR \"role\" IN (?, ?)) ORDER BY \"id\" DESC",
              "SELECT SQL has deterministic quoting and grouping");
        check(result.statement.params == common::Params{
                  std::string("active"), std::int64_t{18},
                  std::string("admin"), std::string("owner")},
              "SELECT parameters follow expression order");

        const auto literals = sql::Builder::insert("flags")
            .value("attempts", 3)
            .value("label", "ready")
            .build();
        check(literals.ok() &&
              std::holds_alternative<std::int64_t>(literals.statement.params[0]) &&
              std::holds_alternative<std::string>(literals.statement.params[1]),
              "ordinary integers and string literals normalize to bindable values");
    }

    std::cout << "== SQL Builder: INSERT and identifier safety ==\n";
    {
        const auto result = sql::Builder::insert(
                "users", common::util::Dialect::MySQL)
            .value("name", std::string("O'Brien"))
            .value("enabled", true)
            .build();
        check(result.ok(), "INSERT builds successfully");
        check(result.statement.sql ==
              "INSERT INTO `users` (`name`, `enabled`) VALUES (?, ?)",
              "MySQL identifiers use backticks");
        check(result.statement.sql.find("O'Brien") == std::string::npos &&
              result.statement.params.size() == 2,
              "values never enter generated SQL");

        const auto quoted = sql::Builder::select("users\"; DROP TABLE audit;--")
            .columns({"id"})
            .build();
        check(quoted.ok() && quoted.statement.sql.find(
                  "\"users\"\"; DROP TABLE audit;--\"") != std::string::npos,
              "identifier content remains inside one quoted identifier");
    }

    std::cout << "== SQL Builder: UPDATE/DELETE safety ==\n";
    {
        const auto blockedUpdate = sql::Builder::update("users")
            .value("enabled", false)
            .build();
        check(blockedUpdate.status.code == common::ErrorCode::QueryError,
              "UPDATE without WHERE is rejected");

        const auto update = sql::Builder::update("users")
            .value("name", std::string("new"))
            .value("enabled", true)
            .where(sql::eq("id", std::int64_t{42}))
            .build();
        check(update.ok() && update.statement.sql ==
              "UPDATE \"users\" SET \"name\" = ?, \"enabled\" = ? WHERE \"id\" = ?",
              "UPDATE emits SET parameters before WHERE parameters");
        check(update.statement.params == common::Params{
                  std::string("new"), true, std::int64_t{42}},
              "UPDATE parameter order is deterministic");

        const auto blockedDelete = sql::Builder::deleteFrom("users").build();
        check(blockedDelete.status.code == common::ErrorCode::QueryError,
              "DELETE without WHERE is rejected");
        const auto deleteAll = sql::Builder::deleteFrom("users").allowAllRows().build();
        check(deleteAll.ok() && deleteAll.statement.sql == "DELETE FROM \"users\"",
              "allowAllRows explicitly permits full-table DELETE");
    }

    std::cout << "== SQL Builder: NULL, IN and invalid specifications ==\n";
    {
        const auto nulls = sql::Builder::select("users")
            .where(sql::eq("deleted_at", nullptr))
            .where(sql::ne("verified_at", nullptr))
            .build();
        check(nulls.ok() && nulls.statement.sql.find(
                  "\"deleted_at\" IS NULL AND \"verified_at\" IS NOT NULL") !=
                  std::string::npos && nulls.statement.params.empty(),
              "NULL equality uses IS NULL without parameters");

        const auto emptyIn = sql::Builder::select("users")
            .where(sql::in("id", {}))
            .build();
        check(emptyIn.ok() && emptyIn.statement.sql.find("WHERE 1 = 0") != std::string::npos,
              "empty IN is a portable false predicate");

        const auto advanced = sql::Builder::select("users")
            .where(sql::not_(sql::any({
                sql::between("age", 10, 20),
                sql::like("name", "bot%"),
                sql::notIn("state", {std::string("disabled"), std::string("deleted")}),
                sql::isNull("owner_id")
            })))
            .build();
        check(advanced.ok() &&
              advanced.statement.sql.find("NOT (\"age\" BETWEEN ? AND ? OR") !=
                  std::string::npos &&
              advanced.statement.params.size() == 5,
              "BETWEEN, LIKE, NOT IN, null checks, and NOT compose structurally");

        const auto duplicate = sql::Builder::insert("users")
            .value("name", std::string("a"))
            .value("name", std::string("b"))
            .build();
        check(duplicate.status.code == common::ErrorCode::QueryError,
              "duplicate value fields are rejected");

        const auto parallel = sql::Builder::insert("users")
            .values({"name", "age"}, {std::string("alice"), std::int64_t{20}})
            .build();
        check(parallel.ok() && parallel.statement.params.size() == 2,
              "parallel field and parameter arrays are supported");

        const auto mismatched = sql::Builder::insert("users")
            .values({"name", "age"}, {std::string("alice")})
            .build();
        check(mismatched.status.code == common::ErrorCode::QueryError,
              "field and parameter count mismatch is rejected");

        const auto wrongOperation = sql::Builder::insert("users")
            .where(sql::eq("id", std::int64_t{1}))
            .value("name", std::string("a"))
            .build();
        check(wrongOperation.status.code == common::ErrorCode::QueryError,
              "operation-incompatible clauses are rejected");
    }

    std::cout << "\npassed " << passed << ", failed " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
