#ifndef DBMW_COMMON_ORACLE_TYPES_H
#define DBMW_COMMON_ORACLE_TYPES_H

#include "dbmw/common/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dbmw::common {
    enum class OracleTypeClass {
        Unknown,
        Text,
        Number,
        Binary,
        Date,
        Timestamp,
        TimestampTz,
        Interval,
        Lob,
        Rowid
    };

    static constexpr std::uint16_t kSqltChr = 1;
    static constexpr std::uint16_t kSqltNum = 2;
    static constexpr std::uint16_t kSqltInt = 3;
    static constexpr std::uint16_t kSqltFlt = 4;
    static constexpr std::uint16_t kSqltStr = 5;
    static constexpr std::uint16_t kSqltVnu = 6;
    static constexpr std::uint16_t kSqltPdn = 7;
    static constexpr std::uint16_t kSqltLng = 8;
    static constexpr std::uint16_t kSqltVcs = 9;
    static constexpr std::uint16_t kSqltRid = 11;
    static constexpr std::uint16_t kSqltDat = 12;
    static constexpr std::uint16_t kSqltVbi = 15;
    static constexpr std::uint16_t kSqltBfloat = 21;
    static constexpr std::uint16_t kSqltBdouble = 22;
    static constexpr std::uint16_t kSqltBin = 23;
    static constexpr std::uint16_t kSqltLbi = 24;
    static constexpr std::uint16_t kSqltUin = 68;
    static constexpr std::uint16_t kSqltSls = 91;
    static constexpr std::uint16_t kSqltAfc = 96;
    static constexpr std::uint16_t kSqltIbfloat = 100;
    static constexpr std::uint16_t kSqltIbdouble = 101;
    static constexpr std::uint16_t kSqltCur = 102;
    static constexpr std::uint16_t kSqltNty = 108;
    static constexpr std::uint16_t kSqltRef = 110;
    static constexpr std::uint16_t kSqltClob = 112;
    static constexpr std::uint16_t kSqltBlob = 113;
    static constexpr std::uint16_t kSqltBfile = 114;
    static constexpr std::uint16_t kSqltCfile = 115;
    static constexpr std::uint16_t kSqltRdd = 116;
    static constexpr std::uint16_t kSqltTimestamp = 187;
    static constexpr std::uint16_t kSqltTimestampTz = 188;
    static constexpr std::uint16_t kSqltIntervalYm = 189;
    static constexpr std::uint16_t kSqltIntervalDs = 190;
    static constexpr std::uint16_t kSqltTimestampLtz = 232;

    OracleTypeClass oracleTypeClass(std::uint16_t sqlt);

    const char *oracleTypeName(std::uint16_t sqlt);

    bool oracleIsLob(std::uint16_t sqlt);

    bool oracleIsRowid(std::uint16_t sqlt);

    Value oracleValueFromText(std::uint16_t sqlt, const std::string &text,
                              std::int32_t precision = 0, std::int32_t scale = 0);

    struct OracleBindValue {
        std::optional<std::string> text;
        std::optional<Blob> raw;
        bool unsupported = false;

        [[nodiscard]] bool isNull() const {
            return !unsupported && !text.has_value() && !raw.has_value();
        }
    };

    OracleBindValue oracleBindValue(const Value &v);

    std::string oracleFormatDouble(double v);

    bool oracleParseTimestamp(const std::string &text, Timestamp &out);

    std::string oracleFormatTimestamp(const Timestamp &t);

    std::vector<std::string> oracleSessionSetupStatements();

    struct OracleReturning {
        bool present = false;
        std::vector<std::string> columns;
        std::size_t firstBind = 0;
        std::size_t bindCount = 0;
    };

    bool oracleParseReturningInto(const std::string &sql, OracleReturning &out);

    std::string oracleMakeReturningSuffix(const std::vector<std::string> &columns,
                                          std::size_t firstBind);

    std::string oracleSqlState(int oraCode);
}

#endif
