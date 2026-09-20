#ifndef DBMW_UTIL_H
#define DBMW_UTIL_H

#include "dbmw/common/context.h"
#include "dbmw/common/logger.h"
#include "dbmw/common/types.h"
#include "dbmw/core/database_manager.h"
#include "dbmw/core/query_cache.h"
#include "dbmw/dbmw.h"
#include "dbmw/async/dbmw_async.h"

#if defined(DBMW_ENABLE_ASYNC_CORO)
#include "dbmw/async/task.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dbmw::common::util {
    enum class RoutineKind { Function, Procedure };

    enum class Dialect { Auto, MySQL, Postgres, SqlServer };

    struct RoutineRef {
        std::string name;
        RoutineKind kind = RoutineKind::Procedure;
        std::string dataSource;
    };

    struct ExecOptions {
        std::string dataSource;
        Dialect dialect = Dialect::Auto;
        bool forcePrimary = true;
        Idempotency idempotency = Idempotency::NonIdempotent;
    };

    struct CreateRoutineOptions : ExecOptions {
        bool replace = false;
        bool ifNotExists = false;
        bool stripDelimiter = true;
    };

    struct DropRoutineOptions : ExecOptions {
        bool ifExists = true;
        bool cascade = false;
    };

    struct CallOptions : ExecOptions {
        bool returnsRows = true;
    };

    struct CreateIndexOptions : ExecOptions {
    };

    struct DropIndexOptions : ExecOptions {
        bool ifExists = true;
    };

    struct IndexSpec {
        std::string table;
        std::string name;
        std::vector<std::string> columns;
        bool unique = false;
        bool ifNotExists = false;
        bool concurrent = false;
        std::string usingMethod;
        std::string options;
    };

    enum class ParamDirection { In, Out, InOut };

    struct CallParam {
        Value value;
        ParamDirection direction = ParamDirection::In;

        CallParam() = default;

        explicit CallParam(Value v) : value(std::move(v)) {
        }

        CallParam(const ParamDirection d, Value v) : value(std::move(v)), direction(d) {
        }
    };

    using CallParams = std::vector<CallParam>;

    struct CallResult {
        Status status;
        std::vector<ResultSet> sets;
        std::int64_t affected = 0;
        std::vector<Value> outParams;

        [[nodiscard]] bool ok() const { return status.ok(); }

        [[nodiscard]] const ResultSet *firstSet() const {
            return sets.empty() ? nullptr : &sets.front();
        }

        [[nodiscard]] std::size_t rowCount() const {
            std::size_t n = 0;
            for (const auto &s: sets) n += s.rowCount();
            return n;
        }
    };

    struct CallPlan {
        std::string preSql;
        Params preParams;
        std::string callSql;
        Params callParams;
        std::string fetchSql;
        std::vector<std::string> outColumns;
        std::size_t outFromRowCount = 0;
        bool needsSameConnection = false;
    };

    inline bool hasOutParams(const CallParams &params) {
        for (const auto &p: params)
            if (p.direction != ParamDirection::In) return true;
        return false;
    }

    inline std::string outVarName(const std::size_t index) {
        return "dbmw_out_" + std::to_string(index);
    }

    inline Status unsupported(std::string msg) {
        return Status::error(ErrorCode::NotSupported, std::move(msg));
    }

    inline Status badSpec(std::string msg) {
        return Status::error(ErrorCode::QueryError, std::move(msg));
    }

    inline const char *dialectName(const Dialect d) {
        switch (d) {
            case Dialect::MySQL: return "mysql";
            case Dialect::Postgres: return "postgres";
            case Dialect::SqlServer: return "sqlserver";
            case Dialect::Auto: break;
        }
        return "auto";
    }

    inline std::string toLowerAscii(std::string s) {
        for (char &c: s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }

    inline bool startsWithFold(const std::string &s, const std::size_t pos, const char *kw) {
        const std::size_t len = std::char_traits<char>::length(kw);
        if (pos + len > s.size()) return false;
        for (std::size_t k = 0; k < len; ++k)
            if (std::tolower(static_cast<unsigned char>(s[pos + k])) !=
                std::tolower(static_cast<unsigned char>(kw[k])))
                return false;
        return true;
    }

    inline Dialect detectDialect(const std::string &dataSource) {
        const auto ds = DBMW::dataSource(dataSource);
        if (!ds) return Dialect::Auto;
        const std::string t = toLowerAscii(ds->driverType());
        if (t.find("mysql") != std::string::npos || t.find("maria") != std::string::npos)
            return Dialect::MySQL;
        if (t.find("postgres") != std::string::npos || t.find("pgsql") != std::string::npos ||
            t == "pg")
            return Dialect::Postgres;
        if (t.find("odbc") != std::string::npos || t.find("sqlserver") != std::string::npos ||
            t.find("mssql") != std::string::npos)
            return Dialect::SqlServer;
        return Dialect::Auto;
    }

    inline Dialect resolveDialect(const ExecOptions &o) {
        return o.dialect != Dialect::Auto ? o.dialect : detectDialect(o.dataSource);
    }

    inline std::string quoteIdent(const std::string &ident, const Dialect d) {
        const char q = d == Dialect::MySQL ? '`' : '"';
        std::string out;
        std::string part;
        const auto flush = [&] {
            if (part.empty()) return;
            if (!out.empty()) out.push_back('.');
            out.push_back(q);
            for (const char c: part) {
                if (c == q) out.push_back(q);
                out.push_back(c);
            }
            out.push_back(q);
            part.clear();
        };
        for (const char c: ident) {
            if (c == '.') flush();
            else part.push_back(c);
        }
        flush();
        return out;
    }

    inline std::size_t stripDelimiterDirectives(std::string &sql) {
        std::size_t removed = 0;
        std::string out;
        std::size_t i = 0;
        while (i < sql.size()) {
            const auto nl = sql.find('\n', i);
            const std::size_t end = nl == std::string::npos ? sql.size() : nl + 1;
            const std::string line = sql.substr(i, end - i);
            std::size_t p = 0;
            while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
            const std::size_t after = p + 9;
            const bool isDirective =
                    startsWithFold(line, p, "DELIMITER") &&
                    (after >= line.size() || std::isspace(static_cast<unsigned char>(line[after])));
            if (isDirective) ++removed;
            else out += line;
            i = end;
        }
        if (removed > 0) sql = std::move(out);
        return removed;
    }

    inline std::string parenArgs(const std::size_t n) {
        std::string s = "(";
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ", ";
            s += '?';
        }
        s += ')';
        return s;
    }

    inline std::string csvArgs(const std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ", ";
            s += '?';
        }
        return s;
    }

    inline Status makeCallSql(const RoutineRef &ref, const std::size_t argCount,
                              const Dialect d, const bool returnsRows, std::string &out) {
        if (ref.name.empty()) return badSpec("dbmw::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("dbmw::util: cannot detect dialect for datasource '" +
                               ref.dataSource + "'; pass Dialect explicitly");
        const std::string name = quoteIdent(ref.name, d);
        switch (d) {
            case Dialect::MySQL:
                out = "CALL " + name + parenArgs(argCount);
                return Status::OK();
            case Dialect::Postgres:
                if (ref.kind == RoutineKind::Function)
                    out = returnsRows
                              ? "SELECT * FROM " + name + parenArgs(argCount)
                              : "SELECT " + name + parenArgs(argCount);
                else {
                    if (returnsRows)
                        return unsupported(
                            "dbmw::util: postgres procedures cannot return a result set; "
                            "use a function with makeCallSql");
                    out = "CALL " + name + parenArgs(argCount);
                }
                return Status::OK();
            case Dialect::SqlServer:
                out = returnsRows
                          ? "{CALL " + name + parenArgs(argCount) + "}"
                          : "EXEC " + name + (argCount == 0 ? std::string() : " " + csvArgs(argCount));
                return Status::OK();
            case Dialect::Auto: break;
        }
        return unsupported("dbmw::util: unknown dialect");
    }

    inline Status makeCallPlan(const RoutineRef &ref, const CallParams &params,
                               const Dialect d, const bool returnsRows, CallPlan &out) {
        out = CallPlan{};
        if (ref.name.empty()) return badSpec("dbmw::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("dbmw::util: cannot detect dialect for datasource '" +
                               ref.dataSource + "'; pass Dialect explicitly");

        const std::string name = quoteIdent(ref.name, d);
        const bool hasOut = hasOutParams(params);

        if (d == Dialect::SqlServer && hasOut)
            return unsupported("dbmw::util: sqlserver OUT/INOUT parameters need DECLARE @var "
                "<type> before EXEC ... OUTPUT, and dbmw cannot infer the type; return the "
                "values as a result set instead");

        if (d == Dialect::Postgres && ref.kind == RoutineKind::Procedure) {
            if (returnsRows)
                return unsupported(
                    "dbmw::util: postgres procedures cannot return a result set; "
                    "use a function with makeCallSql");
            if (hasOut)
                return unsupported("dbmw::util: postgres procedures do not hand OUT/INOUT "
                    "parameters back to the client; use a function with OUT parameters");
        }
        if (d == Dialect::Postgres && ref.kind == RoutineKind::Function && hasOut && !returnsRows)
            return unsupported("dbmw::util: postgres functions with OUT/INOUT parameters must "
                "be called with returnsRows=true (SELECT * FROM f(...))");

        std::vector<std::string> args;
        std::string setParts;
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto &p = params[i];
            if (p.direction == ParamDirection::In) {
                args.emplace_back("?");
                out.callParams.push_back(p.value);
                continue;
            }
            if (d == Dialect::Postgres) {
                if (p.direction == ParamDirection::InOut) {
                    args.emplace_back("?");
                    out.callParams.push_back(p.value);
                }
                ++out.outFromRowCount;
                continue;
            }
            const std::string var = "@" + outVarName(i);
            args.push_back(var);
            out.outColumns.push_back(outVarName(i));
            if (p.direction == ParamDirection::InOut) {
                if (!setParts.empty()) setParts += ", ";
                setParts += var + " = ?";
                out.preParams.push_back(p.value);
            }
        }

        if (d == Dialect::MySQL) {
            out.callSql = "CALL " + name + "(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) out.callSql += ", ";
                out.callSql += args[i];
            }
            out.callSql += ')';
            if (!setParts.empty()) out.preSql = "SET " + setParts;
            if (!out.outColumns.empty()) {
                out.fetchSql = "SELECT ";
                for (std::size_t i = 0; i < out.outColumns.size(); ++i) {
                    if (i) out.fetchSql += ", ";
                    out.fetchSql += "@" + out.outColumns[i] + " AS " + out.outColumns[i];
                }
                out.needsSameConnection = true;
            }
            return Status::OK();
        }
        if (d == Dialect::Postgres) {
            const std::string list = "(";
            std::string inner;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i) inner += ", ";
                inner += args[i];
            }
            out.callSql = "SELECT * FROM " + name + list + inner + ")";
            return Status::OK();
        }
        if (d == Dialect::SqlServer) {
            out.callSql = returnsRows
                              ? "{CALL " + name + parenArgs(args.size()) + "}"
                              : "EXEC " + name + (args.empty()
                                                      ? std::string()
                                                      : " " + csvArgs(
                                                            args.size()));
            return Status::OK();
        }
        return unsupported("dbmw::util: unknown dialect");
    }

    inline Status readOutParams(const ResultSet &rs, const std::vector<std::string> &names,
                                std::vector<Value> &out) {
        out.clear();
        if (names.empty()) return Status::OK();
        if (rs.rows().empty())
            return Status::error(ErrorCode::QueryError,
                                 "dbmw::util: OUT parameter fetch returned no row");
        const auto &row = rs.rows().front();
        out.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (row.has(names[i])) {
                out.push_back(row.at(names[i]));
                continue;
            }
            if (i < row.data().size()) {
                auto it = row.data().begin();
                std::advance(it, static_cast<std::ptrdiff_t>(i));
                out.push_back(it->second);
                continue;
            }
            return Status::error(ErrorCode::QueryError,
                                 "dbmw::util: OUT parameter column '" + names[i] + "' is missing");
        }
        return Status::OK();
    }

    inline Status readOutParamsFromRow(const ResultSet &rs, const std::size_t count,
                                       std::vector<Value> &out) {
        out.clear();
        if (count == 0) return Status::OK();
        if (rs.rows().empty())
            return Status::error(ErrorCode::QueryError,
                                 "dbmw::util: routine returned no row to read OUT/INOUT "
                                 "parameters from");
        const auto &row = rs.rows().front();
        const auto &fields = rs.fields();
        if (fields.size() < count)
            return Status::error(ErrorCode::QueryError,
                                 "dbmw::util: routine returned " + std::to_string(fields.size()) +
                                 " column(s), expected at least " + std::to_string(count) +
                                 " for OUT/INOUT parameters");
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.push_back(row.at(fields[i]));
        return Status::OK();
    }

    inline Status makeDropRoutineSql(const RoutineRef &ref, const DropRoutineOptions &o,
                                     const Dialect d, std::string &out) {
        if (ref.name.empty()) return badSpec("dbmw::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("dbmw::util: cannot detect dialect for datasource '" +
                               ref.dataSource + "'; pass Dialect explicitly");
        if (o.cascade && d != Dialect::Postgres)
            return unsupported("dbmw::util: DROP ... CASCADE is only supported by postgres "
                               "(dialect=" + std::string(dialectName(d)) + ")");
        const char *kindWord = ref.kind == RoutineKind::Function ? "FUNCTION" : "PROCEDURE";
        std::string s = "DROP ";
        s += kindWord;
        if (o.ifExists) s += " IF EXISTS";
        s += ' ';
        s += quoteIdent(ref.name, d);
        if (o.cascade) s += " CASCADE";
        out = std::move(s);
        return Status::OK();
    }

    inline Status makeCreateIndexSql(const IndexSpec &spec, const Dialect d, std::string &out) {
        if (d == Dialect::Auto)
            return unsupported("dbmw::util: cannot detect dialect for datasource; "
                "pass Dialect explicitly");
        if (spec.table.empty() || spec.name.empty())
            return badSpec("dbmw::util: index spec requires both table and name");
        if (spec.columns.empty())
            return badSpec("dbmw::util: index spec requires at least one column");
        if (spec.ifNotExists && d != Dialect::Postgres)
            return unsupported("dbmw::util: CREATE INDEX IF NOT EXISTS is only supported by "
                               "postgres (dialect=" + std::string(dialectName(d)) + ")");
        if (spec.concurrent && d != Dialect::Postgres)
            return unsupported("dbmw::util: CREATE INDEX CONCURRENTLY is only supported by "
                               "postgres (dialect=" + std::string(dialectName(d)) + ")");
        if (!spec.usingMethod.empty() && d == Dialect::SqlServer)
            return unsupported("dbmw::util: sqlserver has no USING clause for CREATE INDEX");

        std::string cols;
        for (std::size_t i = 0; i < spec.columns.size(); ++i) {
            if (i) cols += ", ";
            cols += spec.columns[i];
        }

        std::string s = "CREATE ";
        if (spec.unique) s += "UNIQUE ";
        s += "INDEX ";
        if (spec.concurrent) s += "CONCURRENTLY ";
        if (spec.ifNotExists) s += "IF NOT EXISTS ";
        s += quoteIdent(spec.name, d);
        s += " ON ";
        s += quoteIdent(spec.table, d);
        if (d == Dialect::Postgres && !spec.usingMethod.empty())
            s += " USING " + spec.usingMethod;
        s += " (" + cols + ")";
        if (d == Dialect::MySQL && !spec.usingMethod.empty())
            s += " USING " + spec.usingMethod;
        if (!spec.options.empty()) s += " " + spec.options;
        out = std::move(s);
        return Status::OK();
    }

    inline Status makeDropIndexSql(const std::string &table, const std::string &name,
                                   const bool ifExists, const Dialect d, std::string &out) {
        if (d == Dialect::Auto)
            return unsupported("dbmw::util: cannot detect dialect for datasource; "
                "pass Dialect explicitly");
        if (table.empty() || name.empty())
            return badSpec("dbmw::util: dropIndex requires both table and name");
        std::string s = "DROP INDEX ";
        if (ifExists) s += "IF EXISTS ";
        s += quoteIdent(name, d);
        if (d != Dialect::Postgres) s += " ON " + quoteIdent(table, d);
        out = std::move(s);
        return Status::OK();
    }

    namespace detail {
        class ExecScope {
        public:
            explicit ExecScope(const ExecOptions &o) {
                SqlContext ctx = ContextScope::current();
                if (o.forcePrimary) ctx.shadow = false;
                if (o.idempotency != Idempotency::Unspecified) ctx.idempotency = o.idempotency;
                scope_.emplace(std::move(ctx));
            }

            ExecScope(const ExecScope &) = delete;

            ExecScope &operator=(const ExecScope &) = delete;

        private:
            std::optional<ContextScope> scope_;
        };

        inline Status runDdl(const ExecOptions &o, const std::string &sql) {
            std::int64_t affected = 0;
            Status st;
            {
                const ExecScope scope(o);
                st = o.dataSource.empty()
                         ? DBMW::execute(sql, affected)
                         : DBMW::execute(o.dataSource, sql, affected);
            }
            return st;
        }

        inline Status validateCreateRoutine(const bool replace, const bool ifNotExists,
                                            const Dialect d) {
            if (ifNotExists)
                return unsupported("dbmw::util: CREATE ROUTINE IF NOT EXISTS is supported by "
                    "no dialect in this matrix; drop first, then create");
            if (replace && d != Dialect::Postgres)
                return unsupported("dbmw::util: CREATE OR REPLACE is only supported by "
                                   "postgres (dialect=" + std::string(dialectName(d)) + ")");
            return Status::OK();
        }
    }

    inline Status createRoutine(const std::string &sql, const CreateRoutineOptions &opts = {}) {
        if (sql.empty()) return badSpec("dbmw::util: createRoutine sql is empty");
        const Dialect d = resolveDialect(opts);
        if (const auto s = detail::validateCreateRoutine(opts.replace, opts.ifNotExists, d);
            !s.ok())
            return s;
        std::string finalSql = sql;
        if (opts.stripDelimiter) {
            const std::size_t n = stripDelimiterDirectives(finalSql);
            if (n > 0)
                DBMW_LOG_INFO("dbmw::util: stripped " + std::to_string(n) +
                " DELIMITER directive(s) from routine DDL");
        }
        return detail::runDdl(opts, finalSql);
    }

    inline Status dropRoutine(const RoutineRef &ref, const DropRoutineOptions &opts = {}) {
        DropRoutineOptions o = opts;
        if (o.dataSource.empty()) o.dataSource = ref.dataSource;
        const Dialect d = resolveDialect(o);
        std::string sql;
        if (const auto s = makeDropRoutineSql(ref, o, d, sql); !s.ok()) return s;
        return detail::runDdl(o, sql);
    }

    inline Status createIndex(const IndexSpec &spec, const CreateIndexOptions &opts = {}) {
        const Dialect d = resolveDialect(opts);
        std::string sql;
        if (const auto s = makeCreateIndexSql(spec, d, sql); !s.ok()) return s;
        if (spec.concurrent && core::currentTransactionDepth() > 0)
            return Status::error(ErrorCode::TxError,
                                 "dbmw::util: CREATE INDEX CONCURRENTLY cannot run inside a "
                                 "transaction block");
        return detail::runDdl(opts, sql);
    }

    inline Status createIndexSql(const std::string &sql, const CreateIndexOptions &opts = {}) {
        if (sql.empty()) return badSpec("dbmw::util: createIndexSql sql is empty");
        return detail::runDdl(opts, sql);
    }

    inline Status dropIndex(const std::string &table, const std::string &name,
                            const DropIndexOptions &opts = {}) {
        const Dialect d = resolveDialect(opts);
        std::string sql;
        if (const auto s = makeDropIndexSql(table, name, opts.ifExists, d, sql); !s.ok()) return s;
        return detail::runDdl(opts, sql);
    }


    struct ScriptOptions : ExecOptions {
        bool recursive = true;
        std::string extension = ".sql";
        bool stopOnError = true;
    };

    struct ScriptResult {
        std::string path;
        Status status;
        std::size_t statements = 0;
        std::size_t executed = 0;
    };

    inline void splitSqlScript(const std::string &sql, std::vector<std::string> &out) {
        const std::size_t n = sql.size();
        std::vector<char> prot(n, 0);

        bool inS = false, inD = false, inLine = false, inBlock = false;
        for (std::size_t i = 0; i < n; ++i) {
            const char c = sql[i];
            if (inLine) {
                prot[i] = 1;
                if (c == '\n') inLine = false;
                continue;
            }
            if (inBlock) {
                prot[i] = 1;
                if (c == '*' && i + 1 < n && sql[i + 1] == '/') {
                    prot[i + 1] = 1;
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (inS) {
                prot[i] = 1;
                if (c == '\'') {
                    if (i + 1 < n && sql[i + 1] == '\'') {
                        prot[i + 1] = 1;
                        ++i;
                    } else inS = false;
                }
                continue;
            }
            if (inD) {
                prot[i] = 1;
                if (c == '"') {
                    if (i + 1 < n && sql[i + 1] == '"') {
                        prot[i + 1] = 1;
                        ++i;
                    } else inD = false;
                }
                continue;
            }
            if (c == '-' && i + 1 < n && sql[i + 1] == '-') {
                inLine = true;
                prot[i] = 1;
                prot[i + 1] = 1;
                ++i;
                continue;
            }
            if (c == '#') {
                inLine = true;
                prot[i] = 1;
                continue;
            }
            if (c == '/' && i + 1 < n && sql[i + 1] == '*') {
                inBlock = true;
                prot[i] = 1;
                prot[i + 1] = 1;
                ++i;
                continue;
            }
            if (c == '\'') {
                inS = true;
                prot[i] = 1;
                continue;
            }
            if (c == '"') {
                inD = true;
                prot[i] = 1;
                continue;
            }
        }

        enum Blk { B_BEGIN = 1, B_CASE, B_IF, B_LOOP, B_WHILE, B_REPEAT };
        std::vector<std::pair<int, std::size_t> > stack;
        std::size_t i = 0;
        auto isKw = [&](const std::size_t a, const std::size_t b, const char *kw) -> bool {
            const std::size_t len = std::char_traits<char>::length(kw);
            if (b - a != len) return false;
            for (std::size_t k = 0; k < len; ++k)
                if (std::tolower(static_cast<unsigned char>(sql[a + k])) != kw[k]) return false;
            return true;
        };
        while (i < n) {
            if (prot[i]) {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
            if (j == i) {
                ++i;
                continue;
            }
            if (isKw(i, j, "begin")) stack.emplace_back(B_BEGIN, j);
            else if (isKw(i, j, "case")) stack.emplace_back(B_CASE, j);
            else if (isKw(i, j, "if")) stack.emplace_back(B_IF, j);
            else if (isKw(i, j, "loop")) stack.emplace_back(B_LOOP, j);
            else if (isKw(i, j, "while")) stack.emplace_back(B_WHILE, j);
            else if (isKw(i, j, "repeat")) stack.emplace_back(B_REPEAT, j);
            else if (isKw(i, j, "end")) {
                if (!stack.empty()) {
                    const std::size_t from = stack.back().second;
                    for (std::size_t k = from; k <= j && k < n; ++k) prot[k] = 1;
                    stack.pop_back();
                }
            }
            i = j;
        }

        std::size_t start = 0;
        for (std::size_t k = 0; k <= n; ++k) {
            if (k == n || (sql[k] == ';' && !prot[k])) {
                std::string stmt = sql.substr(start, k - start);
                std::size_t a = 0, b = stmt.size();
                while (a < b && std::isspace(static_cast<unsigned char>(stmt[a]))) ++a;
                while (b > a && std::isspace(static_cast<unsigned char>(stmt[b - 1]))) --b;
                if (a < b) out.push_back(stmt.substr(a, b - a));
                start = k + 1;
            }
        }
    }

    inline bool readSqlFile(const std::string &path, std::string &out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    inline bool endsWith(const std::string &s, const std::string &suffix) {
        if (suffix.size() > s.size()) return false;
        return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    inline Status runScriptText(const std::string &sql, const ScriptOptions &opts = {},
                                std::size_t *executed = nullptr) {
        std::vector<std::string> stmts;
        splitSqlScript(sql, stmts);
        std::size_t done = 0;
        Status lastErr = Status::OK();
        for (const auto &s: stmts) {
            const auto st = detail::runDdl(opts, s);
            if (!st.ok()) {
                lastErr = st;
                if (opts.stopOnError) {
                    if (executed) *executed = done;
                    return st;
                }
            } else {
                ++done;
            }
        }
        if (executed) *executed = done;
        return lastErr;
    }

    inline Status runScripts(const std::vector<std::string> &files, const ScriptOptions &opts = {},
                             std::vector<ScriptResult> *perFile = nullptr) {
        Status lastErr = Status::OK();
        bool anyFail = false;
        for (const auto &f: files) {
            ScriptResult r;
            r.path = f;
            std::string content;
            if (!readSqlFile(f, content)) {
                r.status = Status::error(ErrorCode::IoError,
                                         "dbmw::util: cannot read file: " + f);
                anyFail = true;
                if (perFile) perFile->push_back(r);
                if (opts.stopOnError) return r.status;
                lastErr = r.status;
                continue;
            }
            std::vector<std::string> stmts;
            splitSqlScript(content, stmts);
            r.statements = stmts.size();
            r.status = runScriptText(content, opts, &r.executed);
            if (!r.status.ok()) {
                anyFail = true;
                if (perFile) perFile->push_back(r);
                if (opts.stopOnError) return r.status;
                lastErr = r.status;
                continue;
            }
            if (perFile) perFile->push_back(r);
        }
        return anyFail ? lastErr : Status::OK();
    }

    inline Status runScriptsInDir(const std::string &dir, const ScriptOptions &opts = {},
                                  std::vector<ScriptResult> *perFile = nullptr) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            return Status::error(ErrorCode::IoError, "dbmw::util: directory not found: " + dir);
        std::vector<std::string> files;
        const auto collect = [&](const std::string &p) {
            std::string ext = opts.extension.empty() ? ".sql" : opts.extension;
            if (endsWith(p, ext)) files.push_back(p);
        };
        if (opts.recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (ec) break;
                std::error_code e2;
                if (it->is_regular_file(e2)) collect(it->path().string());
            }
        } else {
            for (auto it = std::filesystem::directory_iterator(dir, ec);
                 it != std::filesystem::directory_iterator(); ++it) {
                if (ec) break;
                std::error_code e2;
                if (it->is_regular_file(e2)) collect(it->path().string());
            }
        }
        if (ec)
            return Status::error(ErrorCode::IoError,
                                 "dbmw::util: failed to walk directory " + dir + ": " + ec.message());
        std::sort(files.begin(), files.end());
        return runScripts(files, opts, perFile);
    }

    inline Status call(const std::string &sql, const Params &params,
                       std::int64_t &affected, const CallOptions &opts = {}) {
        affected = 0;
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? DBMW::execute(sql, params, affected)
                   : DBMW::execute(opts.dataSource, sql, params, affected);
    }

    inline Status callQuery(const std::string &sql, const Params &params,
                            ResultSet &out, const CallOptions &opts = {}) {
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? DBMW::query(sql, params, out)
                   : DBMW::query(opts.dataSource, sql, params, out);
    }

    inline Status callEach(const std::string &sql, const Params &params,
                           const RowCallback &cb, std::uint64_t &rows,
                           const CallOptions &opts = {}) {
        rows = 0;
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? DBMW::queryEach(sql, params, cb, rows)
                   : DBMW::queryEach(opts.dataSource, sql, params, cb, rows);
    }

    inline Status call(core::Session &s, const std::string &sql, const Params &params,
                       std::int64_t &affected) {
        return s.execute(sql, params, affected);
    }

    inline Status callQuery(core::Session &s, const std::string &sql, const Params &params,
                            ResultSet &out) {
        return s.query(sql, params, out);
    }

    inline Status call(const RoutineRef &ref, const CallParams &params, CallResult &out,
                       const CallOptions &opts = {}) {
        out = CallResult{};
        CallOptions o = opts;
        if (o.dataSource.empty()) o.dataSource = ref.dataSource;
        const Dialect d = resolveDialect(o);
        CallPlan plan;
        if (const auto s = makeCallPlan(ref, params, d, o.returnsRows, plan); !s.ok()) {
            out.status = s;
            return s;
        }
        if (plan.needsSameConnection) {
            out.status = unsupported("dbmw::util: OUT/INOUT parameters on dialect=" +
                                     std::string(dialectName(d)) + " must run on one connection; use the "
                                     "core::Session overload");
            return out.status;
        }
        const detail::ExecScope scope(o);
        if (o.returnsRows) {
            std::vector<ResultSet> sets;
            out.status = o.dataSource.empty()
                             ? DBMW::queryAll(plan.callSql, plan.callParams, sets)
                             : DBMW::queryAll(o.dataSource, plan.callSql, plan.callParams, sets);
            out.sets = std::move(sets);
        } else {
            out.status = o.dataSource.empty()
                             ? DBMW::execute(plan.callSql, plan.callParams, out.affected)
                             : DBMW::execute(o.dataSource, plan.callSql, plan.callParams,
                                             out.affected);
        }
        if (!out.status.ok()) return out.status;
        if (plan.outFromRowCount > 0 && !out.sets.empty())
            out.status = readOutParamsFromRow(out.sets.front(), plan.outFromRowCount,
                                              out.outParams);
        return out.status;
    }

    inline Status call(core::Session &s, const RoutineRef &ref, const CallParams &params,
                       CallResult &out, CallOptions opts = {}) {
        out = CallResult{};
        opts.dialect = opts.dialect != Dialect::Auto
                           ? opts.dialect
                           : detectDialect(
                               opts.dataSource.empty() ? ref.dataSource : opts.dataSource);
        CallPlan plan;
        if (const auto st = makeCallPlan(ref, params, opts.dialect, opts.returnsRows, plan);
            !st.ok()) {
            out.status = st;
            return st;
        }
        if (!plan.preSql.empty()) {
            std::int64_t pre = 0;
            if (const auto st = s.execute(plan.preSql, plan.preParams, pre); !st.ok()) {
                out.status = st;
                return st;
            }
        }
        if (opts.returnsRows) out.status = s.queryAll(plan.callSql, plan.callParams, out.sets);
        else out.status = s.execute(plan.callSql, plan.callParams, out.affected);
        if (!out.status.ok()) return out.status;

        if (plan.outFromRowCount > 0 && !out.sets.empty()) {
            if (const auto st = readOutParamsFromRow(out.sets.front(), plan.outFromRowCount,
                                                     out.outParams); !st.ok()) {
                out.status = st;
                return st;
            }
        }
        if (!plan.fetchSql.empty()) {
            ResultSet rs;
            if (const auto st = s.query(plan.fetchSql, rs); !st.ok()) {
                out.status = st;
                return st;
            }
            out.status = readOutParams(rs, plan.outColumns, out.outParams);
        }
        return out.status;
    }
}

namespace dbmw::async::util {
    struct Options : async::Options {
        std::string dataSource;
        common::util::Dialect dialect = common::util::Dialect::Auto;
        bool forcePrimary = true;
        common::Idempotency idempotency = common::Idempotency::NonIdempotent;
        bool replace = false;
        bool ifNotExists = false;
        bool stripDelimiter = true;
    };

    namespace detail {
        inline common::util::ExecOptions toExec(const Options &o) {
            common::util::ExecOptions e;
            e.dataSource = o.dataSource;
            e.dialect = o.dialect;
            e.forcePrimary = o.forcePrimary;
            e.idempotency = o.idempotency;
            return e;
        }

        inline common::util::CreateRoutineOptions toCreate(const Options &o) {
            common::util::CreateRoutineOptions c;
            c.dataSource = o.dataSource;
            c.dialect = o.dialect;
            c.forcePrimary = o.forcePrimary;
            c.idempotency = o.idempotency;
            c.replace = o.replace;
            c.ifNotExists = o.ifNotExists;
            c.stripDelimiter = o.stripDelimiter;
            return c;
        }

        inline void failOp(const OpCallback &cb, const common::Status &st) {
            if (!cb) return;
            OpResult r;
            r.status = st;
            cb(std::move(r));
        }

        inline void failExec(const ExecCallback &cb, const common::Status &st) {
            if (!cb) return;
            ExecResult r;
            r.status = st;
            cb(std::move(r));
        }

        inline void failQuery(const QueryCallback &cb, const common::Status &st) {
            if (!cb) return;
            QueryResult r;
            r.status = st;
            cb(std::move(r));
        }

        inline ExecCallback asExec(OpCallback cb) {
            return [cb = std::move(cb)](ExecResult &&r) {
                if (!cb) return;
                OpResult o;
                o.status = std::move(r.status);
                cb(std::move(o));
            };
        }
    }

    inline Handle call(std::string sql, const common::Params &params, ExecCallback cb,
                       Options opts = {}) {
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::execute(std::move(sql), params, std::move(cb), opts)
                   : async::execute(opts.dataSource, std::move(sql), params, std::move(cb), opts);
    }

    inline Handle callQuery(std::string sql, const common::Params &params, QueryCallback cb,
                            Options opts = {}) {
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::query(std::move(sql), params, std::move(cb), opts)
                   : async::query(opts.dataSource, std::move(sql), params, std::move(cb), opts);
    }

    inline Handle callAll(std::string sql, const common::Params &params,
                          MultiQueryCallback cb, Options opts = {}) {
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::queryAll(std::move(sql), params, std::move(cb), opts)
                   : async::queryAll(opts.dataSource, std::move(sql), params, std::move(cb), opts);
    }

    inline Handle call(const common::util::RoutineRef &ref,
                       const common::util::CallParams &params,
                       MultiQueryCallback cb, Options opts = {}) {
        if (opts.dataSource.empty()) opts.dataSource = ref.dataSource;
        const auto d = opts.dialect != common::util::Dialect::Auto
                           ? opts.dialect
                           : common::util::detectDialect(opts.dataSource);
        common::util::CallPlan plan;
        if (const auto s = common::util::makeCallPlan(ref, params, d, true, plan); !s.ok()) {
            if (cb) {
                MultiQueryResult r;
                r.status = s;
                cb(std::move(r));
            }
            return Handle();
        }
        if (plan.needsSameConnection || plan.outFromRowCount > 0) {
            if (cb) {
                MultiQueryResult r;
                r.status = common::util::unsupported(
                    "dbmw::util: OUT/INOUT parameters are not supported on the async path; "
                    "use the synchronous core::Session overload");
                cb(std::move(r));
            }
            return Handle();
        }
        return callAll(std::move(plan.callSql), plan.callParams, std::move(cb), opts);
    }

    inline Handle createRoutine(std::string sql, OpCallback cb, Options opts = {}) {
        if (sql.empty()) {
            detail::failOp(cb, common::util::badSpec("dbmw::util: createRoutine sql is empty"));
            return {};
        }
        const auto d = opts.dialect != common::util::Dialect::Auto
                           ? opts.dialect
                           : common::util::detectDialect(opts.dataSource);
        if (const auto s = common::util::detail::validateCreateRoutine(opts.replace,
                                                                       opts.ifNotExists, d);
            !s.ok()) {
            detail::failOp(cb, s);
            return {};
        }
        if (opts.stripDelimiter) {
            if (const std::size_t n = common::util::stripDelimiterDirectives(sql); n > 0)
                DBMW_LOG_INFO("dbmw::util: stripped " + std::to_string(n) +
                " DELIMITER directive(s) from routine DDL");
        }
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        std::string finalSql = std::move(sql);
        return opts.dataSource.empty()
                   ? async::execute(finalSql, detail::asExec(std::move(cb)), opts)
                   : async::execute(opts.dataSource, finalSql,
                                    detail::asExec(std::move(cb)), opts);
    }

    inline Handle dropRoutine(common::util::RoutineRef ref, OpCallback cb, Options opts = {}) {
        if (opts.dataSource.empty()) opts.dataSource = ref.dataSource;
        const auto d = opts.dialect != common::util::Dialect::Auto
                           ? opts.dialect
                           : common::util::detectDialect(opts.dataSource);
        std::string sql;
        common::util::DropRoutineOptions dro;
        dro.dataSource = opts.dataSource;
        dro.dialect = d;
        dro.ifExists = true;
        if (const auto s = common::util::makeDropRoutineSql(ref, dro, d, sql); !s.ok()) {
            detail::failOp(cb, s);
            return Handle();
        }
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::execute(sql, detail::asExec(std::move(cb)), opts)
                   : async::execute(opts.dataSource, sql, detail::asExec(std::move(cb)), opts);
    }

    inline Handle createIndex(const common::util::IndexSpec &spec, OpCallback cb,
                              Options opts = {}) {
        const auto d = opts.dialect != common::util::Dialect::Auto
                           ? opts.dialect
                           : common::util::detectDialect(opts.dataSource);
        std::string sql;
        if (const auto s = common::util::makeCreateIndexSql(spec, d, sql); !s.ok()) {
            detail::failOp(cb, s);
            return Handle();
        }
        if (spec.concurrent && core::currentTransactionDepth() > 0) {
            detail::failOp(cb, common::Status::error(
                               common::ErrorCode::TxError,
                               "dbmw::util: CREATE INDEX CONCURRENTLY cannot run inside a transaction block"));
            return Handle();
        }
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::execute(sql, detail::asExec(std::move(cb)), opts)
                   : async::execute(opts.dataSource, sql, detail::asExec(std::move(cb)), opts);
    }

    inline Handle createIndexSql(std::string sql, OpCallback cb, Options opts = {}) {
        if (sql.empty()) {
            detail::failOp(cb, common::util::badSpec("dbmw::util: createIndexSql sql is empty"));
            return Handle();
        }
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::execute(std::move(sql), detail::asExec(std::move(cb)), opts)
                   : async::execute(opts.dataSource, std::move(sql),
                                    detail::asExec(std::move(cb)), opts);
    }

    inline Handle dropIndex(const std::string &table, const std::string &name, OpCallback cb,
                            Options opts = {}) {
        const auto d = opts.dialect != common::util::Dialect::Auto
                           ? opts.dialect
                           : common::util::detectDialect(opts.dataSource);
        std::string sql;
        if (const auto s = common::util::makeDropIndexSql(table, name, true, d, sql); !s.ok()) {
            detail::failOp(cb, s);
            return Handle();
        }
        const common::util::detail::ExecScope scope(detail::toExec(opts));
        return opts.dataSource.empty()
                   ? async::execute(sql, detail::asExec(std::move(cb)), opts)
                   : async::execute(opts.dataSource, sql, detail::asExec(std::move(cb)), opts);
    }

    inline std::future<ExecResult> call(std::string sql, common::Params params,
                                        Options opts = {}) {
        auto p = std::make_shared<std::promise<ExecResult> >();
        auto fut = p->get_future();
        call(std::move(sql), params, [p](ExecResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<QueryResult> callQuery(std::string sql, common::Params params,
                                              Options opts = {}) {
        auto p = std::make_shared<std::promise<QueryResult> >();
        auto fut = p->get_future();
        callQuery(std::move(sql), params,
                  [p](QueryResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<MultiQueryResult> callAll(std::string sql, common::Params params,
                                                 Options opts = {}) {
        auto p = std::make_shared<std::promise<MultiQueryResult> >();
        auto fut = p->get_future();
        callAll(std::move(sql), params,
                [p](MultiQueryResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<OpResult> createRoutine(std::string sql, Options opts = {}) {
        auto p = std::make_shared<std::promise<OpResult> >();
        auto fut = p->get_future();
        createRoutine(std::move(sql), [p](OpResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<OpResult> dropRoutine(common::util::RoutineRef ref, Options opts = {}) {
        auto p = std::make_shared<std::promise<OpResult> >();
        auto fut = p->get_future();
        dropRoutine(std::move(ref), [p](OpResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<OpResult> createIndex(const common::util::IndexSpec &spec,
                                             Options opts = {}) {
        auto p = std::make_shared<std::promise<OpResult> >();
        auto fut = p->get_future();
        createIndex(spec, [p](OpResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<OpResult> createIndexSql(std::string sql, Options opts = {}) {
        auto p = std::make_shared<std::promise<OpResult> >();
        auto fut = p->get_future();
        createIndexSql(std::move(sql), [p](OpResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }

    inline std::future<OpResult> dropIndex(const std::string &table, const std::string &name,
                                           Options opts = {}) {
        auto p = std::make_shared<std::promise<OpResult> >();
        auto fut = p->get_future();
        dropIndex(table, name, [p](OpResult &&r) { p->set_value(std::move(r)); }, opts);
        return fut;
    }


    namespace detail {
        struct ScriptRunState {
            std::vector<std::string> files;
            common::util::ScriptOptions opts;
            std::size_t fileIndex = 0;
            std::vector<common::util::ScriptResult> results;
            bool useText = false;
            ExecCallback userCb;
        };

        inline void scriptRunStatement(std::shared_ptr<ScriptRunState> st,
                                       std::string path,
                                       std::vector<std::string> stmts,
                                       std::size_t idx, std::size_t executed);

        inline void scriptRunFile(std::shared_ptr<ScriptRunState> st);

        inline void scriptRunStatement(std::shared_ptr<ScriptRunState> st,
                                       std::string path,
                                       std::vector<std::string> stmts,
                                       std::size_t idx, std::size_t executed) {
            if (idx >= stmts.size()) {
                common::util::ScriptResult fr;
                fr.path = std::move(path);
                fr.statements = stmts.size();
                fr.executed = executed;
                fr.status = common::Status::OK();
                st->results.push_back(std::move(fr));
                if (st->useText) {
                    ExecResult r;
                    if (st->userCb) st->userCb(std::move(r));
                } else {
                    scriptRunFile(st);
                }
                return;
            }
            std::string s = stmts[idx];
            const common::util::ScriptOptions opts = st->opts;
            const std::string ds = opts.dataSource;
            auto cb = [st, path, stmts, idx, executed](ExecResult &&r) mutable {
                if (!r.status.ok()) {
                    common::util::ScriptResult fr;
                    fr.path = path;
                    fr.statements = stmts.size();
                    fr.executed = executed;
                    fr.status = r.status;
                    st->results.push_back(std::move(fr));
                    if (st->opts.stopOnError) {
                        ExecResult out;
                        out.status = r.status;
                        if (st->userCb) st->userCb(std::move(out));
                        return;
                    }
                    scriptRunStatement(st, std::move(path), std::move(stmts), idx + 1, executed);
                } else {
                    scriptRunStatement(st, std::move(path), std::move(stmts), idx + 1, executed + 1);
                }
            };
            const common::util::detail::ExecScope scope(opts);
            if (ds.empty()) async::execute(std::move(s), common::Params{}, std::move(cb), async::Options{});
            else async::execute(ds, std::move(s), common::Params{}, std::move(cb), async::Options{});
        }

        inline void scriptRunFile(std::shared_ptr<ScriptRunState> st) {
            if (st->fileIndex >= st->files.size()) {
                ExecResult r;
                for (const auto &fr: st->results)
                    if (!fr.status.ok()) {
                        r.status = fr.status;
                        break;
                    }
                if (st->userCb) st->userCb(std::move(r));
                return;
            }
            const std::string path = st->files[st->fileIndex++];
            std::string content;
            if (!common::util::readSqlFile(path, content)) {
                common::util::ScriptResult fr;
                fr.path = path;
                fr.status = common::Status::error(common::ErrorCode::IoError, "dbmw::util: cannot read file: " + path);
                st->results.push_back(std::move(fr));
                if (st->opts.stopOnError) {
                    ExecResult out;
                    out.status = fr.status;
                    if (st->userCb) st->userCb(std::move(out));
                    return;
                }
                scriptRunFile(st);
                return;
            }
            std::vector<std::string> stmts;
            common::util::splitSqlScript(content, stmts);
            scriptRunStatement(st, path, std::move(stmts), 0, 0);
        }
    }

    inline Handle runScriptText(std::string sql, ExecCallback cb, common::util::ScriptOptions opts = {}) {
        auto st = std::make_shared<detail::ScriptRunState>();
        st->useText = true;
        st->opts = std::move(opts);
        st->userCb = std::move(cb);
        std::vector<std::string> stmts;
        common::util::splitSqlScript(sql, stmts);
        detail::scriptRunStatement(st, "<text>", std::move(stmts), 0, 0);
        return Handle();
    }

    inline Handle runScripts(const std::vector<std::string> &files, ExecCallback cb,
                             common::util::ScriptOptions opts = {}) {
        auto st = std::make_shared<detail::ScriptRunState>();
        st->files = files;
        st->opts = std::move(opts);
        st->userCb = std::move(cb);
        detail::scriptRunFile(st);
        return Handle();
    }

    inline Handle runScriptsInDir(const std::string &dir, ExecCallback cb,
                                  common::util::ScriptOptions opts = {}) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) {
            ExecResult r;
            r.status = common::Status::error(common::ErrorCode::IoError, "dbmw::util: directory not found: " + dir);
            if (cb) cb(std::move(r));
            return Handle();
        }
        std::vector<std::string> files;
        const auto collect = [&](const std::string &p) {
            const std::string ext = opts.extension.empty() ? ".sql" : opts.extension;
            if (common::util::endsWith(p, ext)) files.push_back(p);
        };
        if (opts.recursive) {
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 it != std::filesystem::recursive_directory_iterator(); ++it) {
                if (ec) break;
                std::error_code e2;
                if (it->is_regular_file(e2)) collect(it->path().string());
            }
        } else {
            for (auto it = std::filesystem::directory_iterator(dir, ec);
                 it != std::filesystem::directory_iterator(); ++it) {
                if (ec) break;
                std::error_code e2;
                if (it->is_regular_file(e2)) collect(it->path().string());
            }
        }
        if (ec) {
            ExecResult r;
            r.status = common::Status::error(common::ErrorCode::IoError,
                                             "dbmw::util: failed to walk directory " + dir + ": " + ec.message());
            if (cb) cb(std::move(r));
            return Handle();
        }
        std::sort(files.begin(), files.end());
        return runScripts(files, std::move(cb), opts);
    }

    inline std::future<ExecResult> runScriptText(std::string sql, common::util::ScriptOptions opts = {}) {
        auto p = std::make_shared<std::promise<ExecResult> >();
        auto fut = p->get_future();
        runScriptText(std::move(sql), [p](ExecResult &&r) { p->set_value(std::move(r)); }, std::move(opts));
        return fut;
    }

    inline std::future<ExecResult> runScripts(const std::vector<std::string> &files,
                                              common::util::ScriptOptions opts = {}) {
        auto p = std::make_shared<std::promise<ExecResult> >();
        auto fut = p->get_future();
        runScripts(files, [p](ExecResult &&r) { p->set_value(std::move(r)); }, std::move(opts));
        return fut;
    }

    inline std::future<ExecResult> runScriptsInDir(const std::string &dir,
                                                   common::util::ScriptOptions opts = {}) {
        auto p = std::make_shared<std::promise<ExecResult> >();
        auto fut = p->get_future();
        runScriptsInDir(dir, [p](ExecResult &&r) { p->set_value(std::move(r)); }, std::move(opts));
        return fut;
    }
}

#if defined(DBMW_ENABLE_ASYNC_CORO)

namespace dbmw::async::util {
    inline Task<ExecResult> callAsync(std::string sql, common::Params params = {},
                                      Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<ExecResult>;
        co_return co_await Awaiter(
            [sql = std::move(sql), params = std::move(params), opts]
    (typename Awaiter::Callback cb) mutable {
                call(sql, params, std::move(cb), opts);
            });
    }

    inline Task<QueryResult> callQueryAsync(std::string sql, common::Params params = {},
                                            Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<QueryResult>;
        co_return co_await Awaiter(
            [sql = std::move(sql), params = std::move(params), opts]
    (typename Awaiter::Callback cb) mutable {
                callQuery(sql, params, std::move(cb), opts);
            });
    }

    inline Task<MultiQueryResult> callAllAsync(std::string sql, common::Params params = {},
                                               Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<MultiQueryResult>;
        co_return co_await Awaiter(
            [sql = std::move(sql), params = std::move(params), opts]
    (typename Awaiter::Callback cb) mutable {
                callAll(sql, params, std::move(cb), opts);
            });
    }

    inline Task<OpResult> createRoutineAsync(std::string sql, Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<OpResult>;
        co_return co_await Awaiter(
            [sql = std::move(sql), opts](typename Awaiter::Callback cb) mutable {
                createRoutine(sql, std::move(cb), opts);
            });
    }

    inline Task<OpResult> dropRoutineAsync(common::util::RoutineRef ref, Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<OpResult>;
        co_return co_await Awaiter(
            [ref = std::move(ref), opts](typename Awaiter::Callback cb) mutable {
                dropRoutine(ref, std::move(cb), opts);
            });
    }

    inline Task<OpResult> createIndexAsync(common::util::IndexSpec spec, Options opts = {}) {
        using Awaiter = async::detail::OpAwaiter<OpResult>;
        co_return co_await Awaiter(
            [spec = std::move(spec), opts](typename Awaiter::Callback cb) mutable {
                createIndex(spec, std::move(cb), opts);
            });
    }

    inline Task<ExecResult> runScriptTextAsync(std::string sql, common::util::ScriptOptions opts = {}) {
        using Awaiter = async::detail::OpAwaiter<ExecResult>;
        std::vector<std::string> stmts;
        common::util::splitSqlScript(sql, stmts);
        ExecResult res;
        for (const auto &s: stmts) {
            ExecResult r = co_await Awaiter(
                [s, opts](typename Awaiter::Callback cb) mutable {
                    const common::util::detail::ExecScope scope(opts);
                    if (opts.dataSource.empty())
                        async::execute(s, common::Params{}, std::move(cb), async::Options{});
                    else
                        async::execute(opts.dataSource, s, common::Params{}, std::move(cb), async::Options{});
                });
            if (!r.status.ok()) {
                res.status = r.status;
                if (opts.stopOnError) co_return res;
            }
        }
        co_return res;
    }

    inline Task<ExecResult> runScriptsAsync(std::vector<std::string> files,
                                            common::util::ScriptOptions opts = {}) {
        using Awaiter = async::detail::OpAwaiter<ExecResult>;
        co_return co_await Awaiter(
            [files = std::move(files), opts](typename Awaiter::Callback cb) mutable {
                runScripts(files, std::move(cb), opts);
            });
    }

    inline Task<ExecResult> runScriptsInDirAsync(std::string dir, common::util::ScriptOptions opts = {}) {
        using Awaiter = async::detail::OpAwaiter<ExecResult>;
        co_return co_await Awaiter(
            [dir = std::move(dir), opts](typename Awaiter::Callback cb) mutable {
                runScriptsInDir(dir, std::move(cb), opts);
            });
    }
}

#endif

#endif
