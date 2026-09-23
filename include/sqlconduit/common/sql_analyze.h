#ifndef SQLCONDUIT_COMMON_SQL_ANALYZE_H
#define SQLCONDUIT_COMMON_SQL_ANALYZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace sqlconduit::common::sql {
    enum class StatementKind {
        Unknown = 0,
        Select = 1,
        Insert = 2,
        Update = 3,
        Delete = 4,
        Ddl = 5,
        Other = 6
    };

    std::string structuralTemplate(const std::string &sql);

    std::uint64_t fingerprintTemplate(const std::string &sql);

    StatementKind classifyStatement(const std::string &sql);

    bool isWrite(StatementKind kind);

    bool hasWhereClause(const std::string &sql);

    bool hasLimitClause(const std::string &sql);

    bool hasRowLimitClause(const std::string &sql);

    bool hasMultipleStatements(const std::string &sql);

    bool hasMultipleStatements(const std::string &sql, bool allowRoutineBody);

    bool isRoutineDdl(const std::string &sql);
}

#endif
