#include "dbmw/driver/oracle_driver.h"
#include "dbmw/driver/driver_registry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace dbmw::driver {
    namespace {
        [[maybe_unused]] common::Status notConnected(const char *where) {
            return common::Status::error(common::ErrorCode::NotConnected,
                                         std::string("Oracle: not connected (") + where + ")");
        }

        [[maybe_unused]] common::Status paramMismatch(const std::size_t supplied,
                                                      const std::size_t placeholders) {
            return common::Status::error(
                common::ErrorCode::QueryError,
                "parameter mismatch: supplied " + std::to_string(supplied)
                + " parameter(s) but SQL has " + std::to_string(placeholders)
                + " '?' placeholder(s)");
        }

        [[maybe_unused]] common::Status driverDisabled(const char *where) {
            return common::Status::error(
                common::ErrorCode::DriverDisabled,
                std::string("Oracle driver not built (") + where +
                "). Rebuild with -DDBMW_ENABLE_ORACLE=ON");
        }

        [[maybe_unused]] bool validSavepointName(const std::string &name) {
            if (name.empty() || name.size() > 128) return false;
            for (const char c: name) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_';
                if (!ok) return false;
            }
            return true;
        }

        [[maybe_unused]] std::string quotedLiteral(const std::string &text) {
            std::string s = "'";
            for (const char c: text) s += c == '\'' ? "''" : std::string(1, c);
            s += '\'';
            return s;
        }

#ifdef DBMW_ENABLE_ORACLE
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

        bool ociOk(const sword rc) {
            return rc == OCI_SUCCESS || rc == OCI_SUCCESS_WITH_INFO;
        }

        common::Status oracleError(OCIError *err, const common::ErrorCode fallback,
                                   const char *where) {
            OraText stateBuf[8] = {0};
            OraText msgBuf[1024] = {0};
            sb4 code = 0;
            OCIErrorGet(err, 1, stateBuf, &code, msgBuf,
                        static_cast<ub4>(sizeof(msgBuf)), OCI_HTYPE_ERROR);
            std::string msg(reinterpret_cast<const char *>(msgBuf));
            std::string state(reinterpret_cast<const char *>(stateBuf));
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
            if (msg.empty()) msg = "OCI call failed";
            return common::Status::databaseError(fallback,
                                                 std::string("Oracle ") + where + ": " + msg,
                                                 std::move(state), code);
        }

        struct OraColumnMeta {
            std::string name;
            std::uint16_t sqlt = 0;
            std::uint32_t size = 0;
            std::int32_t precision = 0;
            std::int32_t scale = 0;
        };

        std::string trimTrailing(const std::string &s) {
            std::size_t end = s.size();
            while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
            return s.substr(0, end);
        }
#endif
    }

#ifdef DBMW_ENABLE_ORACLE
    std::string OracleConnection::connectString(const config::DataSourceConfig &cfg) const {
        if (!cfg.dsn.empty()) return cfg.dsn;
        const std::string host = cfg.host.empty() ? std::string("localhost") : cfg.host;
        const int port = cfg.port != 0 ? cfg.port : 1521;
        std::string service = cfg.database;
        if (const auto it = cfg.extra.find("service_name"); it != cfg.extra.end())
            service = it->second;
        if (service.empty()) return host + ":" + std::to_string(port);
        return "//" + host + ":" + std::to_string(port) + "/" + service;
    }
#endif

    common::Status OracleConnection::connect(const config::DataSourceConfig &cfg) {
        cfg_ = cfg;
#ifdef DBMW_ENABLE_ORACLE
        close();
        const auto lob = cfg.extra.find("lob_max_bytes");
        lobMaxBytes_ = 4194304;
        if (lob != cfg.extra.end()) {
            const long long v = std::strtoll(lob->second.c_str(), nullptr, 10);
            if (v > 0) lobMaxBytes_ = v;
        }

        sword rc = OCIEnvCreate(&env_, OCI_THREADED, nullptr, nullptr, nullptr, nullptr, 0,
                                nullptr);
        if (!ociOk(rc))
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "Oracle: OCIEnvCreate failed");
        rc = OCIHandleAlloc(env_, reinterpret_cast<void **>(&err_), OCI_HTYPE_ERROR, 0, nullptr);
        if (!ociOk(rc)) {
            OCIHandleFree(env_, OCI_HTYPE_ENV);
            env_ = nullptr;
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "Oracle: OCIHandleAlloc(OCI_HTYPE_ERROR) failed");
        }

        const std::string db = connectString(cfg);
        const std::string user = cfg.user;
        const std::string password = cfg.password;
        rc = OCILogon2(env_, err_, &svc_,
                       reinterpret_cast<const OraText *>(user.data()),
                       static_cast<ub4>(user.size()),
                       reinterpret_cast<const OraText *>(password.data()),
                       static_cast<ub4>(password.size()),
                       reinterpret_cast<const OraText *>(db.data()),
                       static_cast<ub4>(db.size()), OCI_DEFAULT);
        if (!ociOk(rc)) {
            const auto status = oracleError(err_, common::ErrorCode::ConnectionFailed, "connect");
            OCIHandleFree(err_, OCI_HTYPE_ERROR);
            err_ = nullptr;
            OCIHandleFree(env_, OCI_HTYPE_ENV);
            env_ = nullptr;
            auto mapped = status;
            mapped.message = cfg.redact(status.message);
            return mapped;
        }

