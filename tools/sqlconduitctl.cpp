#include "sqlconduit/config/config_loader.h"
#include "sqlconduit/core/idatabase_connection.h"
#include "sqlconduit/driver/idriver.h"
#include "sqlconduit/version.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <string>
#ifdef SQLCONDUIT_CTL_MYSQL
#include "sqlconduit/drivers/mysql.h"
#endif
#ifdef SQLCONDUIT_CTL_POSTGRES
#include "sqlconduit/drivers/postgres.h"
#endif
#ifdef SQLCONDUIT_CTL_ODBC
#include "sqlconduit/drivers/odbc.h"
#endif
#ifdef SQLCONDUIT_CTL_ORACLE
#include "sqlconduit/drivers/oracle.h"
#endif

namespace {
    using json = nlohmann::json;
    std::unique_ptr<sqlconduit::driver::IDriver> driver(const std::string &type) {
#ifdef SQLCONDUIT_CTL_MYSQL
        if (type == "mysql") return sqlconduit::drivers::mysql().factory();
#endif
#ifdef SQLCONDUIT_CTL_POSTGRES
        if (type == "postgres") return sqlconduit::drivers::postgres().factory();
#endif
#ifdef SQLCONDUIT_CTL_ODBC
        if (type == "odbc") return sqlconduit::drivers::odbc().factory();
#endif
#ifdef SQLCONDUIT_CTL_ORACLE
        if (type == "oracle") return sqlconduit::drivers::oracle().factory();
#endif
        (void) type;
        return {};
    }

    int run(int argc, char **argv) {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: sqlconduitctl validate|explain|doctor CONFIG [--connect]\n"
                         "doctor is offline unless --connect is supplied. Output is JSON.\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << sqlconduit::versionString << '\n';
            return 0;
        }
        if (argc < 3 || argc > 4) {
            std::cerr << "Usage: sqlconduitctl validate|explain|doctor CONFIG [--connect]\n"
                         "doctor is offline unless --connect is supplied. Output is JSON.\n";
            return 2;
        }
        const std::string command = argv[1];
        const bool connect = argc == 4 && std::string(argv[3]) == "--connect";
        if ((command != "validate" && command != "explain" && command != "doctor") ||
            (argc == 4 && (!connect || command != "doctor"))) return 2;
        sqlconduit::config::GlobalConfig cfg;
        std::string error;
        if (!sqlconduit::config::ConfigLoader::loadFromFile(argv[2], cfg, error)) {
            // Parser exception messages can include the offending secret. Never
            // echo raw configuration or arbitrary vendor text into diagnostics.
            std::cout << json{{"ok", false}, {"error", "invalid_config"},
                {"advice", "Check file access, JSON/YAML syntax, unknown keys, value ranges and required password_env variables against sqlconduit.schema.json"}}.dump(2) << '\n';
            return 1;
        }
        if (!sqlconduit::config::ConfigLoader::validateTopology(cfg, error)) {
            std::cout << json{{"ok", false}, {"error", "invalid_topology"},
                {"advice", error}}.dump(2) << '\n';
            return 1;
        }
        if (command == "validate") {
            std::cout << json{{"ok", true}, {"version", sqlconduit::versionString},
                {"datasources", cfg.datasources.size()}, {"groups", cfg.groups.size()}}.dump(2) << '\n';
            return 0;
        }
        if (command == "explain") {
            std::cout << sqlconduit::config::ConfigLoader::explain(cfg) << '\n';
            return 0;
        }
        json report{{"ok", true}, {"connected", connect}, {"version", sqlconduit::versionString},
                    {"sources", json::array()}, {"configuration", json::parse(sqlconduit::config::ConfigLoader::explain(cfg))}};
        bool ok = true;
        for (const auto &d : cfg.datasources) {
            json item{{"name", d.name}, {"type", d.type}};
            auto drv = driver(d.type);
            if (!drv) {
                ok = false;
                item["available"] = false;
                const std::string flag = d.type == "postgres" ? "POSTGRES" : d.type == "mysql" ? "MYSQL" :
                                         d.type == "odbc" ? "ODBC" : d.type == "oracle" ? "ORACLE" : "";
                item["advice"] = flag.empty()
                    ? "Custom drivers require an application-specific tool that registers their factories"
                    : "Rebuild sqlconduitctl with SQLCONDUIT_ENABLE_" + flag + "=ON and install the matching client SDK";
            } else {
                item["available"] = true;
                auto conn = drv->createConnection();
                if (!conn) { item["error"] = "driver returned no connection"; ok = false; }
                else {
                    if (connect) {
                        auto st = conn->connect(d);
                        if (st.ok()) st = conn->ping();
                        item["connectionOk"] = st.ok();
                        item["errorCode"] = static_cast<int>(st.code);
                        if (!st.ok()) {
                            ok = false;
                            item["advice"] = "Check host/port, credentials, TLS, database permissions and installed client/ODBC driver; vendor messages are omitted to protect credentials";
                        }
                    }
                    const auto cap = conn->capabilities();
                    item["observed"] = cap.observed;
                    item["clientVersion"] = cap.clientVersion;
                    item["serverVersion"] = cap.serverVersion;
                    item["nativeAsync"] = cap.nativeAsync;
                    item["parameters"] = cap.parameters;
                    item["prepared"] = cap.prepared;
                    item["multipleResults"] = cap.multipleResults;
                    item["asyncConstraints"] = cap.asyncConstraints;
                    conn->close();
                }
            }
            report["sources"].push_back(std::move(item));
        }
        report["ok"] = ok;
        std::cout << report.dump(2) << '\n';
        return ok ? 0 : 1;
    }
}

int main(int argc, char **argv) {
    try { return run(argc, argv); }
    catch (...) {
        std::cerr << "sqlconduitctl: diagnostic operation failed; vendor details withheld\n";
        return 1;
    }
}
