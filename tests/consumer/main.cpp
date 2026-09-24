#include "sqlconduit/client.h"
#include "sqlconduit/version.h"

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

int main() {
    static_assert(SQLCONDUIT_VERSION_MAJOR == 0);
    static_assert(SQLCONDUIT_VERSION_MINOR == 8);

    sqlconduit::Client client;
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
#else
    std::printf("consumer smoke: core only\n");
#endif

    std::printf("consumer smoke OK\n");
    return 0;
}