#ifdef OCI_ATTR_CALL_TIME
        if (cfg.query_timeout_ms > 0) {
            ub4 callTime = static_cast<ub4>(cfg.query_timeout_ms) * 1000u;
            (void) OCIAttrSet(svc_, OCI_HTYPE_SVCCTX, &callTime, 0, OCI_ATTR_CALL_TIME, err_);
        }
#endif

        for (const auto &stmt: common::oracleSessionSetupStatements()) {
            std::int64_t ignored = 0;
            common::ResultSet ignoredRs;
            std::vector<std::string> ignoredKeys;
            const auto st = runStatement(stmt, common::Params{}, false, ignored, ignoredRs, false,
                                         ignoredKeys, common::RowCallback{}, ignored);
            if (!st.ok()) {
                OCILogoff(svc_, err_);
                svc_ = nullptr;
                OCIHandleFree(err_, OCI_HTYPE_ERROR);
                err_ = nullptr;
                OCIHandleFree(env_, OCI_HTYPE_ENV);
                env_ = nullptr;
                return common::Status::error(st.code,
                                             "Oracle: session setup failed: " + st.message);
            }
        }

        open_ = true;
        return common::Status::OK();
#else
        open_ = false;
        return driverDisabled("connect");
#endif
    }

    common::Status OracleConnection::ping() {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("ping");
        ActiveOperation active(operationMtx_, operationActive_);
        std::int64_t ignored = 0;
        common::ResultSet rs;
        std::vector<std::string> ignoredKeys;
        const auto st = runStatement("SELECT 1 FROM DUAL", common::Params{}, true, ignored, rs,
                                     false, ignoredKeys, common::RowCallback{}, ignored);
        if (!st.ok()) {
            auto mapped = st;
            mapped.code = common::ErrorCode::PingFailed;
            return mapped;
        }
        return common::Status::OK();
#else
        return driverDisabled("ping");
#endif
    }

    common::Status OracleConnection::runStatement(const std::string &sql,
                                                  const common::Params &params,
                                                  const bool isQuery, std::int64_t &affected,
                                                  common::ResultSet &out, const bool collectKeys,
                                                  std::vector<std::string> &keyColumns,
                                                  const common::RowCallback &callback,
                                                  std::int64_t &streamedRows,
                                                  const std::string &cacheKey) {
#ifdef DBMW_ENABLE_ORACLE
        affected = 0;
        streamedRows = 0;
        keyColumns.clear();
        out.clear();

        std::size_t found = 0;
        const std::string oraSql = replacePlaceholders(
            sql, [](const std::size_t i) { return ":" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);

        common::OracleReturning returning;
        if (collectKeys) (void) common::oracleParseReturningInto(oraSql, returning);

        const OraText *keyPtr = cacheKey.empty()
                                    ? nullptr
                                    : reinterpret_cast<const OraText *>(cacheKey.data());
        const ub4 keyLen = static_cast<ub4>(cacheKey.size());
        OCIStmt *stmt = nullptr;
        sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                   reinterpret_cast<const OraText *>(oraSql.data()),
                                   static_cast<ub4>(oraSql.size()), keyPtr, keyLen, OCI_NTV_SYNTAX,
                                   OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "prepare");

        struct StatementGuard {
            OCIStmt *stmt;
            OCIError *err;
            const OraText *key;
            ub4 keyLen;

            ~StatementGuard() { if (stmt) OCIStmtRelease(stmt, err, key, keyLen, OCI_DEFAULT); }
        } guard{stmt, err_, keyPtr, keyLen};

        std::vector<std::vector<char> > textBufs;
        std::vector<common::Blob> rawBufs;
        std::vector<sb2> inds;
        std::vector<ub2> rlens;
        std::vector<ub2> rcs;
        static char kEmpty[1] = {0};

        const std::size_t totalBinds = params.size() +
            (returning.present ? returning.bindCount : 0);
        textBufs.reserve(totalBinds + 1);
        rawBufs.reserve(totalBinds + 1);
        inds.reserve(totalBinds + 1);
        rlens.reserve(totalBinds + 1);
        rcs.reserve(totalBinds + 1);

        for (const auto &v: params) {
            const auto bind = common::oracleBindValue(v);
            textBufs.emplace_back();
            rawBufs.emplace_back();
            if (bind.unsupported)
                return common::Status::error(common::ErrorCode::NotSupported,
                                             "Oracle: cannot bind value of type " +
                                             common::valueToString(v));
            if (bind.raw.has_value()) {
                if (bind.raw->size() > 32767)
                    return common::Status::error(
                        common::ErrorCode::NotSupported,
                        "Oracle: binary parameter of " + std::to_string(bind.raw->size()) +
                        " bytes exceeds the 32767 byte direct bind limit; use a temporary LOB");
                rawBufs.back() = *bind.raw;
                textBufs.back().assign(1, '\0');
            } else if (bind.text.has_value()) {
                textBufs.back().assign(bind.text->begin(), bind.text->end());
                textBufs.back().push_back('\0');
            } else {
                textBufs.back().assign(1, '\0');
            }
            inds.push_back(bind.isNull() ? -1 : 0);
            rlens.push_back(0);
            rcs.push_back(0);
        }

        std::vector<std::vector<char> > outBufs;
        if (returning.present) {
            outBufs.reserve(returning.bindCount);
            for (std::size_t i = 0; i < returning.bindCount; ++i) {
                outBufs.emplace_back(512, '\0');
                textBufs.emplace_back(512, '\0');
                rawBufs.emplace_back();
                inds.push_back(0);
                rlens.push_back(0);
                rcs.push_back(0);
            }
        }

        for (std::size_t i = 0; i < totalBinds; ++i) {
            const bool isRaw = !rawBufs[i].empty();
            const ub2 dty = isRaw ? static_cast<ub2>(common::kSqltBin)
                                  : static_cast<ub2>(common::kSqltStr);
            void *valuep = isRaw ? static_cast<void *>(rawBufs[i].data())
                                 : (inds[i] == -1
                                        ? static_cast<void *>(kEmpty)
                                        : static_cast<void *>(textBufs[i].data()));
            const sb4 valueSz = inds[i] == -1
                                    ? 0
                                    : static_cast<sb4>(isRaw ? rawBufs[i].size()
                                                             : textBufs[i].size());
            OCIBind *bindHandle = nullptr;
            const sword brc = OCIBindByPos(stmt, &bindHandle, err_, static_cast<ub4>(i + 1),
                                           valuep, valueSz, dty, &inds[i], &rlens[i], &rcs[i], 0,
                                           nullptr, OCI_DEFAULT);
            if (!ociOk(brc)) return oracleError(err_, common::ErrorCode::QueryError, "bind");
        }

        const ub4 iters = isQuery ? 0u : 1u;
        rc = OCIStmtExecute(svc_, stmt, err_, iters, 0, nullptr, nullptr, OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "execute");

        if (returning.present) {
            common::ResultSet keys;
            keys.setFields(returning.columns);
            common::Row row;
            const std::size_t base = params.size();
            for (std::size_t i = 0; i < returning.bindCount; ++i) {
                const std::size_t idx = base + i;
                if (inds[idx] == -1) {
                    row.set(returning.columns[i], common::Value{nullptr});
                    continue;
                }
                const std::string text(textBufs[idx].data(),
                                       static_cast<std::size_t>(rlens[idx]));
                errno = 0;
                char *end = nullptr;
                const long long parsed = std::strtoll(text.c_str(), &end, 10);
                if (!text.empty() && end == text.c_str() + text.size() && errno != ERANGE)
                    row.set(returning.columns[i],
                            common::Value{static_cast<std::int64_t>(parsed)});
                else
                    row.set(returning.columns[i], common::Value{trimTrailing(text)});
            }
            keys.addRow(std::move(row));
            out = std::move(keys);
        }

        if (!isQuery) {
            ub4 rowCount = 0;
            (void) OCIAttrGet(stmt, OCI_HTYPE_STMT, &rowCount, nullptr, OCI_ATTR_ROW_COUNT, err_);
            affected = static_cast<std::int64_t>(rowCount);
            return common::Status::OK();
        }

        ub4 colCount = 0;
        rc = OCIAttrGet(stmt, OCI_HTYPE_STMT, &colCount, nullptr, OCI_ATTR_PARAM_COUNT, err_);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "describe");

        std::vector<OraColumnMeta> columns;
        columns.reserve(colCount);
        for (ub4 c = 0; c < colCount; ++c) {
            OCIParam *param = nullptr;
            rc = OCIParamGet(stmt, OCI_HTYPE_STMT, err_, reinterpret_cast<void **>(&param),
                             c + 1);
            if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "param");
            OraColumnMeta meta;
            ub2 dtype = 0;
            ub2 dsize = 0;
            sb2 precision = 0;
            sb1 scale = 0;
            OraText *namePtr = nullptr;
            ub4 nameLen = 0;
            (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &dtype, nullptr, OCI_ATTR_DATA_TYPE, err_);
            (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &dsize, nullptr, OCI_ATTR_DATA_SIZE, err_);
            (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &precision, nullptr, OCI_ATTR_PRECISION,
                              err_);
            (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &scale, nullptr, OCI_ATTR_SCALE, err_);
            (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &namePtr, &nameLen, OCI_ATTR_NAME, err_);
            meta.sqlt = static_cast<std::uint16_t>(dtype);
            meta.size = static_cast<std::uint32_t>(dsize);
            meta.precision = static_cast<std::int32_t>(precision);
            meta.scale = static_cast<std::int32_t>(scale);
            if (namePtr != nullptr && nameLen > 0)
                meta.name.assign(reinterpret_cast<const char *>(namePtr),
                                 static_cast<std::size_t>(nameLen));
            else
                meta.name = "COL" + std::to_string(c + 1);
            columns.push_back(std::move(meta));
        }

        std::vector<std::string> fields;
        fields.reserve(columns.size());
        for (const auto &col: columns) fields.push_back(col.name);
        out.setFields(std::move(fields));

        std::vector<std::vector<char> > colBufs;
        std::vector<sb2> colInds(columns.size(), 0);
        std::vector<ub2> colRlens(columns.size(), 0);
        std::vector<ub2> colRcs(columns.size(), 0);
        std::vector<OCILobLocator *> colLobs(columns.size(), nullptr);
        std::vector<OCIDefine *> defines(columns.size(), nullptr);
        colBufs.reserve(columns.size());

        for (std::size_t i = 0; i < columns.size(); ++i) {
            const auto &meta = columns[i];
            if (common::oracleIsLob(meta.sqlt)) {
                OCILobLocator *locator = nullptr;
                rc = OCIDescriptorAlloc(env_, reinterpret_cast<void **>(&locator), OCI_DTYPE_LOB,
                                        0, nullptr);
                if (!ociOk(rc))
                    return oracleError(err_, common::ErrorCode::QueryError, "lob descriptor");
                colLobs[i] = locator;
                const ub2 dty = meta.sqlt == common::kSqltClob
                                    ? static_cast<ub2>(common::kSqltClob)
                                    : static_cast<ub2>(common::kSqltBlob);
                rc = OCIDefineByPos(stmt, &defines[i], err_, static_cast<ub4>(i + 1), &colLobs[i],
                                    static_cast<sb4>(sizeof(OCILobLocator *)), dty, &colInds[i],
                                    &colRlens[i], &colRcs[i], OCI_DEFAULT);
                if (!ociOk(rc))
                    return oracleError(err_, common::ErrorCode::QueryError, "define lob");
                continue;
            }
            std::uint32_t width = meta.size * 4u;
            if (width < 64u) width = 64u;
            if (width > 32768u) width = 32768u;
            colBufs.emplace_back(width, '\0');
            rc = OCIDefineByPos(stmt, &defines[i], err_, static_cast<ub4>(i + 1),
                                colBufs.back().data(), static_cast<sb4>(width),
                                static_cast<ub2>(common::kSqltStr), &colInds[i], &colRlens[i],
                                &colRcs[i], OCI_DEFAULT);
            if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "define");
        }

        std::size_t bufIndex = 0;
        while (true) {
            const sword frc = OCIStmtFetch2(stmt, err_, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
            if (frc == OCI_NO_DATA) break;
            if (!ociOk(frc)) return oracleError(err_, common::ErrorCode::QueryError, "fetch");

            common::Row row;
            std::size_t lobIndex = 0;
            for (std::size_t i = 0; i < columns.size(); ++i) {
                const auto &meta = columns[i];
                if (colInds[i] == -1) {
                    row.set(meta.name, common::Value{nullptr});
                    if (common::oracleIsLob(meta.sqlt)) ++lobIndex;
                    else ++bufIndex;
                    continue;
                }
                if (common::oracleIsLob(meta.sqlt)) {
                    const bool isClob = meta.sqlt == common::kSqltClob;
                    oraub8 length = 0;
                    rc = OCILobGetLength2(svc_, err_, colLobs[lobIndex], &length);
                    if (!ociOk(rc))
                        return oracleError(err_, common::ErrorCode::QueryError, "lob length");
                    if (static_cast<std::int64_t>(length) > lobMaxBytes_)
                        return common::Status::error(
                            common::ErrorCode::NotSupported,
                            "Oracle: LOB column '" + meta.name + "' holds " +
                            std::to_string(static_cast<unsigned long long>(length)) +
                            " bytes, above lob_max_bytes=" + std::to_string(lobMaxBytes_));
                    if (isClob) {
                        std::string text(static_cast<std::size_t>(length) + 1, '\0');
                        oraub8 amount = 0;
                        oraub8 offset = 1;
                        rc = OCILobRead2(svc_, err_, colLobs[lobIndex], nullptr, &amount, offset,
                                         text.data(), static_cast<oraub8>(text.size()),
                                         OCI_ONE_PIECE, nullptr, nullptr, 0, SQLCS_IMPLICIT);
                        if (!ociOk(rc))
                            return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                        text.resize(static_cast<std::size_t>(amount));
                        row.set(meta.name, common::Value{text});
                    } else {
                        common::Blob bytes(static_cast<std::size_t>(length));
                        oraub8 amount = 0;
                        oraub8 offset = 1;
                        rc = OCILobRead2(svc_, err_, colLobs[lobIndex], &amount, nullptr, offset,
                                         bytes.data(), static_cast<oraub8>(bytes.size()),
                                         OCI_ONE_PIECE, nullptr, nullptr, 0, 0);
                        if (!ociOk(rc))
                            return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                        bytes.resize(static_cast<std::size_t>(amount));
                        row.set(meta.name, common::Value{bytes});
                    }
                    ++lobIndex;
                    continue;
                }
                const std::string text(colBufs[bufIndex].data(),
                                       static_cast<std::size_t>(colRlens[i]));
                row.set(meta.name, common::oracleValueFromText(meta.sqlt, text, meta.precision,
                                                              meta.scale));
                ++bufIndex;
            }
            bufIndex = 0;
            lobIndex = 0;

            if (callback) {
                ++streamedRows;
                if (!callback(row)) break;
                continue;
            }
            if (cfg_.max_result_rows > 0 &&
                out.rowCount() >= static_cast<std::size_t>(cfg_.max_result_rows)) {
                return common::Status::error(
                    common::ErrorCode::QueryError,
                    "Oracle: result set exceeds max_result_rows=" +
                    std::to_string(cfg_.max_result_rows) + "; use queryEach() to stream rows");
            }
            out.addRow(std::move(row));
        }

        for (std::size_t i = 0; i < columns.size(); ++i)
            if (colLobs[i] != nullptr) OCIDescriptorFree(colLobs[i], OCI_DTYPE_LOB);

        if (!isQuery && !returning.present) {
            ub4 rowCount = 0;
            (void) OCIAttrGet(stmt, OCI_HTYPE_STMT, &rowCount, nullptr, OCI_ATTR_ROW_COUNT, err_);
            affected = static_cast<std::int64_t>(rowCount);
        }
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        (void) isQuery;
        (void) collectKeys;
        (void) callback;
        (void) cacheKey;
        affected = 0;
        streamedRows = 0;
        keyColumns.clear();
        out.clear();
        return driverDisabled("statement");
#endif
    }

    common::Status OracleConnection::query(const std::string &sql, common::ResultSet &out) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("query");
        ActiveOperation active(operationMtx_, operationActive_);
        std::int64_t ignored = 0;
        std::vector<std::string> ignoredKeys;
        return runStatement(sql, common::Params{}, true, ignored, out, false, ignoredKeys,
                            common::RowCallback{}, ignored);
