#ifndef SQLCONDUIT_COMMON_TYPES_H
#define SQLCONDUIT_COMMON_TYPES_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sqlconduit::common {
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

    struct IntervalYearMonth {
        std::string value;
        bool operator==(const IntervalYearMonth &other) const { return value == other.value; }
    };

    struct IntervalDaySecond {
        std::string value;
        bool operator==(const IntervalDaySecond &other) const { return value == other.value; }
    };

    struct Value;

    struct Array {
        std::vector<Value> items;
    };

    struct Composite {
        std::vector<std::pair<std::string, Value> > fields;

        [[nodiscard]] const Value *find(const std::string &name) const;
    };

    // Database named collection/object values. Unlike Array and Composite, these
    // carry the database type name required by drivers such as Oracle OCI.
    struct TypedArray {
        std::string typeName;
        std::vector<Value> items;
    };

    struct TypedComposite {
        std::string typeName;
        std::vector<std::pair<std::string, Value> > fields;

        [[nodiscard]] const Value *find(const std::string &name) const;
    };

    bool operator==(const Array &a, const Array &b);

    bool operator!=(const Array &a, const Array &b);

    bool operator==(const Composite &a, const Composite &b);

    bool operator!=(const Composite &a, const Composite &b);

    bool operator==(const TypedArray &a, const TypedArray &b);

    bool operator!=(const TypedArray &a, const TypedArray &b);

    bool operator==(const TypedComposite &a, const TypedComposite &b);

    bool operator!=(const TypedComposite &a, const TypedComposite &b);

    using ValueBase = std::variant<std::nullptr_t, bool, std::int64_t, std::uint64_t, double,
        Decimal, std::string, Date, Time, Timestamp, Uuid, Json, Blob,
        IntervalYearMonth, IntervalDaySecond, Array, Composite, TypedArray, TypedComposite>;

    struct Value : ValueBase {
        using ValueBase::ValueBase;

        Value() = default;
    };

    inline bool operator==(const Array &a, const Array &b) { return a.items == b.items; }
    inline bool operator!=(const Array &a, const Array &b) { return !(a == b); }
    inline bool operator==(const Composite &a, const Composite &b) { return a.fields == b.fields; }
    inline bool operator!=(const Composite &a, const Composite &b) { return !(a == b); }

    inline bool operator==(const TypedArray &a, const TypedArray &b) {
        return a.typeName == b.typeName && a.items == b.items;
    }

    inline bool operator!=(const TypedArray &a, const TypedArray &b) { return !(a == b); }

    inline bool operator==(const TypedComposite &a, const TypedComposite &b) {
        return a.typeName == b.typeName && a.fields == b.fields;
    }

    inline bool operator!=(const TypedComposite &a, const TypedComposite &b) { return !(a == b); }

    template<class Visitor>
    decltype(auto) visitValue(Visitor &&vis, Value &v) {
        return std::visit(std::forward<Visitor>(vis), static_cast<ValueBase &>(v));
    }

    template<class Visitor>
    decltype(auto) visitValue(Visitor &&vis, const Value &v) {
        return std::visit(std::forward<Visitor>(vis), static_cast<const ValueBase &>(v));
    }

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

    enum class ParamDirection { In = 0, Out = 1, InOut = 2 };

    enum class ValueType {
        Auto = 0,
        Bool = 1,
        Int64 = 2,
        UInt64 = 3,
        Double = 4,
        Decimal = 5,
        String = 6,
        Date = 7,
        Time = 8,
        Timestamp = 9,
        Uuid = 10,
        Json = 11,
        Blob = 12,
        IntervalYearMonth = 13,
        IntervalDaySecond = 14,
        TypedArray = 15,
        TypedComposite = 16,
        RefCursor = 17
    };

    struct CallParam {
        Value value{nullptr};
        ParamDirection direction = ParamDirection::In;
        ValueType type = ValueType::Auto;
        std::string typeName;
        std::size_t maxBytes = 4096;

        CallParam() = default;

        explicit CallParam(Value v) : value(std::move(v)) {
        }

        CallParam(ParamDirection d, Value v) : value(std::move(v)), direction(d) {
        }

        CallParam(ParamDirection d, ValueType t, Value v = Value{nullptr},
                  std::string databaseTypeName = {}, std::size_t outputMaxBytes = 4096)
            : value(std::move(v)), direction(d), type(t),
              typeName(std::move(databaseTypeName)), maxBytes(outputMaxBytes) {
        }

        static CallParam out(ValueType type, std::size_t maxBytes = 4096) {
            return CallParam{ParamDirection::Out, type, Value{nullptr}, {}, maxBytes};
        }

        static CallParam refCursor() {
            return CallParam{ParamDirection::Out, ValueType::RefCursor};
        }
    };

    using CallParams = std::vector<CallParam>;

    struct CallOutput {
        std::vector<ResultSet> sets;
        std::vector<Value> outParams;
        std::int64_t affected = 0;

        void clear() {
            sets.clear();
            outParams.clear();
            affected = 0;
        }
    };

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
        std::vector<GeneratedKeys> keys;

        [[nodiscard]] std::int64_t totalAffected() const {
            std::int64_t total = 0;
            for (const auto rows: affected) total += rows;
            return total;
        }

        void clear() {
            affected.clear();
            keys.clear();
        }
    };

    class StreamSource {
    public:
        using ReadFn = std::function<std::size_t(void *buf, std::size_t n)>;

        explicit StreamSource(ReadFn read, const std::optional<std::uint64_t> totalSize = std::nullopt,
                              const bool isBinary = true)
            : read_(std::move(read)), totalSize_(totalSize), isBinary_(isBinary) {
        }

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
        Default = 0,
        ReadUncommitted = 1,
        ReadCommitted = 2,
        RepeatableRead = 3,
        Serializable = 4
    };

    struct TransactionOptions {
        IsolationLevel isolation = IsolationLevel::Default;
        bool readOnly = false;
        std::chrono::milliseconds timeout{0};
    };

    enum class ErrorCode {
        Ok = 0,
        ConfigError = 1,
        ConnectionFailed = 2,
        QueryError = 3,
        QueryTimeout = 4,
        Cancelled = 5,
        ConstraintViolation = 6,
        Deadlock = 7,
        PingFailed = 8,
        TxError = 9,
        PoolExhausted = 10,
        PoolClosed = 11,
        CircuitOpen = 12,
        NotConnected = 13,
        DriverDisabled = 14,
        UnknownDriver = 15,
        NotSupported = 16,
        RateLimited = 17,
        SqlBlocked = 18,
        Buffered = 19,
        CursorClosed = 20,
        CursorLimit = 21,
        CursorError = 22,
        Unknown = 23,
        Overloaded = 24,
        MappingError = 25,
        IoError = 26,
        NotInitialized = 27,
        AlreadyInitialized = 28,
        ClientClosed = 29
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
