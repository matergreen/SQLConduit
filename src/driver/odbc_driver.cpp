#include "sqlconduit/driver/odbc_driver.h"
#include "sqlconduit/drivers/odbc.h"
#include "sqlconduit/driver/driver_registry.h"

#include <algorithm>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef SQLCONDUIT_ENABLE_ODBC
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#endif

namespace sqlconduit::driver {
    namespace {
#ifdef SQLCONDUIT_ENABLE_ODBC
        bool succeeded(const SQLRETURN rc) {
            return rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
        }

        bool executionCompleted(const SQLRETURN rc) {
            return succeeded(rc) || rc == SQL_NO_DATA;
        }

        bool validSavepointName(const std::string &name) {
            if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) ||
                                  name[0] == '_'))
                return false;
            return std::all_of(name.begin() + 1, name.end(), [](const char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            });
        }

        struct DiagnosticInfo {
            std::string message;
            std::string sqlState;
            std::int64_t nativeCode = 0;
        };

        DiagnosticInfo diagnostics(const SQLSMALLINT type, const SQLHANDLE handle,
                                   const std::string &where) {
            DiagnosticInfo info;
            info.message = "ODBC " + where;
            SQLSMALLINT record = 1;
            for (;;) {
                SQLCHAR state[6] = {};
                SQLINTEGER native = 0;
                SQLCHAR text[1024] = {};
                SQLSMALLINT length = 0;
                const SQLRETURN rc = SQLGetDiagRec(
                    type, handle, record, state, &native, text,
                    static_cast<SQLSMALLINT>(sizeof(text)), &length);
                if (rc == SQL_NO_DATA) break;
                if (!succeeded(rc)) break;
                if (record == 1) {
                    info.sqlState = reinterpret_cast<char *>(state);
                    info.nativeCode = native;
                }
                info.message += record == 1 ? ": " : "; ";
                info.message += "[" + std::string(reinterpret_cast<char *>(state)) + "/"
                        + std::to_string(native) + "] ";
                info.message.append(reinterpret_cast<char *>(text),
                                    static_cast<std::size_t>(std::max<SQLSMALLINT>(0, length)));
                ++record;
            }
            return info;
        }

        common::Status odbcError(const common::ErrorCode code, const SQLSMALLINT type,
                                 const SQLHANDLE handle, const std::string &where) {
            auto info = diagnostics(type, handle, where);
            return common::Status::databaseError(code, std::move(info.message),
                                                 std::move(info.sqlState), info.nativeCode);
        }

        class StmtGuard {
        public:
            explicit StmtGuard(SQLHSTMT stmt = SQL_NULL_HSTMT) : stmt_(stmt) {
            }

            ~StmtGuard() { release(); }

            StmtGuard(const StmtGuard &) = delete;

            StmtGuard &operator=(const StmtGuard &) = delete;

            StmtGuard(StmtGuard &&other) noexcept : stmt_(other.stmt_) {
                other.stmt_ = SQL_NULL_HSTMT;
            }

            StmtGuard &operator=(StmtGuard &&other) noexcept {
                if (this != &other) {
                    release();
                    stmt_ = other.stmt_;
                    other.stmt_ = SQL_NULL_HSTMT;
                }
                return *this;
            }

            SQLHSTMT get() const { return stmt_; }

        private:
            void release() {
                if (stmt_ == SQL_NULL_HSTMT) return;
                SQLFreeStmt(stmt_, SQL_CLOSE);
                SQLFreeStmt(stmt_, SQL_RESET_PARAMS);
                SQLFreeHandle(SQL_HANDLE_STMT, stmt_);
                stmt_ = SQL_NULL_HSTMT;
            }

            SQLHSTMT stmt_;
        };

        class PreparedStmtReset {
        public:
            explicit PreparedStmtReset(SQLHSTMT stmt) : stmt_(stmt) {
            }

            ~PreparedStmtReset() {
                if (stmt_ == SQL_NULL_HSTMT) return;
                SQLFreeStmt(stmt_, SQL_CLOSE);
                SQLFreeStmt(stmt_, SQL_RESET_PARAMS);
            }

            PreparedStmtReset(const PreparedStmtReset &) = delete;

            PreparedStmtReset &operator=(const PreparedStmtReset &) = delete;

        private:
            SQLHSTMT stmt_;
        };

        class ActiveStatement {
        public:
            ActiveStatement(std::mutex &mutex, void *&slot, void *value)
                : mutex_(mutex), slot_(slot), value_(value) {
                std::lock_guard<std::mutex> lock(mutex_);
                slot_ = value_;
            }

            ~ActiveStatement() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (slot_ == value_) slot_ = nullptr;
            }

            ActiveStatement(const ActiveStatement &) = delete;

            ActiveStatement &operator=(const ActiveStatement &) = delete;

        private:
            std::mutex &mutex_;
            void *&slot_;
            void *value_;
        };

        std::string connectionValue(const std::string &value) {
            if (value.find_first_of(";{}") == std::string::npos) return value;
            std::string out = "{";
            for (const char c: value) {
                out.push_back(c);
                if (c == '}') out.push_back('}');
            }
            out.push_back('}');
            return out;
        }

        std::string buildConnectionString(const config::DataSourceConfig &cfg) {
            const auto raw = cfg.extra.find("connection_string");
            if (raw != cfg.extra.end() && !raw->second.empty()) return raw->second;

            std::string out;
            if (!cfg.dsn.empty()) out += "DSN=" + connectionValue(cfg.dsn) + ";";
            const auto driver = cfg.extra.find("driver");
            if (cfg.dsn.empty() && driver != cfg.extra.end())
                out += "DRIVER=" + connectionValue(driver->second) + ";";
            if (!cfg.host.empty()) out += "SERVER=" + connectionValue(cfg.host) + ";";
            if (cfg.port > 0) out += "PORT=" + std::to_string(cfg.port) + ";";
            if (!cfg.database.empty()) out += "DATABASE=" + connectionValue(cfg.database) + ";";
            if (!cfg.user.empty()) out += "UID=" + connectionValue(cfg.user) + ";";
            if (!cfg.password.empty()) out += "PWD=" + connectionValue(cfg.password) + ";";
            if (cfg.tls_enabled) {
                const auto tls = cfg.extra.find("tls_connection_options");
                if (tls != cfg.extra.end() && !tls->second.empty()) {
                    out += tls->second;
                    if (out.back() != ';') out.push_back(';');
                }
            }
            return out;
        }

        common::Status newStatement(SQLHDBC dbc, const config::DataSourceConfig &cfg,
                                    SQLHSTMT &out) {
            out = SQL_NULL_HSTMT;
            const SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &out);
            if (!succeeded(rc))
                return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_DBC, dbc,
                                 "SQLAllocHandle(STMT)");
            const int timeoutMs = cfg.query_timeout_ms > 0
                                      ? cfg.query_timeout_ms
                                      : cfg.socket_timeout_ms;
            if (timeoutMs > 0) {
                const SQLULEN seconds = static_cast<SQLULEN>(
                    std::max(1, (timeoutMs + 999) / 1000));
                const SQLRETURN timeoutRc = SQLSetStmtAttr(
                    out, SQL_ATTR_QUERY_TIMEOUT,
                    reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0);
                if (!succeeded(timeoutRc)) {
                    const auto error = odbcError(common::ErrorCode::QueryError,
                                                 SQL_HANDLE_STMT, out,
                                                 "SQLSetStmtAttr(QUERY_TIMEOUT)");
                    SQLFreeHandle(SQL_HANDLE_STMT, out);
                    out = SQL_NULL_HSTMT;
                    return error;
                }
            }
            return common::Status::OK();
        }

        common::Value textValue(const SQLSMALLINT sqlType, std::string value) {
            try {
                if (sqlType == static_cast<SQLSMALLINT>(-154))
                    return common::Value{common::Time{std::move(value)}};
                switch (sqlType) {
                    case SQL_TINYINT:
                    case SQL_SMALLINT:
                    case SQL_INTEGER:
                    case SQL_BIGINT:
                        return common::Value{std::stoll(value)};
                    case SQL_BIT:
                        return common::Value{value == "1" || value == "true" || value == "TRUE"};
                    case SQL_REAL:
                    case SQL_FLOAT:
                    case SQL_DOUBLE:
                        return common::Value{std::stod(value)};
                    case SQL_DECIMAL:
                    case SQL_NUMERIC:
                        return common::Value{common::Decimal{std::move(value)}};
                    case SQL_TYPE_DATE:
                    case SQL_DATE:
                        return common::Value{common::Date{std::move(value)}};
                    case SQL_TYPE_TIME:
                    case SQL_TIME:
                        return common::Value{common::Time{std::move(value)}};
                    case SQL_TYPE_TIMESTAMP:
                    case SQL_TIMESTAMP: {
                        common::Timestamp ts{};
                        if (common::tryParseTimestamp(value, ts)) return common::Value{ts};
                        return common::Value{std::move(value)};
                    }
#ifdef SQL_GUID
        case SQL_GUID :
                        return common::Value { common::Uuid{std::move(value)} };
#endif
        default :
                        return common::Value { std::move(value) };
                }
            } catch (...) {
                return common::Value{std::move(value)};
            }
        }

        common::Status readTextColumn(SQLHSTMT stmt, const SQLUSMALLINT column,
                                      const SQLSMALLINT sqlType, common::Value &out) {
            std::string value;
            char buffer[4096];
            for (;;) {
                std::memset(buffer, 0, sizeof(buffer));
                SQLLEN length = 0;
                const SQLRETURN rc = SQLGetData(stmt, column, SQL_C_CHAR, buffer,
                                                sizeof(buffer), &length);
                if (rc == SQL_NO_DATA) break;
                if (length == SQL_NULL_DATA) {
                    out = nullptr;
                    return common::Status::OK();
                }
                if (!succeeded(rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     stmt, "SQLGetData(text)");
                value.append(buffer, std::strlen(buffer));
                if (rc == SQL_SUCCESS) break;
            }
            out = textValue(sqlType, std::move(value));
            return common::Status::OK();
        }

        common::Status readBinaryColumn(SQLHSTMT stmt, const SQLUSMALLINT column,
                                        common::Value &out) {
            common::Blob value;
            std::uint8_t buffer[4096];
            for (;;) {
                SQLLEN length = 0;
                const SQLRETURN rc = SQLGetData(stmt, column, SQL_C_BINARY, buffer,
                                                sizeof(buffer), &length);
                if (rc == SQL_NO_DATA) break;
                if (length == SQL_NULL_DATA) {
                    out = nullptr;
                    return common::Status::OK();
                }
                if (!succeeded(rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     stmt, "SQLGetData(binary)");
                std::size_t count = sizeof(buffer);
                if (rc == SQL_SUCCESS && length != SQL_NO_TOTAL)
                    count = std::min<std::size_t>(sizeof(buffer), static_cast<std::size_t>(length));
                value.insert(value.end(), buffer, buffer + count);
                if (rc == SQL_SUCCESS) break;
            }
            out = std::move(value);
            return common::Status::OK();
        }

        bool binaryType(const SQLSMALLINT type) {
            return type == SQL_BINARY || type == SQL_VARBINARY || type == SQL_LONGVARBINARY;
        }

        common::Status fetchRowsInternal(SQLHSTMT stmt, common::ResultSet *out,
                                         const common::RowCallback *callback,
                                         std::uint64_t *delivered) {
            SQLSMALLINT columns = 0;
            if (const SQLRETURN rc = SQLNumResultCols(stmt, &columns); !succeeded(rc))
                return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt,
                                 "SQLNumResultCols");

            std::vector<std::string> names;
            std::vector<SQLSMALLINT> types;
            names.reserve(static_cast<std::size_t>(columns));
            types.reserve(static_cast<std::size_t>(columns));
            for (SQLUSMALLINT i = 1; i <= static_cast<SQLUSMALLINT>(columns); ++i) {
                SQLCHAR name[512] = {};
                SQLSMALLINT nameLength = 0;
                SQLSMALLINT type = 0;
                SQLULEN size = 0;
                SQLSMALLINT digits = 0;
                SQLSMALLINT nullable = 0;
                const SQLRETURN rc = SQLDescribeCol(stmt, i, name, sizeof(name),
                                                    &nameLength, &type, &size,
                                                    &digits, &nullable);
                if (!succeeded(rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     stmt, "SQLDescribeCol");
                names.emplace_back(reinterpret_cast<char *>(name),
                                   static_cast<std::size_t>(nameLength));
                types.push_back(type);
            }
            if (out) out->setFields(names);
            if (delivered) *delivered = 0;
            if (columns == 0) return common::Status::OK();

            for (;;) {
                const SQLRETURN rc = SQLFetch(stmt);
                if (rc == SQL_NO_DATA) break;
                if (!succeeded(rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     stmt, "SQLFetch");
                common::Row row;
                for (SQLUSMALLINT i = 1; i <= static_cast<SQLUSMALLINT>(columns); ++i) {
                    common::Value value{nullptr};
                    common::Status status = binaryType(types[i - 1])
                                                ? readBinaryColumn(stmt, i, value)
                                                : readTextColumn(stmt, i, types[i - 1], value);
                    if (!status.ok()) return status;
                    row.set(names[i - 1], std::move(value));
                }
                if (out) {
                    out->addRow(std::move(row));
                } else if (callback) {
                    if (delivered) ++*delivered;
                    if (*callback && !(*callback)(row)) break;
                }
            }
            return common::Status::OK();
        }

        common::Status fetchRows(SQLHSTMT stmt, common::ResultSet &out) {
            return fetchRowsInternal(stmt, &out, nullptr, nullptr);
        }

        common::Status fetchEach(SQLHSTMT stmt, const common::RowCallback &callback,
                                 std::uint64_t &rows) {
            return fetchRowsInternal(stmt, nullptr, &callback, &rows);
        }

        struct ParamBinding {
            SQLLEN indicator = 0;
            std::int64_t integer = 0;
            std::uint64_t unsignedInteger = 0;
            double real = 0.0;
            std::string text;
            std::vector<SQLWCHAR> wideText;
            common::Blob blob;
        };

        bool utf8ToSqlWide(const std::string &text, std::vector<SQLWCHAR> &out) {
            out.clear();
            out.reserve(text.size() + 1);
            for (std::size_t i = 0; i < text.size();) {
                const auto first = static_cast<unsigned char>(text[i]);
                std::uint32_t codePoint = 0;
                std::size_t extra = 0;
                if (first <= 0x7f) {
                    codePoint = first;
                } else if ((first & 0xe0) == 0xc0) {
                    codePoint = first & 0x1f;
                    extra = 1;
                } else if ((first & 0xf0) == 0xe0) {
                    codePoint = first & 0x0f;
                    extra = 2;
                } else if ((first & 0xf8) == 0xf0) {
                    codePoint = first & 0x07;
                    extra = 3;
                } else {
                    return false;
                }
                if (i + extra >= text.size()) return false;
                for (std::size_t j = 1; j <= extra; ++j) {
                    const auto next = static_cast<unsigned char>(text[i + j]);
                    if ((next & 0xc0) != 0x80) return false;
                    codePoint = (codePoint << 6) | (next & 0x3f);
                }
                if ((extra == 1 && codePoint < 0x80) ||
                    (extra == 2 && codePoint < 0x800) ||
                    (extra == 3 && codePoint < 0x10000) ||
                    codePoint > 0x10ffff ||
                    (codePoint >= 0xd800 && codePoint <= 0xdfff))
                    return false;
                if constexpr (sizeof(SQLWCHAR) == 2) {
                    if (codePoint <= 0xffff) {
                        out.push_back(static_cast<SQLWCHAR>(codePoint));
                    } else {
                        codePoint -= 0x10000;
                        out.push_back(static_cast<SQLWCHAR>(0xd800 + (codePoint >> 10)));
                        out.push_back(static_cast<SQLWCHAR>(0xdc00 + (codePoint & 0x3ff)));
                    }
                } else {
                    out.push_back(static_cast<SQLWCHAR>(codePoint));
                }
                i += extra + 1;
            }
            out.push_back(0);
            return true;
        }

        common::Status bindParameters(SQLHSTMT stmt, const common::Params &params,
                                      std::vector<ParamBinding> &storage,
                                      const bool utf8NarrowBinding) {
            storage.resize(params.size());
            for (std::size_t i = 0; i < params.size(); ++i) {
                const auto &value = params[i];
                auto &slot = storage[i];
                SQLSMALLINT cType = SQL_C_CHAR;
                SQLSMALLINT sqlType = SQL_VARCHAR;
                SQLULEN columnSize = 1;
                SQLSMALLINT decimalDigits = 0;
                SQLPOINTER data = nullptr;
                SQLLEN bufferLength = 0;

                if (std::holds_alternative<std::nullptr_t>(value)) {
                    slot.indicator = SQL_NULL_DATA;
                } else if (const auto *v = std::get_if<bool>(&value)) {
                    slot.integer = *v ? 1 : 0;
                    slot.indicator = 0;
                    cType = SQL_C_SBIGINT;
                    sqlType = SQL_BIGINT;
                    data = &slot.integer;
                    bufferLength = sizeof(slot.integer);
                } else if (const auto *v = std::get_if<std::int64_t>(&value)) {
                    slot.integer = *v;
                    slot.indicator = 0;
                    cType = SQL_C_SBIGINT;
                    sqlType = SQL_BIGINT;
                    data = &slot.integer;
                    bufferLength = sizeof(slot.integer);
                } else if (const auto *v = std::get_if<std::uint64_t>(&value)) {
                    slot.unsignedInteger = *v;
                    slot.indicator = 0;
                    cType = SQL_C_UBIGINT;
                    sqlType = SQL_BIGINT;
                    data = &slot.unsignedInteger;
                    bufferLength = sizeof(slot.unsignedInteger);
                } else if (const auto *v = std::get_if<double>(&value)) {
                    slot.real = *v;
                    slot.indicator = 0;
                    cType = SQL_C_DOUBLE;
                    sqlType = SQL_DOUBLE;
                    data = &slot.real;
                    bufferLength = sizeof(slot.real);
                } else if (const auto *v = std::get_if<common::Timestamp>(&value)) {
                    slot.text = common::timestampToStringMs(*v);
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    columnSize = static_cast<SQLULEN>(slot.text.size());
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Decimal>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    cType = SQL_C_CHAR;
                    sqlType = SQL_DECIMAL;
                    std::size_t precision = 0;
                    bool fractional = false;
                    for (const char ch: slot.text) {
                        if (ch == 'e' || ch == 'E') break;
                        if (ch == '.') {
                            fractional = true;
                        } else if (ch >= '0' && ch <= '9') {
                            ++precision;
                            if (fractional && decimalDigits < 32767) ++decimalDigits;
                        }
                    }
                    columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, precision));
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Date>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    cType = SQL_C_CHAR;
                    sqlType = SQL_TYPE_DATE;
                    columnSize = static_cast<SQLULEN>(slot.text.size());
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Time>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    cType = SQL_C_CHAR;
                    sqlType = SQL_TYPE_TIME;
                    columnSize = static_cast<SQLULEN>(slot.text.size());
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Uuid>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    cType = SQL_C_CHAR;
#ifdef SQL_GUID
        sqlType= SQL_GUID;
#endif
        columnSize=static_cast<SQLULEN>(slot.text.size());
        data=const_cast<char *>(slot.text.data());
        bufferLength=static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Json>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    cType = SQL_C_CHAR; sqlType = SQL_LONGVARCHAR;
                    columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, slot.text.size()));
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::IntervalYearMonth>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, slot.text.size()));
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::IntervalDaySecond>(&value)) {
                    slot.text = v->value;
                    slot.indicator = static_cast<SQLLEN>(slot.text.size());
                    columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, slot.text.size()));
                    data = const_cast<char *>(slot.text.data());
                    bufferLength = static_cast<SQLLEN>(slot.text.size());
                } else if (const auto *v = std::get_if<common::Blob>(&value)) {
                    slot.blob = *v;
                    slot.indicator = static_cast<SQLLEN>(slot.blob.size());
                    cType = SQL_C_BINARY; sqlType = SQL_VARBINARY;
                    columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, slot.blob.size()));
                    data = slot.blob.empty() ? nullptr : slot.blob.data();
                    bufferLength = static_cast<SQLLEN>(slot.blob.size());
                } else if (const auto *v = std::get_if<std::string>(&value)) {
                    if (!utf8ToSqlWide(*v, slot.wideText)) {
                        slot.text = *v;
                        slot.indicator = static_cast<SQLLEN>(slot.text.size());
                        cType = SQL_C_CHAR;
                        sqlType = slot.text.size() > 8000 ? SQL_LONGVARCHAR : SQL_VARCHAR;
                        columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, slot.text.size()));
                        data = const_cast<char *>(slot.text.data());
                        bufferLength = static_cast<SQLLEN>(slot.text.size());
                    } else if (utf8NarrowBinding) {
                        const std::size_t units = slot.wideText.size() - 1;
                        slot.text = *v;
                        slot.indicator = static_cast<SQLLEN>(slot.text.size());
                        cType = SQL_C_CHAR;
                        sqlType = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
                        columnSize = static_cast<SQLULEN>(std::max<std::size_t>(
                            1, std::max(units, slot.text.size())));
                        data = const_cast<char *>(slot.text.data());
                        bufferLength = static_cast<SQLLEN>(slot.text.size());
                    } else {
                        const std::size_t units = slot.wideText.size() - 1;
                        slot.indicator = static_cast<SQLLEN>(units * sizeof(SQLWCHAR));
                        cType = SQL_C_WCHAR;
                        sqlType = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
                        columnSize = static_cast<SQLULEN>(std::max<std::size_t>(1, units));
                        data = slot.wideText.data();
                        bufferLength = static_cast<SQLLEN>(slot.wideText.size() * sizeof(SQLWCHAR));
                    }
                } else if (std::holds_alternative<common::Array>(value)||
        std::holds_alternative<common::Composite> (value)||
        std::holds_alternative<common::TypedArray> (value)||
        std::holds_alternative<common::TypedComposite> (value)) {
                    return common::Status::error(
                        common::ErrorCode::NotSupported,
                        "ODBC: array/composite parameters are not supported by this driver");
                }

        const SQLRETURN rc = SQLBindParameter(
            stmt, static_cast<SQLUSMALLINT>(i + 1), SQL_PARAM_INPUT,
            cType, sqlType, columnSize, decimalDigits,
            data, bufferLength, &slot.indicator);
                if (!succeeded (rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     stmt, "SQLBindParameter");
            }
            return common::Status::OK();
        }

        common::Status prepareAndBind(SQLHDBC dbc, const config::DataSourceConfig &cfg,
                                      const std::string &sql, const common::Params &params,
                                      SQLHSTMT &stmt, std::vector<ParamBinding> &storage,
                                      const bool utf8NarrowBinding) {
            if (const auto status = newStatement(dbc, cfg, stmt); !status.ok()) return status;
            if (const SQLRETURN rc = SQLPrepare(
                    stmt, reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.data())), SQL_NTS);
                !succeeded(rc))
                return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt,
                                 "SQLPrepare");
            if (const auto status = bindParameters(stmt, params, storage, utf8NarrowBinding);
                !status.ok()) return status;
            return common::Status::OK();
        }
