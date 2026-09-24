#ifndef SQLCONDUIT_UTIL_H
#define SQLCONDUIT_UTIL_H

#include "sqlconduit/common/context.h"
#include "sqlconduit/common/logger.h"
#include "sqlconduit/common/sql_dialect.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/core/query_cache.h"
#include "sqlconduit/client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace sqlconduit::common::util {
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

    using ParamDirection = common::ParamDirection;
    using CallValueType = common::ValueType;
    using CallParam = common::CallParam;
    using CallParams = common::CallParams;

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
        return "sqlconduit_out_" + std::to_string(index);
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
            case Dialect::Oracle: return "oracle";
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

    inline Dialect dialectFromDriverType(const std::string &driverType) {
        const std::string t = toLowerAscii(driverType);
        if (t.find("mysql") != std::string::npos || t.find("maria") != std::string::npos)
            return Dialect::MySQL;
        if (t.find("postgres") != std::string::npos || t.find("pgsql") != std::string::npos ||
            t == "pg")
            return Dialect::Postgres;
        if (t.find("oracle") != std::string::npos || t == "ora" || t == "oci")
            return Dialect::Oracle;
        if (t.find("odbc") != std::string::npos || t.find("sqlserver") != std::string::npos ||
            t.find("mssql") != std::string::npos)
            return Dialect::SqlServer;
        return Dialect::Auto;
    }

    inline Dialect detectDialect(Client &client, const std::string &dataSource) {
        const auto ds = client.dataSource(dataSource);
        return ds ? dialectFromDriverType(ds->driverType()) : Dialect::Auto;
    }

    inline Dialect resolveDialect(Client &client, const ExecOptions &o) {
        return o.dialect != Dialect::Auto
                   ? o.dialect
                   : detectDialect(client, o.dataSource);
    }

    inline std::string quoteStringLiteral(const std::string &value) {
        std::string out = "'";
        out.reserve(value.size() + 2);
        for (const char c: value) {
            if (c == '\'') out.push_back('\'');
            out.push_back(c);
        }
        out.push_back('\'');
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
        if (ref.name.empty()) return badSpec("sqlconduit::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("sqlconduit::util: cannot detect dialect for datasource '" +
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
                            "sqlconduit::util: postgres procedures cannot return a result set; "
                            "use a function with makeCallSql");
                    out = "CALL " + name + parenArgs(argCount);
                }
                return Status::OK();
            case Dialect::SqlServer: {
                const std::string qualified = ref.name.find('.') == std::string::npos
                                                  ? "dbo." + ref.name
                                                  : ref.name;
                if (ref.kind == RoutineKind::Function)
                    out = "SELECT " + qualified + parenArgs(argCount);
                else
                    out = returnsRows
                              ? "{CALL " + qualified + parenArgs(argCount) + "}"
                              : "EXEC " + qualified + (argCount == 0 ? std::string() : " " + csvArgs(argCount));
                return Status::OK();
            }
            case Dialect::Oracle:
                if (ref.kind == RoutineKind::Function)
                    out = returnsRows
                              ? "SELECT * FROM TABLE(" + name + parenArgs(argCount) + ")"
                              : "SELECT " + name + parenArgs(argCount) + " FROM DUAL";
                else {
                    if (returnsRows)
                        return unsupported(
                            "sqlconduit::util: oracle procedures can only return rows through a REF "
                            "CURSOR OUT parameter, which makeCallSql cannot bind");
                    out = "BEGIN " + name + parenArgs(argCount) + "; END;";
                }
                return Status::OK();
            case Dialect::Auto: break;
        }
        return unsupported("sqlconduit::util: unknown dialect");
    }

    inline Status makeCallPlan(const RoutineRef &ref, const CallParams &params,
                               const Dialect d, const bool returnsRows, CallPlan &out) {
        out = CallPlan{};
        if (ref.name.empty()) return badSpec("sqlconduit::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("sqlconduit::util: cannot detect dialect for datasource '" +
                               ref.dataSource + "'; pass Dialect explicitly");

        const std::string name = quoteIdent(ref.name, d);
        const bool hasOut = hasOutParams(params);

        if (d == Dialect::SqlServer && hasOut)
            return unsupported("sqlconduit::util: sqlserver OUT/INOUT parameters need DECLARE @var "
                "<type> before EXEC ... OUTPUT, and Client cannot infer the type; return the "
                "values as a result set instead");

        if (d == Dialect::Oracle && hasOut)
            return unsupported("sqlconduit::util: Oracle output binds are executed through "
                "Client::call()/util::call(), not represented by CallPlan");
        if (d == Dialect::Oracle && ref.kind == RoutineKind::Procedure && returnsRows)
            return unsupported("sqlconduit::util: Oracle procedure result sets require a REF CURSOR "
                "CallParam and Client::call()/util::call()");

        if (d == Dialect::Postgres && ref.kind == RoutineKind::Procedure) {
            if (returnsRows)
                return unsupported(
                    "sqlconduit::util: postgres procedures cannot return a result set; "
                    "use a function with makeCallSql");
            if (hasOut)
                return unsupported("sqlconduit::util: postgres procedures do not hand OUT/INOUT "
                    "parameters back to the client; use a function with OUT parameters");
        }
        if (d == Dialect::Postgres && ref.kind == RoutineKind::Function && hasOut && !returnsRows)
            return unsupported("sqlconduit::util: postgres functions with OUT/INOUT parameters must "
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
            const std::string qualified = ref.name.find('.') == std::string::npos
                                              ? "dbo." + ref.name
                                              : ref.name;
            if (ref.kind == RoutineKind::Function)
                out.callSql = "SELECT " + qualified + parenArgs(args.size());
            else
                out.callSql = returnsRows
                                  ? "{CALL " + qualified + parenArgs(args.size()) + "}"
                                  : "EXEC " + qualified + (args.empty()
                                                               ? std::string()
                                                               : " " + csvArgs(
                                                                     args.size()));
            return Status::OK();
        }
        if (d == Dialect::Oracle) {
            if (ref.kind == RoutineKind::Function)
                out.callSql = "SELECT " + name + parenArgs(args.size()) + " FROM DUAL";
            else
                out.callSql = "BEGIN " + name + parenArgs(args.size()) + "; END;";
            return Status::OK();
        }
        return unsupported("sqlconduit::util: unknown dialect");
    }

    inline Status readOutParams(const ResultSet &rs, const std::vector<std::string> &names,
                                std::vector<Value> &out) {
        out.clear();
        if (names.empty()) return Status::OK();
        if (rs.rows().empty())
            return Status::error(ErrorCode::QueryError,
                                 "sqlconduit::util: OUT parameter fetch returned no row");
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
                                 "sqlconduit::util: OUT parameter column '" + names[i] + "' is missing");
        }
        return Status::OK();
    }

    inline Status readOutParamsFromRow(const ResultSet &rs, const std::size_t count,
                                       std::vector<Value> &out) {
        out.clear();
        if (count == 0) return Status::OK();
        if (rs.rows().empty())
            return Status::error(ErrorCode::QueryError,
                                 "sqlconduit::util: routine returned no row to read OUT/INOUT "
                                 "parameters from");
        const auto &row = rs.rows().front();
        const auto &fields = rs.fields();
        if (fields.size() < count)
            return Status::error(ErrorCode::QueryError,
                                 "sqlconduit::util: routine returned " + std::to_string(fields.size()) +
                                 " column(s), expected at least " + std::to_string(count) +
                                 " for OUT/INOUT parameters");
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.push_back(row.at(fields[i]));
        return Status::OK();
    }

    inline Status makeDropRoutineSql(const RoutineRef &ref, const DropRoutineOptions &o,
                                     const Dialect d, std::string &out) {
        if (ref.name.empty()) return badSpec("sqlconduit::util: routine name is empty");
        if (d == Dialect::Auto)
            return unsupported("sqlconduit::util: cannot detect dialect for datasource '" +
                               ref.dataSource + "'; pass Dialect explicitly");
        if (o.cascade && d != Dialect::Postgres)
            return unsupported("sqlconduit::util: DROP ... CASCADE is only supported by postgres "
                               "(dialect=" + std::string(dialectName(d)) + ")");
        const char *kindWord = ref.kind == RoutineKind::Function ? "FUNCTION" : "PROCEDURE";
        std::string s = "DROP ";
        s += kindWord;
        if (o.ifExists && d != Dialect::Oracle) s += " IF EXISTS";
        s += ' ';
        s += quoteIdent(ref.name, d);
        if (o.cascade) s += " CASCADE";
        if (o.ifExists && d == Dialect::Oracle) {
            s = "BEGIN EXECUTE IMMEDIATE " + quoteStringLiteral(s) +
                "; EXCEPTION WHEN OTHERS THEN IF SQLCODE != -4043 THEN RAISE; END IF; END;";
        }
        out = std::move(s);
        return Status::OK();
    }

    inline Status makeCreateIndexSql(const IndexSpec &spec, const Dialect d, std::string &out) {
        if (d == Dialect::Auto)
            return unsupported("sqlconduit::util: cannot detect dialect for datasource; "
                "pass Dialect explicitly");
        if (spec.table.empty() || spec.name.empty())
            return badSpec("sqlconduit::util: index spec requires both table and name");
        if (spec.columns.empty())
            return badSpec("sqlconduit::util: index spec requires at least one column");
        if (spec.ifNotExists && d != Dialect::Postgres)
            return unsupported("sqlconduit::util: CREATE INDEX IF NOT EXISTS is only supported by "
                               "postgres (dialect=" + std::string(dialectName(d)) + ")");
        if (spec.concurrent && d != Dialect::Postgres)
            return unsupported("sqlconduit::util: CREATE INDEX CONCURRENTLY is only supported by "
                               "postgres (dialect=" + std::string(dialectName(d)) + ")");
        if (!spec.usingMethod.empty() && (d == Dialect::SqlServer || d == Dialect::Oracle))
            return unsupported("sqlconduit::util: dialect=" + std::string(dialectName(d))
                               + " has no USING clause for CREATE INDEX");

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
            return unsupported("sqlconduit::util: cannot detect dialect for datasource; "
                "pass Dialect explicitly");
        if (table.empty() || name.empty())
            return badSpec("sqlconduit::util: dropIndex requires both table and name");
        std::string s = "DROP INDEX ";
        if (ifExists) s += "IF EXISTS ";
        s += quoteIdent(name, d);
        if (d != Dialect::Postgres && d != Dialect::Oracle) s += " ON " + quoteIdent(table, d);
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

        inline Status runDdl(Client &client, const ExecOptions &o,
                             const std::string &sql) {
            std::int64_t affected = 0;
            Status st;
            {
                const ExecScope scope(o);
                st = o.dataSource.empty()
                         ? client.execute(sql, affected)
                         : client.execute(o.dataSource, sql, affected);
            }
            return st;
        }

        inline Status validateCreateRoutine(const bool replace, const bool ifNotExists,
                                            const Dialect d) {
            if (ifNotExists)
                return unsupported("sqlconduit::util: CREATE ROUTINE IF NOT EXISTS is supported by "
                    "no dialect in this matrix; drop first, then create");
            if (replace && d != Dialect::Postgres && d != Dialect::Oracle)
                return unsupported("sqlconduit::util: CREATE OR REPLACE is only supported by "
                                   "postgres and oracle (dialect=" + std::string(dialectName(d)) + ")");
            return Status::OK();
        }
    }

    inline Status createRoutine(Client &client, const std::string &sql,
                                const CreateRoutineOptions &opts = {}) {
        if (sql.empty()) return badSpec("sqlconduit::util: createRoutine sql is empty");
        const Dialect d = resolveDialect(client, opts);
        if (const auto s = detail::validateCreateRoutine(opts.replace, opts.ifNotExists, d);
            !s.ok())
            return s;
        std::string finalSql = sql;
        if (opts.stripDelimiter) {
            const std::size_t n = stripDelimiterDirectives(finalSql);
            if (n > 0)
                SQLCONDUIT_LOG_INFO("sqlconduit::util: stripped " + std::to_string(n) +
                " DELIMITER directive(s) from routine DDL");
        }
        return detail::runDdl(client, opts, finalSql);
    }

    inline Status dropRoutine(Client &client, const RoutineRef &ref,
                              const DropRoutineOptions &opts = {}) {
        DropRoutineOptions o = opts;
        if (o.dataSource.empty()) o.dataSource = ref.dataSource;
        const Dialect d = resolveDialect(client, o);
        std::string sql;
        if (const auto s = makeDropRoutineSql(ref, o, d, sql); !s.ok()) return s;
        return detail::runDdl(client, o, sql);
    }

    inline Status createIndex(Client &client, const IndexSpec &spec,
                              const CreateIndexOptions &opts = {}) {
        const Dialect d = resolveDialect(client, opts);
        std::string sql;
        if (const auto s = makeCreateIndexSql(spec, d, sql); !s.ok()) return s;
        if (spec.concurrent && core::currentTransactionDepth() > 0)
            return Status::error(ErrorCode::TxError,
                                 "sqlconduit::util: CREATE INDEX CONCURRENTLY cannot run inside a "
                                 "transaction block");
        return detail::runDdl(client, opts, sql);
    }

    inline Status createIndexSql(Client &client, const std::string &sql,
                                 const CreateIndexOptions &opts = {}) {
        if (sql.empty()) return badSpec("sqlconduit::util: createIndexSql sql is empty");
        return detail::runDdl(client, opts, sql);
    }

    inline Status dropIndex(Client &client, const std::string &table,
                            const std::string &name,
                            const DropIndexOptions &opts = {}) {
        const Dialect d = resolveDialect(client, opts);
        std::string sql;
        if (const auto s = makeDropIndexSql(table, name, opts.ifExists, d, sql); !s.ok()) return s;
        return detail::runDdl(client, opts, sql);
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

    inline Status runScriptText(Client &client, const std::string &sql,
                                const ScriptOptions &opts = {},
                                std::size_t *executed = nullptr) {
        std::vector<std::string> stmts;
        splitSqlScript(sql, stmts);
        std::size_t done = 0;
        Status lastErr = Status::OK();
        for (const auto &s: stmts) {
            const auto st = detail::runDdl(client, opts, s);
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

    inline Status runScripts(Client &client, const std::vector<std::string> &files,
                             const ScriptOptions &opts = {},
                             std::vector<ScriptResult> *perFile = nullptr) {
        Status lastErr = Status::OK();
        bool anyFail = false;
        for (const auto &f: files) {
            ScriptResult r;
            r.path = f;
            std::string content;
            if (!readSqlFile(f, content)) {
                r.status = Status::error(ErrorCode::IoError,
                                         "sqlconduit::util: cannot read file: " + f);
                anyFail = true;
                if (perFile) perFile->push_back(r);
                if (opts.stopOnError) return r.status;
                lastErr = r.status;
                continue;
            }
            std::vector<std::string> stmts;
            splitSqlScript(content, stmts);
            r.statements = stmts.size();
            r.status = runScriptText(client, content, opts, &r.executed);
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

    inline Status runScriptsInDir(Client &client, const std::string &dir,
                                  const ScriptOptions &opts = {},
                                  std::vector<ScriptResult> *perFile = nullptr) {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
            return Status::error(ErrorCode::IoError, "sqlconduit::util: directory not found: " + dir);
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
                                 "sqlconduit::util: failed to walk directory " + dir + ": " + ec.message());
        std::sort(files.begin(), files.end());
        return runScripts(client, files, opts, perFile);
    }

    inline Status call(Client &client, const std::string &sql, const Params &params,
                       std::int64_t &affected, const CallOptions &opts = {}) {
        affected = 0;
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? client.execute(sql, params, affected)
                   : client.execute(opts.dataSource, sql, params, affected);
    }

    inline Status callQuery(Client &client, const std::string &sql,
                            const Params &params,
                            ResultSet &out, const CallOptions &opts = {}) {
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? client.query(sql, params, out)
                   : client.query(opts.dataSource, sql, params, out);
    }

    inline Status callEach(Client &client, const std::string &sql, const Params &params,
                           const RowCallback &cb, std::uint64_t &rows,
                           const CallOptions &opts = {}) {
        rows = 0;
        const detail::ExecScope scope(opts);
        return opts.dataSource.empty()
                   ? client.queryEach(sql, params, cb, rows)
                   : client.queryEach(opts.dataSource, sql, params, cb, rows);
    }

    inline Status call(core::Session &s, const std::string &sql, const Params &params,
                       std::int64_t &affected) {
        return s.execute(sql, params, affected);
    }

    inline Status callQuery(core::Session &s, const std::string &sql, const Params &params,
                            ResultSet &out) {
        return s.query(sql, params, out);
    }

    inline Status call(Client &client, const RoutineRef &ref,
                       const CallParams &params, CallResult &out,
                       const CallOptions &opts = {}) {
        out = CallResult{};
        CallOptions o = opts;
        if (o.dataSource.empty()) o.dataSource = ref.dataSource;
        const Dialect d = resolveDialect(client, o);
        if (d == Dialect::Oracle && ref.kind == RoutineKind::Procedure) {
            const bool hasRefCursor = std::any_of(params.begin(), params.end(), [](const auto &p) {
                return p.type == common::ValueType::RefCursor;
            });
            if (o.returnsRows && !hasRefCursor) {
                out.status = unsupported("sqlconduit::util: returnsRows=true for an Oracle procedure "
                    "requires CallParam::refCursor()");
                return out.status;
            }
            const std::string sql = "BEGIN " + quoteIdent(ref.name, d) +
                                    parenArgs(params.size()) + "; END;";
            common::CallOutput native;
            const detail::ExecScope scope(o);
            out.status = o.dataSource.empty()
                             ? client.call(sql, params, native)
                             : client.call(o.dataSource, sql, params, native);
            out.sets = std::move(native.sets);
            out.outParams = std::move(native.outParams);
            out.affected = native.affected;
            return out.status;
        }
        CallPlan plan;
        if (const auto s = makeCallPlan(ref, params, d, o.returnsRows, plan); !s.ok()) {
            out.status = s;
            return s;
        }
        if (plan.needsSameConnection) {
            out.status = unsupported("sqlconduit::util: OUT/INOUT parameters on dialect=" +
                                     std::string(dialectName(d)) + " must run on one connection; use the "
                                     "core::Session overload");
            return out.status;
        }
        const detail::ExecScope scope(o);
        if (o.returnsRows) {
            std::vector<ResultSet> sets;
            out.status = o.dataSource.empty()
                             ? client.queryAll(plan.callSql, plan.callParams, sets)
                             : client.queryAll(o.dataSource, plan.callSql, plan.callParams, sets);
            out.sets = std::move(sets);
        } else {
            out.status = o.dataSource.empty()
                             ? client.execute(plan.callSql, plan.callParams, out.affected)
                             : client.execute(o.dataSource, plan.callSql, plan.callParams,
                                              out.affected);
        }
        if (!out.status.ok()) return out.status;
        if (plan.outFromRowCount > 0 && !out.sets.empty())
            out.status = readOutParamsFromRow(out.sets.front(), plan.outFromRowCount,
                                              out.outParams);
        return out.status;
    }

    inline Status call(Client &client, core::Session &s, const RoutineRef &ref,
                       const CallParams &params,
                       CallResult &out, CallOptions opts = {}) {
        out = CallResult{};
        opts.dialect = opts.dialect != Dialect::Auto
                           ? opts.dialect
                           : detectDialect(
                               client, opts.dataSource.empty() ? ref.dataSource : opts.dataSource);
        if (opts.dialect == Dialect::Oracle && ref.kind == RoutineKind::Procedure) {
            const bool hasRefCursor = std::any_of(params.begin(), params.end(), [](const auto &p) {
                return p.type == common::ValueType::RefCursor;
            });
            if (opts.returnsRows && !hasRefCursor) {
                out.status = unsupported("sqlconduit::util: returnsRows=true for an Oracle procedure "
                    "requires CallParam::refCursor()");
                return out.status;
            }
            const std::string sql = "BEGIN " + quoteIdent(ref.name, opts.dialect) +
                                    parenArgs(params.size()) + "; END;";
            common::CallOutput native;
            out.status = s.call(sql, params, native);
            out.sets = std::move(native.sets);
            out.outParams = std::move(native.outParams);
            out.affected = native.affected;
            return out.status;
        }
        CallPlan plan;
        if (const auto st = makeCallPlan(ref, params, opts.dialect, opts.returnsRows, plan);
            !st.ok()) {
            out.status = st;
            return st;
        }
        if (!plan.preSql.empty()) {
            std::int64_t
                    pre = 0;
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

#endif
