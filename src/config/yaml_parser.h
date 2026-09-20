#ifndef DBMW_CONFIG_YAML_PARSER_H
#define DBMW_CONFIG_YAML_PARSER_H

#include <nlohmann/json.hpp>

#include <string>

namespace dbmw::config::detail {
    bool parseYaml(const std::string &text, nlohmann::json &out, std::string &error);
}

#endif