#endif
    }

#ifdef SQLCONDUIT_ENABLE_ODBC
    class OdbcCursor : public core::ICursor {
    public:
        OdbcCursor(StmtGuard guard, std::vector<ParamBinding> storage, std::size_t batchSize)
            : guard_(std::move(guard)), storage_(std::move(storage)), batchSize_(batchSize) {
        }

        ~OdbcCursor() override { reset(); }

        common::Status setupResult() {
            SQLSMALLINT columns = 0;
            if (const SQLRETURN rc = SQLNumResultCols(guard_.get(), &columns); !succeeded(rc))
                return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, guard_.get(),
                                 "SQLNumResultCols");
            names_.reserve(static_cast<std::size_t>(columns));
            types_.reserve(static_cast<std::size_t>(columns));
            for (SQLUSMALLINT i = 1; i <= static_cast<SQLUSMALLINT>(columns); ++i) {
                SQLCHAR name[512] = {};
                SQLSMALLINT nl = 0, type = 0;
                SQLULEN size = 0;
                SQLSMALLINT dig = 0, null = 0;
                if (const SQLRETURN rc = SQLDescribeCol(guard_.get(), i, name, sizeof(name),
                                                        &nl, &type, &size, &dig, &null);
                    !succeeded(rc))
                    return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                                     guard_.get(), "SQLDescribeCol");
                names_.emplace_back(reinterpret_cast<char *>(name),
                                    static_cast<std::size_t>(nl));
                types_.push_back(type);
            }
            open_ = true;
            return common::Status::OK();
        }

        common::Status fetch(std::size_t n, common::ResultSet &out) override {
            if (!open_ || eof_) return common::Status::OK();
            if (!fieldsSet_) {
                out.setFields(names_);
                fieldsSet_ = true;
            }
            const std::size_t want = (n == 0) ? batchSize_ : n;
            for (std::size_t i = 0; i < want; ++i) {
                const SQLRETURN rc = SQLFetch(guard_.get());
                if (rc == SQL_NO_DATA) {
                    eof_ = true;
                    break;
                }
                if (!succeeded(rc))
                    return odbcError(common::ErrorCode::CursorError, SQL_HANDLE_STMT,
                                     guard_.get(), "SQLFetch");
                common::Row row;
                for (SQLUSMALLINT c = 1; c <= static_cast<SQLUSMALLINT>(names_.size()); ++c) {
                    common::Value v{nullptr};
                    common::Status st = binaryType(types_[c - 1])
                                            ? readBinaryColumn(guard_.get(), c, v)
                                            : readTextColumn(guard_.get(), c, types_[c - 1], v);
                    if (!st.ok()) return st;
                    row.set(names_[c - 1], std::move(v));
                }
                out.addRow(std::move(row));
                ++rowsFetched_;
            }
            return common::Status::OK();
        }

        common::Status fetchRow(common::Row &outRow, bool &ok) override {
            ok = false;
            if (!open_ || eof_) return common::Status::OK();
            common::ResultSet tmp;
            const auto st = fetch(1, tmp);
            if (!st.ok()) return st;
            if (tmp.empty()) return common::Status::OK();
            outRow = std::move(tmp.mutableRows().front());
            ok = true;
            return common::Status::OK();
        }

        common::Status close() override {
            reset();
            return common::Status::OK();
        }

        [[nodiscard]] bool isOpen() const override { return open_; }
        [[nodiscard]] bool hasNext() const override { return open_ && !eof_; }
        [[nodiscard]] std::uint64_t rowsFetched() const override { return rowsFetched_; }

    private:
        void reset() {
            guard_ = StmtGuard(SQL_NULL_HSTMT);
            open_ = false;
        }

        StmtGuard guard_;
        std::vector<ParamBinding> storage_;
        std::size_t batchSize_;
        std::vector<std::string> names_;
        std::vector<SQLSMALLINT> types_;
        bool open_ = false;
        bool eof_ = false;
        bool fieldsSet_ = false;
        std::uint64_t rowsFetched_ = 0;
    };
