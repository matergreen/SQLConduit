#include "dbmw/common/oracle_types.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace dbmw::common {
    namespace {
        constexpr std::size_t kReturningKeywordLength = 9;

        int upperChar(const char c) {
            return std::toupper(static_cast<unsigned char>(c));
        }

        bool foldEquals(const std::string &a, const char *b) {
            const std::size_t n = std::strlen(b);
            if (a.size() != n) return false;
            for (std::size_t i = 0; i < n; ++i)
                if (upperChar(a[i]) != upperChar(b[i])) return false;
            return true;
        }

        bool isWordChar(const char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '#';
        }

        long long daysFromCivil(const int y, const unsigned m, const unsigned d) {
            const int yy = y - (m <= 2 ? 1 : 0);
            const int era = (yy >= 0 ? yy : yy - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(yy - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
        }

        bool readUInt(const std::string &s, std::size_t &i, const int width, int &out) {
            if (i + static_cast<std::size_t>(width) > s.size()) return false;
            int v = 0;
            for (int k = 0; k < width; ++k) {
                const char c = s[i + static_cast<std::size_t>(k)];
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                v = v * 10 + (c - '0');
            }
            i += static_cast<std::size_t>(width);
            out = v;
            return true;
        }

        void skipSpaces(const std::string &s, std::size_t &i) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        }

        int hexNibble(const char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        Blob hexToBlob(const std::string &s) {
            Blob b;
            b.reserve(s.size() / 2);
            for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
                const int hi = hexNibble(s[i]);
                const int lo = hexNibble(s[i + 1]);
                if (hi < 0 || lo < 0) break;
                b.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
            }
            return b;
        }

        std::string blobToHex(const Blob &b) {
            static const char *kHex = "0123456789ABCDEF";
            std::string s;
            s.reserve(b.size() * 2);
            for (const std::uint8_t byte: b) {
                s.push_back(kHex[(byte >> 4) & 0x0F]);
                s.push_back(kHex[byte & 0x0F]);
            }
            return s;
        }

        bool parseInt64Strict(const std::string &s, std::int64_t &out) {
            if (s.empty()) return false;
            errno = 0;
            char *end = nullptr;
            const long long v = std::strtoll(s.c_str(), &end, 10);
            if (end != s.c_str() + s.size() || errno == ERANGE) return false;
            out = static_cast<std::int64_t>(v);
            return true;
        }

        bool parseUint64Strict(const std::string &s, std::uint64_t &out) {
            if (s.empty() || s[0] == '-') return false;
            errno = 0;
            char *end = nullptr;
            const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
            if (end != s.c_str() + s.size() || errno == ERANGE) return false;
            out = static_cast<std::uint64_t>(v);
            return true;
        }

        double parseDoubleSimple(const std::string &s) {
            return std::strtod(s.c_str(), nullptr);
        }

        bool validDescriptorAtom(const std::string &value) {
            if (value.empty()) return false;
            for (const unsigned char c: value) {
                if (c <= 0x20 || c == '(' || c == ')' || c == '=') return false;
            }
            return true;
        }

        bool validQuotedDescriptorValue(const std::string &value) {
            if (value.empty()) return false;
            for (const unsigned char c: value) {
                if (c < 0x20 || c == '"') return false;
            }
            return true;
        }
    }

    OracleTypeClass oracleTypeClass(const std::uint16_t sqlt) {
        switch (sqlt) {
            case kSqltChr:
            case kSqltStr:
            case kSqltVcs:
            case kSqltAfc:
            case kSqltLng:
                return OracleTypeClass::Text;
            case kSqltNum:
            case kSqltVnu:
            case kSqltPdn:
            case kSqltInt:
            case kSqltUin:
            case kSqltFlt:
            case kSqltBfloat:
            case kSqltBdouble:
            case kSqltIbfloat:
            case kSqltIbdouble:
                return OracleTypeClass::Number;
            case kSqltBin:
            case kSqltLbi:
            case kSqltVbi:
                return OracleTypeClass::Binary;
            case kSqltDat:
                return OracleTypeClass::Date;
            case kSqltTimestamp:
                return OracleTypeClass::Timestamp;
            case kSqltTimestampTz:
            case kSqltTimestampLtz:
                return OracleTypeClass::TimestampTz;
            case kSqltIntervalYm:
            case kSqltIntervalDs:
                return OracleTypeClass::Interval;
            case kSqltClob:
            case kSqltBlob:
            case kSqltBfile:
            case kSqltCfile:
                return OracleTypeClass::Lob;
            case kSqltRid:
            case kSqltRdd:
                return OracleTypeClass::Rowid;
            default:
                return OracleTypeClass::Unknown;
        }
    }

    const char *oracleTypeName(const std::uint16_t sqlt) {
        switch (sqlt) {
            case kSqltChr: return "VARCHAR2";
            case kSqltNum: return "NUMBER";
            case kSqltInt: return "INTEGER";
            case kSqltFlt: return "FLOAT";
            case kSqltStr: return "STRING";
            case kSqltVnu: return "VARNUM";
            case kSqltPdn: return "PACKED-DECIMAL";
            case kSqltLng: return "LONG";
            case kSqltVcs: return "VARCHAR";
            case kSqltRid: return "ROWID";
            case kSqltDat: return "DATE";
            case kSqltVbi: return "VARRAW";
            case kSqltBfloat: return "BINARY_FLOAT";
            case kSqltBdouble: return "BINARY_DOUBLE";
            case kSqltBin: return "RAW";
            case kSqltLbi: return "LONG RAW";
            case kSqltUin: return "UNSIGNED";
            case kSqltSls: return "DISPLAY";
            case kSqltAfc: return "CHAR";
            case kSqltIbfloat: return "BINARY_FLOAT";
            case kSqltIbdouble: return "BINARY_DOUBLE";
            case kSqltCur: return "REF CURSOR";
            case kSqltNty: return "NAMED TYPE";
            case kSqltRef: return "REF";
            case kSqltClob: return "CLOB";
            case kSqltBlob: return "BLOB";
            case kSqltBfile: return "BFILE";
            case kSqltCfile: return "CFILE";
            case kSqltRdd: return "ROWID DESC";
            case kSqltTimestamp: return "TIMESTAMP";
            case kSqltTimestampTz: return "TIMESTAMP WITH TIME ZONE";
            case kSqltIntervalYm: return "INTERVAL YEAR TO MONTH";
            case kSqltIntervalDs: return "INTERVAL DAY TO SECOND";
            case kSqltTimestampLtz: return "TIMESTAMP WITH LOCAL TIME ZONE";
            default: return "UNKNOWN";
        }
    }

    bool oracleIsLob(const std::uint16_t sqlt) {
        return oracleTypeClass(sqlt) == OracleTypeClass::Lob;
    }

    bool oracleIsRowid(const std::uint16_t sqlt) {
        return oracleTypeClass(sqlt) == OracleTypeClass::Rowid;
    }

    std::string oracleFormatDouble(double v) {
        if (!std::isfinite(v)) return std::string("NULL");
        for (int precision = 15; precision <= 17; ++precision) {
            char buf[64] = {0};
            std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
            const std::string candidate(buf);
            if (std::strtod(candidate.c_str(), nullptr) == v) return candidate;
        }
        char buf[64] = {0};
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        return std::string(buf);
    }

    bool oracleParseTimestamp(const std::string &text, Timestamp &out) {
        std::size_t i = 0;
        skipSpaces(text, i);
        int year = 0;
        int month = 0;
        int day = 0;
        if (!readUInt(text, i, 4, year)) return false;
        if (i >= text.size() || text[i] != '-') return false;
        ++i;
        if (!readUInt(text, i, 2, month)) return false;
        if (i >= text.size() || text[i] != '-') return false;
        ++i;
        if (!readUInt(text, i, 2, day)) return false;
        if (month < 1 || month > 12 || day < 1 || day > 31) return false;

        int hour = 0;
        int minute = 0;
        int second = 0;
        long long nanos = 0;
        skipSpaces(text, i);
        if (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            if (!readUInt(text, i, 2, hour)) return false;
            if (i >= text.size() || text[i] != ':') return false;
            ++i;
            if (!readUInt(text, i, 2, minute)) return false;
            if (i < text.size() && text[i] == ':') {
                ++i;
                if (!readUInt(text, i, 2, second)) return false;
            }
            if (i < text.size() && text[i] == '.') {
                ++i;
                long long frac = 0;
                int digits = 0;
                while (i < text.size() && digits < 9 &&
                       std::isdigit(static_cast<unsigned char>(text[i]))) {
                    frac = frac * 10 + (text[i] - '0');
                    ++i;
                    ++digits;
                }
                while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
                for (int k = digits; k < 9; ++k) frac *= 10;
                nanos = frac;
            }
        }

        long long offsetSeconds = 0;
        bool hasOffset = false;
        skipSpaces(text, i);
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            hasOffset = true;
            const int sign = text[i] == '-' ? -1 : 1;
            ++i;
            int offsetHour = 0;
            int offsetMinute = 0;
            if (!readUInt(text, i, 2, offsetHour)) return false;
            if (i < text.size() && text[i] == ':') {
                ++i;
                if (!readUInt(text, i, 2, offsetMinute)) return false;
            }
            offsetSeconds = sign * (static_cast<long long>(offsetHour) * 3600 +
                static_cast<long long>(offsetMinute) * 60);
        } else if (i < text.size() && (text[i] == 'Z' || text[i] == 'z')) {
            hasOffset = true;
            ++i;
        }

        if (hasOffset) {
            const long long days = daysFromCivil(year, static_cast<unsigned>(month),
                                                 static_cast<unsigned>(day));
            const long long seconds = days * 86400LL +
                static_cast<long long>(hour) * 3600LL +
                static_cast<long long>(minute) * 60LL +
                static_cast<long long>(second) - offsetSeconds;
            out = std::chrono::time_point_cast<Timestamp::duration>(
                std::chrono::system_clock::from_time_t(static_cast<std::time_t>(seconds)) +
                std::chrono::nanoseconds(nanos));
            return true;
        }

        std::tm local{};
        local.tm_year = year - 1900;
        local.tm_mon = static_cast<int>(month) - 1;
        local.tm_mday = day;
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = second;
        local.tm_isdst = -1;
        const std::time_t epoch = std::mktime(&local);
        if (epoch == static_cast<std::time_t>(-1)) return false;
        out = std::chrono::time_point_cast<Timestamp::duration>(
            std::chrono::system_clock::from_time_t(epoch) + std::chrono::nanoseconds(nanos));
        return true;
    }

    std::string oracleFormatTimestamp(const Timestamp &t) {
        return timestampToStringMs(t);
    }

    Value oracleValueFromText(const std::uint16_t sqlt, const std::string &text,
                              const std::int32_t precision, const std::int32_t scale) {
        switch (oracleTypeClass(sqlt)) {
            case OracleTypeClass::Text:
            case OracleTypeClass::Rowid:
                return Value{text};
            case OracleTypeClass::Interval:
                return sqlt == kSqltIntervalYm
                           ? Value{IntervalYearMonth{text}}
                           : Value{IntervalDaySecond{text}};
            case OracleTypeClass::Binary:
                return Value{hexToBlob(text)};
            case OracleTypeClass::Date: {
                Timestamp ts{};
                if (oracleParseTimestamp(text, ts)) return Value{ts};
                return Value{text};
            }
            case OracleTypeClass::Timestamp:
            case OracleTypeClass::TimestampTz: {
                Timestamp ts{};
                if (oracleParseTimestamp(text, ts)) return Value{ts};
                return Value{text};
            }
            case OracleTypeClass::Lob:
                if (sqlt == kSqltClob || sqlt == kSqltCfile) return Value{text};
                return Value{hexToBlob(text)};
            case OracleTypeClass::Number: {
                const bool floatish = sqlt == kSqltFlt || sqlt == kSqltBfloat ||
                    sqlt == kSqltBdouble || sqlt == kSqltIbfloat || sqlt == kSqltIbdouble ||
                    scale < 0;
                if (floatish) return Value{parseDoubleSimple(text)};
                const bool hasFraction = text.find('.') != std::string::npos ||
                    text.find('e') != std::string::npos || text.find('E') != std::string::npos;
                if (!hasFraction && (precision <= 0 || precision > 18)) {
                    std::int64_t asInt = 0;
                    if (parseInt64Strict(text, asInt)) return Value{asInt};
                    std::uint64_t asUint = 0;
                    if (parseUint64Strict(text, asUint)) return Value{asUint};
                    return Value{Decimal{text}};
                }
                if (!hasFraction && scale == 0) {
                    std::int64_t asInt = 0;
                    if (parseInt64Strict(text, asInt)) return Value{asInt};
                    std::uint64_t asUint = 0;
                    if (parseUint64Strict(text, asUint)) return Value{asUint};
                    return Value{Decimal{text}};
                }
                return Value{Decimal{text}};
            }
            case OracleTypeClass::Unknown:
            default:
                return Value{text};
        }
    }

    OracleBindValue oracleBindValue(const Value &v) {
        OracleBindValue out;
        if (std::holds_alternative<std::nullptr_t>(v)) return out;
        if (const auto *p = std::get_if<bool>(&v)) {
            out.text = *p ? std::string("1") : std::string("0");
            return out;
        }
        if (const auto *p = std::get_if<std::int64_t>(&v)) {
            out.text = std::to_string(*p);
            return out;
        }
        if (const auto *p = std::get_if<std::uint64_t>(&v)) {
            out.text = std::to_string(*p);
            return out;
        }
        if (const auto *p = std::get_if<double>(&v)) {
            if (!std::isfinite(*p)) return out;
            out.text = oracleFormatDouble(*p);
            return out;
        }
        if (const auto *p = std::get_if<Decimal>(&v)) {
            out.text = p->value;
            return out;
        }
        if (const auto *p = std::get_if<std::string>(&v)) {
            out.text = *p;
            return out;
        }
        if (const auto *p = std::get_if<Date>(&v)) {
            const std::string &d = p->value;
            if (d.find(' ') == std::string::npos) out.text = d + " 00:00:00";
            else out.text = d;
            return out;
        }
        if (const auto *p = std::get_if<Time>(&v)) {
            out.text = std::string("1970-01-01 ") + p->value;
            return out;
        }
        if (const auto *p = std::get_if<Timestamp>(&v)) {
            out.text = oracleFormatTimestamp(*p);
            return out;
        }
        if (const auto *p = std::get_if<Uuid>(&v)) {
            out.text = p->value;
            return out;
        }
        if (const auto *p = std::get_if<Json>(&v)) {
            out.text = p->value;
            return out;
        }
        if (const auto *p = std::get_if<IntervalYearMonth>(&v)) {
            out.text = p->value;
            return out;
        }
        if (const auto *p = std::get_if<IntervalDaySecond>(&v)) {
            out.text = p->value;
            return out;
        }
        if (const auto *p = std::get_if<Blob>(&v)) {
            out.raw = *p;
            return out;
        }
        out.unsupported = true;
        return out;
    }

    std::vector<std::string> oracleSessionSetupStatements() {
        return {
            "ALTER SESSION SET NLS_DATE_FORMAT = 'YYYY-MM-DD HH24:MI:SS'",
            "ALTER SESSION SET NLS_TIMESTAMP_FORMAT = 'YYYY-MM-DD HH24:MI:SS.FF'",
            "ALTER SESSION SET NLS_TIMESTAMP_TZ_FORMAT = 'YYYY-MM-DD HH24:MI:SS.FF TZH:TZM'",
            "ALTER SESSION SET NLS_NUMERIC_CHARACTERS = '.,'"
        };
    }

    Status oracleBuildConnectDescriptor(const OracleConnectOptions &options, std::string &out) {
        out.clear();
        if (!validDescriptorAtom(options.host))
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle host contains characters unsafe for a connect descriptor");
        if (options.port < 1 || options.port > 65535)
            return Status::error(ErrorCode::ConfigError, "Oracle port must be in range 1..65535");
        if (!options.serviceName.empty() && !options.sid.empty())
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle service_name and sid are mutually exclusive");
        if (options.serviceName.empty() && options.sid.empty())
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle service_name or sid is required");
        const std::string &target = options.sid.empty() ? options.serviceName : options.sid;
        if (!validDescriptorAtom(target))
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle service_name or sid contains unsafe characters");
        if (options.connectionTimeoutMs < 0)
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle connection timeout must be >= 0");
        if (!options.walletLocation.empty() &&
            !validQuotedDescriptorValue(options.walletLocation))
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle wallet_location contains unsafe characters");
        if (!options.serverCertDn.empty() &&
            !validQuotedDescriptorValue(options.serverCertDn))
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle server_cert_dn contains unsafe characters");
        if (!options.tlsEnabled &&
            (!options.walletLocation.empty() || !options.serverCertDn.empty()))
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle wallet_location and server_cert_dn require TLS");
        if (!options.serverCertDn.empty() && !options.tlsVerifyPeer)
            return Status::error(ErrorCode::ConfigError,
                                 "Oracle server_cert_dn requires TLS peer verification");

        out = "(DESCRIPTION=";
        if (options.connectionTimeoutMs > 0) {
            const std::string timeout = std::to_string(options.connectionTimeoutMs) + "ms";
            out += "(CONNECT_TIMEOUT=" + timeout + ")";
            out += "(TRANSPORT_CONNECT_TIMEOUT=" + timeout + ")";
        }
        out += "(ADDRESS=(PROTOCOL=";
        out += options.tlsEnabled ? "TCPS" : "TCP";
        out += ")(HOST=" + options.host + ")(PORT=" + std::to_string(options.port) + "))";
        out += "(CONNECT_DATA=(";
        out += options.sid.empty() ? "SERVICE_NAME=" : "SID=";
        out += target + "))";
        if (options.tlsEnabled) {
            out += "(SECURITY=(SSL_SERVER_DN_MATCH=";
            out += options.tlsVerifyPeer ? "YES" : "NO";
            out += ")";
            if (!options.serverCertDn.empty())
                out += "(SSL_SERVER_CERT_DN=\"" + options.serverCertDn + "\")";
            if (!options.walletLocation.empty())
                out += "(WALLET_LOCATION=\"" + options.walletLocation + "\")";
            out += ")";
        }
        out += ")";
        return Status::OK();
    }

    bool oracleParseReturningInto(const std::string &sql, OracleReturning &out) {
        out = OracleReturning{};
        const std::size_t n = sql.size();
        std::size_t keyword = std::string::npos;
        std::size_t i = 0;
        while (i < n) {
            const char c = sql[i];
            if (c == '\'') {
                ++i;
                while (i < n) {
                    if (sql[i] == '\'') {
                        if (i + 1 < n && sql[i + 1] == '\'') {
                            i += 2;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (c == '"') {
                ++i;
                while (i < n) {
                    if (sql[i] == '"') {
                        if (i + 1 < n && sql[i + 1] == '"') {
                            i += 2;
                            continue;
                        }
                        ++i;
                        break;
                    }
                    ++i;
                }
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c))) {
                const std::size_t start = i;
                while (i < n && isWordChar(sql[i])) ++i;
                if (foldEquals(sql.substr(start, i - start), "RETURNING")) keyword = start;
                continue;
            }
            ++i;
        }
        if (keyword == std::string::npos) return false;

        std::size_t p = keyword + kReturningKeywordLength;
        std::vector<std::string> columns;
        while (p < n) {
            skipSpaces(sql, p);
            if (p >= n) return false;
            std::string name;
            if (sql[p] == '"') {
                ++p;
                while (p < n) {
                    if (sql[p] == '"') {
                        if (p + 1 < n && sql[p + 1] == '"') {
                            name.push_back('"');
                            p += 2;
                            continue;
                        }
                        ++p;
                        break;
                    }
                    name.push_back(sql[p]);
                    ++p;
                }
            } else {
                if (!isWordChar(sql[p])) return false;
                while (p < n && isWordChar(sql[p])) name.push_back(sql[p++]);
                for (char &ch: name) ch = static_cast<char>(upperChar(ch));
            }
            columns.push_back(name);
            skipSpaces(sql, p);
            if (p < n && sql[p] == ',') {
                ++p;
                continue;
            }
            break;
        }
        if (columns.empty()) return false;

        skipSpaces(sql, p);
        std::size_t wordStart = p;
        while (p < n && isWordChar(sql[p])) ++p;
        if (!foldEquals(sql.substr(wordStart, p - wordStart), "INTO")) return false;

        std::vector<std::size_t> binds;
        while (p < n) {
            skipSpaces(sql, p);
            if (p >= n) break;
            if (sql[p] == ',') {
                ++p;
                continue;
            }
            if (sql[p] != ':') return false;
            ++p;
            std::size_t digits = 0;
            while (p < n && std::isdigit(static_cast<unsigned char>(sql[p]))) {
                digits = digits * 10 + static_cast<std::size_t>(sql[p] - '0');
                ++p;
            }
            binds.push_back(digits);
        }
        if (binds.size() != columns.size()) return false;

        out.present = true;
        out.columns = std::move(columns);
        out.bindCount = binds.size();
        out.firstBind = binds.front();
        return true;
    }

    std::string oracleMakeReturningSuffix(const std::vector<std::string> &columns,
                                          const std::size_t firstBind) {
        if (columns.empty()) return std::string();
        std::string s = " RETURNING ";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i) s += ", ";
            s += quoteIdentifier(columns[i]);
        }
        s += " INTO ";
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (i) s += ", ";
            s += ":" + std::to_string(firstBind + i);
        }
        return s;
    }

    std::string oracleSqlState(const int oraCode) {
        const int code = oraCode < 0 ? -oraCode : oraCode;
        switch (code) {
            case 1: return "23000";
            case 54: return "40001";
            case 60: return "40001";
            case 903: return "42000";
            case 904: return "42S22";
            case 911: return "42000";
            case 923: return "42000";
            case 933: return "42000";
            case 942: return "42S02";
            case 955: return "42710";
            case 1013: return "57014";
            case 1017: return "28000";
            case 1400: return "23502";
            case 1401: return "22001";
            case 1408: return "42701";
            case 1438: return "22003";
            case 1461: return "22001";
            case 1722: return "22018";
            case 1830: return "22008";
            case 1843: return "22007";
            case 1858: return "22007";
            case 2290: return "23514";
            case 2291: return "23503";
            case 2292: return "23503";
            case 3113: return "08S01";
            case 3114: return "08S01";
            case 3135: return "08S01";
            case 8177: return "40001";
            case 12162: return "08001";
            case 12170: return "08001";
            case 12514: return "08001";
            case 12537: return "08S01";
            case 12541: return "08001";
            case 12899: return "22001";
            case 22835: return "22001";
            default: return std::string();
        }
    }
}
