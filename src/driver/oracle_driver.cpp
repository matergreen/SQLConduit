#include "dbmw/driver/oracle_driver.h"
#include "dbmw/driver/driver_registry.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
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
        common::Status oracleError(OCIError *err, common::ErrorCode fallback,
                                   const char *where);

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

        const ub2 kCharsetAl32Utf8 = 873;

        ub2 resolveClientCharset(const config::DataSourceConfig &cfg) {
            int charset = cfg.oracle.charset_id;
            if (const auto it = cfg.extra.find("charset_id");
                charset == 873 && it != cfg.extra.end()) {
                const long legacy = std::strtol(it->second.c_str(), nullptr, 10);
                if (legacy > 0 && legacy < 65536) charset = static_cast<int>(legacy);
            }
            if (charset <= 0 || charset >= 65536) return kCharsetAl32Utf8;
            return static_cast<ub2>(charset);
        }

        bool ociOk(const sword rc) {
            return rc == OCI_SUCCESS || rc == OCI_SUCCESS_WITH_INFO;
        }

        enum class LobBindMode { Auto, Raw, Lob };

        LobBindMode resolveLobBindMode(const config::DataSourceConfig &cfg) {
            if (cfg.oracle.blob_bind == "lob") return LobBindMode::Lob;
            if (cfg.oracle.blob_bind == "raw") return LobBindMode::Raw;
            if (const auto it = cfg.extra.find("blob_bind"); it != cfg.extra.end()) {
                if (it->second == "lob") return LobBindMode::Lob;
                if (it->second == "raw") return LobBindMode::Raw;
            }
            return LobBindMode::Auto;
        }

        const std::size_t kDirectBindLimit = 4000;

        class LobBindGuard {
        public:
            LobBindGuard(OCIEnv *env, OCISvcCtx *svc, OCIError *err)
                : env_(env), svc_(svc), err_(err) {}

            ~LobBindGuard() { release(); }

            LobBindGuard(const LobBindGuard &) = delete;

            LobBindGuard &operator=(const LobBindGuard &) = delete;

            OCILobLocator *createBlob(const std::vector<unsigned char> &data) {
                void *p = nullptr;
                if (!ociOk(OCIDescriptorAlloc(env_, &p, OCI_DTYPE_LOB, 0, nullptr))) return nullptr;
                auto *loc = static_cast<OCILobLocator *>(p);
                if (!ociOk(OCILobCreateTemporary(svc_, err_, loc, 0, SQLCS_IMPLICIT, OCI_TEMP_BLOB,
                                                 0, OCI_DURATION_SESSION))) {
                    OCIDescriptorFree(loc, OCI_DTYPE_LOB);
                    return nullptr;
                }
                locs_.push_back(loc);
                oraub8 byteAmt = static_cast<oraub8>(data.size());
                oraub8 charAmt = 0;
                const sword wrc = OCILobWrite2(svc_, err_, loc, &byteAmt, &charAmt, 1,
                                               const_cast<unsigned char *>(data.data()),
                                               static_cast<oraub8>(data.size()), OCI_ONE_PIECE,
                                               nullptr, nullptr, 0, SQLCS_IMPLICIT);
                if (!ociOk(wrc)) return nullptr;
                return loc;
            }

            void release() {
                for (auto *l: locs_) {
                    OCILobFreeTemporary(svc_, err_, l);
                    OCIDescriptorFree(l, OCI_DTYPE_LOB);
                }
                locs_.clear();
            }

        private:
            OCIEnv *env_;
            OCISvcCtx *svc_;
            OCIError *err_;
            std::vector<OCILobLocator *> locs_;
        };

        struct OracleInputStorage {
            OracleInputStorage(OCIEnv *env, OCISvcCtx *svc, OCIError *err,
                               const std::size_t count)
                : lobGuard(env, svc, err), text(count), raw(count), lobs(count, nullptr),
                  indicators(count, 0), lengths(count, 0), returnCodes(count, 0) {}

            LobBindGuard lobGuard;
            std::vector<std::vector<char> > text;
            std::vector<common::Blob> raw;
            std::vector<OCILobLocator *> lobs;
            std::vector<sb2> indicators;
            std::vector<ub2> lengths;
            std::vector<ub2> returnCodes;
        };

        common::Status bindOracleInputs(OCIEnv *env, OCISvcCtx *svc, OCIError *err,
                                        OCIStmt *stmt, const config::DataSourceConfig &cfg,
                                        const common::Params &params,
                                        OracleInputStorage &storage) {
            static char kEmpty[1] = {0};
            const LobBindMode lobMode = resolveLobBindMode(cfg);
            for (std::size_t i = 0; i < params.size(); ++i) {
                const auto value = common::oracleBindValue(params[i]);
                if (value.unsupported)
                    return common::Status::error(common::ErrorCode::NotSupported,
                                                 "Oracle: unsupported parameter type");
                if (value.raw) {
                    if (lobMode == LobBindMode::Lob ||
                        (lobMode == LobBindMode::Auto && value.raw->size() > kDirectBindLimit)) {
                        storage.lobs[i] = storage.lobGuard.createBlob(*value.raw);
                        if (!storage.lobs[i])
                            return oracleError(err, common::ErrorCode::QueryError, "lob bind");
                    } else {
                        if (value.raw->size() > 32767)
                            return common::Status::error(common::ErrorCode::NotSupported,
                                                         "Oracle: raw bind exceeds 32767 bytes");
                        storage.raw[i] = *value.raw;
                    }
                } else if (value.text) {
                    storage.text[i].assign(value.text->begin(), value.text->end());
                    storage.text[i].push_back('\0');
                } else storage.indicators[i] = -1;

                OCIBind *bind = nullptr;
                sword rc = OCI_SUCCESS;
                if (storage.lobs[i]) {
                    rc = OCIBindByPos(stmt, &bind, err, static_cast<ub4>(i + 1), &storage.lobs[i],
                                      sizeof(OCILobLocator *),
                                      static_cast<ub2>(common::kSqltBlob), &storage.indicators[i],
                                      &storage.lengths[i], &storage.returnCodes[i], 0, nullptr,
                                      OCI_DEFAULT);
                } else {
                    const bool raw = !storage.raw[i].empty();
                    void *data = raw ? static_cast<void *>(storage.raw[i].data())
                                     : storage.indicators[i] == -1
                                           ? static_cast<void *>(kEmpty)
                                           : static_cast<void *>(storage.text[i].data());
                    const sb4 size = storage.indicators[i] == -1 ? 0 : static_cast<sb4>(
                        raw ? storage.raw[i].size() : storage.text[i].size());
                    rc = OCIBindByPos(stmt, &bind, err, static_cast<ub4>(i + 1), data, size,
                                      raw ? static_cast<ub2>(common::kSqltBin)
                                          : static_cast<ub2>(common::kSqltStr),
                                      &storage.indicators[i], &storage.lengths[i],
                                      &storage.returnCodes[i], 0, nullptr, OCI_DEFAULT);
                }
                if (!ociOk(rc)) return oracleError(err, common::ErrorCode::QueryError, "bind");
            }
            return common::Status::OK();
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
            if (state.size() != 5) state = common::oracleSqlState(static_cast<int>(code));
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

        class OracleResultReader {
        public:
            OracleResultReader(OCIEnv *env, OCISvcCtx *svc, OCIError *err,
                               const std::int64_t lobMaxBytes)
                : env_(env), svc_(svc), err_(err), lobMaxBytes_(lobMaxBytes) {}

            ~OracleResultReader() {
                for (auto *locator: lobs_)
                    if (locator != nullptr) OCIDescriptorFree(locator, OCI_DTYPE_LOB);
            }

            common::Status setup(OCIStmt *stmt) {
                stmt_ = stmt;
                ub4 count = 0;
                sword rc = OCIAttrGet(stmt_, OCI_HTYPE_STMT, &count, nullptr,
                                      OCI_ATTR_PARAM_COUNT, err_);
                if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "describe");
                columns_.reserve(count);
                for (ub4 c = 0; c < count; ++c) {
                    OCIParam *param = nullptr;
                    rc = OCIParamGet(stmt_, OCI_HTYPE_STMT, err_,
                                     reinterpret_cast<void **>(&param), c + 1);
                    if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError, "param");
                    OraColumnMeta meta;
                    ub2 dtype = 0, dsize = 0;
                    sb2 precision = 0;
                    sb1 scale = 0;
                    OraText *name = nullptr;
                    ub4 nameLen = 0;
                    (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &dtype, nullptr,
                                      OCI_ATTR_DATA_TYPE, err_);
                    (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &dsize, nullptr,
                                      OCI_ATTR_DATA_SIZE, err_);
                    (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &precision, nullptr,
                                      OCI_ATTR_PRECISION, err_);
                    (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &scale, nullptr,
                                      OCI_ATTR_SCALE, err_);
                    (void) OCIAttrGet(param, OCI_DTYPE_PARAM, &name, &nameLen,
                                      OCI_ATTR_NAME, err_);
                    meta.sqlt = static_cast<std::uint16_t>(dtype);
                    meta.size = static_cast<std::uint32_t>(dsize);
                    meta.precision = static_cast<std::int32_t>(precision);
                    meta.scale = static_cast<std::int32_t>(scale);
                    meta.name = name && nameLen
                                    ? std::string(reinterpret_cast<const char *>(name), nameLen)
                                    : "COL" + std::to_string(c + 1);
                    columns_.push_back(std::move(meta));
                }
                indicators_.assign(columns_.size(), 0);
                lengths_.assign(columns_.size(), 0);
                returnCodes_.assign(columns_.size(), 0);
                lobs_.assign(columns_.size(), nullptr);
                defines_.assign(columns_.size(), nullptr);
                buffers_.resize(columns_.size());
                for (std::size_t i = 0; i < columns_.size(); ++i) {
                    const auto &meta = columns_[i];
                    if (common::oracleIsLob(meta.sqlt)) {
                        rc = OCIDescriptorAlloc(env_, reinterpret_cast<void **>(&lobs_[i]),
                                                OCI_DTYPE_LOB, 0, nullptr);
                        if (!ociOk(rc))
                            return oracleError(err_, common::ErrorCode::QueryError,
                                               "lob descriptor");
                        const ub2 dty = meta.sqlt == common::kSqltClob
                                            ? static_cast<ub2>(common::kSqltClob)
                                            : static_cast<ub2>(common::kSqltBlob);
                        rc = OCIDefineByPos(stmt_, &defines_[i], err_, static_cast<ub4>(i + 1),
                                            &lobs_[i], sizeof(OCILobLocator *), dty,
                                            &indicators_[i], &lengths_[i], &returnCodes_[i],
                                            OCI_DEFAULT);
                    } else {
                        std::uint32_t width = std::max<std::uint32_t>(64u, meta.size * 4u);
                        width = std::min<std::uint32_t>(32768u, width);
                        buffers_[i].assign(width, '\0');
                        rc = OCIDefineByPos(stmt_, &defines_[i], err_, static_cast<ub4>(i + 1),
                                            buffers_[i].data(), static_cast<sb4>(width),
                                            static_cast<ub2>(common::kSqltStr), &indicators_[i],
                                            &lengths_[i], &returnCodes_[i], OCI_DEFAULT);
                    }
                    if (!ociOk(rc))
                        return oracleError(err_, common::ErrorCode::QueryError, "define");
                }
                return common::Status::OK();
            }

            std::vector<std::string> fields() const {
                std::vector<std::string> result;
                result.reserve(columns_.size());
                for (const auto &column: columns_) result.push_back(column.name);
                return result;
            }

            common::Status fetchOne(common::Row &row, bool &hasRow) {
                hasRow = false;
                const sword frc = OCIStmtFetch2(stmt_, err_, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
                if (frc == OCI_NO_DATA) return common::Status::OK();
                if (!ociOk(frc)) return oracleError(err_, common::ErrorCode::QueryError, "fetch");
                for (std::size_t i = 0; i < columns_.size(); ++i) {
                    const auto &meta = columns_[i];
                    if (indicators_[i] == -1) {
                        row.set(meta.name, common::Value{nullptr});
                        continue;
                    }
                    if (!common::oracleIsLob(meta.sqlt)) {
                        const std::string text(buffers_[i].data(), lengths_[i]);
                        row.set(meta.name, common::oracleValueFromText(
                            meta.sqlt, text, meta.precision, meta.scale));
                        continue;
                    }
                    const bool clob = meta.sqlt == common::kSqltClob;
                    oraub8 length = 0;
                    sword rc = OCILobGetLength2(svc_, err_, lobs_[i], &length);
                    if (!ociOk(rc))
                        return oracleError(err_, common::ErrorCode::QueryError, "lob length");
                    if (static_cast<std::int64_t>(length) > lobMaxBytes_)
                        return common::Status::error(
                            common::ErrorCode::NotSupported,
                            "Oracle: LOB column '" + meta.name + "' exceeds lob_max_bytes=" +
                            std::to_string(lobMaxBytes_));
                    if (clob) {
                        if (length == 0) row.set(meta.name, common::Value{std::string()});
                        else {
                            const oraub8 maxBytes = static_cast<oraub8>(lobMaxBytes_);
                            const oraub8 capacity = length > (maxBytes - 1) / 4
                                                        ? maxBytes + 1 : length * 4 + 1;
                            std::string text(static_cast<std::size_t>(capacity), '\0');
                            oraub8 bytes = capacity, chars = length;
                            rc = OCILobRead2(svc_, err_, lobs_[i], &bytes, &chars, 1, text.data(),
                                             capacity, OCI_ONE_PIECE, nullptr, nullptr, 0,
                                             SQLCS_IMPLICIT);
                            if (rc == OCI_NEED_DATA || bytes > maxBytes)
                                return common::Status::error(
                                    common::ErrorCode::NotSupported,
                                    "Oracle: CLOB column '" + meta.name +
                                    "' exceeds lob_max_bytes=" + std::to_string(lobMaxBytes_));
                            if (!ociOk(rc))
                                return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                            text.resize(static_cast<std::size_t>(bytes));
                            row.set(meta.name, common::Value{std::move(text)});
                        }
                    } else {
                        common::Blob bytes(static_cast<std::size_t>(length));
                        if (length > 0) {
                            oraub8 amount = length;
                            rc = OCILobRead2(svc_, err_, lobs_[i], &amount, nullptr, 1,
                                             bytes.data(), length, OCI_ONE_PIECE, nullptr, nullptr,
                                             0, 0);
                            if (!ociOk(rc))
                                return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                            bytes.resize(static_cast<std::size_t>(amount));
                        }
                        row.set(meta.name, common::Value{std::move(bytes)});
                    }
                }
                hasRow = true;
                return common::Status::OK();
            }

        private:
            OCIEnv *env_;
            OCISvcCtx *svc_;
            OCIError *err_;
            OCIStmt *stmt_ = nullptr;
            std::int64_t lobMaxBytes_;
            std::vector<OraColumnMeta> columns_;
            std::vector<std::vector<char> > buffers_;
            std::vector<sb2> indicators_;
            std::vector<ub2> lengths_;
            std::vector<ub2> returnCodes_;
            std::vector<OCILobLocator *> lobs_;
            std::vector<OCIDefine *> defines_;
        };

        std::string trimTrailing(const std::string &s) {
            std::size_t end = s.size();
            while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
            return s.substr(0, end);
        }