#endif

    OdbcConnection::~OdbcConnection() { OdbcConnection::close(); }

    common::Status OdbcConnection::connect(const config::DataSourceConfig &cfg) {
        cfg_ = cfg;
#ifdef SQLCONDUIT_ENABLE_ODBC
        close();
        SQLHENV env = SQL_NULL_HENV;
        if (const SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
            !succeeded(rc))
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "ODBC SQLAllocHandle(ENV) failed");
        env_ = env;
        if (const SQLRETURN rc = SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
                                               reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
            !succeeded(rc)) {
            const auto status = odbcError(common::ErrorCode::ConnectionFailed,
                                          SQL_HANDLE_ENV, env, "SQLSetEnvAttr");
            close();
            return status;
        }

        SQLHDBC dbc = SQL_NULL_HDBC;
        if (const SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc); !succeeded(rc)) {
            const auto status = odbcError(common::ErrorCode::ConnectionFailed,
                                          SQL_HANDLE_ENV, env, "SQLAllocHandle(DBC)");
            close();
            return status;
        }
        dbc_ = dbc;
        if (cfg.connection_timeout_ms > 0) {
            const SQLULEN seconds = static_cast<SQLULEN>(
                std::max(1, (cfg.connection_timeout_ms + 999) / 1000));
            SQLSetConnectAttr(dbc, SQL_LOGIN_TIMEOUT,
                              reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0);
#ifdef SQL_ATTR_CONNECTION_TIMEOUT
        SQLSetConnectAttr(dbc, SQL_ATTR_CONNECTION_TIMEOUT,
                          reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0);
#endif
        }

        if (cfg.tls_enabled && cfg.extra.find("connection_string") == cfg.extra.end() &&
            cfg.extra.find("tls_connection_options") == cfg.extra.end()) {
            close();
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "ODBC TLS requires driver-specific extra.tls_connection_options "
                "or extra.connection_string");
        }
        const std::string connectionString = buildConnectionString(cfg);
        if (connectionString.empty()) {
            close();
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "ODBC datasource requires dsn, extra.driver, or extra.connection_string");
        }
        SQLCHAR completed[1024] = {};
        SQLSMALLINT completedLength = 0;
        const SQLRETURN rc = SQLDriverConnect(
            dbc, nullptr,
            reinterpret_cast<SQLCHAR *>(const_cast<char *>(connectionString.c_str())), SQL_NTS,
            completed, sizeof(completed), &completedLength, SQL_DRIVER_NOPROMPT);
        if (!succeeded(rc)) {
            const auto status = odbcError(common::ErrorCode::ConnectionFailed,
                                          SQL_HANDLE_DBC, dbc, "SQLDriverConnect");
            close();
            auto redacted = status;
            redacted.message = cfg.redact(std::move(redacted.message));
            return redacted;
        }
        open_ = true;
        txOpen_ = false;
        utf8NarrowBinding_ = false;
        SQLCHAR driverName[256] = {};
        SQLSMALLINT driverNameLength = 0;
        if (succeeded(SQLGetInfo(dbc, SQL_DRIVER_NAME, driverName, sizeof(driverName),
                                 &driverNameLength))) {
            std::string name(reinterpret_cast<char *>(driverName),
                             std::min<std::size_t>(
                                 sizeof(driverName) - 1,
                                 static_cast<std::size_t>(std::max<SQLSMALLINT>(0, driverNameLength))));
            std::transform(name.begin(), name.end(), name.begin(), [](const unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            utf8NarrowBinding_ = name.find("tdsodbc") != std::string::npos ||
                                 name.find("freetds") != std::string::npos;
        }
        if (const auto mode = cfg.extra.find("unicode_binding"); mode != cfg.extra.end()) {
            if (mode->second == "wide") {
                utf8NarrowBinding_ = false;
            } else if (mode->second == "utf8") {
                utf8NarrowBinding_ = true;
            } else if (mode->second != "auto") {
                close();
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "ODBC extra.unicode_binding must be auto, wide, or utf8");
            }
        }
        SQLUINTEGER isolation = 0;
        if (succeeded(SQLGetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
                                        &isolation, 0, nullptr)))
            defaultIsolation_ = isolation;
        return common::Status::OK();
