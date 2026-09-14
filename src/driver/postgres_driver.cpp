#include "dbmw/driver/postgres_driver.h"
#include "dbmw/driver/driver_registry.h"

#include <optional>
#include <algorithm>
#include <string>
#include <memory>
#include <utility>
#include <vector>
#include <atomic>

#ifdef DBMW_ENABLE_POSTGRES
#include <pqxx/pqxx>

#if defined(PQXX_VERSION_MAJOR) && defined(PQXX_VERSION_MINOR)
#  if (PQXX_VERSION_MAJOR > 7) || (PQXX_VERSION_MAJOR == 7 && PQXX_VERSION_MINOR >= 10)
#    define DBMW_PQXX_HAS_EXEC_WITH_PARAMS 1
#  endif
#endif
#endif

namespace dbmw::driver {
    namespace {
        [[maybe_unused]] common::Status notConnected(const char *where) {
            return common::Status::error(common::ErrorCode::NotConnected,
                                         std::string("PostgreSQL: not connected (") + where + ")");
        }

        [[maybe_unused]] common::Status paramMismatch(std::size_t supplied,
                                                      std::size_t placeholders) {
            return common::Status::error(
                common::ErrorCode::QueryError,
                "parameter mismatch: supplied " + std::to_string(supplied)
                + " parameter(s) but SQL has " + std::to_string(placeholders)
                + " '?' placeholder(s)");
        }

#ifdef DBMW_ENABLE_POSTGRES
        common::Status postgresError(const common::ErrorCode fallback,
                                     const char *where, const std::exception &error) {
            std::string state;
            if (const auto *sql = dynamic_cast<const pqxx::sql_error *>(&error))
                state = sql->sqlstate();
            return common::Status::databaseError(
                fallback, std::string("PostgreSQL ") + where + ": " + error.what(),
                std::move(state));
        }

        class ActiveOperation {
        public:
            ActiveOperation(std::mutex &mutex, bool &active)
                : mutex_(mutex), active_(active) {
                std::lock_guard<std::mutex> lock(mutex_);
                active_ = true;
            }
            ~ActiveOperation() {
                std::lock_guard<std::mutex> lock(mutex_);
                active_ = false;
            }
            ActiveOperation(const ActiveOperation &) = delete;
            ActiveOperation &operator=(const ActiveOperation &) = delete;

        private:
            std::mutex &mutex_;
            bool &active_;
        };
#endif

        std::string toByteaHex(const common::Blob &b) {
            static const char *kHex = "0123456789abcdef";
            std::string s = "\\x";
            s.reserve(2 + b.size() * 2);
            for (const std::uint8_t byte: b) {
                s.push_back(kHex[(byte >> 4) & 0x0F]);
                s.push_back(kHex[byte & 0x0F]);
            }
            return s;
        }

