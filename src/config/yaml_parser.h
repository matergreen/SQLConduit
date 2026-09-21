#ifndef SQLCONDUIT_CONFIG_YAML_PARSER_H
#define SQLCONDUIT_CONFIG_YAML_PARSER_H

#include <nlohmann/json.hpp>

#include <string>

namespace sqlconduit::config::detail
{
    bool parseYaml(const std::string& text, nlohmann::json& out, std::string& error);
}

#endif
