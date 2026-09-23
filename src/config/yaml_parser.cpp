#include "yaml_parser.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sqlconduit::config::detail {
    namespace {
        using json = nlohmann::json;

        struct Line {
            std::size_t number = 0;
            std::size_t indent = 0;
            std::string text;
        };

        std::string trim(const std::string &value) {
            std::size_t first = 0;
            while (first < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[first])))
                ++first;
            std::size_t last = value.size();
            while (last > first &&
                   std::isspace(static_cast<unsigned char>(value[last - 1])))
                --last;
            return value.substr(first, last - first);
        }

        std::string lower(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::string withoutComment(const std::string &value) {
            bool single = false;
            bool doubleQuoted = false;
            bool escaped = false;
            for (std::size_t i = 0; i < value.size(); ++i) {
                const char c = value[i];
                if (doubleQuoted && escaped) {
                    escaped = false;
                    continue;
                }
                if (doubleQuoted && c == '\\') {
                    escaped = true;
                    continue;
                }
                if (!doubleQuoted && c == '\'') {
                    if (single && i + 1 < value.size() && value[i + 1] == '\'') {
                        ++i;
                        continue;
                    }
                    single = !single;
                    continue;
                }
                if (!single && c == '"') {
                    doubleQuoted = !doubleQuoted;
                    continue;
                }
                if (!single && !doubleQuoted && c == '#' &&
                    (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])))) {
                    return value.substr(0, i);
                }
            }
            return value;
        }

        std::size_t mappingColon(const std::string &value) {
            bool single = false;
            bool doubleQuoted = false;
            bool escaped = false;
            int flowDepth = 0;
            for (std::size_t i = 0; i < value.size(); ++i) {
                const char c = value[i];
                if (doubleQuoted && escaped) {
                    escaped = false;
                    continue;
                }
                if (doubleQuoted && c == '\\') {
                    escaped = true;
                    continue;
                }
                if (!doubleQuoted && c == '\'') {
                    if (single && i + 1 < value.size() && value[i + 1] == '\'') {
                        ++i;
                        continue;
                    }
                    single = !single;
                    continue;
                }
                if (!single && c == '"') {
                    doubleQuoted = !doubleQuoted;
                    continue;
                }
                if (single || doubleQuoted) continue;
                if (c == '[' || c == '{') ++flowDepth;
                else if (c == ']' || c == '}') --flowDepth;
                else if (c == ':' && flowDepth == 0 &&
                         (i + 1 == value.size() ||
                          std::isspace(static_cast<unsigned char>(value[i + 1])))) {
                    return i;
                }
            }
            return std::string::npos;
        }

        bool sequenceLine(const std::string &value) {
            return !value.empty() && value[0] == '-' &&
                   (value.size() == 1 ||
                    std::isspace(static_cast<unsigned char>(value[1])));
        }

        class Parser {
        public:
            explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {
            }

            bool parse(json &out, std::string &error) {
                if (lines_.empty()) {
                    error = "YAML document is empty";
                    return false;
                }
                std::size_t index = 0;
                if (!parseNode(index, lines_.front().indent, out, error)) return false;
                if (index != lines_.size())
                    return fail(lines_[index], "unexpected indentation", error);
                return true;
            }

        private:
            bool fail(const Line &line, const std::string &message, std::string &error) const {
                error = "line " + std::to_string(line.number) + ": " + message;
                return false;
            }

            bool parseNode(std::size_t &index, const std::size_t indent,
                           json &out, std::string &error) {
                if (index >= lines_.size()) {
                    error = "unexpected end of YAML document";
                    return false;
                }
                if (lines_[index].indent != indent)
                    return fail(lines_[index], "unexpected indentation", error);
                if (sequenceLine(lines_[index].text))
                    return parseSequence(index, indent, out, error);
                return parseMapping(index, indent, out, error);
            }

            bool parseKey(const Line &line, const std::string &raw,
                          std::string &key, std::string &error) {
                const std::string value = trim(raw);
                if (value.empty()) return fail(line, "mapping key is empty", error);
                if (value.front() == '\'' || value.front() == '"') {
                    json parsed;
                    if (!parseScalar(line, value, parsed, error)) return false;
                    if (!parsed.is_string()) return fail(line, "mapping key must be a string", error);
                    key = parsed.get<std::string>();
                } else {
                    key = value;
                }
                return true;
            }

            bool parseMappingEntry(std::size_t &index, const std::size_t indent,
                                   json &object, std::string &error) {
                const Line line = lines_[index];
                if (line.indent != indent || sequenceLine(line.text))
                    return fail(line, "expected a mapping entry", error);
                const std::size_t colon = mappingColon(line.text);
                if (colon == std::string::npos)
                    return fail(line, "expected ':' after mapping key", error);
                std::string key;
                if (!parseKey(line, line.text.substr(0, colon), key, error)) return false;
                if (object.contains(key))
                    return fail(line, "duplicate mapping key '" + key + "'", error);
                const std::string rawValue = trim(line.text.substr(colon + 1));
                ++index;
                json value;
                if (rawValue.empty()) {
                    if (index < lines_.size() && lines_[index].indent > indent) {
                        if (!parseNode(index, lines_[index].indent, value, error)) return false;
                    } else {
                        value = nullptr;
                    }
                } else if (!parseScalar(line, rawValue, value, error)) {
                    return false;
                }
                object[key] = std::move(value);
                return true;
            }

            bool parseMapping(std::size_t &index, const std::size_t indent,
                              json &out, std::string &error) {
                out = json::object();
                while (index < lines_.size()) {
                    if (lines_[index].indent < indent) break;
                    if (lines_[index].indent > indent)
                        return fail(lines_[index], "unexpected indentation", error);
                    if (sequenceLine(lines_[index].text)) break;
                    if (!parseMappingEntry(index, indent, out, error)) return false;
                }
                return true;
            }

            bool parseSequence(std::size_t &index, const std::size_t indent,
                               json &out, std::string &error) {
                out = json::array();
                while (index < lines_.size() && lines_[index].indent == indent &&
                       sequenceLine(lines_[index].text)) {
                    const Line line = lines_[index];
                    const std::string item = trim(line.text.substr(1));
                    ++index;
                    json value;
                    if (item.empty()) {
                        if (index >= lines_.size() || lines_[index].indent <= indent)
                            return fail(line, "sequence item has no value", error);
                        if (!parseNode(index, lines_[index].indent, value, error)) return false;
                    } else {
                        const std::size_t colon = mappingColon(item);
                        if (colon == std::string::npos) {
                            if (!parseScalar(line, item, value, error)) return false;
                            if (index < lines_.size() && lines_[index].indent > indent)
                                return fail(lines_[index],
                                            "scalar sequence item cannot have nested content", error);
                        } else {
                            value = json::object();
                            std::string key;
                            if (!parseKey(line, item.substr(0, colon), key, error)) return false;
                            const std::string rawValue = trim(item.substr(colon + 1));
                            if (rawValue.empty()) {
                                if (index < lines_.size() && lines_[index].indent > indent + 2) {
                                    json child;
                                    if (!parseNode(index, lines_[index].indent, child, error))
                                        return false;
                                    value[key] = std::move(child);
                                } else {
                                    value[key] = nullptr;
                                }
                            } else {
                                json firstValue;
                                if (!parseScalar(line, rawValue, firstValue, error)) return false;
                                value[key] = std::move(firstValue);
                            }
                            while (index < lines_.size() && lines_[index].indent > indent) {
                                if (lines_[index].indent != indent + 2)
                                    return fail(lines_[index], "unexpected indentation", error);
                                if (!parseMappingEntry(index, indent + 2, value, error)) return false;
                            }
                        }
                    }
                    out.push_back(std::move(value));
                }
                return true;
            }

            bool parseFlowSequence(const Line &line, const std::string &value,
                                   json &out, std::string &error) {
                out = json::array();
                const std::string body = trim(value.substr(1, value.size() - 2));
                if (body.empty()) return true;
                bool single = false;
                bool doubleQuoted = false;
                bool escaped = false;
                int depth = 0;
                std::size_t start = 0;
                for (std::size_t i = 0; i <= body.size(); ++i) {
                    const char c = i < body.size() ? body[i] : ',';
                    if (doubleQuoted && escaped) {
                        escaped = false;
                        continue;
                    }
                    if (doubleQuoted && c == '\\') {
                        escaped = true;
                        continue;
                    }
                    if (!doubleQuoted && c == '\'') {
                        single = !single;
                        continue;
                    }
                    if (!single && c == '"') {
                        doubleQuoted = !doubleQuoted;
                        continue;
                    }
                    if (single || doubleQuoted) continue;
                    if (c == '[' || c == '{') ++depth;
                    else if (c == ']' || c == '}') --depth;
                    else if (c == ',' && depth == 0) {
                        const std::string part = trim(body.substr(start, i - start));
                        if (part.empty()) return fail(line, "empty value in flow sequence", error);
                        json element;
                        if (!parseScalar(line, part, element, error)) return false;
                        out.push_back(std::move(element));
                        start = i + 1;
                    }
                }
                return true;
            }

            bool parseScalar(const Line &line, const std::string &raw,
                             json &out, std::string &error) {
                const std::string value = trim(raw);
                if (value.empty()) return fail(line, "scalar value is empty", error);
                if (value.front() == '\'') {
                    if (value.size() < 2 || value.back() != '\'')
                        return fail(line, "unterminated single-quoted string", error);
                    std::string decoded;
                    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
                        if (value[i] == '\'' && i + 2 < value.size() && value[i + 1] == '\'') {
                            decoded.push_back('\'');
                            ++i;
                        } else {
                            decoded.push_back(value[i]);
                        }
                    }
                    out = std::move(decoded);
                    return true;
                }
                if (value.front() == '"') {
                    try {
                        out = json::parse(value);
                    } catch (const json::exception &e) {
                        return fail(line, std::string("invalid double-quoted string: ") + e.what(),
                                    error);
                    }
                    if (!out.is_string()) return fail(line, "quoted scalar must be a string", error);
                    return true;
                }
                if (value.front() == '[' && value.back() == ']') {
                    try {
                        out = json::parse(value);
                        return true;
                    } catch (const json::exception &) {
                        return parseFlowSequence(line, value, out, error);
                    }
                }
                if (value.front() == '{' && value.back() == '}') {
                    try {
                        out = json::parse(value);
                        return true;
                    } catch (const json::exception &e) {
                        return fail(line, std::string("invalid flow mapping: ") + e.what(), error);
                    }
                }
                const std::string normalized = lower(value);
                if (normalized == "true") {
                    out = true;
                    return true;
                }
                if (normalized == "false") {
                    out = false;
                    return true;
                }
                if (normalized == "null" || value == "~") {
                    out = nullptr;
                    return true;
                }
                try {
                    json number = json::parse(value);
                    if (number.is_number()) {
                        out = std::move(number);
                        return true;
                    }
                } catch (const json::exception &) {
                }
                out = value;
                return true;
            }

            std::vector<Line> lines_;
        };

        bool tokenize(const std::string &text, std::vector<Line> &lines, std::string &error) {
            std::istringstream input(text);
            std::string raw;
            std::size_t number = 0;
            while (std::getline(input, raw)) {
                ++number;
                if (!raw.empty() && raw.back() == '\r') raw.pop_back();
                std::size_t indent = 0;
                while (indent < raw.size() && raw[indent] == ' ') ++indent;
                if (indent < raw.size() && raw[indent] == '\t') {
                    error = "line " + std::to_string(number) +
                            ": tabs are not allowed for YAML indentation";
                    return false;
                }
                const std::string content = trim(withoutComment(raw.substr(indent)));
                if (content.empty() || content == "---" || content == "...") continue;
                lines.push_back(Line{number, indent, content});
            }
            return true;
        }
    }

    bool parseYaml(const std::string &text, nlohmann::json &out, std::string &error) {
        std::vector<Line> lines;
        if (!tokenize(text, lines, error)) return false;
        return Parser(std::move(lines)).parse(out, error);
    }
}