        [[maybe_unused]] common::Blob parseBytea(const std::string &s) {
            common::Blob b;
            auto nibble = [](const char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            if (s.size() >= 2 && s[0] == '\\' && s[1] == 'x') {
                b.reserve((s.size() - 2) / 2);
                for (std::size_t i = 2; i + 1 < s.size(); i += 2) {
                    const int hi = nibble(s[i]);
                    const int lo = nibble(s[i + 1]);
                    if (hi < 0 || lo < 0) break;
                    b.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
                }
                return b;
            }
            b.assign(s.begin(), s.end());
            return b;
        }

#ifdef DBMW_ENABLE_POSTGRES
        std::string connValue(const std::string &value) {
            std::string out = "'";
            for (const char c: value) {
                if (c == '\\' || c == '\'') out.push_back('\\');
                out.push_back(c);
            }
            out.push_back('\'');
            return out;
        }

        static constexpr pqxx::oid kBool = 16;
        static constexpr pqxx::oid kBytea = 17;
        static constexpr pqxx::oid kInt8 = 20;
        static constexpr pqxx::oid kInt2 = 21;
        static constexpr pqxx::oid kInt4 = 23;
        static constexpr pqxx::oid kDate = 1082;
        static constexpr pqxx::oid kTime = 1083;
        static constexpr pqxx::oid kTimestamp = 1114;
        static constexpr pqxx::oid kTimestamptz = 1184;
        static constexpr pqxx::oid kTimetz = 1266;
        static constexpr pqxx::oid kNumeric = 1700;
        static constexpr pqxx::oid kJson = 114;
        static constexpr pqxx::oid kJsonb = 3802;
        static constexpr pqxx::oid kUuid = 2950;
        static constexpr pqxx::oid kFloat4 = 700;
        static constexpr pqxx::oid kFloat8 = 701;

        template<typename Field>
        common::Value fieldToValue(const Field &f) {
            using common::Value;
            if (f.is_null()) return Value{nullptr};
            try {
                switch (f.type()) {
                    case kBool:   return Value{f.template as<bool>()};
                    case kInt2:
                    case kInt4:   return Value{static_cast<std::int64_t>(f.template as<int>())};
                    case kInt8:   return Value{static_cast<std::int64_t>(f.template as<long long>())};
                    case kFloat4:
                    case kFloat8: return Value{f.template as<double>()};
                    case kNumeric:return Value{common::Decimal{f.template as<std::string>()}};
                    case kBytea:  return Value{parseBytea(f.template as<std::string>())};
                    case kDate:   return Value{common::Date{f.template as<std::string>()}};
                    case kTime:
                    case kTimetz: return Value{common::Time{f.template as<std::string>()}};
                    case kUuid:   return Value{common::Uuid{f.template as<std::string>()}};
                    case kJson:
                    case kJsonb:  return Value{common::Json{f.template as<std::string>()}};
                    case kTimestamp:
                    case kTimestamptz: {
                        const std::string s = f.template as<std::string>();
                        common::Timestamp ts{};
                        if (common::tryParseTimestamp(s, ts)) return Value{ts};
                        return Value{s};
                    }
                    default:      return Value{f.template as<std::string>()};
                }
            } catch (...) {
                return Value{f.template as<std::string>()};
            }
        }

        void fillResultSet(const pqxx::result &r, common::ResultSet &out, int maxRows = 0) {
            if (maxRows > 0 && r.size() > static_cast<pqxx::result::size_type>(maxRows)) {
                throw std::runtime_error(
                    "result set exceeded max_result_rows ("
                    + std::to_string(r.size()) + " > " + std::to_string(maxRows)
                    + "); use queryEach() to stream the result instead");
            }
            const auto ncols = r.columns();
            std::vector<std::string> fields;
            fields.reserve(ncols);
            for (pqxx::row::size_type c = 0; c < ncols; ++c) {
                fields.emplace_back(r.column_name(c));
            }
            out.setFields(std::move(fields));

            for (auto const &row : r) {
                common::Row out_row;
                for (auto const &field : row) {
                    out_row.set(field.name(), fieldToValue(field));
                }
                out.addRow(std::move(out_row));
            }
        }

        void appendParams(pqxx::params &p, const common::Params &ps) {
            for (const auto &v : ps) {
                if (std::holds_alternative<std::nullptr_t>(v)) {
                    p.append(std::optional<std::string>{});
                } else if (const auto *x = std::get_if<bool>(&v)) {
                    p.append(std::optional<bool>{*x});
                } else if (const auto *x = std::get_if<std::int64_t>(&v)) {
                    p.append(std::optional<long long>{static_cast<long long>(*x)});
                } else if (const auto *x = std::get_if<std::uint64_t>(&v)) {
                    p.append(std::optional<std::string>{std::to_string(*x)});
                } else if (const auto *x = std::get_if<double>(&v)) {
                    p.append(std::optional<double>{*x});
                } else if (const auto *x = std::get_if<common::Decimal>(&v)) {
                    p.append(std::optional<std::string>{x->value});
                } else if (const auto *x = std::get_if<common::Timestamp>(&v)) {
                    p.append(std::optional<std::string>{common::timestampToUtcStringMs(*x)});
                } else if (const auto *x = std::get_if<common::Date>(&v)) {
                    p.append(std::optional<std::string>{x->value});
                } else if (const auto *x = std::get_if<common::Time>(&v)) {
                    p.append(std::optional<std::string>{x->value});
                } else if (const auto *x = std::get_if<common::Uuid>(&v)) {
                    p.append(std::optional<std::string>{x->value});
                } else if (const auto *x = std::get_if<common::Json>(&v)) {
                    p.append(std::optional<std::string>{x->value});
                } else if (const auto *x = std::get_if<common::Blob>(&v)) {
                    p.append(std::optional<std::string>{toByteaHex(*x)});
                } else if (const auto *x = std::get_if<std::string>(&v)) {
                    p.append(std::optional<std::string>{*x});
                } else {
                    p.append(std::optional<std::string>{});
                }
            }
        }

        pqxx::result execParams(pqxx::transaction_base &tx, const std::string &sql,
                                pqxx::params &parms) {
#if defined(DBMW_PQXX_HAS_EXEC_WITH_PARAMS)
            return tx.exec(pqxx::zview{sql}, std::move(parms));
#else
            return tx.exec_params(pqxx::zview{sql}, std::move(parms));
#endif
        }

        pqxx::result execPrepared(pqxx::transaction_base &tx, const std::string &name,
                                  pqxx::params &parms) {
#if defined(DBMW_PQXX_HAS_EXEC_WITH_PARAMS)
            return tx.exec(pqxx::prepped{name}, std::move(parms));
#else
            return tx.exec_prepared(pqxx::zview{name}, std::move(parms));
#endif
        }

        common::Status streamRows(pqxx::transaction_base &tx, const std::string &sql,
                                  const common::Params &params,
                                  const common::RowCallback &callback,
                                  std::uint64_t &rows) {
            constexpr const char *kCursor = "dbmw_stream_cursor";
            pqxx::params bound;
            appendParams(bound, params);
            const std::string declare = std::string("DECLARE ") + kCursor
                + " NO SCROLL CURSOR FOR " + sql;
            if (params.empty()) tx.exec(declare);
            else execParams(tx, declare, bound);
            rows = 0;
            try {
                bool keepGoing = true;
                while (keepGoing) {
                    const auto chunk = tx.exec(std::string("FETCH FORWARD 256 FROM ") + kCursor);
                    if (chunk.empty()) break;
                    for (const auto &source: chunk) {
                        common::Row row;
                        for (const auto &field: source)
                            row.set(field.name(), fieldToValue(field));
                        ++rows;
                        if (callback && !callback(row)) {
                            keepGoing = false;
                            break;
                        }
                    }
                }
                tx.exec(std::string("CLOSE ") + kCursor);
            } catch (...) {
                try { tx.exec(std::string("CLOSE ") + kCursor); } catch (...) {}
                throw;
            }
            return common::Status::OK();
        }
#endif
    }

#ifdef DBMW_ENABLE_POSTGRES
    class PgCursor : public core::ICursor {
    public:
        explicit PgCursor(PostgresConnection &owner) : owner_(owner) {}

