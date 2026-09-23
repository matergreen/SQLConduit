#include "sqlconduit/driver/mysql_driver.h"
#include "sqlconduit/drivers/mysql.h"
#include "sqlconduit/common/logger.h"
#include "sqlconduit/driver/driver_registry.h"

#include <cstring>
#include <algorithm>
#include <string>
#include <memory>
#include <utility>
#include <vector>

#ifdef SQLCONDUIT_ENABLE_MYSQL
#include <mysql.h>
#endif

namespace sqlconduit::driver {
    namespace {
        [[maybe_unused]] bool validSavepointName(const std::string &name) {
            if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) ||
                                  name[0] == '_'))
                return false;
            return std::all_of(name.begin() + 1, name.end(), [](const char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            });
        }

        [[maybe_unused]] common::Status notConnected(const char *where) {
            return common::Status::error(common::ErrorCode::NotConnected,
                                         std::string("MySQL: not connected (") + where + ")");
        }

#ifdef SQLCONDUIT_ENABLE_MYSQL
        constexpr std::size_t kInitialColBytes = 4096;

        class StmtGuard {
        public:
            explicit StmtGuard(MYSQL_STMT *s = nullptr) : s_(s) {
            }

            ~StmtGuard() { if (s_) mysql_stmt_close(s_); }

            StmtGuard(const StmtGuard &) = delete;

            StmtGuard &operator=(const StmtGuard &) = delete;

            StmtGuard(StmtGuard &&other) noexcept : s_(other.s_) { other.s_ = nullptr; }

            StmtGuard &operator=(StmtGuard &&other) noexcept {
                if (this != &other) {
                    if (s_) mysql_stmt_close(s_);
                    s_ = other.s_;
                    other.s_ = nullptr;
                }
                return *this;
            }

            MYSQL_STMT *get() const { return s_; }

        private:
            MYSQL_STMT *s_;
        };

        class ActiveMysqlOperation {
        public:
            ActiveMysqlOperation(std::mutex &mutex, unsigned long &slot,
                                 const unsigned long threadId)
                : mutex_(mutex), slot_(slot), threadId_(threadId) {
                std::lock_guard<std::mutex> lock(mutex_);
                slot_ = threadId_;
            }

            ~ActiveMysqlOperation() {
                std::lock_guard<std::mutex> lock(mutex_);
                if (slot_ == threadId_) slot_ = 0;
            }

            ActiveMysqlOperation(const ActiveMysqlOperation &) = delete;

            ActiveMysqlOperation &operator=(const ActiveMysqlOperation &) = delete;

        private:
            std::mutex &mutex_;
            unsigned long &slot_;
            unsigned long threadId_;
        };

        struct ParamStorage {
            std::vector<MYSQL_BIND> bind;
            std::vector<std::vector<char> > strBuf;
            MysqlBoolArray isNull;
            std::vector<long long> intBuf;
            std::vector<unsigned long long> uintBuf;
            std::vector<double> dblBuf;
            std::vector<unsigned long> len;
        };

        void setStringParam(ParamStorage &st, std::size_t i, const char *data, std::size_t size) {
            st.strBuf[i].assign(data, data + size);
            if (st.strBuf[i].empty()) st.strBuf[i].push_back('\0');
            st.len[i] = static_cast<unsigned long>(size);
            st.bind[i].buffer = st.strBuf[i].data();
            st.bind[i].buffer_length = st.len[i];
        }

        common::Value fieldToValue(enum_field_types type, unsigned int flags,
                                   const char *data, unsigned long len) {
            using common::Value;
            switch (type) {
                case MYSQL_TYPE_TINY:
                case MYSQL_TYPE_SHORT:
                case MYSQL_TYPE_INT24:
                case MYSQL_TYPE_LONG:
                case MYSQL_TYPE_LONGLONG: {
                    if ((flags & UNSIGNED_FLAG) != 0) {
                        try {
                            return Value{
                                static_cast<std::uint64_t>(
                                    std::stoull(std::string(data, len)))
                            };
                        } catch (...) {
                            return Value{std::string(data, len)};
                        }
                    }
                    try { return Value{std::stoll(std::string(data, len))}; } catch (...) {
                        return Value{std::string(data, len)};
                    }
                }
                case MYSQL_TYPE_FLOAT:
                case MYSQL_TYPE_DOUBLE: {
                    try { return Value{std::stod(std::string(data, len))}; } catch (...) {
                        return Value{std::string(data, len)};
                    }
                }
                case MYSQL_TYPE_DECIMAL:
                case MYSQL_TYPE_NEWDECIMAL: {
                    return Value{common::Decimal{std::string(data, len)}};
                }
                case MYSQL_TYPE_DATE:
                case MYSQL_TYPE_NEWDATE:
                    return Value{common::Date{std::string(data, len)}};
                case MYSQL_TYPE_TIME:
                    return Value{common::Time{std::string(data, len)}};
                case MYSQL_TYPE_DATETIME:
                case MYSQL_TYPE_TIMESTAMP: {
                    const std::string s(data, len);
                    common::Timestamp ts{};
                    if (common::tryParseTimestamp(s, ts)) return Value{ts};
                    return Value{s};
                }
                case static_cast<enum_field_types>(245):
                    return Value{common::Json{std::string(data, len)}};
                case MYSQL_TYPE_TINY_BLOB:
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_MEDIUM_BLOB:
                case MYSQL_TYPE_LONG_BLOB: {
                    common::Blob b(len);
                    if (len > 0) std::memcpy(b.data(), data, len);
                    return Value{std::move(b)};
                }
                default:
                    return Value{std::string(data, len)};
            }
        }
