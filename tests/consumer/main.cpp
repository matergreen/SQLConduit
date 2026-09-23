#include "sqlconduit/client.h"
#include "sqlconduit/driver/driver_factory.h"
#include "sqlconduit/driver/driver_registry.h"
#include "sqlconduit/version.h"

#include <cstdio>

int main() {
    static_assert(SQLCONDUIT_VERSION_MAJOR == 0);
    static_assert(SQLCONDUIT_VERSION_MINOR == 7);

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

    sqlconduit::driver::registerBuiltinDrivers();
    auto &registry = sqlconduit::driver::DriverRegistry::instance();
    const auto types = registry.registeredTypes();

    std::printf("consumer smoke: %zu driver(s) registered:", types.size());
    for (const auto &type: types) std::printf(" %s", type.c_str());
    std::printf("\n");

#if defined(SQLCONDUIT_CONSUMER_REQUIRE_DRIVERS)
    if (types.empty()) {
        std::printf("consumer smoke FAILED: no driver registered\n");
        return 1;
    }
#endif

    for (const auto &type: types) {
        auto driver = registry.create(type);
        if (!driver || !driver->createConnection()) {
            std::printf("consumer smoke FAILED: cannot instantiate driver '%s'\n", type.c_str());
            return 1;
        }
    }

    std::printf("consumer smoke OK\n");
    return 0;
}
