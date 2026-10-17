#include "sqlconduit/client.h"
#include "sqlconduit/config/config_loader.h"
#ifdef SQLCONDUIT_CTL_MYSQL
#include "sqlconduit/drivers/mysql.h"
#endif
#ifdef SQLCONDUIT_CTL_POSTGRES
#include "sqlconduit/drivers/postgres.h"
#endif
#ifdef SQLCONDUIT_CTL_ODBC
#include "sqlconduit/drivers/odbc.h"
#endif
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    sqlconduit::Client client;
#ifdef SQLCONDUIT_CTL_MYSQL
    if (!client.addDriver(sqlconduit::drivers::mysql()).ok()) return 1;
#endif
#ifdef SQLCONDUIT_CTL_POSTGRES
    if (!client.addDriver(sqlconduit::drivers::postgres()).ok()) return 1;
#endif
#ifdef SQLCONDUIT_CTL_ODBC
    if (!client.addDriver(sqlconduit::drivers::odbc()).ok()) return 1;
#endif
    const auto initialized = client.init(argv[1]);
    if (!initialized.ok()) return 1;
    std::cout << "{\"ready\":true}" << std::endl;
    std::string command;
    while (std::getline(std::cin, command) && command != "quit") {
        sqlconduit::common::Status status;
        if (command == "async") status = client.queryAsync("SELECT 1").get().status;
        else if (command == "query") {
            sqlconduit::common::ResultSet rows;
            status = client.query("SELECT 1", rows);
        } else return 2;
        sqlconduit::core::ConnectionPool::Stats pool;
        (void) client.poolStats(pool);
        std::cout << "{\"ok\":" << (status.ok() ? "true" : "false")
                  << ",\"code\":" << static_cast<int>(status.code)
                  << ",\"connectionBroken\":" << (status.connectionBroken ? "true" : "false")
                  << ",\"created\":" << pool.connectionsCreated
                  << ",\"invalidated\":" << pool.invalidatedConnections << "}" << std::endl;
    }
    client.shutdown();
}
