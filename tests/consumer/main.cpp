#include "sqlconduit/driver/driver_factory.h"
#include "sqlconduit/driver/driver_registry.h"

#include <cstdio>

int main()
{
    sqlconduit::driver::registerBuiltinDrivers();
    auto& registry = sqlconduit::driver::DriverRegistry::instance();
    const auto types = registry.registeredTypes();

    std::printf("consumer smoke: %zu driver(s) registered:", types.size());
    for (const auto& type : types) std::printf(" %s", type.c_str());
    std::printf("\n");

    if (types.empty())
    {
        std::printf("consumer smoke FAILED: no driver registered\n");
        return 1;
    }

    for (const auto& type : types)
    {
        auto driver = registry.create(type);
        if (!driver || !driver->createConnection())
        {
            std::printf("consumer smoke FAILED: cannot instantiate driver '%s'\n", type.c_str());
            return 1;
        }
    }

    std::printf("consumer smoke OK\n");
    return 0;
}
