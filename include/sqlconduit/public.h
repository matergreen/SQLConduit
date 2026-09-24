#ifndef SQLCONDUIT_PUBLIC_H
#define SQLCONDUIT_PUBLIC_H

// Curated re-export facade.
//
// The library is internally organized into deep namespaces (common, core, sql,
// mapping, drivers, ...) which is good for separating concerns, but it forces
// callers to qualify every public symbol through those namespaces, e.g.
// sqlconduit::common::util::Dialect or sqlconduit::core::Cursor.
//
// This header lifts the most-used public types and free functions into the
// top-level `sqlconduit` namespace so callers can write sqlconduit::Dialect,
// sqlconduit::Value, sqlconduit::Builder, sqlconduit::eq(...),
// sqlconduit::call(...), sqlconduit::classifyStatement(...), etc.
//
// It is purely additive: every deep name (sqlconduit::common::util::Dialect,
// sqlconduit::core::Cursor, sqlconduit::common::sql::StatementKind, ...) remains
// valid and is preserved for backward compatibility. Internal code keeps using
// the canonical deep namespaces.
//
// Only high-level public API is lifted. Internal helpers (makeCallSql,
// makeCallPlan, parenArgs, unsupported, ...) stay in their deep namespaces.

#include "sqlconduit/common/sql_analyze.h"
#include "sqlconduit/common/sql_dialect.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/util.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/sql_builder.h"

namespace sqlconduit {
    // Common value model used across the public API.
    using common::Value;
    using common::Timestamp;
    using common::Blob;
    using common::ErrorCode;
    using common::Status;
    using common::Params;
    using common::ParamDirection;
    using common::ValueType;
    using common::IsolationLevel;
    using common::ResultSet;
    using common::Row;
    using common::CallParam;
    using common::CallParams;

    // Dialect selection and routine references.
    using common::util::Dialect;
    using common::util::RoutineKind;
    using common::util::RoutineRef;

    // Routine / DDL execution options and results.
    using common::util::ExecOptions;
    using common::util::CreateRoutineOptions;
    using common::util::DropRoutineOptions;
    using common::util::CallOptions;
    using common::util::CreateIndexOptions;
    using common::util::DropIndexOptions;
    using common::util::IndexSpec;
    using common::util::CallResult;
    using common::util::ScriptOptions;
    using common::util::ScriptResult;

    // Forward / session cursor.
    using core::Cursor;

    // Cross-dialect SQL builder.
    using sql::Builder;
    using sql::Condition;
    using sql::Operation;
    using sql::CompareOperator;
    using sql::SortDirection;
    using sql::BuildResult;

    using sql::eq;
    using sql::ne;
    using sql::lt;
    using sql::le;
    using sql::gt;
    using sql::ge;
    using sql::like;
    using sql::in;
    using sql::notIn;
    using sql::between;
    using sql::isNull;
    using sql::isNotNull;
    using sql::all;
    using sql::any;
    using sql::not_;

    // SQL statement analysis utilities (observability / auditing).
    using common::sql::StatementKind;
    using common::sql::structuralTemplate;
    using common::sql::fingerprintTemplate;
    using common::sql::classifyStatement;
    using common::sql::isWrite;
    using common::sql::hasWhereClause;
    using common::sql::hasLimitClause;
    using common::sql::hasRowLimitClause;
    using common::sql::hasMultipleStatements;
    using common::sql::isRoutineDdl;

    // Routine calling and DDL helpers.
    using common::util::quoteIdent;
    using common::util::call;
    using common::util::callQuery;
    using common::util::callEach;
    using common::util::createRoutine;
    using common::util::dropRoutine;
    using common::util::createIndex;
    using common::util::dropIndex;
    using common::util::runScripts;
    using common::util::runScriptsInDir;
}

#endif