#else
        (void) sql;
        out.clear();
        return driverDisabled("query");
#endif
    }

    common::Status OracleConnection::query(const std::string &sql, const common::Params &params,
                                           common::ResultSet &out) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("query");
        ActiveOperation active(operationMtx_, operationActive_);
        std::int64_t ignored = 0;
        std::vector<std::string> ignoredKeys;
        return runStatement(sql, params, true, ignored, out, false, ignoredKeys,
                            common::RowCallback{}, ignored);
#else
        (void) sql;
        (void) params;
        out.clear();
        return driverDisabled("query");
#endif
    }

    common::Status OracleConnection::execute(const std::string &sql, std::int64_t &affected) {
#ifdef DBMW_ENABLE_ORACLE
        affected = 0;
        if (!open_ || !svc_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        common::ResultSet ignored;
        std::vector<std::string> ignoredKeys;
        return runStatement(sql, common::Params{}, false, affected, ignored, false, ignoredKeys,
                            common::RowCallback{}, affected);
#else
        (void) sql;
        affected = 0;
        return driverDisabled("execute");
#endif
    }

    common::Status OracleConnection::execute(const std::string &sql, const common::Params &params,
                                             std::int64_t &affected) {
#ifdef DBMW_ENABLE_ORACLE
        affected = 0;
        if (!open_ || !svc_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        common::ResultSet ignored;
        std::vector<std::string> ignoredKeys;
        return runStatement(sql, params, false, affected, ignored, false, ignoredKeys,
                            common::RowCallback{}, affected);
#else
        (void) sql;
        (void) params;
        affected = 0;
        return driverDisabled("execute");
#endif
    }

    common::Status OracleConnection::execute(const std::string &sql, std::int64_t &affected,
                                             common::GeneratedKeys &out) {
        out.clear();
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        std::vector<std::string> keyColumns;
        const auto st = runStatement(sql, common::Params{}, false, affected, out.rows, true,
                                     keyColumns, common::RowCallback{}, affected);
        if (!st.ok()) return st;
        return common::Status::OK();
#else
        (void) sql;
        affected = 0;
        return driverDisabled("execute");
#endif
    }

    common::Status OracleConnection::execute(const std::string &sql, const common::Params &params,
                                             std::int64_t &affected, common::GeneratedKeys &out) {
        out.clear();
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("execute");
        ActiveOperation active(operationMtx_, operationActive_);
        std::vector<std::string> keyColumns;
        const auto st = runStatement(sql, params, false, affected, out.rows, true, keyColumns,
                                     common::RowCallback{}, affected);
        if (!st.ok()) return st;
        return common::Status::OK();
#else
        (void) sql;
        (void) params;
        affected = 0;
        return driverDisabled("execute");
#endif
    }

    common::Status OracleConnection::queryEach(const std::string &sql,
                                               const common::Params &params,
                                               const common::RowCallback &callback,
                                               std::uint64_t &rows) {
#ifdef DBMW_ENABLE_ORACLE
        rows = 0;
        if (!open_ || !svc_) return notConnected("stream");
        ActiveOperation active(operationMtx_, operationActive_);
        common::ResultSet ignored;
        std::vector<std::string> ignoredKeys;
        std::int64_t affected = 0;
        std::int64_t streamed = 0;
        const auto st = runStatement(sql, params, true, affected, ignored, false, ignoredKeys,
                                     callback, streamed);
        rows = static_cast<std::uint64_t>(streamed);
        return st;
#else
        (void) sql;
        (void) params;
        (void) callback;
        rows = 0;
        return driverDisabled("stream");
#endif
    }

    bool OracleConnection::supportsPrepared() const {
#ifdef DBMW_ENABLE_ORACLE
        return true;
#else
        return false;
#endif
    }

    common::Status OracleConnection::prepare(const std::string &sql,
                                             const common::Params &typesSample,
                                             core::PreparedStatementHandle &out) {
        out = core::PreparedStatementHandle{};
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("prepare");
        std::size_t found = 0;
        const std::string oraSql = replacePlaceholders(
            sql, [](const std::size_t i) { return ":" + std::to_string(i + 1); }, found);
        if (found != typesSample.size()) return paramMismatch(typesSample.size(), found);

        const std::string key = sql + common::paramTypeSignature(typesSample);
        if (const auto it = preparedCache_.find(key); it != preparedCache_.end()) {
            preparedLru_.remove(key);
            preparedLru_.push_back(key);
            out = it->second;
            return common::Status::OK();
        }

        const std::string cacheKey = "dbmw_ps_" + std::to_string(++preparedSeq_);
        OCIStmt *stmt = nullptr;
        const sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                         reinterpret_cast<const OraText *>(oraSql.data()),
                                         static_cast<ub4>(oraSql.size()),
                                         reinterpret_cast<const OraText *>(cacheKey.data()),
                                         static_cast<ub4>(cacheKey.size()), OCI_NTV_SYNTAX,
                                         OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "prepare");
        (void) OCIStmtRelease(stmt, err_, reinterpret_cast<const OraText *>(cacheKey.data()),
                              static_cast<ub4>(cacheKey.size()), OCI_DEFAULT);

        const core::PreparedStatementHandle h =
                core::PreparedStatementHandle::make(preparedSeq_, nullptr);
        preparedCache_[key] = h;
        preparedSql_[preparedSeq_] = oraSql;
        preparedLru_.push_back(key);
        if (preparedLimit_ > 0) {
            while (preparedCache_.size() > static_cast<std::size_t>(preparedLimit_)) {
                const std::string oldKey = preparedLru_.front();
                preparedLru_.pop_front();
                const auto old = preparedCache_.find(oldKey);
                if (old == preparedCache_.end()) continue;
                dropCachedStatement(old->second.id());
                preparedCache_.erase(old);
            }
        }
        out = h;
        return common::Status::OK();
#else
        (void) sql;
        (void) typesSample;
        return driverDisabled("prepare");
#endif
    }

    common::Status OracleConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                     const common::Params &params,
                                                     common::ResultSet &out) {
#ifdef DBMW_ENABLE_ORACLE
        out.clear();
        if (!open_ || !svc_) return notConnected("executePrepared");
        const auto it = preparedSql_.find(h.id());
        if (it == preparedSql_.end())
            return common::Status::error(common::ErrorCode::QueryError,
                                         "Oracle: prepared statement handle is not valid");
        ActiveOperation active(operationMtx_, operationActive_);
        std::int64_t affected = 0;
        std::int64_t streamed = 0;
        std::vector<std::string> ignoredKeys;
        return runStatement(it->second, params, true, affected, out, false, ignoredKeys,
                            common::RowCallback{}, streamed, "dbmw_ps_" + std::to_string(h.id()));
#else
        (void) h;
        (void) params;
        out.clear();
        return driverDisabled("executePrepared");
#endif
    }

    common::Status OracleConnection::executePrepared(const core::PreparedStatementHandle &h,
                                                     const common::Params &params,
                                                     std::int64_t &affected) {
#ifdef DBMW_ENABLE_ORACLE
        affected = 0;
        if (!open_ || !svc_) return notConnected("executePrepared");
        const auto it = preparedSql_.find(h.id());
        if (it == preparedSql_.end())
            return common::Status::error(common::ErrorCode::QueryError,
                                         "Oracle: prepared statement handle is not valid");
        ActiveOperation active(operationMtx_, operationActive_);
        common::ResultSet ignored;
        std::int64_t streamed = 0;
        std::vector<std::string> ignoredKeys;
        return runStatement(it->second, params, false, affected, ignored, false, ignoredKeys,
                            common::RowCallback{}, streamed, "dbmw_ps_" + std::to_string(h.id()));
#else
        (void) h;
        (void) params;
        affected = 0;
        return driverDisabled("executePrepared");
#endif
    }

    void OracleConnection::dropCachedStatement(const std::uint64_t id) {
#ifdef DBMW_ENABLE_ORACLE
        const auto it = preparedSql_.find(id);
        if (it == preparedSql_.end()) return;
        const std::string cacheKey = "dbmw_ps_" + std::to_string(id);
        const OraText *keyPtr = reinterpret_cast<const OraText *>(cacheKey.data());
        const ub4 keyLen = static_cast<ub4>(cacheKey.size());
        if (svc_ && err_) {
            OCIStmt *stmt = nullptr;
            const sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                             reinterpret_cast<const OraText *>(it->second.data()),
                                             static_cast<ub4>(it->second.size()), keyPtr, keyLen,
                                             OCI_NTV_SYNTAX, OCI_DEFAULT);
            if (ociOk(rc) && stmt != nullptr)
                (void) OCIStmtRelease(stmt, err_, keyPtr, keyLen, OCI_STRLS_CACHE_DELETE);
        }
        preparedSql_.erase(it);
