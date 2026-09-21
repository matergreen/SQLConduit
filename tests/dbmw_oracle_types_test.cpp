#include "dbmw/common/oracle_types.h"
#include "dbmw/common/types.h"
#include "dbmw/mapping.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace dbmw;

static int g_failed = 0;
static int g_passed = 0;

static void check(const bool cond, const std::string &name) {
    if (cond) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

struct OraRow {
    std::int64_t id = 0;
    std::string name;
    std::int64_t amount = 0;
};

namespace dbmw::mapping {
    template<>
    struct RowMapper<OraRow> {
        static Mapping<OraRow> describe() {
            return Mapping<OraRow>()
                    .field(&OraRow::id, "id", FieldFlags::PrimaryKey | FieldFlags::Generated)
                    .field(&OraRow::name, "name")
                    .field(&OraRow::amount, "amount");
        }
    };
}

int main() {
    std::cout << "== Oracle 连接描述符 ==\n";
    {
        common::OracleConnectOptions options;
        options.host = "db.internal";
        options.port = 1522;
        options.serviceName = "APP_PDB";
        options.connectionTimeoutMs = 2500;
        std::string descriptor;
        const auto plain = common::oracleBuildConnectDescriptor(options, descriptor);
        check(plain.ok() &&
              descriptor.find("(CONNECT_TIMEOUT=2500ms)") != std::string::npos &&
              descriptor.find("(TRANSPORT_CONNECT_TIMEOUT=2500ms)") != std::string::npos &&
              descriptor.find("(PROTOCOL=TCP)") != std::string::npos &&
              descriptor.find("(SERVICE_NAME=APP_PDB)") != std::string::npos,
              "service name、TCP 与登录前超时写入描述符");

        options.serviceName.clear();
        options.sid = "ORCL";
        options.tlsEnabled = true;
        options.walletLocation = "/opt/oracle/wallet";
        options.serverCertDn = "CN=db.example.com,O=Example";
        const auto tls = common::oracleBuildConnectDescriptor(options, descriptor);
        check(tls.ok() && descriptor.find("(PROTOCOL=TCPS)") != std::string::npos &&
              descriptor.find("(SID=ORCL)") != std::string::npos &&
              descriptor.find("(SSL_SERVER_DN_MATCH=YES)") != std::string::npos &&
              descriptor.find("(WALLET_LOCATION=\"/opt/oracle/wallet\")") !=
                  std::string::npos &&
              descriptor.find("(SSL_SERVER_CERT_DN=\"CN=db.example.com,O=Example\")") !=
                  std::string::npos,
              "SID、TCPS、wallet 与服务端证书 DN 组成明确 TLS 语义");

        options.serviceName = "PDB";
        check(!common::oracleBuildConnectDescriptor(options, descriptor).ok(),
              "service_name 与 sid 同时存在时拒绝歧义配置");
        options.serviceName.clear();
        options.tlsVerifyPeer = false;
        check(!common::oracleBuildConnectDescriptor(options, descriptor).ok(),
              "关闭 peer verification 时拒绝无效的 server_cert_dn");
        options.serverCertDn.clear();
        check(common::oracleBuildConnectDescriptor(options, descriptor).ok() &&
              descriptor.find("(SSL_SERVER_DN_MATCH=NO)") != std::string::npos,
              "显式关闭 peer verification 会写入 DN_MATCH=NO");
    }

    std::cout << "== Oracle 类型分类 ==\n";
    {
        check(common::oracleTypeClass(common::kSqltNum) == common::OracleTypeClass::Number,
              "SQLT_NUM 归类为 Number");
        check(common::oracleTypeClass(common::kSqltChr) == common::OracleTypeClass::Text,
              "SQLT_CHR 归类为 Text");
        check(common::oracleTypeClass(common::kSqltDat) == common::OracleTypeClass::Date,
              "SQLT_DAT 归类为 Date");
        check(common::oracleTypeClass(common::kSqltTimestamp) == common::OracleTypeClass::Timestamp,
              "SQLT_TIMESTAMP 归类为 Timestamp");
        check(common::oracleTypeClass(common::kSqltTimestampTz) ==
              common::OracleTypeClass::TimestampTz, "SQLT_TIMESTAMP_TZ 归类为 TimestampTz");
        check(common::oracleTypeClass(common::kSqltIntervalDs) ==
              common::OracleTypeClass::Interval, "SQLT_INTERVAL_DS 归类为 Interval");
        check(common::oracleTypeClass(common::kSqltBin) == common::OracleTypeClass::Binary,
              "SQLT_BIN 归类为 Binary");
        check(common::oracleTypeClass(common::kSqltClob) == common::OracleTypeClass::Lob,
              "SQLT_CLOB 归类为 Lob");
        check(common::oracleTypeClass(common::kSqltRid) == common::OracleTypeClass::Rowid,
              "SQLT_RID 归类为 Rowid");
        check(std::string(common::oracleTypeName(common::kSqltTimestampTz)) ==
              "TIMESTAMP WITH TIME ZONE", "SQLT_TIMESTAMP_TZ 的名称");
        check(common::oracleIsLob(common::kSqltBlob) && !common::oracleIsLob(common::kSqltClob +
              1000), "oracleIsLob 只认 LOB 类型码");
    }

    std::cout << "== NUMBER 文本解析 ==\n";
    {
        const auto v = common::oracleValueFromText(common::kSqltNum, "42", 9, 0);
        check(std::holds_alternative<std::int64_t>(v) && std::get<std::int64_t>(v) == 42,
              "NUMBER(9,0) '42' 解析为 int64 42");

        const auto big = common::oracleValueFromText(common::kSqltNum, "9223372036854775808", 38,
                                                     0);
        check(std::holds_alternative<std::uint64_t>(big) &&
              std::get<std::uint64_t>(big) == 9223372036854775808ULL,
              "NUMBER(38,0) 超过 int64 的值解析为 uint64");

        const auto dec = common::oracleValueFromText(common::kSqltNum, "3.14", 9, 2);
        check(std::holds_alternative<common::Decimal>(dec) &&
              std::get<common::Decimal>(dec).value == "3.14",
              "NUMBER(9,2) '3.14' 解析为 Decimal 保留精度");

        const auto flt = common::oracleValueFromText(common::kSqltBdouble, "1.5", 0, 0);
        check(std::holds_alternative<double>(flt) && std::get<double>(flt) == 1.5,
              "BINARY_DOUBLE '1.5' 解析为 double");
    }

    std::cout << "== 日期与时间戳 ==\n";
    {
        const auto d = common::oracleValueFromText(common::kSqltDat, "2020-01-02 03:04:05", 0, 0);
        check(std::holds_alternative<common::Timestamp>(d),
              "Oracle DATE 文本解析为 Timestamp（DATE 带时分秒）");
        if (const auto *ts = std::get_if<common::Timestamp>(&d))
            check(common::timestampToString(*ts) == "2020-01-02 03:04:05",
                  "DATE 往返保持 2020-01-02 03:04:05");

        const auto tz = common::oracleValueFromText(common::kSqltTimestampTz,
                                                    "2020-01-02 03:04:05.123 +08:00", 0, 0);
        check(std::holds_alternative<common::Timestamp>(tz), "TIMESTAMP WITH TIME ZONE 可解析");
        if (const auto *ts = std::get_if<common::Timestamp>(&tz))
            check(common::timestampToUtcStringMs(*ts) == "2020-01-01 19:04:05.123+00",
                  "带 +08:00 偏移的时间戳折算为 UTC 瞬间");

        common::Timestamp parsed{};
        check(common::oracleParseTimestamp("2026-09-21 10:20:30", parsed),
              "oracleParseTimestamp 接受无偏移时间");
        check(!common::oracleParseTimestamp("not-a-date", parsed),
              "oracleParseTimestamp 拒绝非法文本");
    }

    std::cout << "== RAW 与 INTERVAL ==\n";
    {
        const auto raw = common::oracleValueFromText(common::kSqltBin, "0A1B2C", 0, 0);
        const auto *blob = std::get_if<common::Blob>(&raw);
        check(blob != nullptr && blob->size() == 3 && (*blob)[0] == 0x0A && (*blob)[2] == 0x2C,
              "RAW 十六进制文本解析为 Blob");

        const auto iv = common::oracleValueFromText(common::kSqltIntervalDs, "+01 02:03:04.000000",
                                                    0, 0);
        const auto *daySecond = std::get_if<common::IntervalDaySecond>(&iv);
        check(daySecond != nullptr && daySecond->value == "+01 02:03:04.000000",
              "INTERVAL DAY TO SECOND 保留强类型");

        const auto ym = common::oracleValueFromText(common::kSqltIntervalYm, "+03-02", 0, 0);
        const auto *yearMonth = std::get_if<common::IntervalYearMonth>(&ym);
        check(yearMonth != nullptr && yearMonth->value == "+03-02",
              "INTERVAL YEAR TO MONTH 保留强类型");
    }

    std::cout << "== 参数绑定 ==\n";
    {
        const auto nullBind = common::oracleBindValue(common::Value{nullptr});
        check(nullBind.isNull() && !nullBind.unsupported, "nullptr 绑定为 SQL NULL");

        const auto boolBind = common::oracleBindValue(common::Value{true});
        check(boolBind.text && *boolBind.text == "1", "bool true 绑定为 '1'（Oracle 无布尔类型）");

        const auto intBind = common::oracleBindValue(common::Value{std::int64_t(-7)});
        check(intBind.text && *intBind.text == "-7", "int64 绑定为十进制文本");

        const auto dbl = common::oracleBindValue(common::Value{0.1});
        check(dbl.text && *dbl.text == "0.1", "double 0.1 走最短往返，不输出 0.10000000000000001");

        const auto dateBind = common::oracleBindValue(common::Value{common::Date{"2020-01-02"}});
        check(dateBind.text && *dateBind.text == "2020-01-02 00:00:00",
              "Date 补零时分秒后再绑定（NLS_DATE_FORMAT 固定）");

        const auto timeBind = common::oracleBindValue(common::Value{common::Time{"01:02:03"}});
        check(timeBind.text && *timeBind.text == "1970-01-01 01:02:03",
              "Time 绑定为 epoch 日期 + 时间（Oracle 无 TIME 类型）");

        const auto blob = common::oracleBindValue(common::Value{common::Blob{0x01, 0x02}});
        check(blob.raw.has_value() && blob.raw->size() == 2, "Blob 走 SQLT_BIN 原始绑定");

        const auto interval = common::oracleBindValue(
            common::Value{common::IntervalDaySecond{"+01 02:03:04"}});
        check(interval.text && *interval.text == "+01 02:03:04",
              "强类型 INTERVAL 可作为 Oracle 输入参数");

        common::Array arr;
        arr.items.push_back(common::Value{std::int64_t(1)});
        check(common::oracleBindValue(common::Value{arr}).unsupported,
              "Array 明确报 unsupported，不静默绑 NULL");
    }

    std::cout << "== RETURNING ... INTO 解析 ==\n";
    {
        common::OracleReturning r;
        check(common::oracleParseReturningInto(
                  R"(INSERT INTO "t" ("a", "b") VALUES (:1, :2) RETURNING "id" INTO :3)", r) &&
              r.present && r.columns.size() == 1 && r.columns[0] == "id" && r.firstBind == 3 &&
              r.bindCount == 1, "单列 RETURNING ... INTO :3");

        check(common::oracleParseReturningInto(
                  R"(INSERT INTO "t" VALUES (:1) RETURNING "id", "ts" INTO :2, :3)", r) &&
              r.columns.size() == 2 && r.firstBind == 2 && r.bindCount == 2,
              "多列 RETURNING 与 INTO 一一对应");

        check(common::oracleParseReturningInto("INSERT INTO t VALUES (:1) RETURNING id INTO :2",
                                               r) &&
              r.columns.size() == 1 && r.columns[0] == "ID",
              "未加引号的列名按 Oracle 语义折叠为大写");

        check(!common::oracleParseReturningInto("INSERT INTO t VALUES (:1)", r),
              "没有 RETURNING 子句时返回 false");

        check(!common::oracleParseReturningInto("INSERT INTO t VALUES (:1) RETURNING id", r),
              "只有 RETURNING 没有 INTO 视为非法");

        check(common::oracleParseReturningInto(
                  R"(INSERT INTO t VALUES ('RETURNING fake') RETURNING "id" INTO :2)", r) &&
              r.columns.size() == 1 && r.columns[0] == "id",
              "字符串字面量里的 RETURNING 关键字被跳过");

        check(common::oracleMakeReturningSuffix({"id", "ts"}, 3) ==
              " RETURNING \"id\", \"ts\" INTO :3, :4", "生成 RETURNING 后缀");
    }

    std::cout << "== 会话初始化 ==\n";
    {
        const auto stmts = common::oracleSessionSetupStatements();
        check(stmts.size() == 4, "连接后固定执行 4 条 ALTER SESSION");
        bool hasDate = false;
        bool hasNumeric = false;
        for (const auto &s: stmts) {
            if (s.find("NLS_DATE_FORMAT") != std::string::npos) hasDate = true;
            if (s.find("NLS_NUMERIC_CHARACTERS") != std::string::npos) hasNumeric = true;
        }
        check(hasDate && hasNumeric, "固定 NLS_DATE_FORMAT 与 NLS_NUMERIC_CHARACTERS");
    }

    std::cout << "== mapping：Oracle 生成键回写 ==\n";
    {
        using dbmw::common::util::Dialect;
        const std::string base = mapping::insertSql<OraRow>("t", Dialect::Oracle);
        std::size_t placeholders = 0;
        for (const char c: base)
            if (c == '?') ++placeholders;
        const std::string expected = base + " RETURNING \"id\" INTO :" +
            std::to_string(placeholders + 1);
        check(mapping::insertSqlReturning<OraRow>("t", Dialect::Oracle) == expected,
              "Oracle 方言追加 RETURNING \"id\" INTO :<参数个数+1>");
        check(mapping::insertSqlReturning<OraRow>("t", Dialect::Postgres) ==
              base + " RETURNING \"id\"", "Postgres 方言仍是无 INTO 的 RETURNING");
        const std::string myBase = mapping::insertSql<OraRow>("t", Dialect::MySQL);
        check(mapping::insertSqlReturning<OraRow>("t", Dialect::MySQL) == myBase,
              "MySQL 方言不追加任何 RETURNING");
    }

    std::cout << "== ORA -> SQLSTATE 映射 ==\n";
    {
        using dbmw::common::oracleSqlState;
        check(oracleSqlState(1) == "23000", "ORA-00001 唯一约束 -> 23000");
        check(oracleSqlState(1400) == "23502", "ORA-01400 NOT NULL -> 23502");
        check(oracleSqlState(2291) == "23503", "ORA-02291 外键 -> 23503");
        check(oracleSqlState(2290) == "23514", "ORA-02290 检查约束 -> 23514");
        check(oracleSqlState(60) == "40001", "ORA-00060 死锁 -> 40001");
        check(oracleSqlState(8177) == "40001", "ORA-08177 串行化冲突 -> 40001");
        check(oracleSqlState(54) == "40001", "ORA-00054 资源忙 -> 40001");
        check(oracleSqlState(1013) == "57014", "ORA-01013 取消 -> 57014");
        check(oracleSqlState(942) == "42S02", "ORA-00942 表不存在 -> 42S02");
        check(oracleSqlState(904) == "42S22", "ORA-00904 无效列 -> 42S22");
        check(oracleSqlState(933) == "42000", "ORA-00933 语法错误 -> 42000");
        check(oracleSqlState(1017) == "28000", "ORA-01017 认证失败 -> 28000");
        check(oracleSqlState(3113) == "08S01", "ORA-03113 连接断开 -> 08S01");
        check(oracleSqlState(12541) == "08001", "ORA-12541 无监听 -> 08001");
        check(oracleSqlState(-1) == "23000", "负值 ORA 码按绝对值归一");
        check(oracleSqlState(99999).empty(), "未收录的 ORA 码返回空，不臆造 SQLSTATE");

        const auto dup = dbmw::common::Status::databaseError(
            dbmw::common::ErrorCode::QueryError, "ORA-00001", oracleSqlState(1), 1);
        check(dup.code == dbmw::common::ErrorCode::ConstraintViolation,
              "ORA-00001 经 databaseError 归类为 ConstraintViolation");
        const auto dead = dbmw::common::Status::databaseError(
            dbmw::common::ErrorCode::QueryError, "ORA-00060", oracleSqlState(60), 60);
        check(dead.code == dbmw::common::ErrorCode::Deadlock && dead.retryable,
              "ORA-00060 归类为可重试的 Deadlock");
        const auto lost = dbmw::common::Status::databaseError(
            dbmw::common::ErrorCode::QueryError, "ORA-03113", oracleSqlState(3113), 3113);
        check(lost.connectionBroken, "ORA-03113 标记为 connectionBroken");
    }

    std::cout << "\n通过 " << g_passed << " 项，失败 " << g_failed << " 项\n";
    return g_failed == 0 ? 0 : 1;
}
