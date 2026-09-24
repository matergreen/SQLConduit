#include "sqlconduit/config/config_loader.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
    int failures = 0;

    void check(const bool condition, const std::string &message) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << message << '\n';
        if (!condition) ++failures;
    }

    nlohmann::json readJson(const std::filesystem::path &path) {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("cannot open " + path.string());
        return nlohmann::json::parse(input);
    }
}

int main() {
    const auto root = std::filesystem::path(SQLCONDUIT_SOURCE_DIR);
    const auto schema = readJson(root / "config/sqlconduit.schema.json");

    check(schema.value("$schema", "") == "https://json-schema.org/draft/2020-12/schema",
          "schema declares JSON Schema 2020-12");
    check(schema.value("type", "") == "object" &&
          schema.value("additionalProperties", true) == false,
          "root configuration rejects unknown fields");
    check(schema.contains("$defs") && schema["$defs"].contains("datasource") &&
          schema["$defs"].contains("group") && schema["$defs"].contains("observability"),
          "schema publishes datasource, group, and observability contracts");
    check(schema["properties"].contains("datasources") &&
          schema["properties"].contains("prepared_cache") &&
          schema["properties"].contains("async"),
          "schema covers required and current top-level configuration blocks");

    const auto example = readJson(root / "config/datasources.json.example");
    check(example.value("$schema", "") == "./sqlconduit.schema.json",
          "JSON example advertises the local schema to editors");

    std::ifstream yamlInput(root / "config/datasource.yaml.example");
    std::string yamlSchemaDirective;
    std::getline(yamlInput, yamlSchemaDirective);
    check(yamlSchemaDirective ==
          "# yaml-language-server: $schema=./sqlconduit.schema.json",
          "YAML example advertises the shared schema to language servers");

    sqlconduit::config::GlobalConfig parsed;
    std::string error;
    check(sqlconduit::config::ConfigLoader::loadFromFile(
              (root / "config/datasources.json.example").string(), parsed, error),
          "JSON example satisfies runtime configuration validation: " + error);

    error.clear();
    check(sqlconduit::config::ConfigLoader::loadFromFile(
              (root / "config/datasource.yaml.example").string(), parsed, error),
          "YAML example satisfies the same runtime configuration contract: " + error);

    const auto invalidPath = std::filesystem::temp_directory_path() /
                             "sqlconduit_schema_contract_invalid.json";
    {
        std::ofstream output(invalidPath);
        output << R"({"pool":{"min":0,"max":1,"borrow_timout_ms":10},"datasources":[{"name":"main","type":"mock"}]})";
    }
    error.clear();
    check(!sqlconduit::config::ConfigLoader::loadFromFile(
              invalidPath.string(), parsed, error) &&
          error.find("[unknown_field]") != std::string::npos &&
          error.find("/pool/borrow_timout_ms") != std::string::npos,
          "runtime errors identify unknown fields with a JSON Pointer path");

    {
        std::ofstream output(invalidPath);
        output << R"({"pool":{"min":3,"max":2},"datasources":[{"name":"main","type":"mock"}]})";
    }
    error.clear();
    check(!sqlconduit::config::ConfigLoader::loadFromFile(
              invalidPath.string(), parsed, error) &&
          error.find("[invalid_value]") != std::string::npos &&
          error.find("/pool") != std::string::npos,
          "invalid ranges fail instead of being silently normalized");
    std::filesystem::remove(invalidPath);

    return failures == 0 ? 0 : 1;
}