        ~PgCursor() override { try { close(); } catch (...) {} }

        common::Status open(const std::string &pgSql, const common::Params &params,
                            const core::CursorOptions &opts) {
            if (owner_.tx_) {
                tx_ = owner_.tx_.get();
                ownsTx_ = false;
            } else if (opts.auto_transaction) {
                ownedTx_ = std::make_unique<PgTx>(*owner_.conn_);
                tx_ = ownedTx_.get();
                ownsTx_ = true;
            } else {
                return common::Status::error(
                    common::ErrorCode::CursorError,
                    "PostgreSQL requires an active transaction for server-side cursors; "
                    "open within a transaction or set CursorOptions.auto_transaction=true");
            }
            const std::string name = "dbmw_cursor_" + std::to_string(++gCursorSeq_);
            const std::string scroll = opts.scrollable ? "SCROLL" : "NO SCROLL";
            const std::string declare = "DECLARE " + name + " " + scroll
                + " CURSOR FOR " + pgSql;
            pqxx::params bound;
            appendParams(bound, params);
            try {
                if (params.empty()) tx_->exec(declare);
                else execParams(*tx_, declare, bound);
            } catch (const std::exception &e) {
                rollbackOwned();
                return postgresError(common::ErrorCode::CursorError, "DECLARE CURSOR", e);
            }
            name_ = name;
            batchSize_ = opts.batch_size > 0 ? opts.batch_size : 256;
            open_ = true;
            return common::Status::OK();
        }

        common::Status fetch(std::size_t n, common::ResultSet &out) override {
            if (!open_) return common::Status::OK();
            const std::size_t want = (n == 0) ? batchSize_ : n;
            try {
                ActiveOperation active(owner_.operationMtx_, owner_.operationActive_);
                const auto rows = tx_->exec("FETCH FORWARD "
                    + std::to_string(want) + " FROM " + name_);
                ensureFields(out, rows);
                if (rows.empty()) { open_ = false; eof_ = true; return common::Status::OK(); }
                for (const auto &source : rows) {
                    common::Row row;
                    for (const auto &field : source)
                        row.set(field.name(), fieldToValue(field));
                    out.addRow(std::move(row));
                    ++rowsFetched_;
                }
                return common::Status::OK();
            } catch (const std::exception &e) {
                open_ = false;
                return postgresError(common::ErrorCode::CursorError, "FETCH", e);
            }
        }