#endif
    }

#ifdef SQLCONDUIT_ENABLE_MYSQL
    namespace {
        common::Status rowLimitExceeded(std::uint64_t rows, int limit) {
            if (limit <= 0 || rows <= static_cast<std::uint64_t>(limit))
                return common::Status::OK();
            return common::Status::error(
                common::ErrorCode::QueryError,
                "result set exceeded max_result_rows (" + std::to_string(rows)
                + " > " + std::to_string(limit)
                + "); use queryEach() to stream the result instead");
        }
    }
#endif

    common::Status MySQLConnection::connect(const config::DataSourceConfig &cfg) {
        cfg_ = cfg;
#ifdef SQLCONDUIT_ENABLE_MYSQL
        close();

        m_ = mysql_init(nullptr);
        if (!m_) {
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "MySQL: mysql_init failed (out of memory)");
        }

        if (cfg.connection_timeout_ms > 0) {
            unsigned int t = static_cast<unsigned int>(cfg.connection_timeout_ms / 1000);
            mysql_options(m_, MYSQL_OPT_CONNECT_TIMEOUT, &t);
        }
        const int ioTimeoutMs = cfg.query_timeout_ms > 0
                                    ? cfg.query_timeout_ms
                                    : cfg.socket_timeout_ms;
        if (ioTimeoutMs > 0) {
            unsigned int t = static_cast<unsigned int>(std::max(1, (ioTimeoutMs + 999) / 1000));
            mysql_options(m_, MYSQL_OPT_READ_TIMEOUT, &t);
            mysql_options(m_, MYSQL_OPT_WRITE_TIMEOUT, &t);
        }
        auto it = cfg.extra.find("charset");
        if (it != cfg.extra.end()) {
            mysql_options(m_, MYSQL_SET_CHARSET_NAME, it->second.c_str());
        }
        if (cfg.tls_enabled) {
            mysql_ssl_set(m_,
                          cfg.tls_key.empty() ? nullptr : cfg.tls_key.c_str(),
                          cfg.tls_cert.empty() ? nullptr : cfg.tls_cert.c_str(),
                          cfg.tls_ca.empty() ? nullptr : cfg.tls_ca.c_str(),
                          nullptr, nullptr);
#if defined(MYSQL_VERSION_ID) && MYSQL_VERSION_ID >= 80000 && !defined(MARIADB_VERSION_ID)
        const mysql_ssl_mode mode = cfg.tls_verify_peer
                                        ? SSL_MODE_VERIFY_IDENTITY
                                        : SSL_MODE_REQUIRED;
        mysql_options(m_, MYSQL_OPT_SSL_MODE, &mode);
#else
        MysqlBool verify = cfg.tls_verify_peer ? 1 : 0;
        mysql_options(m_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
#endif
        }

        const char *host = cfg.host.empty() ? nullptr : cfg.host.c_str();
        const char *user = cfg.user.empty() ? nullptr : cfg.user.c_str();
        const char *pass = cfg.password.empty() ? nullptr : cfg.password.c_str();
        const char *db = cfg.database.empty() ? nullptr : cfg.database.c_str();

        if (!mysql_real_connect(m_, host, user, pass, db,
                                cfg.port, nullptr, 0)) {
            std::string err = "MySQL connect failed: ";
            err += mysql_error(m_);
            const auto native = static_cast<std::int64_t>(mysql_errno(m_));
            const std::string state = mysql_sqlstate(m_) ? mysql_sqlstate(m_) : "";
            mysql_close(m_);
            m_ = nullptr;
            open_ = false;
            return common::Status::databaseError(common::ErrorCode::ConnectionFailed,
                                                 cfg.redact(std::move(err)), state, native);
        }

        open_ = true;
        return common::Status::OK();
#else
        open_ = false;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "MySQL driver not built. Rebuild with -DSQLCONDUIT_ENABLE_MYSQL=ON");
#endif
    }

#ifdef SQLCONDUIT_ENABLE_MYSQL
    namespace {
        void freeMysqlResult(MYSQL_RES *r) { if (r) mysql_free_result(r); }

        common::Status fillResultSet(MYSQL_RES *res, const config::DataSourceConfig &cfg,
                                     common::ResultSet &out) {
            if (const auto st = rowLimitExceeded(mysql_num_rows(res), cfg.max_result_rows);
                !st.ok())
                return st;

            const unsigned int nfields = mysql_num_fields(res);
            MYSQL_FIELD *fields = mysql_fetch_fields(res);

            std::vector<std::string> names;
            names.reserve(nfields);
            for (unsigned int i = 0; i < nfields; ++i) names.emplace_back(fields[i].name);
            out.setFields(std::move(names));

            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res)) != nullptr) {
                unsigned long *lengths = mysql_fetch_lengths(res);
                common::Row r;
                for (unsigned int i = 0; i < nfields; ++i) {
                    const char *colName = fields[i].name;
                    if (row[i] == nullptr) {
                        r.set(colName, nullptr);
                        continue;
                    }
                    r.set(colName, fieldToValue(fields[i].type, fields[i].flags,
                                                row[i], lengths[i]));
                }
                out.addRow(std::move(r));
            }
            return common::Status::OK();
        }
    }