#else
        (void) id;
#endif
    }

    void OracleConnection::closeAllPrepared() {
        std::vector<std::uint64_t> ids;
        ids.reserve(preparedSql_.size());
        for (const auto &entry: preparedSql_) ids.push_back(entry.first);
        for (const std::uint64_t id: ids) dropCachedStatement(id);
        preparedSql_.clear();
        preparedCache_.clear();
        preparedLru_.clear();
    }

    void OracleConnection::setPreparedCacheLimit(const int maxPerConnection) {
        preparedLimit_ = maxPerConnection;
    }

    common::Status OracleConnection::openCursor(const std::string &sql,
                                                const common::Params &params,
                                                const core::CursorOptions &opts,
                                                std::unique_ptr<core::ICursor> &out) {
        out.reset();
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("cursor");
        if (opts.scrollable)
            return common::Status::error(common::ErrorCode::NotSupported,
                                         "Oracle: scrollable cursors are not supported");
        (void) sql;
        (void) params;
        return common::Status::error(common::ErrorCode::NotSupported,
                                     "Oracle: cursors are not implemented yet; use queryEach()");
#else
        (void) sql;
        (void) params;
        (void) opts;
        return driverDisabled("cursor");
#endif
    }

    std::string OracleConnection::escapeLiteral(const common::Value &v) const {
        if (const auto *p = std::get_if<bool>(&v)) return *p ? "1" : "0";
        if (const auto *p = std::get_if<common::Date>(&v))
            return "DATE '" + quotedLiteral(p->value) + "'";
        if (const auto *p = std::get_if<common::Time>(&v))
            return "TIMESTAMP '1970-01-01 " + p->value + "'";
        if (const auto *p = std::get_if<common::Timestamp>(&v))
            return "TIMESTAMP '" + common::timestampToStringMs(*p) + "'";
        if (const auto *p = std::get_if<common::Blob>(&v)) {
            static const char *kHex = "0123456789ABCDEF";
            std::string hex;
            hex.reserve(p->size() * 2);
            for (const std::uint8_t byte: *p) {
                hex.push_back(kHex[(byte >> 4) & 0x0F]);
                hex.push_back(kHex[byte & 0x0F]);
            }
            return "HEXTORAW('" + hex + "')";
        }
        return common::escapeLiteralGeneric(v);
    }

    common::Status OracleConnection::begin() {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("begin");
        txOpen_ = true;
        return common::Status::OK();
#else
        return driverDisabled("begin");
#endif
    }

    common::Status OracleConnection::begin(const common::TransactionOptions &options) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("begin");
        if (options.isolation == common::IsolationLevel::ReadUncommitted ||
            options.isolation == common::IsolationLevel::RepeatableRead)
            return common::Status::error(common::ErrorCode::NotSupported,
                                         "Oracle: isolation level not supported (only read "
                                         "committed and serializable exist)");
        std::string sql;
        if (options.readOnly)
            sql = "SET TRANSACTION READ ONLY";
        else if (options.isolation == common::IsolationLevel::Serializable)
            sql = "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
        else if (options.isolation == common::IsolationLevel::ReadCommitted)
            sql = "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
        if (!sql.empty()) {
            std::int64_t ignored = 0;
            common::ResultSet ignoredRs;
            std::vector<std::string> ignoredKeys;
            const auto st = runStatement(sql, common::Params{}, false, ignored, ignoredRs, false,
                                         ignoredKeys, common::RowCallback{}, ignored);
            if (!st.ok()) {
                auto mapped = st;
                mapped.code = common::ErrorCode::TxError;
                return mapped;
            }
        }
        txOpen_ = true;
        return common::Status::OK();
