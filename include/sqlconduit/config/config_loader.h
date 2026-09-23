#ifndef SQLCONDUIT_CONFIG_CONFIG_LOADER_H
#define SQLCONDUIT_CONFIG_CONFIG_LOADER_H

#include "sqlconduit/config/datasource_config.h"
#include <string>

namespace sqlconduit::config {
    class ConfigLoader {
    public:
        static bool loadFromFile(const std::string &path, GlobalConfig &out, std::string &error);
    };
}

#endif
