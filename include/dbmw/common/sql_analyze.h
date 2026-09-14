#ifndef DBMW_COMMON_SQL_ANALYZE_H
#define DBMW_COMMON_SQL_ANALYZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace dbmw::common::sql {
    enum class StatementKind {
        Unknown,
        Select,
        Insert,
        Update,
        Delete,
        Ddl,
        Other
    };

    std::string structuralTemplate(const std::string &sql);

    std::uint64_t fingerprintTemplate(const std::string &sql);

    StatementKind classifyStatement(const std::string &sql);

    bool isWrite(StatementKind kind);

    bool hasWhereClause(const std::string &sql);

    bool hasLimitClause(const std::string &sql);

    bool hasMultipleStatements(const std::string &sql);
}

#endif
