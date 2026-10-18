#include "sqlconduit/pagination.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>

namespace sqlconduit::detail {
    namespace {
        common::Status invalid(const std::string &message) {
            return common::Status::error(common::ErrorCode::QueryError, "pagination: " + message);
        }
        // A conservative lexical gate, not a general SQL rewriter. Keep opaque text
        // out of top-level clause detection; never manufacture COUNT from arbitrary SQL.
        common::Status clauses(const std::string &sql, std::vector<std::string> &tokens) {
            if (sql.find('\0') != std::string::npos) return invalid("NUL bytes are unsupported, including inside quoted text");
            std::size_t i = 0;
            int depth = 0;
            while (i < sql.size()) {
                const auto c = static_cast<unsigned char>(sql[i]);
                if (c == 0 || c == ';' || c == '#') return invalid("omit terminators, NUL and dialect-ambiguous hash syntax");
                if (std::isspace(c)) { ++i; continue; }
                if (sql.compare(i, 2, "--") == 0) {
                    const auto end = sql.find('\n', i + 2);
                    i = end == std::string::npos ? sql.size() : end + 1; continue;
                }
                if (sql.compare(i, 2, "/*") == 0) {
                    if (sql.compare(i, 3, "/*!") == 0) return invalid("executable comments are unsupported");
                    i += 2; int nested = 1;
                    while (i < sql.size() && nested) {
                        if (sql.compare(i, 2, "/*") == 0) { ++nested; i += 2; }
                        else if (sql.compare(i, 2, "*/") == 0) { --nested; i += 2; }
                        else ++i;
                    }
                    if (nested) return invalid("unterminated comment");
                    continue;
                }
                if ((c == 'q' || c == 'Q') && i + 2 < sql.size() && sql[i + 1] == '\'') {
                    const auto open = sql[i + 2];
                    const char close = open == '[' ? ']' : open == '{' ? '}' : open == '(' ? ')' : open == '<' ? '>' : open;
                    const auto end = sql.find(std::string{close, '\''}, i + 3);
                    if (end == std::string::npos) return invalid("unterminated alternative literal");
                    if (!depth) tokens.emplace_back("_");
                    i = end + 2; continue;
                }
                if (c == '$') {
                    auto end = i + 1;
                    while (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_')) ++end;
                    if (end < sql.size() && sql[end] == '$' &&
                        (end == i + 1 || !std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
                        const auto delimiter = sql.substr(i, end - i + 1);
                        const auto close = sql.find(delimiter, end + 1);
                        if (close == std::string::npos) return invalid("unterminated dollar literal");
                        if (!depth) tokens.emplace_back("_");
                        i = close + delimiter.size(); continue;
                    }
                    return invalid("use portable positional or named binding, not dollar placeholders");
                }
                const bool bracket = c == '[' && (i == 0 ||
                    (!std::isalnum(static_cast<unsigned char>(sql[i - 1])) && sql[i - 1] != '_' &&
                     sql[i - 1] != ')' && sql[i - 1] != ']' && sql[i - 1] != '?'));
                if (c == '\'' || c == '"' || c == '`' || bracket) {
                    const char close = bracket ? ']' : static_cast<char>(c);
                    ++i; bool closed = false;
                    while (i < sql.size()) {
                        if (!bracket && sql[i] == '\\' && i + 1 < sql.size()) { i += 2; continue; }
                        if (sql[i++] == close) {
                            if (i < sql.size() && sql[i] == close) { ++i; continue; }
                            closed = true; break;
                        }
                    }
                    if (!closed) return invalid("unterminated quoted text");
                    if (!depth) tokens.emplace_back("_");
                    continue;
                }
                if (c == '(') { if (!depth) tokens.emplace_back("_"); ++depth; ++i; continue; }
                if (c == ')') { if (--depth < 0) return invalid("unbalanced parentheses"); ++i; continue; }
                if (std::isalnum(c) || c == '_') {
                    const auto start = i++;
                    while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) ++i;
                    if (!depth) {
                        auto token = sql.substr(start, i - start);
                        std::transform(token.begin(), token.end(), token.begin(), [](unsigned char x) { return static_cast<char>(std::toupper(x)); });
                        tokens.push_back(std::move(token));
                    }
                    continue;
                }
                ++i;
            }
            return depth ? invalid("unbalanced parentheses") : common::Status{};
        }
        common::Status validateSelect(const std::string &sql, bool requireOrder) {
            std::vector<std::string> tokens;
            if (auto status = clauses(sql, tokens); !status.ok()) return status;
            if (tokens.empty() || tokens.front() != "SELECT") return invalid("requires a SELECT; wrap or handle CTEs explicitly");
            bool ordered = false;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                const auto &token = tokens[i];
                if (token == "LIMIT" || token == "OFFSET" || token == "FETCH" || token == "TOP" || token == "INTO" ||
                    token == "FOR" || token == "LOCK" || token == "OPTION")
                    return invalid("existing pagination, SELECT INTO, locking and trailing options are unsupported");
                if (token == "ORDER" && i + 2 < tokens.size() && tokens[i + 1] == "BY") ordered = true;
            }
            if (requireOrder && !ordered) return invalid("requires a stable top-level ORDER BY including a unique tie-breaker");
            return {};
        }
    }