        common::Status fetchRow(common::Row &outRow, bool &ok) override {
            ok = false;
            if (!open_) return common::Status::OK();
            common::ResultSet tmp;
            const auto st = fetch(1, tmp);
            if (!st.ok()) return st;
            if (tmp.empty()) return common::Status::OK();
            outRow = std::move(tmp.rows()[0]);
            ok = true;
            return common::Status::OK();
        }

        common::Status close() override {
            if (!open_) return common::Status::OK();
            open_ = false;
            common::Status st = common::Status::OK();
            try {
                ActiveOperation active(owner_.operationMtx_, owner_.operationActive_);
                tx_->exec("CLOSE " + name_);
            } catch (const std::exception &e) {
                st = postgresError(common::ErrorCode::CursorError, "CLOSE CURSOR", e);
            }
            rollbackOwned();
            return st;
        }

        [[nodiscard]] bool isOpen() const override { return open_; }
        [[nodiscard]] bool hasNext() const override { return open_ && !eof_; }
        [[nodiscard]] std::uint64_t rowsFetched() const override { return rowsFetched_; }

    private:
        void rollbackOwned() {
            if (ownsTx_ && ownedTx_) {
                try { ownedTx_->commit(); } catch (...) {}
                ownedTx_.reset();
            }
        }

        void ensureFields(common::ResultSet &out, const pqxx::result &r) {
            if (fieldsSet_) return;
            const auto ncols = r.columns();
            std::vector<std::string> fields;
            fields.reserve(ncols);
            for (pqxx::row::size_type c = 0; c < ncols; ++c)
                fields.emplace_back(r.column_name(c));
            out.setFields(std::move(fields));
            fieldsSet_ = true;
        }

        PostgresConnection &owner_;
        PgTx *tx_ = nullptr;
        std::unique_ptr<PgTx> ownedTx_;
        bool ownsTx_ = false;
        std::string name_;
        std::size_t batchSize_ = 256;
        bool open_ = false;
        bool eof_ = false;
        bool fieldsSet_ = false;
        std::uint64_t rowsFetched_ = 0;
        static std::atomic<std::uint64_t> gCursorSeq_;
    };

    std::atomic<std::uint64_t> PgCursor::gCursorSeq_{0};
#endif

    common::Status PostgresConnection::connect(const config::DataSourceConfig &cfg) {
        cfg_ = cfg;
#ifdef DBMW_ENABLE_POSTGRES
        close();

        std::string cs;
        cs += "host=" + connValue(cfg.host.empty() ? std::string("localhost") : cfg.host);
        cs += " port=" + std::to_string(cfg.port != 0 ? cfg.port : 5432);
        if (!cfg.user.empty())     cs += " user=" + connValue(cfg.user);
        if (!cfg.password.empty()) cs += " password=" + connValue(cfg.password);
        if (!cfg.database.empty()) cs += " dbname=" + connValue(cfg.database);
        if (cfg.connection_timeout_ms > 0)
            cs += " connect_timeout=" + std::to_string(
                std::max(1, (cfg.connection_timeout_ms + 999) / 1000));
        if (cfg.tls_enabled) {
            cs += std::string(" sslmode=") + (cfg.tls_verify_peer ? "verify-full" : "require");
            if (!cfg.tls_ca.empty()) cs += " sslrootcert=" + connValue(cfg.tls_ca);
            if (!cfg.tls_cert.empty()) cs += " sslcert=" + connValue(cfg.tls_cert);
            if (!cfg.tls_key.empty()) cs += " sslkey=" + connValue(cfg.tls_key);
        }

        try {
            conn_ = std::make_unique<pqxx::connection>(cs);
        } catch (std::exception const &e) {
            auto status = postgresError(common::ErrorCode::ConnectionFailed, "connect", e);
            status.message = cfg.redact(std::move(status.message));
            return status;
        }
        if (!conn_->is_open()) {
            conn_.reset();
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "PostgreSQL: connection closed immediately after connect");
        }

