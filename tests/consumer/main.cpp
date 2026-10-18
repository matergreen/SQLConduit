#include "sqlconduit/client.h"
#include "sqlconduit/mapping.h"
#include "sqlconduit/sql_builder.h"
#include "sqlconduit/typed_sql_builder.h"
#include "sqlconduit/query_page.h"
#include "sqlconduit/version.h"
#ifdef SQLCONDUIT_CONSUMER_OPENTELEMETRY
#include "sqlconduit/exporters/opentelemetry.h"
#endif

#if defined(SQLCONDUIT_CONSUMER_MYSQL)
#include "sqlconduit/drivers/mysql.h"
#elif defined(SQLCONDUIT_CONSUMER_POSTGRES)
#include "sqlconduit/drivers/postgres.h"
#elif defined(SQLCONDUIT_CONSUMER_ODBC)
#include "sqlconduit/drivers/odbc.h"
#elif defined(SQLCONDUIT_CONSUMER_ORACLE)
#include "sqlconduit/drivers/oracle.h"
#endif

#include <cstdio>
#include <string_view>

struct ConsumerEntity { int id = 0; std::string name; };
SQLCONDUIT_ENTITY(ConsumerEntity, "consumer_probe", id, name)

int main() {
    static_assert(SQLCONDUIT_VERSION_MAJOR == 1);
    static_assert(SQLCONDUIT_VERSION_MINOR == 2);
    static_assert(std::string_view(SQLCONDUIT_VERSION_STRING) == "1.2.0");

    const auto statement = sqlconduit::sql::Builder::select("consumer_probe")
        .columns({"id"})
        .where(sqlconduit::sql::eq("id", 1))
        .build();
    if (!statement.ok() || statement.statement.params.size() != 1) {
        std::printf("consumer smoke FAILED: SQL Builder package contract mismatch\n");
        return 1;
    }

    sqlconduit::Client client;
    const auto pagePlan = sqlconduit::sql::select<ConsumerEntity>().orderByAsc(&ConsumerEntity::id).buildPage();
    if (!pagePlan.ok() || !pagePlan.countQuery ||
        client.queryPage<ConsumerEntity>(pagePlan).status.code != sqlconduit::common::ErrorCode::NotInitialized ||
        client.queryPageAsync<ConsumerEntity>(pagePlan).get().status.code != sqlconduit::common::ErrorCode::NotInitialized) {
        std::printf("consumer smoke FAILED: pagination package contract mismatch\n");
        return 1;
    }
    const auto typed = sqlconduit::sql::select<ConsumerEntity>().eq(&ConsumerEntity::id, 1).build();
    if (!typed.ok() || typed.statement.params.size() != 1 ||
        client.query<ConsumerEntity>(typed).status.code != sqlconduit::common::ErrorCode::NotInitialized) {
        std::printf("consumer smoke FAILED: typed Builder/entity registration package contract mismatch\n");
        return 1;
    }
    sqlconduit::common::OperationOptions options;
    options.timeout = std::chrono::milliseconds(100);
    if (client.withOptions(options).queryColumnNamed<int>("SELECT :value", {{"value", 1}}).status.code !=
            sqlconduit::common::ErrorCode::NotInitialized ||
        client.withOptions(options).execute(statement).status.code != sqlconduit::common::ErrorCode::NotInitialized) {
        std::printf("consumer smoke FAILED: operation/builder/column package contract mismatch\n");
        return 1;
    }
    if (client.queryScalar<int>("SELECT 1").status.code != sqlconduit::common::ErrorCode::NotInitialized ||
        client.queryScalarNamedAsync<int>("SELECT :value", {{"value", 1}}).get().status.code !=
            sqlconduit::common::ErrorCode::NotInitialized ||
        !sqlconduit::common::bindNamed("SELECT :value", {{"value", 1}}).ok()) {
        std::printf("consumer smoke FAILED: direct/named query package contract mismatch\n");
        return 1;
    }
    if (client.diagnostics().version != SQLCONDUIT_VERSION_STRING ||
        client.capabilities().status.code != sqlconduit::common::ErrorCode::NotInitialized) {
        std::printf("consumer smoke FAILED: diagnostics package contract mismatch\n");
        return 1;
    }
#ifdef SQLCONDUIT_CONSUMER_OPENTELEMETRY
    client.setObserver(sqlconduit::exporters::openTelemetryObserver());
    client.setExecutionPhaseObserver(sqlconduit::exporters::openTelemetryPhaseObserver());
    (void) sqlconduit::exporters::openTelemetryContext();
#endif
    client.setObserver([](const sqlconduit::common::OperationEvent &) {
    });
    if (!client.slowSqlStats().empty() || !client.recentSlowSql().empty()) {
        std::printf("consumer smoke FAILED: new Client must have empty observability state\n");
        return 1;
    }
    client.clearSlowSqlStats();
    if (client.queryAsync("SELECT 1").get().status.code !=
        sqlconduit::common::ErrorCode::NotInitialized) {
        std::printf("consumer smoke FAILED: uninitialized async contract mismatch\n");
        return 1;
    }
    if (client.isRunning()) {
        std::printf("consumer smoke FAILED: default Client must be stopped\n");
        return 1;
    }

#if defined(SQLCONDUIT_CONSUMER_MYSQL)
    const auto registration = sqlconduit::drivers::mysql();
#elif defined(SQLCONDUIT_CONSUMER_POSTGRES)
    const auto registration = sqlconduit::drivers::postgres();
#elif defined(SQLCONDUIT_CONSUMER_ODBC)
    const auto registration = sqlconduit::drivers::odbc();
#elif defined(SQLCONDUIT_CONSUMER_ORACLE)
    const auto registration = sqlconduit::drivers::oracle();
#endif

#if defined(SQLCONDUIT_CONSUMER_MYSQL) || defined(SQLCONDUIT_CONSUMER_POSTGRES) || \
    defined(SQLCONDUIT_CONSUMER_ODBC) || defined(SQLCONDUIT_CONSUMER_ORACLE)
    if (!client.addDriver(registration).ok()) {
        std::printf("consumer smoke FAILED: cannot register driver '%s'\n",
                    registration.type.c_str());
        return 1;
    }
    auto driver = registration.factory();
    if (!driver || !driver->createConnection()) {
        std::printf("consumer smoke FAILED: cannot instantiate driver '%s'\n",
                    registration.type.c_str());
        return 1;
    }
    std::printf("consumer smoke: registered %s\n", registration.type.c_str());
#elif defined(SQLCONDUIT_CONSUMER_OPENTELEMETRY)
    std::printf("consumer smoke: OpenTelemetry adapter\n");
#else
    std::printf("consumer smoke: core only\n");
#endif

    std::printf("consumer smoke OK\n");
    return 0;
}