#endif
    }

#ifdef DBMW_ENABLE_ORACLE
    class OracleCursor final : public core::ICursor {
    public:
        OracleCursor(OCIStmt *stmt, OCIError *err,
                     std::unique_ptr<OracleResultReader> reader,
                     std::mutex &operationMtx, bool &operationActive,
                     const std::size_t batchSize)
            : stmt_(stmt), err_(err), reader_(std::move(reader)),
              operationMtx_(operationMtx), operationActive_(operationActive),
              batchSize_(batchSize), fields_(reader_->fields()) {}

        ~OracleCursor() override { (void) close(); }

        common::Status fetch(const std::size_t n, common::ResultSet &out) override {
            if (!open_ || eof_) return common::Status::OK();
            ActiveOperation active(operationMtx_, operationActive_);
            if (out.fields().empty()) out.setFields(fields_);
            const std::size_t wanted = n == 0 ? batchSize_ : n;
            for (std::size_t i = 0; i < wanted; ++i) {
                common::Row row;
                bool ok = false;
                const auto status = reader_->fetchOne(row, ok);
                if (!status.ok()) return status;
                if (!ok) {
                    eof_ = true;
                    break;
                }
                out.addRow(std::move(row));
                ++rowsFetched_;
            }
            return common::Status::OK();
        }

        common::Status fetchRow(common::Row &out, bool &ok) override {
            ok = false;
            common::ResultSet one;
            const auto status = fetch(1, one);
            if (!status.ok() || one.empty()) return status;
            out = std::move(one.mutableRows().front());
            ok = true;
            return common::Status::OK();
        }

        common::Status close() override {
            if (!open_) return common::Status::OK();
            reader_.reset();
            const sword rc = OCIStmtRelease(stmt_, err_, nullptr, 0, OCI_DEFAULT);
            stmt_ = nullptr;
            open_ = false;
            return ociOk(rc) ? common::Status::OK()
                             : oracleError(err_, common::ErrorCode::CursorError,
                                           "cursor close");
        }

        [[nodiscard]] bool isOpen() const override { return open_; }
        [[nodiscard]] bool hasNext() const override { return open_ && !eof_; }
        [[nodiscard]] std::uint64_t rowsFetched() const override { return rowsFetched_; }

    private:
        OCIStmt *stmt_;
        OCIError *err_;
        std::unique_ptr<OracleResultReader> reader_;
        std::mutex &operationMtx_;
        bool &operationActive_;
        std::size_t batchSize_;
        std::vector<std::string> fields_;
        bool open_ = true;
        bool eof_ = false;
        std::uint64_t rowsFetched_ = 0;
    };

    common::Status OracleConnection::connectString(const config::DataSourceConfig &cfg,
                                                   std::string &out) const {
        if (const auto it = cfg.extra.find("connection_string"); it != cfg.extra.end()) {
            if (cfg.tls_enabled)
                return common::Status::error(
                    common::ErrorCode::ConfigError,
                    "Oracle TLS options cannot be combined with extra.connection_string");
            if (it->second.empty())
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "Oracle extra.connection_string must not be empty");
            out = it->second;
            return common::Status::OK();
        }
        if (!cfg.dsn.empty()) {
            if (cfg.tls_enabled)
                return common::Status::error(common::ErrorCode::ConfigError,
                                             "Oracle TLS options cannot be combined with dsn");
            out = cfg.dsn;
            return common::Status::OK();
        }
        if (cfg.tls_enabled && (!cfg.tls_cert.empty() || !cfg.tls_key.empty()))
            return common::Status::error(
                common::ErrorCode::ConfigError,
                "Oracle TLS requires a wallet; tls.cert/tls.key are not consumed directly");
        common::OracleConnectOptions options;
        options.host = cfg.host.empty() ? std::string("localhost") : cfg.host;
        options.port = cfg.port != 0 ? cfg.port : 1521;
        options.serviceName = cfg.oracle.service_name.empty()
                                  ? cfg.database : cfg.oracle.service_name;
        options.sid = cfg.oracle.sid;
        if (options.serviceName.empty() && options.sid.empty()) {
            if (const auto it = cfg.extra.find("service_name"); it != cfg.extra.end())
                options.serviceName = it->second;
            if (const auto it = cfg.extra.find("sid"); it != cfg.extra.end())
                options.sid = it->second;
        }
        options.connectionTimeoutMs = cfg.connection_timeout_ms;
        options.tlsEnabled = cfg.tls_enabled;
        options.tlsVerifyPeer = cfg.tls_verify_peer;
        options.walletLocation = cfg.oracle.wallet_location.empty()
                                     ? cfg.tls_ca : cfg.oracle.wallet_location;
        if (options.walletLocation.empty())
            if (const auto it = cfg.extra.find("wallet_location"); it != cfg.extra.end())
                options.walletLocation = it->second;
        options.serverCertDn = cfg.oracle.server_cert_dn;
        if (options.serverCertDn.empty())
            if (const auto it = cfg.extra.find("server_cert_dn"); it != cfg.extra.end())
                options.serverCertDn = it->second;
        return common::oracleBuildConnectDescriptor(options, out);
    }