#else
        (void) cfg;
        open_ = false;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "ODBC driver not built. Rebuild with -DSQLCONDUIT_ENABLE_ODBC=ON");
#endif
    }

    common::Status OdbcConnection::ping() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_) return common::Status::error(common::ErrorCode::PingFailed, "not connected");
#ifdef SQL_ATTR_CONNECTION_DEAD
        SQLUINTEGER dead = SQL_CD_TRUE;
        if (const SQLRETURN rc = SQLGetConnectAttr(
                static_cast<SQLHDBC>(dbc_), SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr);
            succeeded(rc) && dead == SQL_CD_FALSE)
            return common::Status::OK();
#endif
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto status = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !status.ok())
            return common::Status::error(common::ErrorCode::PingFailed, status.message);
        StmtGuard stmt(raw);
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        const auto it = cfg_.extra.find("ping_query");
        const std::string sql = it == cfg_.extra.end() ? "SELECT 1" : it->second;
        if (const SQLRETURN rc = SQLExecDirect(
                stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
            !succeeded(rc))
            return odbcError(common::ErrorCode::PingFailed, SQL_HANDLE_STMT,
                             stmt.get(), "ping");
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::query(const std::string &sql, common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (query)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto status = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !status.ok())
            return status;
        StmtGuard stmt(raw);
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecDirect(
                stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
            !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                             stmt.get(), "SQLExecDirect(query)");
        return fetchRows(stmt.get(), out);
#else
        (void) sql;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::execute(const std::string &sql, int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        affected = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (execute)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto status = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !status.ok())
            return status;
        StmtGuard stmt(raw);
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecDirect(
                stmt.get(), reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
            !executionCompleted(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                             stmt.get(), "SQLExecDirect(execute)");
        SQLLEN rows = 0;
        if (const SQLRETURN rc = SQLRowCount(stmt.get(), &rows); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                             stmt.get(), "SQLRowCount");
        affected = rows < 0 ? 0 : static_cast<std::int64_t>(rows);
        return common::Status::OK();
#else
        (void) sql;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::query(const std::string &sql, const common::Params &params,
                                         common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (query)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        std::vector<ParamBinding> storage;
        const auto status = prepareAndBind(static_cast<SQLHDBC>(dbc_), cfg_, sql, params,
                                           raw, storage, utf8NarrowBinding_);
        StmtGuard stmt(raw);
        if (!status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecute(raw); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw,
                             "SQLExecute");
        return fetchRows(stmt.get(), out);
#else
        (void) sql;
        (void) params;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::execute(const std::string &sql, const common::Params &params,
                                           int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        affected = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (execute)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        std::vector<ParamBinding> storage;
        const auto status = prepareAndBind(static_cast<SQLHDBC>(dbc_), cfg_, sql, params,
                                           raw, storage, utf8NarrowBinding_);
        StmtGuard stmt(raw);
        if (!status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecute(raw); !executionCompleted(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw,
                             "SQLExecute");
        SQLLEN rows = 0;
        if (const SQLRETURN rc = SQLRowCount(stmt.get(), &rows); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT,
                             stmt.get(), "SQLRowCount");
        affected = rows < 0 ? 0 : static_cast<std::int64_t>(rows);
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::queryEach(const std::string &sql,
                                             const common::Params &params,
                                             const common::RowCallback &callback,
                                             std::uint64_t &rows) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        rows = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (stream)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        std::vector<ParamBinding> storage;
        const auto status = prepareAndBind(static_cast<SQLHDBC>(dbc_), cfg_, sql, params,
                                           raw, storage, utf8NarrowBinding_);
        StmtGuard stmt(raw);
        if (!status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecute(raw); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw,
                             "SQLExecute(stream)");
        return fetchEach(raw, callback, rows);
#else
        (void) sql;
        (void) params;
        (void) callback;
        rows = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::begin() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (begin)");
        if (txOpen_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: transaction already open");
        const SQLRETURN rc = SQLSetConnectAttr(static_cast<SQLHDBC>(dbc_), SQL_ATTR_AUTOCOMMIT,
                                               reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
        if (!succeeded(rc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "begin");
        txOpen_ = true;
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::begin(const common::TransactionOptions &options) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (begin)");
        SQLUINTEGER isolation = static_cast<SQLUINTEGER>(defaultIsolation_);
        switch (options.isolation) {
            case common::IsolationLevel::Default: break;
            case common::IsolationLevel::ReadUncommitted: isolation = SQL_TXN_READ_UNCOMMITTED;
                break;
            case common::IsolationLevel::ReadCommitted: isolation = SQL_TXN_READ_COMMITTED;
                break;
            case common::IsolationLevel::RepeatableRead: isolation = SQL_TXN_REPEATABLE_READ;
                break;
            case common::IsolationLevel::Serializable: isolation = SQL_TXN_SERIALIZABLE;
                break;
        }
        if (isolation != 0) {
            const SQLRETURN rc = SQLSetConnectAttr(
                static_cast<SQLHDBC>(dbc_), SQL_ATTR_TXN_ISOLATION,
                reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(isolation)), 0);
            if (!succeeded(rc))
                return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                                 static_cast<SQLHDBC>(dbc_), "set transaction isolation");
        }
        const SQLULEN access = options.readOnly ? SQL_MODE_READ_ONLY : SQL_MODE_READ_WRITE;
        if (const SQLRETURN rc = SQLSetConnectAttr(
                static_cast<SQLHDBC>(dbc_), SQL_ATTR_ACCESS_MODE,
                reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(access)), 0);
            !succeeded(rc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "set transaction access mode");
        return begin();
#else
        (void) options;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::commit() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_ || !txOpen_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: no active transaction");
        const SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_COMMIT);
        const SQLRETURN autoRc = SQLSetConnectAttr(
            static_cast<SQLHDBC>(dbc_), SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
        txOpen_ = false;
        if (!succeeded(rc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "commit");
        if (!succeeded(autoRc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "restore autocommit");
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::rollback() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!open_ || !txOpen_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: no active transaction");
        const SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_ROLLBACK);
        const SQLRETURN autoRc = SQLSetConnectAttr(
            static_cast<SQLHDBC>(dbc_), SQL_ATTR_AUTOCOMMIT,
            reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
        txOpen_ = false;
        if (!succeeded(rc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "rollback");
        if (!succeeded(autoRc))
            return odbcError(common::ErrorCode::TxError, SQL_HANDLE_DBC,
                             static_cast<SQLHDBC>(dbc_), "restore autocommit");
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::savepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: invalid savepoint or no active transaction");
        const auto style = cfg_.extra.find("savepoint_style");
        const std::string sql = style != cfg_.extra.end() && style->second == "sqlserver"
                                    ? "SAVE TRANSACTION " + name
                                    : "SAVEPOINT " + name;
        std::int64_t affected = 0;
        auto status = execute(sql, affected);
        if (!status.ok()) status.code = common::ErrorCode::TxError;
        return status;
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::releaseSavepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: invalid savepoint or no active transaction");
        const auto style = cfg_.extra.find("savepoint_style");
        if (style != cfg_.extra.end() && style->second == "sqlserver")
            return common::Status::OK();
        std::int64_t affected = 0;
        auto status = execute("RELEASE SAVEPOINT " + name, affected);
        if (!status.ok()) status.code = common::ErrorCode::TxError;
        return status;
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::rollbackToSavepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "ODBC: invalid savepoint or no active transaction");
        const auto style = cfg_.extra.find("savepoint_style");
        const std::string sql = style != cfg_.extra.end() && style->second == "sqlserver"
                                    ? "ROLLBACK TRANSACTION " + name
                                    : "ROLLBACK TO SAVEPOINT " + name;
        std::int64_t affected = 0;
        auto status = execute(sql, affected);
        if (!status.ok()) status.code = common::ErrorCode::TxError;
        return status;
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    void OdbcConnection::close() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        closeAllPrepared();
        if (dbc_) {
            if (txOpen_) SQLEndTran(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_), SQL_ROLLBACK);
            if (open_) SQLDisconnect(static_cast<SQLHDBC>(dbc_));
            SQLFreeHandle(SQL_HANDLE_DBC, static_cast<SQLHDBC>(dbc_));
        }
        if (env_) SQLFreeHandle(SQL_HANDLE_ENV, static_cast<SQLHENV>(env_));
#endif
        dbc_ = nullptr;
        env_ = nullptr;
        txOpen_ = false;
        defaultIsolation_ = 0;
        utf8NarrowBinding_ = false;
        open_ = false;
    }

    common::Status OdbcConnection::cancel() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        void *stmt = nullptr; {
            std::lock_guard<std::mutex> lock(activeStmtMtx_);
            stmt = activeStmt_;
            activeStmt_ = nullptr;
        }
        if (!stmt)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: no active statement to cancel");
        const SQLRETURN rc = SQLCancelHandle(SQL_HANDLE_STMT, static_cast<SQLHSTMT>(stmt));
        if (!succeeded(rc))
            return odbcError(common::ErrorCode::Cancelled, SQL_HANDLE_STMT,
                             static_cast<SQLHSTMT>(stmt), "SQLCancelHandle");
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::openCursor(const std::string &sql, const common::Params &params,
                                              const core::CursorOptions &opts,
                                              std::unique_ptr<core::ICursor> &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        out.reset();
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (openCursor)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto s = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !s.ok()) return s;
        StmtGuard stmt(raw);
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (opts.scrollable) {
            const SQLRETURN rc = SQLSetStmtAttr(
                raw, SQL_ATTR_CURSOR_TYPE,
                reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_CURSOR_STATIC)), 0);
            if (!succeeded(rc))
                return odbcError(common::ErrorCode::CursorError, SQL_HANDLE_STMT, raw,
                                 "set SCROLL cursor");
        }
        if (const SQLRETURN rc = SQLPrepare(
                raw, reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.data())), SQL_NTS);
            !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw, "SQLPrepare");
        std::vector<ParamBinding> storage;
        if (const auto s = bindParameters(raw, params, storage, utf8NarrowBinding_); !s.ok()) return s;
        if (const SQLRETURN rc = SQLExecute(raw); !succeeded(rc))
            return odbcError(common::ErrorCode::CursorError, SQL_HANDLE_STMT, raw, "SQLExecute");
        auto cur = std::make_unique<OdbcCursor>(std::move(stmt), std::move(storage),
                                                opts.batch_size > 0 ? opts.batch_size : 256);
        const auto st = cur->setupResult();
        if (!st.ok()) return st;
        out = std::move(cur);
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        (void) opts;
        out.reset();
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::execute(const std::string &sql, int64_t &affected,
                                           common::GeneratedKeys &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        out.clear();
        affected = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (execute)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto status = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !status.ok())
            return status;
        StmtGuard stmt(raw);
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecDirect(
                raw, reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.c_str())), SQL_NTS);
            !executionCompleted(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw,
                             "SQLExecDirect(execute/keys)");
        const auto st = fetchRows(stmt.get(), out.rows);
        if (!st.ok()) return st;
        SQLLEN rows = 0;
        if (const SQLRETURN rc = SQLRowCount(raw, &rows); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw, "SQLRowCount");
        affected = rows < 0
                       ? static_cast<std::int64_t>(out.rows.rows().size())
                       : static_cast<std::int64_t>(rows);
        return common::Status::OK();