    PreparedPage preparePage(const sql::PageQuery &query, const PageOptions &options, common::util::Dialect detected) {
        PreparedPage result;
        result.status = query.status;
        if (!result.status.ok()) return result;
        constexpr auto max = (std::numeric_limits<std::int64_t>::max)();
        if (options.page < 1 || options.pageSize < 1 || options.pageSize == max ||
            options.page - 1 > (max - options.pageSize - 1) / options.pageSize) {
            result.status = invalid("invalid page/size or offset overflow"); return result;
        }
        result.status = validateSelect(query.statement.sql, true);
        if (!result.status.ok()) return result;
        if (options.includeTotal) {
            result.count = options.countQuery ? options.countQuery : query.countQuery;
            if (!result.count) { result.status = invalid("explicit COUNT required for raw SQL; use Builder::buildPage for generated COUNT"); return result; }
            result.status = validateSelect(result.count->sql, false);
            if (!result.status.ok()) return result;
        }
        using common::util::Dialect;
        auto dialect = options.dialect != Dialect::Auto ? options.dialect :
                       query.dialect != Dialect::Auto ? query.dialect : detected;
        if (dialect != Dialect::MySQL && dialect != Dialect::Postgres && dialect != Dialect::Oracle && dialect != Dialect::SqlServer) {
            result.status = common::Status::error(common::ErrorCode::NotSupported, "pagination requires an explicit dialect for a custom driver"); return result;
        }
        const auto offset = (options.page - 1) * options.pageSize;
        result.data = query.statement;
        // A newline prevents a trailing -- comment from swallowing the added clauses.
        if (dialect == Dialect::MySQL || dialect == Dialect::Postgres) {
            result.data.sql += "\nLIMIT ? OFFSET ?";
            result.data.params.emplace_back(options.pageSize + 1);
            result.data.params.emplace_back(offset);
        } else {
            result.data.sql += "\nOFFSET ? ROWS FETCH NEXT ? ROWS ONLY";
            result.data.params.emplace_back(offset);
            result.data.params.emplace_back(options.pageSize + 1);
        }
        return result;
    }

    void finishPageRows(PageRows &page) {
        if (!page.status.ok()) { page.rows.clear(); page.hasNext = false; page.total.reset(); return; }
        page.hasNext = page.rows.rowCount() > static_cast<std::uint64_t>(page.pageSize);
        if (page.hasNext) page.rows.mutableRows().resize(static_cast<std::size_t>(page.pageSize));
    }

    common::Status readPageTotal(const common::ResultSet &rows, std::int64_t &out) {
        if (rows.rowCount() != 1 || rows.rows()[0].size() != 1 || rows.fields().size() > 1 ||
            (!rows.fields().empty() && !rows.rows()[0].has(rows.fields()[0])))
            return invalid("COUNT must return exactly one nonnegative integer value");
        const auto &value = rows.rows()[0].data().begin()->second;
        if (const auto *number = std::get_if<std::int64_t>(&value); number && *number >= 0) { out = *number; return {}; }
        if (const auto *number = std::get_if<std::uint64_t>(&value); number && *number <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            out = static_cast<std::int64_t>(*number); return {};
        }
        // Oracle can expose aggregate NUMBER values as exact Decimal. Accept an
        // integral decimal without floating-point rounding or permissive text casts.
        if (const auto *decimal = std::get_if<common::Decimal>(&value)) {
            const auto dot = decimal->value.find('.');
            const auto length = dot == std::string::npos ? decimal->value.size() : dot;
            const auto *begin = decimal->value.data();
            std::int64_t parsed = 0;
            const auto converted = std::from_chars(begin, begin + length, parsed);
            if (converted.ec == std::errc{} && converted.ptr == begin + length && parsed >= 0 &&
                (dot == std::string::npos || (dot + 1 < decimal->value.size() &&
                    decimal->value.find_first_not_of('0', dot + 1) == std::string::npos))) {
                out = parsed; return {};
            }
        }
        return invalid("COUNT must return exactly one nonnegative integer value");
    }
}