#endif

    common::Status OracleConnection::connect(const config::DataSourceConfig &cfg) {
        cfg_ = cfg;
#ifdef DBMW_ENABLE_ORACLE
        close();
        lobMaxBytes_ = cfg.oracle.lob_max_bytes > 0
                           ? cfg.oracle.lob_max_bytes : 4194304;
        if (const auto lob = cfg.extra.find("lob_max_bytes");
            cfg.oracle.lob_max_bytes == 4194304 && lob != cfg.extra.end()) {
            const long long legacy = std::strtoll(lob->second.c_str(), nullptr, 10);
            if (legacy > 0) lobMaxBytes_ = legacy;
        }

        const ub2 clientCharset = resolveClientCharset(cfg);
        sword rc = OCIEnvNlsCreate(&env_, OCI_THREADED, nullptr, nullptr, nullptr, nullptr, 0,
                                   nullptr, clientCharset, clientCharset);
        if (!ociOk(rc))
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "Oracle: OCIEnvNlsCreate failed");
        rc = OCIHandleAlloc(env_, reinterpret_cast<void **>(&err_), OCI_HTYPE_ERROR, 0, nullptr);
        if (!ociOk(rc)) {
            OCIHandleFree(env_, OCI_HTYPE_ENV);
            env_ = nullptr;
            return common::Status::error(common::ErrorCode::ConnectionFailed,
                                         "Oracle: OCIHandleAlloc(OCI_HTYPE_ERROR) failed");
        }

        std::string db;
        const auto connectStringStatus = connectString(cfg, db);
        if (!connectStringStatus.ok()) {
            OCIHandleFree(err_, OCI_HTYPE_ERROR);
            err_ = nullptr;
            OCIHandleFree(env_, OCI_HTYPE_ENV);
            env_ = nullptr;
            return connectStringStatus;
        }
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
            ub4 callTime = static_cast<ub4>(cfg.query_timeout_ms);
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
        std::vector<OCILobLocator *> lobLocs;
        static char kEmpty[1] = {0};

        const std::size_t totalBinds = params.size() +
            (returning.present ? returning.bindCount : 0);
        textBufs.reserve(totalBinds + 1);
        rawBufs.reserve(totalBinds + 1);
        inds.reserve(totalBinds + 1);
        rlens.reserve(totalBinds + 1);
        rcs.reserve(totalBinds + 1);

        const LobBindMode lobMode = resolveLobBindMode(cfg_);
        LobBindGuard lobGuard(env_, svc_, err_);

        for (const auto &v: params) {
            const auto bind = common::oracleBindValue(v);
            textBufs.emplace_back();
            rawBufs.emplace_back();
            lobLocs.emplace_back(nullptr);
            if (bind.unsupported)
                return common::Status::error(common::ErrorCode::NotSupported,
                                             "Oracle: cannot bind value of type " +
                                             common::valueToString(v));
            if (bind.raw.has_value()) {
                if (lobMode == LobBindMode::Lob ||
                    (lobMode == LobBindMode::Auto && bind.raw->size() > kDirectBindLimit)) {
                    auto *loc = lobGuard.createBlob(*bind.raw);
                    if (loc == nullptr)
                        return oracleError(err_, common::ErrorCode::QueryError, "lob bind");
                    lobLocs.back() = loc;
                    textBufs.back().assign(1, '\0');
                } else {
                    if (bind.raw->size() > 32767)
                        return common::Status::error(
                            common::ErrorCode::NotSupported,
                            "Oracle: binary parameter of " + std::to_string(bind.raw->size()) +
                            " bytes exceeds the 32767 byte direct bind limit; set "
                            "oracle.blob_bind=lob to bind it as a temporary BLOB");
                    rawBufs.back() = *bind.raw;
                    textBufs.back().assign(1, '\0');
                }
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
                lobLocs.emplace_back(nullptr);
                inds.push_back(0);
                rlens.push_back(0);
                rcs.push_back(0);
            }
        }

        for (std::size_t i = 0; i < totalBinds; ++i) {
            OCIBind *bindHandle = nullptr;
            if (lobLocs[i] != nullptr) {
                const sword brc = OCIBindByPos(
                    stmt, &bindHandle, err_, static_cast<ub4>(i + 1), &lobLocs[i],
                    static_cast<sb4>(sizeof(OCILobLocator *)),
                    static_cast<ub2>(common::kSqltBlob), &inds[i], &rlens[i], &rcs[i], 0, nullptr,
                    OCI_DEFAULT);
                if (!ociOk(brc)) return oracleError(err_, common::ErrorCode::QueryError, "bind");
                continue;
            }
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

        struct LobColumnGuard {
            std::vector<OCILobLocator *> &locators;

            ~LobColumnGuard() {
                for (auto *locator: locators)
                    if (locator != nullptr) OCIDescriptorFree(locator, OCI_DTYPE_LOB);
            }
        } lobColumnGuard{colLobs};

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
            for (std::size_t i = 0; i < columns.size(); ++i) {
                const auto &meta = columns[i];
                if (colInds[i] == -1) {
                    row.set(meta.name, common::Value{nullptr});
                    if (!common::oracleIsLob(meta.sqlt)) ++bufIndex;
                    continue;
                }
                if (common::oracleIsLob(meta.sqlt)) {
                    const bool isClob = meta.sqlt == common::kSqltClob;
                    oraub8 length = 0;
                    rc = OCILobGetLength2(svc_, err_, colLobs[i], &length);
                    if (!ociOk(rc))
                        return oracleError(err_, common::ErrorCode::QueryError, "lob length");
                    if (static_cast<std::int64_t>(length) > lobMaxBytes_)
                        return common::Status::error(
                            common::ErrorCode::NotSupported,
                            "Oracle: LOB column '" + meta.name + "' holds " +
                            std::to_string(static_cast<unsigned long long>(length)) +
                            " bytes, above lob_max_bytes=" + std::to_string(lobMaxBytes_));
                    if (isClob) {
                        if (length == 0) {
                            row.set(meta.name, common::Value{std::string()});
                            continue;
                        }
                        const oraub8 maxBytes = static_cast<oraub8>(lobMaxBytes_);
                        const oraub8 capacity = length > (maxBytes - 1) / 4
                                                    ? maxBytes + 1
                                                    : length * 4 + 1;
                        std::string text(static_cast<std::size_t>(capacity), '\0');
                        oraub8 byteAmount = capacity;
                        oraub8 charAmount = length;
                        oraub8 offset = 1;
                        rc = OCILobRead2(svc_, err_, colLobs[i], &byteAmount, &charAmount, offset,
                                         text.data(), capacity,
                                         OCI_ONE_PIECE, nullptr, nullptr, 0, SQLCS_IMPLICIT);
                        if (rc == OCI_NEED_DATA || byteAmount > maxBytes)
                            return common::Status::error(
                                common::ErrorCode::NotSupported,
                                "Oracle: CLOB column '" + meta.name +
                                "' exceeds lob_max_bytes=" + std::to_string(lobMaxBytes_));
                        if (!ociOk(rc))
                            return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                        text.resize(static_cast<std::size_t>(byteAmount));
                        row.set(meta.name, common::Value{text});
                    } else {
                        if (length == 0) {
                            row.set(meta.name, common::Value{common::Blob{}});
                            continue;
                        }
                        common::Blob bytes(static_cast<std::size_t>(length));
                        oraub8 amount = 0;
                        oraub8 offset = 1;
                        rc = OCILobRead2(svc_, err_, colLobs[i], &amount, nullptr, offset,
                                         bytes.data(), static_cast<oraub8>(bytes.size()),
                                         OCI_ONE_PIECE, nullptr, nullptr, 0, 0);
                        if (!ociOk(rc))
                            return oracleError(err_, common::ErrorCode::QueryError, "lob read");
                        bytes.resize(static_cast<std::size_t>(amount));
                        row.set(meta.name, common::Value{bytes});
                    }
                    continue;
                }
                const std::string text(colBufs[bufIndex].data(),
                                       static_cast<std::size_t>(colRlens[i]));
                row.set(meta.name, common::oracleValueFromText(meta.sqlt, text, meta.precision,
                                                              meta.scale));
                ++bufIndex;
            }
            bufIndex = 0;

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

    common::Status OracleConnection::queryAll(const std::string &sql,
                                              std::vector<common::ResultSet> &out) {
        return queryAll(sql, common::Params{}, out);
    }

    common::Status OracleConnection::queryAll(const std::string &sql,
                                              const common::Params &params,
                                              std::vector<common::ResultSet> &out) {
        out.clear();
#ifdef DBMW_ENABLE_ORACLE
#if defined(OCI_RESULT_TYPE_SELECT) && defined(OCI_ATTR_STMT_TYPE) && defined(OCI_STMT_SELECT)
        if (!open_ || !svc_) return notConnected("queryAll");
        ActiveOperation active(operationMtx_, operationActive_);
        std::size_t found = 0;
        const std::string oraSql = replacePlaceholders(
            sql, [](const std::size_t i) { return ":" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);
        OCIStmt *stmt = nullptr;
        sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                   reinterpret_cast<const OraText *>(oraSql.data()),
                                   static_cast<ub4>(oraSql.size()), nullptr, 0, OCI_NTV_SYNTAX,
                                   OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError,
                                           "queryAll prepare");
        struct QueryAllGuard {
            OCIStmt *stmt;
            OCIError *err;
            ~QueryAllGuard() {
                if (stmt) OCIStmtRelease(stmt, err, nullptr, 0, OCI_DEFAULT);
            }
        } guard{stmt, err_};
        OracleInputStorage storage(env_, svc_, err_, params.size());
        if (const auto status = bindOracleInputs(env_, svc_, err_, stmt, cfg_, params, storage);
            !status.ok()) return status;
        ub2 statementType = 0;
        rc = OCIAttrGet(stmt, OCI_HTYPE_STMT, &statementType, nullptr, OCI_ATTR_STMT_TYPE, err_);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError,
                                           "queryAll statement type");
        rc = OCIStmtExecute(svc_, stmt, err_, statementType == OCI_STMT_SELECT ? 0u : 1u,
                            0, nullptr, nullptr, OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError,
                                           "queryAll execute");

        const auto readSet = [&](OCIStmt *result, common::ResultSet &set) -> common::Status {
            OracleResultReader reader(env_, svc_, err_, lobMaxBytes_);
            if (const auto status = reader.setup(result); !status.ok()) return status;
            set.setFields(reader.fields());
            while (true) {
                common::Row row;
                bool hasRow = false;
                if (const auto status = reader.fetchOne(row, hasRow); !status.ok()) return status;
                if (!hasRow) break;
                if (cfg_.max_result_rows > 0 &&
                    set.rowCount() >= static_cast<std::size_t>(cfg_.max_result_rows))
                    return common::Status::error(
                        common::ErrorCode::QueryError,
                        "Oracle: result set exceeds max_result_rows=" +
                        std::to_string(cfg_.max_result_rows));
                set.addRow(std::move(row));
            }
            return common::Status::OK();
        };

        if (statementType == OCI_STMT_SELECT) {
            common::ResultSet set;
            if (const auto status = readSet(stmt, set); !status.ok()) return status;
            out.push_back(std::move(set));
            return common::Status::OK();
        }
        while (true) {
            void *result = nullptr;
            ub4 resultType = 0;
            rc = OCIStmtGetNextResult(stmt, err_, &result, &resultType, OCI_DEFAULT);
            if (rc == OCI_NO_DATA) break;
            if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::QueryError,
                                               "implicit result");
            if (resultType != OCI_RESULT_TYPE_SELECT || result == nullptr)
                return common::Status::error(common::ErrorCode::NotSupported,
                                             "Oracle: unsupported implicit result type");
            auto *child = static_cast<OCIStmt *>(result);
            common::ResultSet set;
            const auto status = readSet(child, set);
            const sword releaseRc = OCIStmtRelease(child, err_, nullptr, 0, OCI_DEFAULT);
            if (!status.ok()) return status;
            if (!ociOk(releaseRc))
                return oracleError(err_, common::ErrorCode::QueryError,
                                   "implicit result release");
            out.push_back(std::move(set));
        }
        return common::Status::OK();
