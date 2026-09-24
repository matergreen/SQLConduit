#ifndef SQLCONDUIT_COMMON_SQL_DIALECT_H
#define SQLCONDUIT_COMMON_SQL_DIALECT_H

#include <string>

namespace sqlconduit::common::util {
    enum class Dialect {
        Auto = 0,
        MySQL = 1,
        Postgres = 2,
        SqlServer = 3,
        Oracle = 4
    };

    enum class RoutineKind { Function = 0, Procedure = 1 };

    struct RoutineRef {
        std::string name;
        RoutineKind kind = RoutineKind::Procedure;
        std::string dataSource;
    };

    inline std::string quoteIdent(const std::string &identifier, const Dialect dialect) {
        const char quote = dialect == Dialect::MySQL ? '`' : '"';
        std::string out;
        std::string part;
        const auto flush = [&] {
            if (part.empty()) return;
            if (!out.empty()) out.push_back('.');
            out.push_back(quote);
            for (const char c: part) {
                if (c == quote) out.push_back(quote);
                out.push_back(c);
            }
            out.push_back(quote);
            part.clear();
        };
        for (const char c: identifier) {
            if (c == '.') flush();
            else part.push_back(c);
        }
        flush();
        return out;
    }
}

#endif