        if (cfg.query_timeout_ms > 0) {
            try {
                pqxx::nontransaction setup{*conn_};
                setup.exec("SET statement_timeout = " + std::to_string(cfg.query_timeout_ms));
            } catch (std::exception const &e) {
                const auto status = postgresError(common::ErrorCode::ConnectionFailed,
                                                  "set statement_timeout", e);
                conn_.reset();
                return status;
            }
        }

        auto it = cfg.extra.find("charset");
        if (it != cfg.extra.end()) {
            try {
                conn_->set_client_encoding(it->second);
            } catch (std::exception const &e) {
                return postgresError(common::ErrorCode::ConnectionFailed,
                                     "set_client_encoding", e);
            }
        }

        open_ = true;
        return common::Status::OK();
#else
        open_ = false;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver not built. Rebuild with -DDBMW_ENABLE_POSTGRES=ON");
#endif
    }

    common::Status PostgresConnection::ping() {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("ping");
        try {
            PgTx tx{*conn_};
            tx.exec("SELECT 1");
            tx.commit();
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::PingFailed, "ping", e);
        }
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::query(const std::string &sql, common::ResultSet &out) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("query");
        ActiveOperation active(operationMtx_, operationActive_);
        try {
            if (tx_) {
                fillResultSet(tx_->exec(sql), out, cfg_.max_result_rows);
            } else {
                PgTx tx{*conn_};
                fillResultSet(tx.exec(sql), out, cfg_.max_result_rows);
                tx.commit();
            }
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "query", e);
        }
#else
        (void) sql;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::query(const std::string &sql, const common::Params &params,
                                             common::ResultSet &out) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("query");
        ActiveOperation active(operationMtx_, operationActive_);

        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);

        try {
            pqxx::params pp;
            appendParams(pp, params);
            if (tx_) {
                fillResultSet(execParams(*tx_, pgSql, pp), out, cfg_.max_result_rows);
            } else {
                PgTx tx{*conn_};
                fillResultSet(execParams(tx, pgSql, pp), out, cfg_.max_result_rows);
                tx.commit();
            }
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "query", e);
        }
#else
        (void) sql;
        (void) params;
        (void) out;
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::execute(const std::string &sql, std::int64_t &affected) {
#ifdef DBMW_ENABLE_POSTGRES
        affected = 0;
        if (!open_ || !conn_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        try {
            if (tx_) {
                affected = static_cast<std::int64_t>(tx_->exec(sql).affected_rows());
            } else {
                PgTx tx{*conn_};
                affected = static_cast<std::int64_t>(tx.exec(sql).affected_rows());
                tx.commit();
            }
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "execute", e);
        }
#else
        (void) sql;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::execute(const std::string &sql, const common::Params &params,
                                               std::int64_t &affected) {
#ifdef DBMW_ENABLE_POSTGRES
        affected = 0;
        if (!open_ || !conn_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);

        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);

        try {
            pqxx::params pp;
            appendParams(pp, params);
            if (tx_) {
                affected = static_cast<std::int64_t>(execParams(*tx_, pgSql, pp).affected_rows());
            } else {
                PgTx tx{*conn_};
                affected = static_cast<std::int64_t>(execParams(tx, pgSql, pp).affected_rows());
                tx.commit();
            }
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "execute", e);
        }
#else
        (void) sql;
        (void) params;
        affected = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::queryEach(const std::string &sql,
                                                 const common::Params &params,
                                                 const common::RowCallback &callback,
                                                 std::uint64_t &rows) {
#ifdef DBMW_ENABLE_POSTGRES
        rows = 0;
        if (!open_ || !conn_) return notConnected("stream");
        ActiveOperation active(operationMtx_, operationActive_);
        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);
        try {
            if (tx_) return streamRows(*tx_, pgSql, params, callback, rows);
            PgTx tx{*conn_};
            const auto status = streamRows(tx, pgSql, params, callback, rows);
            if (status.ok()) tx.commit();
            return status;
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "stream", e);
        }