#else
        return core::IDatabaseConnection::queryAll(sql, params, out);
#endif
#else
        (void) sql;
        (void) params;
        return driverDisabled("queryAll");
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
        // Keep the caller-facing SQL here. runStatement() owns placeholder rewriting and must see
        // the original question marks again when a cached statement is executed.
        preparedSql_[preparedSeq_] = sql;
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
            std::size_t ignored = 0;
            const std::string oraSql = replacePlaceholders(
                it->second, [](const std::size_t i) { return ":" + std::to_string(i + 1); },
                ignored);
            const sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                             reinterpret_cast<const OraText *>(oraSql.data()),
                                             static_cast<ub4>(oraSql.size()), keyPtr, keyLen,
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

    common::Status OracleConnection::executeBatch(const std::string &sql,
                                                  const common::ParamBatch &batch,
                                                  common::BatchResult &out) {
        out.clear();
#ifdef DBMW_ENABLE_ORACLE
#if defined(OCI_ATTR_DML_ROW_COUNT_ARRAY) && defined(OCI_BATCH_ERRORS) && \
    (defined(OCI_RETURN_ROW_COUNT_ARRAY) || defined(OCI_RETURN_ROWCOUNT_ARRAY))
        if (batch.empty()) return common::Status::OK();
        if (!open_ || !svc_) return notConnected("batch");
        if (batch.size() > static_cast<std::size_t>(std::numeric_limits<ub4>::max()))
            return common::Status::error(common::ErrorCode::QueryError,
                                         "Oracle: batch size exceeds OCI ub4 iteration limit");
        common::OracleReturning returning;
        std::size_t placeholderCount = 0;
        const std::string oraSql = replacePlaceholders(
            sql, [](const std::size_t i) { return ":" + std::to_string(i + 1); },
            placeholderCount);
        if (common::oracleParseReturningInto(oraSql, returning))
            return core::IDatabaseConnection::executeBatch(sql, batch, out);
        if (batch.front().size() != placeholderCount)
            return paramMismatch(batch.front().size(), placeholderCount);
        for (const auto &row: batch)
            if (row.size() != placeholderCount)
                return paramMismatch(row.size(), placeholderCount);

        struct Column {
            std::size_t stride = 1;
            std::vector<char> values;
            std::vector<sb2> indicators;
            std::vector<ub2> lengths;
            std::vector<ub2> returnCodes;
        };
        std::vector<Column> columns(placeholderCount);
        for (std::size_t c = 0; c < placeholderCount; ++c) {
            for (const auto &row: batch) {
                const auto value = common::oracleBindValue(row[c]);
                if (value.unsupported || value.raw)
                    return core::IDatabaseConnection::executeBatch(sql, batch, out);
                if (value.text && value.text->size() + 1 > 32767)
                    return core::IDatabaseConnection::executeBatch(sql, batch, out);
                if (value.text) columns[c].stride = std::max(columns[c].stride,
                                                             value.text->size() + 1);
            }
            auto &column = columns[c];
            column.values.assign(column.stride * batch.size(), '\0');
            column.indicators.assign(batch.size(), 0);
            column.lengths.assign(batch.size(), 0);
            column.returnCodes.assign(batch.size(), 0);
            for (std::size_t r = 0; r < batch.size(); ++r) {
                const auto value = common::oracleBindValue(batch[r][c]);
                if (!value.text) {
                    column.indicators[r] = -1;
                    continue;
                }
                std::memcpy(column.values.data() + r * column.stride,
                            value.text->data(), value.text->size());
                column.lengths[r] = static_cast<ub2>(value.text->size() + 1);
            }
        }

        const bool ownTransaction = !inTransaction();
        if (ownTransaction) {
            const auto status = begin();
            if (!status.ok()) return status;
        }
        ActiveOperation active(operationMtx_, operationActive_);
        OCIStmt *stmt = nullptr;
        sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                   reinterpret_cast<const OraText *>(oraSql.data()),
                                   static_cast<ub4>(oraSql.size()), nullptr, 0, OCI_NTV_SYNTAX,
                                   OCI_DEFAULT);
        struct BatchStatementGuard {
            OCIStmt *&stmt;
            OCIError *err;
            ~BatchStatementGuard() {
                if (stmt) OCIStmtRelease(stmt, err, nullptr, 0, OCI_DEFAULT);
            }
        } guard{stmt, err_};
        auto fail = [&](common::Status status) {
            if (ownTransaction) (void) rollback();
            out.clear();
            return status;
        };
        if (!ociOk(rc)) return fail(oracleError(err_, common::ErrorCode::QueryError,
                                                "batch prepare"));
        for (std::size_t c = 0; c < columns.size(); ++c) {
            auto &column = columns[c];
            OCIBind *bind = nullptr;
            rc = OCIBindByPos(stmt, &bind, err_, static_cast<ub4>(c + 1),
                              column.values.data(), static_cast<sb4>(column.stride),
                              static_cast<ub2>(common::kSqltStr), column.indicators.data(),
                              column.lengths.data(), column.returnCodes.data(), 0, nullptr,
                              OCI_DEFAULT);
            if (!ociOk(rc))
                return fail(oracleError(err_, common::ErrorCode::QueryError, "batch bind"));
            rc = OCIBindArrayOfStruct(bind, err_, static_cast<ub4>(column.stride),
                                      sizeof(sb2), sizeof(ub2), sizeof(ub2));
            if (!ociOk(rc))
                return fail(oracleError(err_, common::ErrorCode::QueryError,
                                        "batch bind layout"));
        }