#endif

    common::Status MySQLConnection::ping() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("ping");
        if (mysql_ping(m_) == 0) return common::Status::OK();
        return lastError("mysql_ping");
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::query(const std::string &sql, common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("query");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        if (mysql_real_query(m_, sql.data(), sql.size()) != 0)
            return lastError("mysql_real_query");

        std::unique_ptr<MYSQL_RES, void(*)(MYSQL_RES *)> res(
            mysql_store_result(m_), &freeMysqlResult);
        if (!res) {
            if (mysql_field_count(m_) == 0) return drainRemainingResults();
            return lastError("mysql_store_result");
        }

        if (const auto st = fillResultSet(res.get(), cfg_, out); !st.ok()) return st;
        return drainRemainingResults();
#else
        (void) sql;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::queryAll(const std::string &sql,
                                             std::vector<common::ResultSet> &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out.clear();
        if (!open_ || !m_) return notConnected("queryAll");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        if (mysql_real_query(m_, sql.data(), sql.size()) != 0)
            return lastError("mysql_real_query");

        for (;;) {
            std::unique_ptr<MYSQL_RES, void(*)(MYSQL_RES *)>
            res(
                mysql_store_result(m_), &freeMysqlResult);
            if (res) {
                common::ResultSet rs;
                if (const auto st = fillResultSet(res.get(), cfg_, rs); !st.ok()) return st;
                out.push_back(std::move(rs));
            } else if (mysql_field_count(m_) != 0) {
                return lastError("mysql_store_result");
            }
            const int rc = mysql_next_result(m_);
            if (rc > 0) return lastError("mysql_next_result");
            if (rc < 0) break;
        }
        return common::Status::OK();
#else
        (void) sql;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::queryAll(const std::string &sql,
                                             const common::Params &params,
                                             std::vector<common::ResultSet> &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out.clear();
        if (params.empty()) return queryAll(sql, out);
        std::string built;
        if (const auto st = buildSql(sql, params, built); !st.ok()) return st;
        return queryAll(built, out);
#else
        (void) sql;
        (void) params;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::drainRemainingResults() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        std::size_t discarded = 0;
        for (;;) {
            const int rc = mysql_next_result(m_);
            if (rc > 0) return lastError("mysql_next_result");
            if (rc < 0) break;
            std::unique_ptr<MYSQL_RES, void(*)(MYSQL_RES *)>
            res(
                mysql_store_result(m_), &freeMysqlResult);
            ++discarded;
        }
        if (discarded > 0)
            SQLCONDUIT_LOG_WARN("mysql: discarded " + std::to_string(discarded) +
            " extra result set(s); use queryAll() to collect them");
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::execute(const std::string &sql, std::int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        affected = 0;
        if (!open_ || !m_) return notConnected("execute");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        if (mysql_real_query(m_, sql.data(), sql.size()) != 0)
            return lastError("mysql_real_query");
        affected = static_cast<std::int64_t>(mysql_affected_rows(m_));
        return drainRemainingResults();
#else
        (void) sql;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

#ifdef SQLCONDUIT_ENABLE_MYSQL
    namespace {
        common::Status bindParamsOnly(MYSQL_STMT *stmt, const common::Params &params,
                                      ParamStorage &st) {
            const unsigned long want = mysql_stmt_param_count(stmt);
            if (want != params.size()) {
                return common::Status::error(
                    common::ErrorCode::QueryError,
                    "parameter mismatch: statement expects " + std::to_string(want)
                    + " parameter(s) but " + std::to_string(params.size()) + " supplied");
            }
            if (want == 0) return common::Status::OK();

            const std::size_t n = params.size();
            st.bind.resize(n);
            std::memset(st.bind.data(), 0, n * sizeof(MYSQL_BIND));
            st.strBuf.resize(n);
            st.isNull = std::make_unique<MysqlBool[]>(n);
            st.intBuf.assign(n, 0);
            st.uintBuf.assign(n, 0);
            st.dblBuf.assign(n, 0.0);
            st.len.assign(n, 0);

            for (std::size_t i = 0; i < n; ++i) {
                const auto &v = params[i];
                MYSQL_BIND &b = st.bind[i];
                b.length = &st.len[i];
                b.is_null = &st.isNull[i];

                if (std::holds_alternative<std::nullptr_t>(v)) {
                    b.buffer_type = MYSQL_TYPE_NULL;
                    st.isNull[i] = 1;
                } else if (const auto *x = std::get_if<bool>(&v)) {
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    st.intBuf[i] = *x ? 1 : 0;
                    b.buffer = &st.intBuf[i];
                } else if (const auto *x = std::get_if<std::int64_t>(&v)) {
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    st.intBuf[i] = *x;
                    b.buffer = &st.intBuf[i];
                } else if (const auto *x = std::get_if<std::uint64_t>(&v)) {
                    b.buffer_type = MYSQL_TYPE_LONGLONG;
                    b.is_unsigned = 1;
                    st.uintBuf[i] = static_cast<unsigned long long>(*x);
                    b.buffer = &st.uintBuf[i];
                } else if (const auto *x = std::get_if<double>(&v)) {
                    b.buffer_type = MYSQL_TYPE_DOUBLE;
                    st.dblBuf[i] = *x;
                    b.buffer = &st.dblBuf[i];
                } else if (const auto *x = std::get_if<common::Timestamp>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    const std::string s = common::timestampToStringMs(*x);
                    setStringParam(st, i, s.data(), s.size());
                } else if (const auto *x = std::get_if<common::Decimal>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::Date>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::Time>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::Uuid>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::Json>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::IntervalYearMonth>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::IntervalDaySecond>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->value.data(), x->value.size());
                } else if (const auto *x = std::get_if<common::Blob>(&v)) {
                    b.buffer_type = MYSQL_TYPE_BLOB;
                    st.strBuf[i].resize(x->size());
                    if (!x->empty()) std::memcpy(st.strBuf[i].data(), x->data(), x->size());
                    if (st.strBuf[i].empty()) st.strBuf[i].push_back('\0');
                    st.len[i] = static_cast<unsigned long>(x->size());
                    b.buffer = st.strBuf[i].data();
                    b.buffer_length = st.len[i];
                } else if (const auto *x = std::get_if<std::string>(&v)) {
                    b.buffer_type = MYSQL_TYPE_STRING;
                    setStringParam(st, i, x->data(), x->size());
                } else if (std::holds_alternative<common::Array>(v) ||
                           std::holds_alternative<common::Composite>(v) ||
                           std::holds_alternative<common::TypedArray>(v) ||
                           std::holds_alternative<common::TypedComposite>(v)) {
                    return common::Status::error(
                        common::ErrorCode::NotSupported,
                        "MySQL: array/composite parameters are not supported by this driver");
                } else {
                    b.buffer_type = MYSQL_TYPE_NULL;
                    st.isNull[i] = 1;
                }
            }

            if (mysql_stmt_bind_param(stmt, st.bind.data()) != 0) {
                return common::Status::error(common::ErrorCode::QueryError,
                                             std::string("mysql_stmt_bind_param: ") + mysql_stmt_error(stmt));
            }
            return common::Status::OK();
        }

        common::Status prepareAndBind(MYSQL *m, const std::string &sql,
                                      const common::Params &params,
                                      StmtGuard &guard, ParamStorage &st) {
            MYSQL_STMT *stmt = mysql_stmt_init(m);
            if (!stmt) {
                return common::Status::error(common::ErrorCode::QueryError,
                                             "MySQL: mysql_stmt_init failed (out of memory)");
            }
            guard = StmtGuard(stmt);

            if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
                return common::Status::error(common::ErrorCode::QueryError,
                                             std::string("mysql_stmt_prepare: ") + mysql_stmt_error(stmt));
            }

            return bindParamsOnly(stmt, params, st);
        }

        common::Status fetchPreparedInternal(MYSQL_STMT *stmt, common::ResultSet *out,
                                             const common::RowCallback *callback,
                                             std::uint64_t *delivered,
                                             int maxRows = 0) {
            std::unique_ptr<MYSQL_RES, void(*)(MYSQL_RES *)>
            meta(
                mysql_stmt_result_metadata(stmt),
                [](MYSQL_RES *r) { if (r) mysql_free_result(r); });
            if (!meta) return common::Status::OK();

            const unsigned int nfields = mysql_num_fields(meta.get());
            MYSQL_FIELD *fields = mysql_fetch_fields(meta.get());
            if (nfields == 0) return common::Status::OK();

            std::vector<std::vector<char> > buf(nfields);
            std::vector<unsigned long> len(nfields);
            MysqlBoolArray isNull = std::make_unique<MysqlBool[]>(nfields);
            std::vector<MYSQL_BIND> bind(nfields);
            std::memset(bind.data(), 0, nfields * sizeof(MYSQL_BIND));

            for (unsigned int i = 0; i < nfields; ++i) {
                buf[i].resize(kInitialColBytes);
                bind[i].buffer_type = MYSQL_TYPE_STRING;
                bind[i].buffer = buf[i].data();
                bind[i].buffer_length = kInitialColBytes;
                bind[i].length = &len[i];
                bind[i].is_null = &isNull[i];
            }

            if (mysql_stmt_bind_result(stmt, bind.data()) != 0) {
                return common::Status::error(common::ErrorCode::QueryError,
                                             std::string("mysql_stmt_bind_result: ") + mysql_stmt_error(stmt));
            }

            std::vector<std::string> names;
            names.reserve(nfields);
            for (unsigned int i = 0; i < nfields; ++i) names.emplace_back(fields[i].name);
            if (out) out->setFields(names);
            if (delivered) *delivered = 0;
            std::uint64_t fetched = 0;

            for (;;) {
                const int rc = mysql_stmt_fetch(stmt);
                if (rc == MYSQL_NO_DATA) break;
                if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
                    return common::Status::error(common::ErrorCode::QueryError,
                                                 std::string("mysql_stmt_fetch: ") + mysql_stmt_error(stmt));
                }

                if (rc == MYSQL_DATA_TRUNCATED) {
                    for (unsigned int i = 0; i < nfields; ++i) {
                        if (len[i] <= buf[i].size()) continue;
                        buf[i].resize(static_cast<std::size_t>(len[i]) + 1);
                        bind[i].buffer = buf[i].data();
                        bind[i].buffer_length = len[i];
                        MYSQL_BIND one = bind[i];
                        if (mysql_stmt_fetch_column(stmt, &one, i, 0) != 0) {
                            return common::Status::error(
                                common::ErrorCode::QueryError,
                                std::string("mysql_stmt_fetch_column: ") + mysql_stmt_error(stmt));
                        }
                    }
                }

                common::Row r;
                for (unsigned int i = 0; i < nfields; ++i) {
                    const char *colName = fields[i].name;
                    if (isNull[i]) {
                        r.set(colName, nullptr);
                        continue;
                    }
                    r.set(colName, fieldToValue(fields[i].type, fields[i].flags,
                                                buf[i].data(), len[i]));
                }
                if (out) {
                    ++fetched;
                    if (maxRows > 0 && fetched > static_cast<std::uint64_t>(maxRows))
                        return rowLimitExceeded(fetched, maxRows);
                    out->addRow(std::move(r));
                } else if (callback) {
                    if (delivered) ++*delivered;
                    if (*callback && !(*callback)(r)) break;
                }
            }
            return common::Status::OK();
        }

        common::Status fetchPrepared(MYSQL_STMT *stmt, common::ResultSet &out,
                                     int maxRows = 0) {
            return fetchPreparedInternal(stmt, &out, nullptr, nullptr, maxRows);
        }

        common::Status fetchPreparedEach(MYSQL_STMT *stmt,
                                         const common::RowCallback &callback,
                                         std::uint64_t &rows) {
            return fetchPreparedInternal(stmt, nullptr, &callback, &rows);
        }
    }

    class MyCursor : public core::ICursor {
    public:
        MyCursor(MySQLConnection &owner, StmtGuard guard, ParamStorage storage,
                 std::size_t batchSize)
            : owner_(owner), guard_(std::move(guard)), storage_(std::move(storage)),
              batchSize_(batchSize) {
        }

        ~MyCursor() override { reset(); }

        common::Status setupResult() {
            meta_ = mysql_stmt_result_metadata(guard_.get());
            if (!meta_) {
                eof_ = true;
                return common::Status::OK();
            }
            nfields_ = mysql_num_fields(meta_);
            fields_.reserve(nfields_);
            types_.reserve(nfields_);
            flags_.reserve(nfields_);
            MYSQL_FIELD *f = mysql_fetch_fields(meta_);
            for (unsigned int i = 0; i < nfields_; ++i) {
                fields_.emplace_back(f[i].name);
                types_.push_back(f[i].type);
                flags_.push_back(f[i].flags);
            }
            buf_.assign(nfields_, std::vector<char>(kInitialColBytes));
            len_.assign(nfields_, 0);
            isNull_ = std::make_unique<MysqlBool[]>(nfields_);
            bind_.resize(nfields_);
            std::memset(bind_.data(), 0, nfields_ * sizeof(MYSQL_BIND));
            for (unsigned int i = 0; i < nfields_; ++i) {
                bind_[i].buffer_type = MYSQL_TYPE_STRING;
                bind_[i].buffer = buf_[i].data();
                bind_[i].buffer_length = static_cast<unsigned long>(kInitialColBytes);
                bind_[i].length = &len_[i];
                bind_[i].is_null = &isNull_[i];
            }
            if (mysql_stmt_bind_result(guard_.get(), bind_.data()) != 0)
                return owner_.lastError("mysql_stmt_bind_result(openCursor)");
            open_ = true;
            return common::Status::OK();
        }

        common::Status fetch(std::size_t n, common::ResultSet &out) override {
            if (!open_ || eof_) return common::Status::OK();
            ActiveMysqlOperation active(owner_.operationMtx_, owner_.activeThreadId_,
                                        mysql_thread_id(owner_.m_));
            if (!fieldsSet_) {
                out.setFields(fields_);
                fieldsSet_ = true;
            }
            const std::size_t want = (n == 0) ? batchSize_ : n;
            for (std::size_t i = 0; i < want; ++i) {
                const int rc = mysql_stmt_fetch(guard_.get());
                if (rc == MYSQL_NO_DATA) {
                    eof_ = true;
                    break;
                }
                if (rc != 0 && rc != MYSQL_DATA_TRUNCATED)
                    return owner_.lastError("mysql_stmt_fetch(openCursor)");
                if (rc == MYSQL_DATA_TRUNCATED) {
                    for (unsigned int c = 0; c < nfields_; ++c) {
                        if (len_[c] <= buf_[c].size()) continue;
                        buf_[c].resize(static_cast<std::size_t>(len_[c]) + 1);
                        bind_[c].buffer = buf_[c].data();
                        bind_[c].buffer_length = len_[c];
                        MYSQL_BIND one = bind_[c];
                        if (mysql_stmt_fetch_column(guard_.get(), &one, c, 0) != 0)
                            return owner_.lastError("mysql_stmt_fetch_column(openCursor)");
                    }
                }
                common::Row row;
                for (unsigned int c = 0; c < nfields_; ++c) {
                    const char *colName = fields_[c].c_str();
                    if (isNull_[c]) {
                        row.set(colName, nullptr);
                        continue;
                    }
                    row.set(colName, fieldToValue(types_[c], flags_[c],
                                                  buf_[c].data(), len_[c]));
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
            outRow = std::move(tmp.rows()[0]);
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
            if (meta_) {
                mysql_free_result(meta_);
                meta_ = nullptr;
            }
            guard_ = StmtGuard(nullptr);
            open_ = false;
        }

        MySQLConnection &owner_;
        StmtGuard guard_;
        ParamStorage storage_;
        std::size_t batchSize_;
        MYSQL_RES *meta_ = nullptr;
        unsigned int nfields_ = 0;
        std::vector<std::string> fields_;
        std::vector<enum_field_types> types_;
        std::vector<unsigned int> flags_;
        std::vector<std::vector<char> > buf_;
        std::vector<unsigned long> len_;
        MysqlBoolArray isNull_;
        std::vector<MYSQL_BIND> bind_;
        bool open_ = false;
        bool eof_ = false;
        bool fieldsSet_ = false;
        std::uint64_t rowsFetched_ = 0;
    };

#endif

    common::Status MySQLConnection::query(const std::string &sql, const common::Params &params,
                                          common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("query");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));

        StmtGuard guard(nullptr);
        ParamStorage st;
        if (const auto s = prepareAndBind(m_, sql, params, guard, st); !s.ok()) return s;

        if (mysql_stmt_execute(guard.get()) != 0) return stmtError("mysql_stmt_execute", guard.get());
        return fetchPrepared(guard.get(), out, cfg_.max_result_rows);
#else
        (void) sql;
        (void) params;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::execute(const std::string &sql, const common::Params &params,
                                            std::int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        affected = 0;
        if (!open_ || !m_) return notConnected("execute");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));

        StmtGuard guard(nullptr);
        ParamStorage st;
        if (const auto s = prepareAndBind(m_, sql, params, guard, st); !s.ok()) return s;

        if (mysql_stmt_execute(guard.get()) != 0) return stmtError("mysql_stmt_execute", guard.get());
        affected = static_cast<std::int64_t>(mysql_stmt_affected_rows(guard.get()));
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::execute(const std::string &sql, std::int64_t &affected,
                                            common::GeneratedKeys &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out.clear();
        if (const auto s = execute(sql, affected); !s.ok()) return s;
        const auto id = mysql_insert_id(m_);
        if (id != 0) {
            common::Row r;
            r.set("insert_id", static_cast<std::int64_t>(id));
            out.rows.addRow(std::move(r));
        }
        return common::Status::OK();
#else
        (void) sql;
        (void) affected;
        out.clear();
        return common::Status::error(common::ErrorCode::NotSupported, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::execute(const std::string &sql, const common::Params &params,
                                            std::int64_t &affected, common::GeneratedKeys &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out.clear();
        if (!open_ || !m_) return notConnected("execute");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        StmtGuard guard(nullptr);
        ParamStorage st;
        if (const auto s = prepareAndBind(m_, sql, params, guard, st); !s.ok()) return s;
        if (mysql_stmt_execute(guard.get()) != 0)
            return stmtError("mysql_stmt_execute", guard.get());
        affected = static_cast<std::int64_t>(mysql_stmt_affected_rows(guard.get()));
        const auto id = mysql_insert_id(m_);
        if (id != 0) {
            common::Row r;
            r.set("insert_id", static_cast<std::int64_t>(id));
            out.rows.addRow(std::move(r));
        }
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        (void) affected;
        out.clear();
        return common::Status::error(common::ErrorCode::NotSupported, "MySQL driver disabled");
#endif
    }

    bool MySQLConnection::supportsPrepared() const {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        return true;
#else
        return false;
#endif
    }

    common::Status MySQLConnection::prepare(const std::string &sql,
                                            const common::Params &typesSample,
                                            core::PreparedStatementHandle &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out = core::PreparedStatementHandle{};
        if (!open_ || !m_) return notConnected("prepare");
        const std::string key = sql + common::paramTypeSignature(typesSample);
        if (const auto it = preparedCache_.find(key); it != preparedCache_.end()) {
            preparedLru_.remove(key);
            preparedLru_.push_back(key);
            out = it->second;
            return common::Status::OK();
        }
        MYSQL_STMT *stmt = mysql_stmt_init(m_);
        if (!stmt)
            return common::Status::error(common::ErrorCode::QueryError,
                                         "MySQL: mysql_stmt_init failed (out of memory)");
        if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            const auto msg = std::string("mysql_stmt_prepare: ") + mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return common::Status::error(common::ErrorCode::QueryError, std::move(msg));
        }
        const auto id = ++preparedSeq_;
        core::PreparedStatementHandle h =
                core::PreparedStatementHandle::make(id, static_cast<void *>(stmt));
        preparedCache_[key] = h;
        preparedKeys_[id] = key;
        preparedLru_.push_back(key);
        if (preparedLimit_ > 0) {
            while (preparedCache_.size() > static_cast<std::size_t>(preparedLimit_)) {
                const std::string oldKey = preparedLru_.front();
                preparedLru_.pop_front();
                if (const auto oit = preparedCache_.find(oldKey); oit != preparedCache_.end()) {
                    preparedKeys_.erase(oit->second.id());
                    if (MYSQL_STMT *s = static_cast<MYSQL_STMT *>(oit->second.native()))
                        mysql_stmt_close(s);
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
        return common::Status::error(common::ErrorCode::NotSupported, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                    const common::Params &params,
                                                    common::ResultSet &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("executePrepared");
        const auto key = preparedKeys_.find(h.id());
        const auto cached = key == preparedKeys_.end()
                                ? preparedCache_.end()
                                : preparedCache_.find(key->second);
        MYSQL_STMT *stmt = cached == preparedCache_.end()
                               ? nullptr
                               : static_cast<MYSQL_STMT *>(cached->second.native());
        if (!h.valid() || !stmt)
            return common::Status::error(common::ErrorCode::QueryError,
                                         "MySQL: prepared handle is invalid or has been evicted");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        ParamStorage st;
        if (const auto s = bindParamsOnly(stmt, params, st); !s.ok()) return s;
        if (mysql_stmt_execute(stmt) != 0) return stmtError("mysql_stmt_execute(prepared)", stmt);
        return fetchPrepared(stmt, out, cfg_.max_result_rows);
#else
        (void) h;
        (void) params;
        (void) out;
        return common::Status::error(common::ErrorCode::NotSupported, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                    const common::Params &params,
                                                    std::int64_t &affected) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        affected = 0;
        if (!open_ || !m_) return notConnected("executePrepared");
        const auto key = preparedKeys_.find(h.id());
        const auto cached = key == preparedKeys_.end()
                                ? preparedCache_.end()
                                : preparedCache_.find(key->second);
        MYSQL_STMT *stmt = cached == preparedCache_.end()
                               ? nullptr
                               : static_cast<MYSQL_STMT *>(cached->second.native());
        if (!h.valid() || !stmt)
            return common::Status::error(common::ErrorCode::QueryError,
                                         "MySQL: prepared handle is invalid or has been evicted");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        ParamStorage st;
        if (const auto s = bindParamsOnly(stmt, params, st); !s.ok()) return s;
        if (mysql_stmt_execute(stmt) != 0) return stmtError("mysql_stmt_execute(prepared)", stmt);
        affected = static_cast<std::int64_t>(mysql_stmt_affected_rows(stmt));
        return common::Status::OK();
#else
        (void) h;
        (void) params;
        affected = 0;
        return common::Status::error(common::ErrorCode::NotSupported, "MySQL driver disabled");
#endif
    }

    void MySQLConnection::closeAllPrepared() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        for (auto &kv: preparedCache_) {
            if (MYSQL_STMT *s = static_cast<MYSQL_STMT *>(kv.second.native()))
                mysql_stmt_close(s);
        }
        preparedCache_.clear();
        preparedKeys_.clear();
        preparedLru_.clear();
#endif
    }

    void MySQLConnection::setPreparedCacheLimit(int maxPerConnection) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        preparedLimit_ = maxPerConnection;
#else
        (void) maxPerConnection;
#endif
    }

    common::Status MySQLConnection::queryEach(const std::string &sql,
                                              const common::Params &params,
                                              const common::RowCallback &callback,
                                              std::uint64_t &rows) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        rows = 0;
        if (!open_ || !m_) return notConnected("stream");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        StmtGuard guard(nullptr);
        ParamStorage storage;
        if (const auto status = prepareAndBind(m_, sql, params, guard, storage); !status.ok())
            return status;
        if (mysql_stmt_execute(guard.get()) != 0)
            return stmtError("mysql_stmt_execute(stream)", guard.get());
        return fetchPreparedEach(guard.get(), callback, rows);
#else
        (void) sql;
        (void) params;
        (void) callback;
        rows = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::openCursor(const std::string &sql, const common::Params &params,
                                               const core::CursorOptions &opts,
                                               std::unique_ptr<core::ICursor> &out) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        out.reset();
        if (!open_ || !m_) return notConnected("openCursor");
        ActiveMysqlOperation active(operationMtx_, activeThreadId_, mysql_thread_id(m_));
        StmtGuard guard(nullptr);
        ParamStorage storage;
        if (const auto s = prepareAndBind(m_, sql, params, guard, storage); !s.ok()) return s;
        if (mysql_stmt_execute(guard.get()) != 0)
            return stmtError("mysql_stmt_execute(openCursor)", guard.get());
        auto cur = std::make_unique<MyCursor>(*this, std::move(guard), std::move(storage),
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
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    std::string MySQLConnection::escapeLiteral(const common::Value &v) const {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (const auto *s = std::get_if<std::string>(&v)) {
            if (!m_ || !open_) return common::escapeLiteralGeneric(v);
            std::vector<char> buf(s->size() * 2 + 1, '\0');
            const unsigned long n = mysql_real_escape_string(
                m_, buf.data(), s->data(), static_cast<unsigned long>(s->size()));
            std::string out;
            out.reserve(static_cast<std::size_t>(n) + 2);
            out.push_back('\'');
            out.append(buf.data(), static_cast<std::size_t>(n));
            out.push_back('\'');
            return out;
        }
        return common::escapeLiteralGeneric(v);
#else
        return common::escapeLiteralGeneric(v);
#endif
    }

    common::Status MySQLConnection::begin() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("begin");
        if (txOpen_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "MySQL: a transaction is already open on this connection");
        if (mysql_query(m_, "START TRANSACTION") != 0) return lastError("START TRANSACTION");
        txOpen_ = true;
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::begin(const common::TransactionOptions &options) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("begin");
        if (txOpen_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "MySQL: a transaction is already open on this connection");
        const char *level = nullptr;
        switch (options.isolation) {
            case common::IsolationLevel::Default: break;
            case common::IsolationLevel::ReadUncommitted: level = "READ UNCOMMITTED";
                break;
            case common::IsolationLevel::ReadCommitted: level = "READ COMMITTED";
                break;
            case common::IsolationLevel::RepeatableRead: level = "REPEATABLE READ";
                break;
            case common::IsolationLevel::Serializable: level = "SERIALIZABLE";
                break;
        }
        if (level) {
            const std::string sql = std::string("SET TRANSACTION ISOLATION LEVEL ") + level;
            if (mysql_query(m_, sql.c_str()) != 0) return lastError("SET TRANSACTION ISOLATION");
        }
        if (options.readOnly && mysql_query(m_, "SET TRANSACTION READ ONLY") != 0)
            return lastError("SET TRANSACTION READ ONLY");
        return begin();
#else
        (void) options;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::commit() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("commit");
        if (mysql_query(m_, "COMMIT") != 0) return lastError("COMMIT");
        txOpen_ = false;
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::rollback() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!open_ || !m_) return notConnected("rollback");
        if (mysql_query(m_, "ROLLBACK") != 0) return lastError("ROLLBACK");
        txOpen_ = false;
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::savepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "MySQL: invalid savepoint or no active transaction");
        if (mysql_query(m_, ("SAVEPOINT " + name).c_str()) != 0) return lastError("SAVEPOINT");
        return common::Status::OK();
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::releaseSavepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "MySQL: invalid savepoint or no active transaction");
        if (mysql_query(m_, ("RELEASE SAVEPOINT " + name).c_str()) != 0)
            return lastError("RELEASE SAVEPOINT");
        return common::Status::OK();
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::rollbackToSavepoint(const std::string &name) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "MySQL: invalid savepoint or no active transaction");
        if (mysql_query(m_, ("ROLLBACK TO SAVEPOINT " + name).c_str()) != 0)
            return lastError("ROLLBACK TO SAVEPOINT");
        return common::Status::OK();
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    void MySQLConnection::close() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        closeAllPrepared();
        if (m_) {
            mysql_close(m_);
            m_ = nullptr;
        }
#endif
        open_ = false;
        txOpen_ = false;
    }

    common::Status MySQLConnection::cancel() {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        std::lock_guard<std::mutex> lock(operationMtx_);
        if (!m_ || !open_ || activeThreadId_ == 0)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "MySQL: no active query to cancel");

        MySQLConnection control;
        if (const auto status = control.connect(cfg_); !status.ok())
            return common::Status::error(common::ErrorCode::Cancelled,
                                         "MySQL cancel control connection failed: " + status.message);
        if (mysql_kill(control.m_, activeThreadId_) != 0)
            return common::Status::databaseError(
                common::ErrorCode::Cancelled,
                std::string("MySQL mysql_kill: ") + mysql_error(control.m_),
                mysql_sqlstate(control.m_) ? mysql_sqlstate(control.m_) : "",
                static_cast<std::int64_t>(mysql_errno(control.m_)));
        return common::Status::OK();
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "MySQL driver disabled");
#endif
    }

    common::Status MySQLConnection::lastError(const char *where) {
#ifdef SQLCONDUIT_ENABLE_MYSQL
        std::string msg = where;
        msg += ": ";
        msg += mysql_error(m_);
        return common::Status::databaseError(
            common::ErrorCode::QueryError, std::move(msg),
            mysql_sqlstate(m_) ? mysql_sqlstate(m_) : "",
            static_cast<std::int64_t>(mysql_errno(m_)));
#else
        (void) where;
        return common::Status::error(common::ErrorCode::QueryError, "MySQL error");
#endif
    }

#ifdef SQLCONDUIT_ENABLE_MYSQL
    common::Status MySQLConnection::stmtError(const char *where, MYSQL_STMT *stmt) {
        std::string msg = where;
        msg += ": ";
        msg += stmt ? mysql_stmt_error(stmt) : "(null statement)";
        return common::Status::databaseError(
            common::ErrorCode::QueryError, std::move(msg),
            stmt && mysql_stmt_sqlstate(stmt) ? mysql_stmt_sqlstate(stmt) : "",
            stmt ? static_cast<std::int64_t>(mysql_stmt_errno(stmt)) : 0);
    }
#endif

}

namespace sqlconduit::drivers {
    driver::DriverRegistration mysql() {
        return {"mysql", [] { return std::make_unique<driver::MySQLDriver>(); }};
    }
}
