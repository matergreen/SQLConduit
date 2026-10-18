#ifndef SQLCONDUIT_TEST_SQL_BUILDER_INTEGRATION_H
#define SQLCONDUIT_TEST_SQL_BUILDER_INTEGRATION_H

#include "sqlconduit/client.h"
#include "sqlconduit/sql_builder.h"
#include "sqlconduit/query_page.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SqlBuilderPageRow { std::string name; std::int64_t qty = 0; };
SQLCONDUIT_MAP(SqlBuilderPageRow, SQLCONDUIT_FIELD(name) SQLCONDUIT_FIELD(qty))

inline sqlconduit::common::Status runSqlBuilderCrud(
    sqlconduit::Client &client, const std::string &table,
    const sqlconduit::common::util::Dialect dialect,
    std::vector<sqlconduit::sql::FieldValue> insertValues) {
    using sqlconduit::common::ErrorCode;
    using sqlconduit::common::ResultSet;
    using sqlconduit::common::Status;

    const auto fail = [](std::string message) {
        return Status::error(ErrorCode::QueryError,
                             "SQL Builder integration: " + std::move(message));
    };

    auto insert = sqlconduit::sql::Builder::insert(table, dialect)
        .values(std::move(insertValues))
        .build();
    if (!insert.ok()) return insert.status;
    std::int64_t affected = 0;
    if (auto status = client.execute(insert.statement.sql, insert.statement.params, affected);
        !status.ok())
        return status;
    if (affected != 1) return fail("INSERT affected row count is not one");

    const auto select = sqlconduit::sql::Builder::select(table, dialect)
        .columns({"name", "qty"})
        .where(sqlconduit::sql::eq("name", std::string("builder-row")))
        .build();
    if (!select.ok()) return select.status;
    ResultSet rows;
    if (auto status = client.query(select.statement.sql, select.statement.params, rows);
        !status.ok())
        return status;
    if (rows.rowCount() != 1 ||
        !std::holds_alternative<std::int64_t>(rows.rows()[0].at("qty")) ||
        std::get<std::int64_t>(rows.rows()[0].at("qty")) != 5)
        return fail("SELECT did not return the inserted row");

    const auto pageQuery = sqlconduit::sql::Builder::select(table, dialect)
        .columns({"name", "qty"})
        .where(sqlconduit::sql::eq("name", std::string("builder-row")))
        .orderBy("name").buildPage();
    sqlconduit::PageOptions pageOptions;
    pageOptions.pageSize = 1;
    pageOptions.includeTotal = true;
    auto page = client.queryPage<SqlBuilderPageRow>(pageQuery, pageOptions);
    if (!page.ok()) return page.status;
    if (page.items.size() != 1 || page.items[0].qty != 5 || page.total != 1 || page.hasNext)
        return fail("pagination result/count mismatch");
    auto asyncPage = client.queryPageAsync<SqlBuilderPageRow>(pageQuery, pageOptions).get();
    if (!asyncPage.ok()) return asyncPage.status;
    if (asyncPage.items.size() != 1 || asyncPage.total != 1 || asyncPage.hasNext)
        return fail("async pagination result/count mismatch");
    pageOptions.page = 2;
    if (auto status = client.withSession([&](sqlconduit::core::Session &session) {
        auto empty = session.queryPage<SqlBuilderPageRow>(pageQuery, pageOptions);
        if (!empty.ok()) return empty.status;
        return empty.items.empty() && empty.total == 1 && !empty.hasNext ? Status{} : fail("beyond-last-page mismatch");
    }); !status.ok()) return status;

    const auto update = sqlconduit::sql::Builder::update(table, dialect)
        .value("qty", std::int64_t{6})
        .where(sqlconduit::sql::eq("name", std::string("builder-row")))
        .build();
    if (!update.ok()) return update.status;
    if (auto status = client.execute(update.statement.sql, update.statement.params, affected);
        !status.ok())
        return status;
    if (affected != 1) return fail("UPDATE affected row count is not one");
    rows.clear();
    if (auto status = client.query(select.statement.sql, select.statement.params, rows);
        !status.ok())
        return status;
    if (rows.rowCount() != 1 ||
        !std::holds_alternative<std::int64_t>(rows.rows()[0].at("qty")) ||
        std::get<std::int64_t>(rows.rows()[0].at("qty")) != 6)
        return fail("UPDATE result was not observable through SELECT");

    const auto remove = sqlconduit::sql::Builder::deleteFrom(table, dialect)
        .where(sqlconduit::sql::eq("name", std::string("builder-row")))
        .build();
    if (!remove.ok()) return remove.status;
    if (auto status = client.execute(remove.statement.sql, remove.statement.params, affected);
        !status.ok())
        return status;
    if (affected != 1) return fail("DELETE affected row count is not one");
    return Status::OK();
}

#endif