#if defined(OCI_RETURN_ROW_COUNT_ARRAY)
        const ub4 rowCountMode = OCI_RETURN_ROW_COUNT_ARRAY;
#else
        const ub4 rowCountMode = OCI_RETURN_ROWCOUNT_ARRAY;
#endif
        const ub4 executeMode = rowCountMode | (ownTransaction ? OCI_BATCH_ERRORS : 0u);
        rc = OCIStmtExecute(svc_, stmt, err_, static_cast<ub4>(batch.size()), 0, nullptr, nullptr,
                            executeMode);
        ub4 errorCount = 0;
        if (ownTransaction)
            (void) OCIAttrGet(stmt, OCI_HTYPE_STMT, &errorCount, nullptr,
                              OCI_ATTR_NUM_DML_ERRORS, err_);
        if (!ociOk(rc) || errorCount != 0)
            return fail(oracleError(err_, common::ErrorCode::QueryError, "array DML"));
        ub8 *rowCounts = nullptr;
        ub4 rowCountBytes = 0;
        rc = OCIAttrGet(stmt, OCI_HTYPE_STMT, &rowCounts, &rowCountBytes,
                        OCI_ATTR_DML_ROW_COUNT_ARRAY, err_);
        if (!ociOk(rc) || rowCounts == nullptr)
            return fail(oracleError(err_, common::ErrorCode::QueryError,
                                    "array DML row counts"));
        out.affected.reserve(batch.size());
        out.keys.resize(batch.size());
        for (std::size_t i = 0; i < batch.size(); ++i)
            out.affected.push_back(static_cast<std::int64_t>(rowCounts[i]));
        if (ownTransaction) {
            const auto status = commit();
            if (!status.ok()) {
                out.clear();
                return status;
            }
        }
        return common::Status::OK();