#else
        (void) options;
        return driverDisabled("begin");
#endif
    }

    common::Status OracleConnection::commit() {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("commit");
        ActiveOperation active(operationMtx_, operationActive_);
        const sword rc = OCITransCommit(svc_, err_, OCI_DEFAULT);
        txOpen_ = false;
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::TxError, "commit");
        return common::Status::OK();
#else
        return driverDisabled("commit");
#endif
    }

    common::Status OracleConnection::rollback() {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("rollback");
        ActiveOperation active(operationMtx_, operationActive_);
        const sword rc = OCITransRollback(svc_, err_, OCI_DEFAULT);
        txOpen_ = false;
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::TxError, "rollback");
        return common::Status::OK();
#else
        return driverDisabled("rollback");
#endif
    }

    common::Status OracleConnection::savepoint(const std::string &name) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("savepoint");
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "Oracle: invalid savepoint or no active transaction");
        std::int64_t ignored = 0;
        common::ResultSet ignoredRs;
        std::vector<std::string> ignoredKeys;
        const auto st = runStatement("SAVEPOINT " + name, common::Params{}, false, ignored,
                                     ignoredRs, false, ignoredKeys, common::RowCallback{}, ignored);
        if (!st.ok()) {
            auto mapped = st;
            mapped.code = common::ErrorCode::TxError;
            return mapped;
        }
        return common::Status::OK();
