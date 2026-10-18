#include "sqlconduit/common/named_params.h"
#include <map>
#include <set>

namespace sqlconduit::common {
    namespace {
        bool first(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
        bool next(char c) { return first(c) || (c >= '0' && c <= '9'); }
    }

    BoundQuery bindNamed(const std::string &sql, const NamedParams &params) {
        BoundQuery out;
        auto fail = [&](std::string message) {
            out.status = Status::error(ErrorCode::QueryError, std::move(message));
            out.sql.clear();
            out.params.clear();
            return out;
        };
        std::map<std::string, const Value *> values;
        std::set<std::string> used;
        for (const auto &entry : params.values) {
            const auto &name = entry.first;
            if (name.empty() || !first(name[0])) return fail("invalid named parameter name");
            for (char c : name) if (!next(c)) return fail("invalid named parameter name");
            if (!values.emplace(name, &entry.second).second) return fail("duplicate named parameter: " + name);
        }
        out.sql.reserve(sql.size());
        for (std::size_t i = 0; i < sql.size();) {
            const auto start = i;
            const char c = sql[i];
            const bool bracketIdentifier = c == '[' && (i == 0 ||
                (!next(sql[i - 1]) && sql[i - 1] != ')' && sql[i - 1] != ']'));
            if (c == '\'' || c == '"' || c == '`' || bracketIdentifier) {
                const char end = c == '[' ? ']' : c;
                bool closed = false;
                ++i;
                while (i < sql.size()) {
                    if (sql[i] == '\\' && c != '[' && i + 1 < sql.size()) { i += 2; continue; }
                    if (sql[i++] == end) {
                        if (i < sql.size() && sql[i] == end) { ++i; continue; }
                        closed = true;
                        break;
                    }
                }
                if (!closed) return fail("unterminated SQL quote in named query");
            } else if (c == '#') {
                return fail("hash comments/operators are dialect-ambiguous in named queries; use portable SQL");
            } else if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
                i = sql.find('\n', i);
                if (i == std::string::npos) i = sql.size();
            } else if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                i += 2;
                unsigned depth = 1;
                while (i < sql.size() && depth) {
                    if (sql.compare(i, 2, "/*") == 0) { ++depth; i += 2; }
                    else if (sql.compare(i, 2, "*/") == 0) { --depth; i += 2; }
                    else ++i;
                }
                if (depth) return fail("unterminated SQL comment in named query");
            } else if ((c == 'q' || c == 'Q') && i + 2 < sql.size() && sql[i + 1] == '\'') {
                const char open = sql[i + 2];
                const char close = open == '[' ? ']' : open == '(' ? ')' : open == '{' ? '}' : open == '<' ? '>' : open;
                const auto end = sql.find(std::string{close, '\''}, i + 3);
                if (end == std::string::npos) return fail("unterminated Oracle alternative quote");
                i = end + 2;
            } else if (c == '$' && (i == 0 || !next(sql[i - 1]))) {
                std::size_t end = i + 1;
                if (end < sql.size() && first(sql[end])) while (end < sql.size() && next(sql[end])) ++end;
                if (end < sql.size() && sql[end] == '$') {
                    const auto tag = sql.substr(i, end - i + 1);
                    const auto close = sql.find(tag, end + 1);
                    if (close == std::string::npos) return fail("unterminated SQL dollar quote");
                    i = close + tag.size();
                } else return fail("native positional placeholders are not supported in named queries");
            } else if (c == ':' && i + 1 < sql.size() && sql[i + 1] == ':') {
                i += 2; // PostgreSQL cast, not a named parameter.
            } else if (c == ':' && i + 1 < sql.size() && first(sql[i + 1])) {
                i += 2;
                while (i < sql.size() && next(sql[i])) ++i;
                const auto name = sql.substr(start + 1, i - start - 1);
                const auto found = values.find(name);
                if (found == values.end()) return fail("missing named parameter: " + name);
                out.params.push_back(*found->second);
                used.insert(name);
                out.sql += '?';
                continue;
            } else {
                if (c == '?') return fail("positional placeholders cannot be mixed with named parameters");
                if (c == ':' && i + 1 < sql.size() && sql[i + 1] >= '0' && sql[i + 1] <= '9')
                    return fail("native positional placeholders are not supported in named queries");
                ++i;
            }
            out.sql.append(sql, start, i - start);
        }
        for (const auto &entry : values)
            if (!used.count(entry.first)) return fail("unused named parameter: " + entry.first);
        return out;
    }
}