#else
        return core::IDatabaseConnection::executeBatch(sql, batch, out);
#endif
#else
        (void) sql;
        (void) batch;
        return driverDisabled("batch");
#endif
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
        ActiveOperation active(operationMtx_, operationActive_);
        std::size_t found = 0;
        const std::string oraSql = replacePlaceholders(
            sql, [](const std::size_t i) { return ":" + std::to_string(i + 1); }, found);
        if (found != params.size()) return paramMismatch(params.size(), found);

        OCIStmt *stmt = nullptr;
        sword rc = OCIStmtPrepare2(svc_, &stmt, err_,
                                   reinterpret_cast<const OraText *>(oraSql.data()),
                                   static_cast<ub4>(oraSql.size()), nullptr, 0, OCI_NTV_SYNTAX,
                                   OCI_DEFAULT);
        if (!ociOk(rc)) return oracleError(err_, common::ErrorCode::CursorError, "cursor prepare");
        const auto releaseOnError = [&] {
            if (stmt != nullptr) {
                (void) OCIStmtRelease(stmt, err_, nullptr, 0, OCI_DEFAULT);
                stmt = nullptr;
            }
        };

        std::vector<std::vector<char> > textBuffers(params.size());
        std::vector<common::Blob> rawBuffers(params.size());
        std::vector<OCILobLocator *> lobLocators(params.size(), nullptr);
        std::vector<sb2> indicators(params.size(), 0);
        std::vector<ub2> lengths(params.size(), 0);
        std::vector<ub2> returnCodes(params.size(), 0);
        LobBindGuard lobGuard(env_, svc_, err_);
        static char kEmpty[1] = {0};
        const LobBindMode lobMode = resolveLobBindMode(cfg_);
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto value = common::oracleBindValue(params[i]);
            if (value.unsupported) {
                releaseOnError();
                return common::Status::error(common::ErrorCode::NotSupported,
                                             "Oracle: unsupported cursor parameter type");
            }
            if (value.raw) {
                if (lobMode == LobBindMode::Lob ||
                    (lobMode == LobBindMode::Auto && value.raw->size() > kDirectBindLimit)) {
                    lobLocators[i] = lobGuard.createBlob(*value.raw);
                    if (!lobLocators[i]) {
                        const auto status = oracleError(err_, common::ErrorCode::CursorError,
                                                       "cursor lob bind");
                        releaseOnError();
                        return status;
                    }
                } else rawBuffers[i] = *value.raw;
            } else if (value.text) {
                textBuffers[i].assign(value.text->begin(), value.text->end());
                textBuffers[i].push_back('\0');
            } else {
                indicators[i] = -1;
            }
            OCIBind *bind = nullptr;
            if (lobLocators[i]) {
                rc = OCIBindByPos(stmt, &bind, err_, static_cast<ub4>(i + 1), &lobLocators[i],
                                  sizeof(OCILobLocator *), static_cast<ub2>(common::kSqltBlob),
                                  &indicators[i], &lengths[i], &returnCodes[i], 0, nullptr,
                                  OCI_DEFAULT);
            } else {
                const bool raw = !rawBuffers[i].empty();
                void *data = raw ? static_cast<void *>(rawBuffers[i].data())
                                 : indicators[i] == -1
                                       ? static_cast<void *>(kEmpty)
                                       : static_cast<void *>(textBuffers[i].data());
                const sb4 size = indicators[i] == -1 ? 0 : static_cast<sb4>(
                    raw ? rawBuffers[i].size() : textBuffers[i].size());
                rc = OCIBindByPos(stmt, &bind, err_, static_cast<ub4>(i + 1), data, size,
                                  raw ? static_cast<ub2>(common::kSqltBin)
                                      : static_cast<ub2>(common::kSqltStr),
                                  &indicators[i], &lengths[i], &returnCodes[i], 0, nullptr,
                                  OCI_DEFAULT);
            }
            if (!ociOk(rc)) {
                const auto status = oracleError(err_, common::ErrorCode::CursorError,
                                                "cursor bind");
                releaseOnError();
                return status;
            }
        }
        rc = OCIStmtExecute(svc_, stmt, err_, 0, 0, nullptr, nullptr, OCI_DEFAULT);
        if (!ociOk(rc)) {
            const auto status = oracleError(err_, common::ErrorCode::CursorError,
                                            "cursor execute");
            releaseOnError();
            return status;
        }
        auto reader = std::make_unique<OracleResultReader>(env_, svc_, err_, lobMaxBytes_);
        if (const auto status = reader->setup(stmt); !status.ok()) {
            releaseOnError();
            return status;
        }
        out = std::make_unique<OracleCursor>(stmt, err_, std::move(reader), operationMtx_,
                                             operationActive_,
                                             opts.batch_size > 0 ? opts.batch_size : 256);
        stmt = nullptr;
        return common::Status::OK();
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