#else
        (void) name;
        return driverDisabled("savepoint");
#endif
    }

    common::Status OracleConnection::releaseSavepoint(const std::string &name) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("releaseSavepoint");
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "Oracle: invalid savepoint or no active transaction");
        return common::Status::OK();
#else
        (void) name;
        return driverDisabled("releaseSavepoint");
#endif
    }

    common::Status OracleConnection::rollbackToSavepoint(const std::string &name) {
#ifdef DBMW_ENABLE_ORACLE
        if (!open_ || !svc_) return notConnected("rollbackToSavepoint");
        if (!txOpen_ || !validSavepointName(name))
            return common::Status::error(common::ErrorCode::TxError,
                                         "Oracle: invalid savepoint or no active transaction");
        std::int64_t ignored = 0;
        common::ResultSet ignoredRs;
        std::vector<std::string> ignoredKeys;
        const auto st = runStatement("ROLLBACK TO SAVEPOINT " + name, common::Params{}, false,
                                     ignored, ignoredRs, false, ignoredKeys,
                                     common::RowCallback{}, ignored);
        if (!st.ok()) {
            auto mapped = st;
            mapped.code = common::ErrorCode::TxError;
            return mapped;
        }
        return common::Status::OK();
#else
        (void) name;
        return driverDisabled("rollbackToSavepoint");
#endif
    }

    common::Status OracleConnection::cancel() {
#ifdef DBMW_ENABLE_ORACLE
        try {
            if (!open_ || !svc_) return notConnected("cancel");
            const sword rc = OCIBreak(svc_, err_);
            if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::Cancelled, "cancel");
            return common::Status::OK();
        } catch (...) {
            return common::Status::error(common::ErrorCode::Cancelled,
                                         "Oracle: cancel threw an exception");
        }
#else
        return driverDisabled("cancel");
#endif
    }

    void OracleConnection::close() {
#ifdef DBMW_ENABLE_ORACLE
        closeAllPrepared();
        if (svc_ && err_) {
            if (txOpen_) (void) OCITransRollback(svc_, err_, OCI_DEFAULT);
            (void) OCILogoff(svc_, err_);
        }
        txOpen_ = false;
        operationActive_ = false;
        svc_ = nullptr;
        if (err_) OCIHandleFree(err_, OCI_HTYPE_ERROR);
        err_ = nullptr;
        if (env_) OCIHandleFree(env_, OCI_HTYPE_ENV);
        env_ = nullptr;
#endif
        open_ = false;
    }

    void registerOracleDriver() {
        DriverRegistry::instance().registerDriver("oracle", [] {
            return std::unique_ptr<IDriver>(new OracleDriver());
        });
    }
}
