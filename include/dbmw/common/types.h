#ifndef DBMW_COMMON_TYPES_H
#define DBMW_COMMON_TYPES_H

#include <chrono>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dbmw::common {
    using Timestamp = std::chrono::system_clock::time_point;

    using Blob = std::vector<std::uint8_t>;

    struct Decimal {
        std::string value;
        bool operator==(const Decimal &other) const { return value == other.value; }
    };
    struct Date {
        std::string value;
        bool operator==(const Date &other) const { return value == other.value; }
    };
    struct Time {
        std::string value;
        bool operator==(const Time &other) const { return value == other.value; }
    };
    struct Uuid {
        std::string value;
        bool operator==(const Uuid &other) const { return value == other.value; }
    };
    struct Json {
        std::string value;
        bool operator==(const Json &other) const { return value == other.value; }
    };

    using Value = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
                               Decimal, std::string, Date, Time, Timestamp, Uuid, Json, Blob>;

    class Row {
    public:
        using Map = std::map<std::string, Value>;

        void set(const std::string &col, Value v) { data_[col] = std::move(v); }
        [[nodiscard]] bool has(const std::string &col) const { return data_.find(col) != data_.end(); }

        [[nodiscard]] const Value &at(const std::string &col) const {
            static const Value kNull{nullptr};
            auto it = data_.find(col);
            return it == data_.end() ? kNull : it->second;
        }

        [[nodiscard]] const Map &data() const { return data_; }
        [[nodiscard]] size_t size() const { return data_.size(); }

    private:
        Map data_;
    };

    class ResultSet {
    public:
        void setFields(std::vector<std::string> fields) { fields_ = std::move(fields); }

        [[nodiscard]] const std::vector<std::string> &fields() const { return fields_; }

        void addRow(Row row) { rows_.push_back(std::move(row)); }
        [[nodiscard]] const std::vector<Row> &rows() const { return rows_; }
        [[nodiscard]] std::vector<Row> &mutableRows() { return rows_; }
        [[nodiscard]] size_t rowCount() const { return rows_.size(); }
        [[nodiscard]] bool empty() const { return rows_.empty(); }
        void clear() {
            fields_.clear();
            rows_.clear();
            transformed = false;
        }

        bool transformed = false;

    private:
        std::vector<std::string> fields_;
        std::vector<Row> rows_;
    };

    using Params = std::vector<Value>;
    using ParamBatch = std::vector<Params>;
    using RowCallback = std::function<bool(const Row &)>;

    struct SqlRenderOptions {
        bool includeStringValues = false;
        bool includeBlobValues = false;
        std::size_t maxParamLength = 256;
        std::size_t maxSqlLength = 8192;
    };

    struct GeneratedKeys {
        ResultSet rows;

        [[nodiscard]] bool empty() const { return rows.empty(); }

        void clear() { rows.clear(); }

        [[nodiscard]] std::int64_t lastInsertId() const;
    };

    struct BatchResult {
        std::vector<std::int64_t> affected;
        // 与 affected 一一对应的每批生成键。只有驱动/基类批量实现能拿到才填
        // （PG 靠 SQL 自带 RETURNING，MySQL 靠 mysql_insert_id 合成）；
        // 拿不到时保持为空，调用方按“该批无生成键”处理。
        std::vector<GeneratedKeys> keys;

        [[nodiscard]] std::int64_t totalAffected() const {
            std::int64_t total = 0;
            for (const auto rows: affected) total += rows;
            return total;
        }
        void clear() { affected.clear(); keys.clear(); }
    };

    class StreamSource {
    public:
        using ReadFn = std::function<std::size_t(void *buf, std::size_t n)>;

        explicit StreamSource(ReadFn read, const std::optional<std::uint64_t> totalSize = std::nullopt,
                              const bool isBinary = true)
            : read_(std::move(read)), totalSize_(totalSize), isBinary_(isBinary) {}

        explicit StreamSource(std::istream &in, bool isBinary = true);

        std::size_t read(void *buf, std::size_t n) {
            if (!read_ || n == 0) return 0;
            return read_(buf, n);
        }

        [[nodiscard]] std::optional<std::uint64_t> totalSize() const { return totalSize_; }

        [[nodiscard]] bool isBinary() const { return isBinary_; }

    private:
        ReadFn read_;
        std::optional<std::uint64_t> totalSize_;
        bool isBinary_ = true;
    };

    using StreamParam = std::variant<Value, StreamSource>;
    using StreamParams = std::vector<StreamParam>;
    using StreamParamBatch = std::vector<StreamParams>;

    enum class IsolationLevel {
        Default,
        ReadUncommitted,
        ReadCommitted,
        RepeatableRead,
        Serializable
    };

    struct TransactionOptions {
        IsolationLevel isolation = IsolationLevel::Default;
        bool readOnly = false;
        std::chrono::milliseconds timeout{0};
    };

    enum class ErrorCode {
        Ok = 0,
        ConfigError,
        ConnectionFailed,
        QueryError,
        QueryTimeout,
        Cancelled,
        ConstraintViolation,
        Deadlock,
        PingFailed,
        TxError,
        PoolExhausted,
        PoolClosed,
        CircuitOpen,
        NotConnected,
        DriverDisabled,
        UnknownDriver,
        NotSupported,
        RateLimited,
        SqlBlocked,
        Buffered,
        CursorClosed,
        CursorLimit,
        CursorError,
        Unknown,
        Overloaded,
        MappingError,
        IoError
    };

    const char *errorCodeToString(ErrorCode c);

    struct Status {
        ErrorCode code = ErrorCode::Ok;
        std::string message;
        std::string sqlState;
        std::int64_t nativeCode = 0;
        bool retryable = false;
        bool connectionBroken = false;

        [[nodiscard]] bool ok() const { return code == ErrorCode::Ok; }

        static Status OK() { return Status{}; }

        static Status error(ErrorCode c, std::string msg) {
            Status status;
            status.code = c;
            status.message = std::move(msg);
            return status;
        }

        static Status databaseError(ErrorCode fallback, std::string msg,
                                    std::string state, std::int64_t vendorCode = 0);
    };

    std::string valueToString(const Value &v);

    std::string timestampToString(const Timestamp &t);

    std::string timestampToStringMs(const Timestamp &t);

    std::string timestampToUtcStringMs(const Timestamp &t);

    bool tryParseTimestamp(const std::string &s, Timestamp &out);

    std::string escapeLiteralGeneric(const Value &v);

    std::string quoteIdentifier(const std::string &ident);

    Status streamParamsToParams(const StreamParams &params, Params &out);

    std::string paramTypeSignature(const Params &params);
}

#endif