#else
        (void) sql; (void) params; (void) callback; rows = 0;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::executeBatch(const std::string &sql,
                                                    const common::ParamBatch &batch,
                                                    common::BatchResult &out) {
#ifdef DBMW_ENABLE_POSTGRES
        out.clear();
        if (!open_ || !conn_) return notConnected("batch");
        ActiveOperation active(operationMtx_, operationActive_);
        try {
            auto run = [&](pqxx::transaction_base &transaction) {
                out.affected.reserve(batch.size());
                for (const auto &params: batch) {
                    std::size_t found = 0;
                    const std::string pgSql = replacePlaceholders(
                        sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
                    if (found != params.size()) return paramMismatch(params.size(), found);
                    pqxx::params bound;
                    appendParams(bound, params);
                    out.affected.push_back(static_cast<std::int64_t>(
                        execParams(transaction, pgSql, bound).affected_rows()));
                }
                return common::Status::OK();
            };
            if (tx_) return run(*tx_);

            PgTx transaction{*conn_};
            const auto status = run(transaction);
            if (status.ok()) {
                transaction.commit();
                return status;
            }
            out.clear();
            return status;
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::QueryError, "batch", e);
        }
#else
        (void) sql; (void) batch; out.clear();
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::execute(const std::string &sql, std::int64_t &affected,
                                              common::GeneratedKeys &out) {
#ifdef DBMW_ENABLE_POSTGRES
        out.clear();
        if (!open_ || !conn_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        try {
            pqxx::result r;
            if (tx_) r = tx_->exec(sql);
            else { PgTx w{*conn_}; r = w.exec(sql); w.commit(); }
            affected = static_cast<std::int64_t>(r.affected_rows());
            fillResultSet(r, out.rows, 0);
            return common::Status::OK();
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::QueryError, "execute(keys)", e);
        }
#else
        (void) sql; (void) affected; out.clear();
        return common::Status::error(common::ErrorCode::NotSupported, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::execute(const std::string &sql, const common::Params &params,
                                              std::int64_t &affected, common::GeneratedKeys &out) {
#ifdef DBMW_ENABLE_POSTGRES
        out.clear();
        if (!open_ || !conn_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);
        try {
            pqxx::params pp;
            appendParams(pp, params);
            pqxx::result r;
            if (tx_) r = execParams(*tx_, pgSql, pp);
            else { PgTx w{*conn_}; r = execParams(w, pgSql, pp); w.commit(); }
            affected = static_cast<std::int64_t>(r.affected_rows());
            fillResultSet(r, out.rows, 0);
            return common::Status::OK();
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::QueryError, "execute(keys)", e);
        }
#else
        (void) sql; (void) params; (void) affected; out.clear();
        return common::Status::error(common::ErrorCode::NotSupported, "PostgreSQL driver disabled");
#endif
    }

    bool PostgresConnection::supportsPrepared() const {
#ifdef DBMW_ENABLE_POSTGRES
        return true;
#else
        return false;
#endif
    }

    common::Status PostgresConnection::prepare(const std::string &sql,
                                              const common::Params &typesSample,
                                              core::PreparedStatementHandle &out) {
#ifdef DBMW_ENABLE_POSTGRES
        out = core::PreparedStatementHandle{};
        if (!open_ || !conn_) return notConnected("prepare");
        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != typesSample.size()) return paramMismatch(typesSample.size(), found);
        const std::string key = sql + common::paramTypeSignature(typesSample);
        if (const auto it = preparedCache_.find(key); it != preparedCache_.end()) {
            preparedLru_.remove(key);
            preparedLru_.push_back(key);
            out = it->second;
            return common::Status::OK();
        }
        const std::string name = "dbmw_ps_" + std::to_string(++preparedSeq_);
        try {
            conn_->prepare(name, pgSql);
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::QueryError, "prepare", e);
        }
        core::PreparedStatementHandle h =
            core::PreparedStatementHandle::make(preparedSeq_, nullptr);
        preparedCache_[key] = h;
        preparedNames_[preparedSeq_] = name;
        preparedLru_.push_back(key);
        if (preparedLimit_ > 0) {
            while (preparedCache_.size() > static_cast<std::size_t>(preparedLimit_)) {
                const std::string oldKey = preparedLru_.front();
                preparedLru_.pop_front();
                if (const auto oit = preparedCache_.find(oldKey); oit != preparedCache_.end()) {
                    if (const auto nit = preparedNames_.find(oit->second.id());
                        nit != preparedNames_.end()) {
                        try { conn_->unprepare(nit->second); } catch (...) {}
                        preparedNames_.erase(nit);
                    }
                    preparedCache_.erase(oit);
                }
            }
        }
        out = h;
        return common::Status::OK();
#else
        (void) sql; (void) typesSample; out = core::PreparedStatementHandle{};
        return common::Status::error(common::ErrorCode::NotSupported, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                      const common::Params &params,
                                                      common::ResultSet &out) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("executePrepared");
        auto it = preparedNames_.find(h.id());
        if (it == preparedNames_.end())
            return common::Status::error(common::ErrorCode::QueryError,
                                         "PostgreSQL: invalid prepared handle");
        const std::string &name = it->second;
        pqxx::params pp;
        appendParams(pp, params);
        try {
            if (tx_) {
                fillResultSet(execPrepared(*tx_, name, pp), out, cfg_.max_result_rows);
            } else {
                PgTx w{*conn_};
                fillResultSet(execPrepared(w, name, pp), out, cfg_.max_result_rows);
                w.commit();
            }
            return common::Status::OK();
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::QueryError, "exec_prepared", e);
        }
