#ifndef SQLCONDUIT_CONFIG_CONFIG_LOADER_H
#define SQLCONDUIT_CONFIG_CONFIG_LOADER_H

#include "sqlconduit/config/datasource_config.h"
#include <string>

namespace sqlconduit::config {
    class ConfigLoader {
    public:
        static bool loadFromFile(const std::string &path, GlobalConfig &out, std::string &error);

        // Offline semantic validation; never creates connections or worker threads.
        static bool validateTopology(const GlobalConfig &config, std::string &error);

        // JSON with resolved defaults. Credentials, connection strings and vendor
        // extras are always redacted. Intended for diagnostics, not persistence.
        [[nodiscard]] static std::string explain(const GlobalConfig &config);
    };
}

#endif
