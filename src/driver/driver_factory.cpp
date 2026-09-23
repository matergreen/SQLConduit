#include "sqlconduit/driver/driver_factory.h"
#include "sqlconduit/driver/driver_registry.h"
#include "sqlconduit/driver/mysql_driver.h"
#include "sqlconduit/driver/postgres_driver.h"
#include "sqlconduit/driver/odbc_driver.h"
#include "sqlconduit/driver/oracle_driver.h"

namespace sqlconduit::driver {
    std::unique_ptr<IDriver> createDriver(const std::string &type) {
        return DriverRegistry::instance().create(type);
    }

    void registerBuiltinDrivers() {
        registerMySQLDriver();
        registerPostgresDriver();
        registerOdbcDriver();
        registerOracleDriver();
    }
}