#else
        (void) h; (void) params; (void) out;
        return common::Status::error(common::ErrorCode::NotSupported, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                      const common::Params &params,
                                                      std::int64_t &affected) {
#ifdef DBMW_ENABLE_POSTGRES
        affected = 0;
        if (!open_ || !conn_) return notConnected("executePrepared");
        auto it = preparedNames_.find(h.id());
        if (it == preparedNames_.end())
            return common::Status::error(common::ErrorCode::QueryError,
                                         "PostgreSQL: invalid prepared handle");
        const std::string &name = it->second;
        pqxx::params pp;
        appendParams(pp, params);
        try {
            if (tx_) {
                affected = static_cast<std::int64_t>(
                    execPrepared(*tx_, name, pp).affected_rows());
            } else {
                PgTx w{*conn_};
                affected = static_cast<std::int64_t>(
                    execPrepared(w, name, pp).affected_rows());
                w.commit();
            }
            return common::Status::OK();
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::QueryError, "exec_prepared", e);
        }
#else
        (void) h; (void) params; affected = 0;
        return common::Status::error(common::ErrorCode::NotSupported, "PostgreSQL driver disabled");
#endif
    }

    void PostgresConnection::closeAllPrepared() {
#ifdef DBMW_ENABLE_POSTGRES
        for (auto &kv : preparedNames_) {
            try { conn_->unprepare(kv.second); } catch (...) {}
        }
        preparedCache_.clear();
        preparedNames_.clear();
        preparedLru_.clear();
#endif
    }

    void PostgresConnection::setPreparedCacheLimit(int maxPerConnection) {
#ifdef DBMW_ENABLE_POSTGRES
        preparedLimit_ = maxPerConnection;
#else
        (void) maxPerConnection;
#endif
    }

    common::Status PostgresConnection::openCursor(const std::string &sql, const common::Params &params,
                                                  const core::CursorOptions &opts,
                                                  std::unique_ptr<core::ICursor> &out) {
#ifdef DBMW_ENABLE_POSTGRES
        out.reset();
        if (!open_ || !conn_) return notConnected("openCursor");
        ActiveOperation active(operationMtx_, operationActive_);
        std::size_t found = 0;
        const std::string pgSql = replacePlaceholders(
            sql, [](std::size_t i) { return "$" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);
        try {
            auto cur = std::make_unique<PgCursor>(*this);
            const auto st = cur->open(pgSql, params, opts);
            if (!st.ok()) return st;
            out = std::move(cur);
            return common::Status::OK();
        } catch (const std::exception &e) {
            return postgresError(common::ErrorCode::CursorError, "openCursor", e);
        }
#else
        (void) sql; (void) params; (void) opts; out.reset();
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    std::string PostgresConnection::escapeLiteral(const common::Value &v) const {
        if (const auto *b = std::get_if<common::Blob>(&v)) {
            std::string s = "'";
            s += toByteaHex(*b);
            s += '\'';
            return s;
        }
        return common::escapeLiteralGeneric(v);
    }

    common::Status PostgresConnection::begin() {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("begin");
        if (tx_)
            return common::Status::error(common::ErrorCode::TxError, "PostgreSQL: transaction already active");
        try {
            tx_ = std::make_unique<PgTx>(*conn_);
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::TxError, "BEGIN", e);
        }
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::begin(const common::TransactionOptions &options) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!open_ || !conn_) return notConnected("begin");
        if (tx_)
            return common::Status::error(common::ErrorCode::TxError,
                                         "PostgreSQL: transaction already active");
        try {
            tx_ = std::make_unique<PgTx>(*conn_);
            std::string settings;
            switch (options.isolation) {
                case common::IsolationLevel::Default: break;
                case common::IsolationLevel::ReadUncommitted:
                    settings = " ISOLATION LEVEL READ UNCOMMITTED"; break;
                case common::IsolationLevel::ReadCommitted:
                    settings = " ISOLATION LEVEL READ COMMITTED"; break;
                case common::IsolationLevel::RepeatableRead:
                    settings = " ISOLATION LEVEL REPEATABLE READ"; break;
                case common::IsolationLevel::Serializable:
                    settings = " ISOLATION LEVEL SERIALIZABLE"; break;
            }
            if (options.readOnly) settings += " READ ONLY";
            if (!settings.empty()) tx_->exec("SET TRANSACTION" + settings);
            return common::Status::OK();
        } catch (std::exception const &e) {
            tx_.reset();
            return postgresError(common::ErrorCode::TxError, "BEGIN", e);
        }
#else
        (void) options;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::commit() {
#ifdef DBMW_ENABLE_POSTGRES
        if (!tx_)
            return common::Status::error(common::ErrorCode::TxError, "PostgreSQL: no active transaction");
        try {
            tx_->commit();
            tx_.reset();
            return common::Status::OK();
        } catch (std::exception const &e) {
            tx_.reset();
            return postgresError(common::ErrorCode::TxError, "COMMIT", e);
        }
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::rollback() {
#ifdef DBMW_ENABLE_POSTGRES
        if (!tx_)
            return common::Status::error(common::ErrorCode::TxError, "PostgreSQL: no active transaction");
        try {
            tx_->abort();
            tx_.reset();
            return common::Status::OK();
        } catch (std::exception const &e) {
            tx_.reset();
            return postgresError(common::ErrorCode::TxError, "ROLLBACK", e);
        }
#else
        return common::Status::error(common::ErrorCode::DriverDisabled, "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::savepoint(const std::string &name) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!tx_) return common::Status::error(common::ErrorCode::TxError,
                                                "PostgreSQL: no active transaction");
        try {
            tx_->exec("SAVEPOINT " + common::quoteIdentifier(name));
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::TxError, "SAVEPOINT", e);
        }
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::releaseSavepoint(const std::string &name) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!tx_) return common::Status::error(common::ErrorCode::TxError,
                                                "PostgreSQL: no active transaction");
        try {
            tx_->exec("RELEASE SAVEPOINT " + common::quoteIdentifier(name));
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::TxError, "RELEASE SAVEPOINT", e);
        }
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::rollbackToSavepoint(const std::string &name) {
#ifdef DBMW_ENABLE_POSTGRES
        if (!tx_) return common::Status::error(common::ErrorCode::TxError,
                                                "PostgreSQL: no active transaction");
        try {
            tx_->exec("ROLLBACK TO SAVEPOINT " + common::quoteIdentifier(name));
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::TxError, "ROLLBACK TO SAVEPOINT", e);
        }
