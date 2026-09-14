#ifndef DBMW_CONFIG_CONFIG_LOADER_H
#define DBMW_CONFIG_CONFIG_LOADER_H

#include "dbmw/config/datasource_config.h"
#include <string>

namespace dbmw::config {
    class ConfigLoader {
    public:
        static bool loadFromFile(const std::string &path, GlobalConfig &out, std::string &error);
    };
}

#endif