#else
        (void) sql;
        affected = 0;
        out.clear();
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::execute(const std::string &sql, const common::Params &params,
                                           int64_t &affected, common::GeneratedKeys &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        out.clear();
        affected = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (execute)");
        SQLHSTMT raw = SQL_NULL_HSTMT;
        std::vector<ParamBinding> storage;
        const auto status = prepareAndBind(static_cast<SQLHDBC>(dbc_), cfg_, sql, params,
                                           raw, storage, utf8NarrowBinding_);
        StmtGuard stmt(raw);
        if (!status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, raw);
        if (const SQLRETURN rc = SQLExecute(raw); !executionCompleted(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw,
                             "SQLExecute(keys)");
        const auto st = fetchRows(stmt.get(), out.rows);
        if (!st.ok()) return st;
        SQLLEN rows = 0;
        if (const SQLRETURN rc = SQLRowCount(stmt.get(), &rows); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt.get(),
                             "SQLRowCount");
        affected = rows < 0
                       ? static_cast<std::int64_t>(out.rows.rows().size())
                       : static_cast<std::int64_t>(rows);
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        affected = 0;
        out.clear();
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    bool OdbcConnection::supportsPrepared() const {
#ifdef SQLCONDUIT_ENABLE_ODBC
        return true;
#else
        return false;
#endif
    }

    common::Status OdbcConnection::prepare(const std::string &sql,
                                           const common::Params &typesSample,
                                           core::PreparedStatementHandle &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        out = core::PreparedStatementHandle{};
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (prepare)");
        const std::string key = sql + common::paramTypeSignature(typesSample);
        if (const auto it = preparedCache_.find(key); it != preparedCache_.end()) {
            preparedLru_.splice(preparedLru_.end(), preparedLru_, it->second.lru);
            out = it->second.handle;
            return common::Status::OK();
        }
        SQLHSTMT raw = SQL_NULL_HSTMT;
        if (const auto status = newStatement(static_cast<SQLHDBC>(dbc_), cfg_, raw); !status.ok())
            return status;
        if (const SQLRETURN rc = SQLPrepare(
                raw, reinterpret_cast<SQLCHAR *>(const_cast<char *>(sql.data())), SQL_NTS);
            !succeeded(rc)) {
            SQLFreeHandle(SQL_HANDLE_STMT, raw);
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, raw, "SQLPrepare");
        }
        const std::uint64_t id = ++preparedSeq_;
        core::PreparedStatementHandle h =
                core::PreparedStatementHandle::make(id, reinterpret_cast<void *>(raw));
        preparedLru_.push_back(key);
        preparedCache_.emplace(key, PreparedEntry{h, std::prev(preparedLru_.end())});
        preparedKeys_[id] = key;
        if (preparedLimit_ > 0) {
            while (preparedCache_.size() > static_cast<std::size_t>(preparedLimit_)) {
                const std::string oldKey = preparedLru_.front();
                preparedLru_.pop_front();
                if (const auto oit = preparedCache_.find(oldKey); oit != preparedCache_.end()) {
                    preparedKeys_.erase(oit->second.handle.id());
                    SQLHSTMT old = reinterpret_cast<SQLHSTMT>(oit->second.handle.native());
                    if (old != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, old);
                    preparedCache_.erase(oit);
                }
            }
        }
        out = h;
        return common::Status::OK();
#else
        (void) sql;
        (void) typesSample;
        out = core::PreparedStatementHandle{};
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                   const common::Params &params,
                                                   common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        out.clear();
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (executePrepared)");
        const auto key = preparedKeys_.find(h.id());
        const auto cached = key == preparedKeys_.end()
                                ? preparedCache_.end()
                                : preparedCache_.find(key->second);
        if (!h.valid() || cached == preparedCache_.end() ||
            cached->second.handle.native() == nullptr)
            return common::Status::error(common::ErrorCode::QueryError,
                                         "ODBC: prepared handle is invalid or has been evicted");
        SQLHSTMT stmt = reinterpret_cast<SQLHSTMT>(cached->second.handle.native());
        std::vector<ParamBinding> storage;
        PreparedStmtReset reset(stmt);
        if (const auto status = bindParameters(stmt, params, storage, utf8NarrowBinding_);
            !status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, reinterpret_cast<void *>(stmt));
        if (const SQLRETURN rc = SQLExecute(stmt); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt,
                             "SQLExecute(prepared)");
        return fetchRows(stmt, out);
#else
        (void) h;
        (void) params;
        out.clear();
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    common::Status OdbcConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                   const common::Params &params,
                                                   int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        affected = 0;
        if (!open_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "ODBC: not connected (executePrepared)");
        const auto key = preparedKeys_.find(h.id());
        const auto cached = key == preparedKeys_.end()
                                ? preparedCache_.end()
                                : preparedCache_.find(key->second);
        if (!h.valid() || cached == preparedCache_.end() ||
            cached->second.handle.native() == nullptr)
            return common::Status::error(common::ErrorCode::QueryError,
                                         "ODBC: prepared handle is invalid or has been evicted");
        SQLHSTMT stmt = reinterpret_cast<SQLHSTMT>(cached->second.handle.native());
        std::vector<ParamBinding> storage;
        PreparedStmtReset reset(stmt);
        if (const auto status = bindParameters(stmt, params, storage, utf8NarrowBinding_);
            !status.ok()) return status;
        ActiveStatement active(activeStmtMtx_, activeStmt_, reinterpret_cast<void *>(stmt));
        if (const SQLRETURN rc = SQLExecute(stmt); !executionCompleted(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt,
                             "SQLExecute(prepared)");
        SQLLEN rows = 0;
        if (const SQLRETURN rc = SQLRowCount(stmt, &rows); !succeeded(rc))
            return odbcError(common::ErrorCode::QueryError, SQL_HANDLE_STMT, stmt, "SQLRowCount");
        affected = rows < 0 ? 0 : static_cast<std::int64_t>(rows);
        return common::Status::OK();
#else
        (void) h;
        (void) params;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "ODBC driver disabled");
#endif
    }

    void OdbcConnection::closeAllPrepared() {
#ifdef SQLCONDUIT_ENABLE_ODBC
        for (auto &kv: preparedCache_) {
            SQLHSTMT stmt = reinterpret_cast<SQLHSTMT>(kv.second.handle.native());
            if (stmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        }
        preparedCache_.clear();
        preparedKeys_.clear();
        preparedLru_.clear();
#endif
    }

    void OdbcConnection::setPreparedCacheLimit(int maxPerConnection) {
#ifdef SQLCONDUIT_ENABLE_ODBC
        preparedLimit_ = maxPerConnection;
        while (preparedLimit_ > 0 &&
               preparedCache_.size() > static_cast<std::size_t>(preparedLimit_)) {
            const std::string oldKey = preparedLru_.front();
            preparedLru_.pop_front();
            const auto old = preparedCache_.find(oldKey);
            if (old == preparedCache_.end()) continue;
            preparedKeys_.erase(old->second.handle.id());
            SQLHSTMT stmt = reinterpret_cast<SQLHSTMT>(old->second.handle.native());
            if (stmt != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            preparedCache_.erase(old);
        }
#else
        (void) maxPerConnection;
#endif
    }

}

namespace sqlconduit::drivers {
    driver::DriverRegistration odbc() {
        return {"odbc", [] { return std::make_unique<driver::OdbcDriver>(); }};
    }
}