#else
        (void) name;
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    void PostgresConnection::close() {
#ifdef DBMW_ENABLE_POSTGRES
        closeAllPrepared();
        if (tx_) { try { tx_->abort(); } catch (...) {} tx_.reset(); }
        if (conn_) { try { conn_->close(); } catch (...) {} conn_.reset(); }
#endif
        open_ = false;
    }

    common::Status PostgresConnection::cancel() {
#ifdef DBMW_ENABLE_POSTGRES
        std::lock_guard<std::mutex> lock(operationMtx_);
        if (!conn_ || !open_ || !operationActive_)
            return common::Status::error(common::ErrorCode::NotConnected,
                                         "PostgreSQL: no active query to cancel");
        try {
            conn_->cancel_query();
            return common::Status::OK();
        } catch (std::exception const &e) {
            return postgresError(common::ErrorCode::Cancelled, "cancel_query", e);
        }
#else
        return common::Status::error(common::ErrorCode::DriverDisabled,
                                     "PostgreSQL driver disabled");
#endif
    }

    common::Status PostgresConnection::lastError(const char *where) const
    {
        std::string msg = where;
        msg += ": ";
        msg += lastErr_.empty() ? "(unknown error)" : lastErr_;
        return common::Status::error(common::ErrorCode::QueryError, std::move(msg));
    }

    void registerPostgresDriver() {
        DriverRegistry::instance().registerDriver("postgres",
                                                  []() { return std::make_unique<PostgresDriver>(); });
    }
}
