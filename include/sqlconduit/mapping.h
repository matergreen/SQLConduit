#ifndef SQLCONDUIT_MAPPING_H
#define SQLCONDUIT_MAPPING_H

#include "sqlconduit/common/types.h"
#include "sqlconduit/common/pg_types.h"
#include "sqlconduit/common/oracle_types.h"
#include "sqlconduit/core/cursor.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/client.h"
#include "sqlconduit/public.h"
#include "sqlconduit/sql_builder.h"
#include "sqlconduit/util.h"
#include "sqlconduit/version.h"

#include <any>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sqlconduit {
    template<class T>
    struct EntityResult {
        common::Status status;
        std::vector<T> items;
        async::ExecutionMode mode = async::ExecutionMode::Native;
        [[nodiscard]] bool ok() const { return status.ok(); }
        std::vector<T> &itemsOrThrow() & { common::throwIfError(status); return items; }
        const std::vector<T> &itemsOrThrow() const & { common::throwIfError(status); return items; }
        std::vector<T> itemsOrThrow() && { common::throwIfError(status); return std::move(items); }
    };

    template<class T>
    struct EntityOne {
        common::Status status;
        std::optional<T> value;
        async::ExecutionMode mode = async::ExecutionMode::Native;
        [[nodiscard]] bool ok() const { return status.ok(); }

        const T &valueOrThrow() const & {
            common::throwIfError(status);
            if (!value) throw common::StatusError(mappingMissing());
            return *value;
        }
        T valueOrThrow() && {
            common::throwIfError(status);
            if (!value) throw common::StatusError(mappingMissing());
            return std::move(*value);
        }
        common::Status mappingMissing() const {
            return common::Status::error(common::ErrorCode::MappingError, "query returned no row");
        }
    };

    template<class T>
    struct ScalarResult {
        common::Status status;
        std::optional<T> value;
        bool hasRow = false;
        bool isNull = false;
        async::ExecutionMode mode = async::ExecutionMode::Native;
        [[nodiscard]] bool ok() const { return status.ok(); }

        const T &valueOrThrow() const & {
            common::throwIfError(status);
            if (!value) throw common::StatusError(mappingMissing());
            return *value;
        }
        T valueOrThrow() && {
            common::throwIfError(status);
            if (!value) throw common::StatusError(mappingMissing());
            return std::move(*value);
        }
        common::Status mappingMissing() const {
            return common::Status::error(common::ErrorCode::MappingError, isNull ? "scalar is SQL NULL" : "query returned no row");
        }
    };

    template<class T>
    struct WriteResult {
        common::Status status;
        std::int64_t affected = 0;
        common::GeneratedKeys keys;
        [[nodiscard]] bool ok() const { return status.ok(); }
    };

    template<class T>
    struct BatchWriteResult {
        common::Status status;
        common::BatchResult batch;
        [[nodiscard]] bool ok() const { return status.ok(); }
    };
}

namespace sqlconduit::mapping {
    enum class FieldFlags : unsigned {
        None = 0,
        PrimaryKey = 1u << 0,
        Generated = 1u << 1,
        ReadOnly = 1u << 2,
        Lossy = 1u << 3,
        Textual = 1u << 4
    };

    constexpr FieldFlags operator|(const FieldFlags a, const FieldFlags b) noexcept {
        return static_cast<FieldFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
    }

    constexpr bool hasFlag(const FieldFlags v, const FieldFlags bit) noexcept {
        return (static_cast<unsigned>(v) & static_cast<unsigned>(bit)) != 0u;
    }

    enum class ExtraColumns { Ignore = 0, Error = 1 };

    enum class MissingColumns { Ignore = 0, Error = 1 };

    enum class WriteCols { Writable = 0, All = 1, PrimaryKey = 2 };

    namespace detail {
        template<class...>
        struct AlwaysFalse : std::false_type {
        };

        template<class T>
        struct IsOptional : std::false_type {
        };

        template<class T>
        struct IsOptional<std::optional<T> > : std::true_type {
        };
    }

    template<class U>
    struct TypeName {
        static std::string name() {
            if constexpr (std::is_same_v<U, bool>) return "bool";
            else if constexpr (std::is_same_v<U, std::int8_t>) return "int8_t";
            else if constexpr (std::is_same_v<U, std::int16_t>) return "int16_t";
            else if constexpr (std::is_same_v<U, std::int32_t>) return "int32_t";
            else if constexpr (std::is_same_v<U, std::int64_t>) return "int64_t";
            else if constexpr (std::is_same_v<U, std::uint8_t>) return "uint8_t";
            else if constexpr (std::is_same_v<U, std::uint16_t>) return "uint16_t";
            else if constexpr (std::is_same_v<U, std::uint32_t>) return "uint32_t";
            else if constexpr (std::is_same_v<U, std::uint64_t>) return "uint64_t";
            else if constexpr (std::is_same_v<U, float>) return "float";
            else if constexpr (std::is_same_v<U, double>) return "double";
            else if constexpr (std::is_same_v<U, std::string>) return "std::string";
            else if constexpr (std::is_same_v<U, common::Decimal>) return "Decimal";
            else if constexpr (std::is_same_v<U, common::Date>) return "Date";
            else if constexpr (std::is_same_v<U, common::Time>) return "Time";
            else if constexpr (std::is_same_v<U, common::Timestamp>) return "Timestamp";
            else if constexpr (std::is_same_v<U, common::Uuid>) return "Uuid";
            else if constexpr (std::is_same_v<U, common::Json>) return "Json";
            else if constexpr (std::is_same_v<U, common::IntervalYearMonth>) return "IntervalYearMonth";
            else if constexpr (std::is_same_v<U, common::IntervalDaySecond>) return "IntervalDaySecond";
            else if constexpr (std::is_same_v<U, common::Blob>) return "Blob";
            else if constexpr (std::is_same_v<U, common::Array>) return "Array";
            else if constexpr (std::is_same_v<U, common::Composite>) return "Composite";
            else if constexpr (std::is_same_v<U, common::TypedArray>) return "TypedArray";
            else if constexpr (std::is_same_v<U, common::TypedComposite>) return "TypedComposite";
            else if constexpr (std::is_same_v<U, common::PgPoint>) return "PgPoint";
            else if constexpr (std::is_same_v<U, common::PgLine>) return "PgLine";
            else if constexpr (std::is_same_v<U, common::PgLseg>) return "PgLseg";
            else if constexpr (std::is_same_v<U, common::PgBox>) return "PgBox";
            else if constexpr (std::is_same_v<U, common::PgPath>) return "PgPath";
            else if constexpr (std::is_same_v<U, common::PgPolygon>) return "PgPolygon";
            else if constexpr (std::is_same_v<U, common::PgCircle>) return "PgCircle";
            else if constexpr (detail::IsOptional<U>::value)
                return "std::optional<" + TypeName<typename U::value_type>::name() + ">";
            else if constexpr (std::is_enum_v<U>) return "enum";
            else return "custom-type";
        }
    };

    inline const char *valueTypeName(const common::Value &v) {
        switch (v.index()) {
            case 0: return "NULL";
            case 1: return "bool";
            case 2: return "int64";
            case 3: return "uint64";
            case 4: return "double";
            case 5: return "Decimal";
            case 6: return "string";
            case 7: return "Date";
            case 8: return "Time";
            case 9: return "Timestamp";
            case 10: return "Uuid";
            case 11: return "Json";
            case 12: return "Blob";
            case 13: return "Array";
            case 14: return "Composite";
            default: return "unknown";
        }
    }

    inline common::Status mapError(std::string msg) {
        return common::Status::error(common::ErrorCode::MappingError, std::move(msg));
    }

    inline common::Status typeError(const std::string &target, const common::Value &v) {
        return mapError("cannot convert " + std::string(valueTypeName(v)) + " to " + target);
    }

    inline bool tryParseIntegral(const std::string &s, std::int64_t &out) {
        if (s.empty()) return false;
        try {
            std::size_t pos = 0;
            const long long v = std::stoll(s, &pos);
            if (pos != s.size()) return false;
            out = static_cast<std::int64_t>(v);
            return true;
        } catch (...) { return false; }
    }

    inline bool tryParseReal(const std::string &s, double &out) {
        if (s.empty()) return false;
        try {
            std::size_t pos = 0;
            const double v = std::stod(s, &pos);
            if (pos != s.size()) return false;
            out = v;
            return true;
        } catch (...) { return false; }
    }

    template<class Dst, class Src>
    constexpr bool fitsIn(const Src s) {
        static_assert(std::is_integral_v<Src> && std::is_integral_v<Dst>);
        if constexpr (std::is_unsigned_v<Dst> && std::is_unsigned_v<Src>)
            return static_cast<std::uint64_t>(s) <=
                   static_cast<std::uint64_t>(std::numeric_limits<Dst>::max());
        else if constexpr (std::is_unsigned_v<Dst> && std::is_signed_v<Src>)
            return s >= 0 && static_cast<std::uint64_t>(s) <=
                   static_cast<std::uint64_t>(std::numeric_limits<Dst>::max());
        else if constexpr (std::is_signed_v<Dst> && std::is_unsigned_v<Src>)
            return static_cast<std::uint64_t>(s) <=
                   static_cast<std::uint64_t>(std::numeric_limits<Dst>::max());
        else
            return static_cast<std::int64_t>(s) >=
                   static_cast<std::int64_t>(std::numeric_limits<Dst>::min()) &&
                   static_cast<std::int64_t>(s) <=
                   static_cast<std::int64_t>(std::numeric_limits<Dst>::max());
    }

    template<class U, class Enable = void>
    struct ValueConverter {
        static common::Status fromValue(const common::Value &, U &, FieldFlags) {
            static_assert(detail::AlwaysFalse<U>::value,
                          "sqlconduit::mapping: target type has no built-in conversion; "
                          "specialize sqlconduit::mapping::ValueConverter<T>");
            return mapError("unsupported target type");
        }

        static common::Value toValue(const U &) {
            static_assert(detail::AlwaysFalse<U>::value,
                          "sqlconduit::mapping: target type has no built-in conversion; "
                          "specialize sqlconduit::mapping::ValueConverter<T>");
            return common::Value(nullptr);
        }
    };

    template<>
    struct ValueConverter<bool, void> {
        static common::Status fromValue(const common::Value &v, bool &out, FieldFlags) {
            if (const auto p = std::get_if<bool>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            return typeError("bool", v);
        }

        static common::Value toValue(const bool in) { return common::Value(in); }
    };

    template<class U>
    struct ValueConverter<U, std::enable_if_t<std::is_integral_v<U> && !std::is_same_v<U, bool>> > {
        static common::Status fromValue(const common::Value &v, U &out, const FieldFlags flags) {
            const std::string target = TypeName<U>::name();
            if (const auto p = std::get_if<std::int64_t>(&v)) {
                if (!fitsIn<U>(*p))
                    return mapError("value " + std::to_string(*p) + " out of range for " + target);
                out = static_cast<U>(*p);
                return common::Status::OK();
            }
            if (const auto p = std::get_if<std::uint64_t>(&v)) {
                if (!fitsIn<U>(*p))
                    return mapError("value " + std::to_string(*p) + " out of range for " + target);
                out = static_cast<U>(*p);
                return common::Status::OK();
            }
            if (hasFlag(flags, FieldFlags::Lossy)) {
                if (const auto p = std::get_if<double>(&v)) {
                    if (!std::isfinite(*p)) return mapError("non-finite double for " + target);
                    const double t = *p < 0 ? std::ceil(*p) : std::floor(*p);
                    if (t < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                        t > static_cast<double>(std::numeric_limits<std::int64_t>::max()))
                        return mapError("double " + std::to_string(*p) + " out of range for " + target);
                    const auto iv = static_cast<std::int64_t>(t);
                    if (!fitsIn<U>(iv))
                        return mapError("value " + std::to_string(iv) + " out of range for " + target);
                    out = static_cast<U>(iv);
                    return common::Status::OK();
                }
                std::int64_t iv = 0;
                bool parsed = false;
                if (const auto p = std::get_if<common::Decimal>(&v)) parsed = tryParseIntegral(p->value, iv);
                else if (const auto p = std::get_if<std::string>(&v)) parsed = tryParseIntegral(*p, iv);
                if (parsed) {
                    if (!fitsIn<U>(iv))
                        return mapError("value " + std::to_string(iv) + " out of range for " + target);
                    out = static_cast<U>(iv);
                    return common::Status::OK();
                }
            }
            return typeError(target, v);
        }

        static common::Value toValue(const U in) {
            if constexpr (std::is_unsigned_v<U>) return common::Value(static_cast<std::uint64_t>(in));
            else return common::Value(static_cast<std::int64_t>(in));
        }
    };

    template<class U>
    struct ValueConverter<U, std::enable_if_t<std::is_floating_point_v<U> > > {
        static common::Status fromValue(const common::Value &v, U &out, const FieldFlags flags) {
            const std::string target = TypeName<U>::name();
            if (const auto p = std::get_if<double>(&v)) {
                if constexpr (!std::is_same_v<U, double>) {
                    if (*p > static_cast<double>(std::numeric_limits<U>::max()) ||
                        *p < static_cast<double>(std::numeric_limits<U>::lowest()))
                        return mapError("double " + std::to_string(*p) + " out of range for " + target);
                }
                out = static_cast<U>(*p);
                return common::Status::OK();
            }
            if (hasFlag(flags, FieldFlags::Lossy)) {
                if (const auto p = std::get_if<std::int64_t>(&v)) {
                    out = static_cast<U>(*p);
                    return common::Status::OK();
                }
                if (const auto p = std::get_if<std::uint64_t>(&v)) {
                    out = static_cast<U>(*p);
                    return common::Status::OK();
                }
                double d = 0;
                bool parsed = false;
                if (const auto p = std::get_if<common::Decimal>(&v)) parsed = tryParseReal(p->value, d);
                else if (const auto p = std::get_if<std::string>(&v)) parsed = tryParseReal(*p, d);
                if (parsed) {
                    out = static_cast<U>(d);
                    return common::Status::OK();
                }
            }
            return typeError(target, v);
        }

        static common::Value toValue(const U in) { return common::Value(static_cast<double>(in)); }
    };

    template<>
    struct ValueConverter<std::string, void> {
        static common::Status fromValue(const common::Value &v, std::string &out, const FieldFlags flags) {
            if (const auto p = std::get_if<std::string>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            if (const auto p = std::get_if<common::Decimal>(&v)) {
                out = p->value;
                return common::Status::OK();
            }
            if (const auto p = std::get_if<common::Date>(&v)) {
                out = p->value;
                return common::Status::OK();
            }
            if (const auto p = std::get_if<common::Time>(&v)) {
                out = p->value;
                return common::Status::OK();
            }
            if (const auto p = std::get_if<common::Uuid>(&v)) {
                out = p->value;
                return common::Status::OK();
            }
            if (const auto p = std::get_if<common::Json>(&v)) {
                out = p->value;
                return common::Status::OK();
            }
            if (hasFlag(flags, FieldFlags::Lossy)) {
                if (std::holds_alternative<std::nullptr_t>(v)) return typeError("std::string", v);
                if (std::holds_alternative<common::Blob>(v)) return typeError("std::string", v);
                if (const auto p = std::get_if<common::Timestamp>(&v)) {
                    out = common::timestampToStringMs(*p);
                    return common::Status::OK();
                }
                out = common::valueToString(v);
                return common::Status::OK();
            }
            return typeError("std::string", v);
        }

        static common::Value toValue(const std::string &in) { return common::Value(in); }
    };

    template<class Strong>
    struct StrongTextConverter {
        static common::Status fromValue(const common::Value &v, Strong &out, const FieldFlags flags) {
            if (const auto p = std::get_if<Strong>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            if (hasFlag(flags, FieldFlags::Textual))
                if (const auto p = std::get_if<std::string>(&v)) {
                    out = Strong{*p};
                    return common::Status::OK();
                }
            return typeError(TypeName<Strong>::name(), v);
        }

        static common::Value toValue(const Strong &in) { return common::Value(in); }
    };

    template<>
    struct ValueConverter<common::Decimal, void> : StrongTextConverter<common::Decimal> {
    };

    template<>
    struct ValueConverter<common::Date, void> : StrongTextConverter<common::Date> {
    };

    template<>
    struct ValueConverter<common::Time, void> : StrongTextConverter<common::Time> {
    };

    template<>
    struct ValueConverter<common::Uuid, void> : StrongTextConverter<common::Uuid> {
    };

    template<>
    struct ValueConverter<common::Json, void> : StrongTextConverter<common::Json> {
    };

    template<>
    struct ValueConverter<common::Blob, void> {
        static common::Status fromValue(const common::Value &v, common::Blob &out, FieldFlags) {
            if (const auto p = std::get_if<common::Blob>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            return typeError("Blob", v);
        }

        static common::Value toValue(const common::Blob &in) { return common::Value(in); }
    };

    template<>
    struct ValueConverter<common::Array, void> {
        static common::Status fromValue(const common::Value &v, common::Array &out, FieldFlags) {
            if (const auto p = std::get_if<common::Array>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            return typeError("Array", v);
        }

        static common::Value toValue(const common::Array &in) { return common::Value(in); }
    };

    template<>
    struct ValueConverter<common::Composite, void> {
        static common::Status fromValue(const common::Value &v, common::Composite &out, FieldFlags) {
            if (const auto p = std::get_if<common::Composite>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            return typeError("Composite", v);
        }

        static common::Value toValue(const common::Composite &in) { return common::Value(in); }
    };

    template<class U>
    struct ValueConverter<std::vector<U>, void> {
        static common::Status fromValue(const common::Value &v, std::vector<U> &out,
                                        const FieldFlags flags) {
            const auto *array = std::get_if<common::Array>(&v);
            if (!array) return typeError("std::vector", v);
            out.clear();
            out.reserve(array->items.size());
            for (const auto &item: array->items) {
                U tmp{};
                if (const auto s = ValueConverter<U>::fromValue(item, tmp, flags); !s.ok()) return s;
                out.push_back(std::move(tmp));
            }
            return common::Status::OK();
        }

        static common::Value toValue(const std::vector<U> &in) {
            common::Array array;
            array.items.reserve(in.size());
            for (const auto &item: in) array.items.push_back(ValueConverter<U>::toValue(item));
            return common::Value(std::move(array));
        }
    };

    template<class G>
    struct PgGeometryTraits;

    template<>
    struct PgGeometryTraits<common::PgPoint> {
        static bool parse(const std::string &text, common::PgPoint &out) {
            return common::pgParsePoint(text, out);
        }

        static std::string format(const common::PgPoint &v) { return common::pgFormatPoint(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgLine> {
        static bool parse(const std::string &text, common::PgLine &out) {
            return common::pgParseLine(text, out);
        }

        static std::string format(const common::PgLine &v) { return common::pgFormatLine(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgLseg> {
        static bool parse(const std::string &text, common::PgLseg &out) {
            return common::pgParseLseg(text, out);
        }

        static std::string format(const common::PgLseg &v) { return common::pgFormatLseg(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgBox> {
        static bool parse(const std::string &text, common::PgBox &out) {
            return common::pgParseBox(text, out);
        }

        static std::string format(const common::PgBox &v) { return common::pgFormatBox(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgPath> {
        static bool parse(const std::string &text, common::PgPath &out) {
            return common::pgParsePath(text, out);
        }

        static std::string format(const common::PgPath &v) { return common::pgFormatPath(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgPolygon> {
        static bool parse(const std::string &text, common::PgPolygon &out) {
            return common::pgParsePolygon(text, out);
        }

        static std::string format(const common::PgPolygon &v) { return common::pgFormatPolygon(v); }
    };

    template<>
    struct PgGeometryTraits<common::PgCircle> {
        static bool parse(const std::string &text, common::PgCircle &out) {
            return common::pgParseCircle(text, out);
        }

        static std::string format(const common::PgCircle &v) { return common::pgFormatCircle(v); }
    };

    template<class G>
    struct PgGeometryConverter {
        static common::Status fromValue(const common::Value &v, G &out, const FieldFlags flags) {
            if (const auto p = std::get_if<common::Json>(&v))
                if (PgGeometryTraits<G>::parse(p->value, out)) return common::Status::OK();
            if (hasFlag(flags, FieldFlags::Textual))
                if (const auto p = std::get_if<std::string>(&v))
                    if (PgGeometryTraits<G>::parse(*p, out)) return common::Status::OK();
            return typeError(TypeName<G>::name(), v);
        }

        static common::Value toValue(const G &in) {
            return common::Value(common::Json{PgGeometryTraits<G>::format(in)});
        }
    };

    template<>
    struct ValueConverter<common::PgPoint, void> : PgGeometryConverter<common::PgPoint> {
    };

    template<>
    struct ValueConverter<common::PgLine, void> : PgGeometryConverter<common::PgLine> {
    };

    template<>
    struct ValueConverter<common::PgLseg, void> : PgGeometryConverter<common::PgLseg> {
    };

    template<>
    struct ValueConverter<common::PgBox, void> : PgGeometryConverter<common::PgBox> {
    };

    template<>
    struct ValueConverter<common::PgPath, void> : PgGeometryConverter<common::PgPath> {
    };

    template<>
    struct ValueConverter<common::PgPolygon, void> : PgGeometryConverter<common::PgPolygon> {
    };

    template<>
    struct ValueConverter<common::PgCircle, void> : PgGeometryConverter<common::PgCircle> {
    };

    template<>
    struct ValueConverter<common::Timestamp, void> {
        static common::Status fromValue(const common::Value &v, common::Timestamp &out, const FieldFlags flags) {
            if (const auto p = std::get_if<common::Timestamp>(&v)) {
                out = *p;
                return common::Status::OK();
            }
            if (hasFlag(flags, FieldFlags::Textual)) {
                std::string text;
                if (const auto p = std::get_if<std::string>(&v)) text = *p;
                else if (const auto p = std::get_if<common::Date>(&v)) text = p->value;
                else if (const auto p = std::get_if<common::Time>(&v)) text = p->value;
                if (!text.empty() && common::tryParseTimestamp(text, out)) return common::Status::OK();
            }
            return typeError("Timestamp", v);
        }

        static common::Value toValue(const common::Timestamp &in) { return common::Value(in); }
    };

    template<class U>
    struct ValueConverter<std::optional<U> > {
        static_assert(!detail::IsOptional<U>::value, "nested std::optional is not supported");

        static common::Status fromValue(const common::Value &v, std::optional<U> &out, const FieldFlags flags) {
            if (std::holds_alternative<std::nullptr_t>(v)) {
                out.reset();
                return common::Status::OK();
            }
            U tmp{};
            if (const auto s = ValueConverter<U>::fromValue(v, tmp, flags); !s.ok()) return s;
            out = std::move(tmp);
            return common::Status::OK();
        }

        static common::Value toValue(const std::optional<U> &in) {
            return in ? ValueConverter<U>::toValue(*in) : common::Value(nullptr);
        }
    };

    template<class U>
    struct ValueConverter<U, std::enable_if_t<std::is_enum_v<U> > > {
        using Under = std::underlying_type_t<U>;

        static common::Status fromValue(const common::Value &v, U &out, FieldFlags) {
            Under u{};
            if (const auto s = ValueConverter<Under>::fromValue(v, u, FieldFlags::None); !s.ok()) return s;
            out = static_cast<U>(u);
            return common::Status::OK();
        }

        static common::Value toValue(const U in) {
            return ValueConverter<Under>::toValue(static_cast<Under>(in));
        }
    };

    template<class T>
    class Mapping {
    public:
        using Assign = std::function<common::Status(const common::Value &, T &)>;
        using Write = std::function<common::Value(const T &)>;

        struct Column {
            std::string name;
            std::string target;
            FieldFlags flags = FieldFlags::None;
            Assign assign;
            Write write;
            std::any member;
        };

        template<class M>
        Mapping &field(M T::*ptr, std::string column, const FieldFlags flags = FieldFlags::None) {
            Column c;
            c.name = column;
            c.target = TypeName<M>::name();
            c.flags = flags;
            c.member = ptr;
            c.assign = [ptr, flags, target = c.target](const common::Value &v, T &out) -> common::Status {
                auto s = ValueConverter<M>::fromValue(v, out.*ptr, flags);
                if (!s.ok() && s.code != common::ErrorCode::MappingError)
                    return mapError("target " + target + ": " + s.message);
                return s;
            };
            c.write = [ptr](const T &in) -> common::Value { return ValueConverter<M>::toValue(in.*ptr); };
            columns_.push_back(std::move(c));
            return *this;
        }

        Mapping &extraColumns(const ExtraColumns p) {
            extra_ = p;
            return *this;
        }

        Mapping &table(std::string name) { table_ = std::move(name); return *this; }
        [[nodiscard]] const std::string &tableName() const noexcept { return table_; }

        // Compare actual, same-type member pointers; never reinterpret offsets or object bytes.
        template<class M>
        [[nodiscard]] const Column *columnFor(M T::*member) const {
            if (!member) return nullptr;
            const Column *found = nullptr;
            for (const auto &column : columns_) {
                const auto ptr = std::any_cast<M T::*>(&column.member);
                if (ptr && *ptr == member) {
                    if (found) return nullptr; // An ambiguous registration is not a usable field.
                    found = &column;
                }
            }
            return found;
        }

        Mapping &missingColumns(const MissingColumns p) {
            missing_ = p;
            return *this;
        }

        [[nodiscard]] std::size_t size() const noexcept { return columns_.size(); }
        [[nodiscard]] const std::vector<Column> &columns() const noexcept { return columns_; }
        [[nodiscard]] ExtraColumns extraPolicy() const noexcept { return extra_; }
        [[nodiscard]] MissingColumns missingPolicy() const noexcept { return missing_; }

        [[nodiscard]] std::vector<std::string> columnNames(const WriteCols which = WriteCols::All) const {
            std::vector<std::string> out;
            for (const auto &c: columns_) {
                if (which == WriteCols::PrimaryKey && !hasFlag(c.flags, FieldFlags::PrimaryKey)) continue;
                if (which == WriteCols::Writable && !writable(c)) continue;
                out.push_back(c.name);
            }
            return out;
        }

        [[nodiscard]] common::Status fromRow(const common::Row &row, T &out) const {
            for (const auto &c: columns_) {
                const auto it = row.data().find(c.name);
                if (it == row.data().end()) {
                    if (missing_ == MissingColumns::Error)
                        return mapError("column '" + c.name + "' not found in result set (target " + c.target + ")");
                    continue;
                }
                if (const auto s = c.assign(it->second, out); !s.ok())
                    return mapError("column '" + c.name + "': " + s.message);
            }
            if (extra_ == ExtraColumns::Error) {
                for (const auto &kv: row.data())
                    if (!isDeclaredByName(kv.first))
                        return mapError("undeclared column '" + kv.first + "' in result set");
            }
            return common::Status::OK();
        }

        static bool writable(const Column &c) {
            return !hasFlag(c.flags, FieldFlags::Generated) && !hasFlag(c.flags, FieldFlags::ReadOnly);
        }

        [[nodiscard]] bool isDeclaredByName(const std::string &n) const {
            for (const auto &c: columns_) if (c.name == n) return true;
            return false;
        }

    private:
        std::vector<Column> columns_;
        std::string table_;
        ExtraColumns extra_ = ExtraColumns::Ignore;
        MissingColumns missing_ = MissingColumns::Ignore;
    };

    template<class T>
    struct RowMapper;

    namespace detail {
        template<class T, class = void>
        struct HasDescribe : std::false_type {
        };

        template<class T>
        struct HasDescribe<T, std::void_t<decltype(RowMapper<T>::describe())> > : std::true_type {
        };
    }

    template<class T>
    const Mapping<T> &mappingFor() {
        static_assert(detail::HasDescribe<T>::value,
                      "sqlconduit::mapping: specialize sqlconduit::mapping::RowMapper<T> for the entity "
                      "and provide static Mapping<T> describe()");
        static const Mapping<T> m = RowMapper<T>::describe();
        return m;
    }

    template<class T>
    common::Status fromRows(const common::ResultSet &rs, std::vector<T> &out) {
        const auto &m = mappingFor<T>();
        out.clear();
        out.reserve(rs.rowCount());
        for (const auto &row: rs.rows()) {
            T item{};
            if (const auto s = m.fromRow(row, item); !s.ok()) {
                out.clear();
                return s;
            }
            out.push_back(std::move(item));
        }
        return common::Status::OK();
    }

    template<class T>
    common::Status fromRow(const common::Row &row, T &out) {
        return mappingFor<T>().fromRow(row, out);
    }

    template<class T>
    common::Params paramsOf(const T &entity, const WriteCols which = WriteCols::Writable) {
        const auto &m = mappingFor<T>();
        common::Params out;
        out.reserve(m.size());
        for (const auto &c: m.columns()) {
            if (which == WriteCols::PrimaryKey && !hasFlag(c.flags, FieldFlags::PrimaryKey)) continue;
            if (which == WriteCols::Writable && !Mapping<T>::writable(c)) continue;
            out.push_back(c.write(entity));
        }
        return out;
    }

    template<class T>
    common::Params updateParamsOf(const T &entity) {
        const auto &m = mappingFor<T>();
        common::Params set;
        common::Params keys;
        for (const auto &c: m.columns()) {
            if (hasFlag(c.flags, FieldFlags::PrimaryKey)) keys.push_back(c.write(entity));
            else if (Mapping<T>::writable(c)) set.push_back(c.write(entity));
        }
        set.insert(set.end(), keys.begin(), keys.end());
        return set;
    }

    template<class T>
    common::ParamBatch batchOf(const std::vector<T> &entities, const WriteCols which = WriteCols::Writable) {
        common::ParamBatch out;
        out.reserve(entities.size());
        for (const auto &e: entities) out.push_back(paramsOf(e, which));
        return out;
    }

    inline common::util::Dialect dialectOf(const core::Session &s) {
        return common::util::dialectFromDriverType(s.driverType());
    }

    inline common::util::Dialect defaultDialect(Client &client) {
        return common::util::detectDialect(client, std::string());
    }


    inline std::string joinIdentifiers(const std::vector<std::string> &cols,
                                       const common::util::Dialect d = common::util::Dialect::Auto) {
        std::string s;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) s += ", ";
            s += common::util::quoteIdent(cols[i], d);
        }
        return s;
    }

    inline std::string placeholders(const std::size_t n) {
        std::string s;
        for (std::size_t i = 0; i < n; ++i) {
            if (i) s += ", ";
            s += "?";
        }
        return s;
    }

    inline std::string buildAssignList(const std::vector<std::string> &cols,
                                       const common::util::Dialect d = common::util::Dialect::Auto) {
        std::string s;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (i) s += ", ";
            s += common::util::quoteIdent(cols[i], d) + " = ?";
        }
        return s;
    }

    template<class T>
    std::string insertSql(std::string table,
                          const common::util::Dialect d = common::util::Dialect::Auto) {
        const auto cols = mappingFor<T>().columnNames(WriteCols::Writable);
        auto builder = sql::Builder::insert(std::move(table), d);
        for (const auto &column: cols) builder.value(column, std::int64_t{0});
        const auto result = builder.build();
        return result.ok() ? result.statement.sql : std::string();
    }

    template<class T>
    std::string updateSql(std::string table,
                          const common::util::Dialect d = common::util::Dialect::Auto) {
        const auto &m = mappingFor<T>();
        std::vector<std::string> setCols;
        std::vector<std::string> keyCols;
        for (const auto &c: m.columns()) {
            if (hasFlag(c.flags, FieldFlags::PrimaryKey)) keyCols.push_back(c.name);
            else if (Mapping<T>::writable(c)) setCols.push_back(c.name);
        }
        if (keyCols.empty() || setCols.empty()) return std::string();
        auto builder = sql::Builder::update(std::move(table), d);
        for (const auto &column: setCols) builder.value(column, std::int64_t{0});
        for (const auto &column: keyCols) builder.where(sql::eq(column, std::int64_t{0}));
        const auto result = builder.build();
        return result.ok() ? result.statement.sql : std::string();
    }

    template<class T>
    std::string updateSql(std::string table, const std::vector<std::string> &setCols,
                          const std::vector<std::string> &whereCols,
                          const common::util::Dialect d = common::util::Dialect::Auto) {
        const auto &m = mappingFor<T>();
        if (setCols.empty() || whereCols.empty()) return std::string();
        for (const auto &n: setCols)
            if (!m.isDeclaredByName(n)) return std::string();
        for (const auto &n: whereCols)
            if (!m.isDeclaredByName(n)) return std::string();
        auto builder = sql::Builder::update(std::move(table), d);
        for (const auto &column: setCols) builder.value(column, std::int64_t{0});
        for (const auto &column: whereCols) builder.where(sql::eq(column, std::int64_t{0}));
        const auto result = builder.build();
        return result.ok() ? result.statement.sql : std::string();
    }

    template<class T>
    std::string insertSqlReturning(std::string table, const common::util::Dialect d) {
        const std::string base = insertSql<T>(table, d);
        std::vector<std::string> gen;
        for (const auto &c: mappingFor<T>().columns())
            if (hasFlag(c.flags, FieldFlags::Generated)) gen.push_back(c.name);
        if (gen.empty()) return base;
        if (d == common::util::Dialect::Postgres)
            return base + " RETURNING " + joinIdentifiers(gen, d);
        if (d == common::util::Dialect::Oracle) {
            const std::size_t paramCount = mappingFor<T>().columnNames(WriteCols::Writable).size();
            return base + common::oracleMakeReturningSuffix(gen, paramCount + 1);
        }
        if (d == common::util::Dialect::SqlServer) {
            const auto pos = base.find(" VALUES");
            if (pos != std::string::npos) {
                std::string cols;
                for (std::size_t i = 0; i < gen.size(); ++i) {
                    if (i) cols += ',';
                    cols += "INSERTED." + gen[i];
                }
                return base.substr(0, pos) + " OUTPUT " + cols + base.substr(pos);
            }
        }
        return base;
    }

    template<class T>
    common::Status applyGeneratedKeys(const common::GeneratedKeys &keys, T &entity) {
        if (keys.empty()) return common::Status::OK();
        const auto &m = mappingFor<T>();
        const auto &rows = keys.rows.rows();
        const common::Row *row = rows.empty() ? nullptr : &rows.front();
        std::size_t filled = 0;
        for (const auto &c: m.columns()) {
            if (!hasFlag(c.flags, FieldFlags::Generated) || row == nullptr) continue;
            const auto it = row->data().find(c.name);
            if (it == row->data().end()) continue;
            if (const auto s = c.assign(it->second, entity); !s.ok())
                return mapError("generated key column '" + c.name + "': " + s.message);
            ++filled;
        }
        if (filled == 0) {
            const auto id = keys.lastInsertId();
            if (id != 0) {
                const common::Value v(static_cast<std::int64_t>(id));
                for (const auto &c: m.columns()) {
                    if (!hasFlag(c.flags, FieldFlags::Generated)) continue;
                    if (c.assign(v, entity).ok()) break;
                }
            }
        }
        return common::Status::OK();
    }
}

namespace sqlconduit {
    template<class T>
    EntityResult<T> queryAs(Client &client, const std::string &sql) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = client.query(sql, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    template<class T>
    EntityResult<T> queryAs(Client &client, const std::string &sql,
                            const common::Params &params) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = client.query(sql, params, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    template<class T>
    EntityResult<T> queryAs(Client &client, const std::string &dataSource,
                            const std::string &sql,
                            const common::Params &params) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = client.query(dataSource, sql, params, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    template<class T>
    EntityResult<T> queryAs(core::Session &s, const std::string &sql) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = s.query(sql, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    template<class T>
    EntityResult<T> queryAs(core::Session &s, const std::string &sql,
                            const common::Params &params) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = s.query(sql, params, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    namespace detail {
        template<class T, class Runner>
        EntityOne<T> queryOneAsImpl(Runner &&runner) {
            EntityOne<T> o;
            common::ResultSet rows;
            o.status = std::forward<Runner>(runner)(rows);
            if (!o.status.ok()) return o;
            if (rows.rowCount() > 1) {
                o.status = mapping::mapError("queryOneAs: expected at most 1 row, got " +
                                             std::to_string(rows.rowCount()));
                return o;
            }
            if (rows.empty()) return o;
            T item{};
            o.status = mapping::fromRow<T>(rows.rows().front(), item);
            if (o.status.ok()) o.value = std::move(item);
            return o;
        }
    }

    template<class T>
    EntityOne<T> queryOneAs(Client &client, const std::string &sql) {
        return detail::queryOneAsImpl<T>([&](common::ResultSet &rows) {
            return client.query(sql, rows);
        });
    }

    template<class T>
    EntityOne<T> queryOneAs(Client &client, const std::string &sql,
                            const common::Params &params) {
        return detail::queryOneAsImpl<T>([&](common::ResultSet &rows) {
            return client.query(sql, params, rows);
        });
    }

    template<class T>
    EntityOne<T> queryOneAs(Client &client, const std::string &dataSource,
                            const std::string &sql,
                            const common::Params &params) {
        return detail::queryOneAsImpl<T>([&](common::ResultSet &rows) {
            return client.query(dataSource, sql, params, rows);
        });
    }

    template<class T>
    EntityOne<T> queryOneAs(core::Session &s, const std::string &sql) {
        return detail::queryOneAsImpl<T>([&](common::ResultSet &rows) {
            return s.query(sql, rows);
        });
    }

    template<class T>
    EntityOne<T> queryOneAs(core::Session &s, const std::string &sql,
                            const common::Params &params) {
        return detail::queryOneAsImpl<T>([&](common::ResultSet &rows) {
            return s.query(sql, params, rows);
        });
    }

    template<class T>
    common::Status queryEachAs(Client &client, const std::string &sql,
                               const common::Params &params,
                               const std::function<bool(T &&)> &cb, std::uint64_t &rows) {
        rows = 0;
        std::uint64_t counted = 0;
        common::Status mapErr;
        common::RowCallback raw = [&cb, &counted, &mapErr](const common::Row &row) -> bool {
            T item{};
            if (const auto s = mapping::fromRow<T>(row, item); !s.ok()) {
                mapErr = s;
                return false;
            }
            ++counted;
            return cb(std::move(item));
        };
        std::uint64_t driverRows = 0;
        const auto st = client.queryEach(sql, params, raw, driverRows);
        rows = counted;
        if (st.ok() && !mapErr.ok()) return mapErr;
        return st;
    }

    template<class T>
    common::Status queryEachAs(Client &client, const std::string &dataSource,
                               const std::string &sql,
                               const common::Params &params,
                               const std::function<bool(T &&)> &cb, std::uint64_t &rows) {
        rows = 0;
        std::uint64_t counted = 0;
        common::Status mapErr;
        common::RowCallback raw = [&cb, &counted, &mapErr](const common::Row &row) -> bool {
            T item{};
            if (const auto s = mapping::fromRow<T>(row, item); !s.ok()) {
                mapErr = s;
                return false;
            }
            ++counted;
            return cb(std::move(item));
        };
        std::uint64_t driverRows = 0;
        const auto st = client.queryEach(dataSource, sql, params, raw, driverRows);
        rows = counted;
        if (st.ok() && !mapErr.ok()) return mapErr;
        return st;
    }

    template<class T>
    common::Status queryEachAs(core::Session &s, const std::string &sql, const common::Params &params,
                               const std::function<bool(T &&)> &cb, std::uint64_t &rows) {
        rows = 0;
        std::uint64_t counted = 0;
        common::Status mapErr;
        common::RowCallback raw = [&cb, &counted, &mapErr](const common::Row &row) -> bool {
            T item{};
            if (const auto s = mapping::fromRow<T>(row, item); !s.ok()) {
                mapErr = s;
                return false;
            }
            ++counted;
            return cb(std::move(item));
        };
        std::uint64_t driverRows = 0;
        const auto st = s.queryEach(sql, params, raw, driverRows);
        rows = counted;
        if (st.ok() && !mapErr.ok()) return mapErr;
        return st;
    }

    template<class T>
    EntityResult<T> fetchAs(core::ICursor &c, const std::size_t n) {
        common::ResultSet rs;
        EntityResult<T> r;
        r.status = c.fetch(n, rs);
        if (r.status.ok()) r.status = mapping::fromRows<T>(rs, r.items);
        return r;
    }

    template<class T>
    EntityResult<T> keysAs(const common::GeneratedKeys &keys) {
        EntityResult<T> r;
        r.status = mapping::fromRows<T>(keys.rows, r.items);
        return r;
    }

    template<class T>
    WriteResult<T> insertAs(Client &client, std::string table, T &entity) {
        WriteResult<T> r;
        const common::Params p = mapping::paramsOf(entity);
        r.status = client.withSession([&](core::Session &s) {
            return s.execute(mapping::insertSqlReturning<T>(table, mapping::dialectOf(s)), p,
                             r.affected, r.keys);
        });
        if (r.status.ok()) r.status = mapping::applyGeneratedKeys(r.keys, entity);
        return r;
    }

    template<class T>
    WriteResult<T> insertAs(core::Session &s, std::string table, T &entity) {
        WriteResult<T> r;
        const std::string sql = mapping::insertSqlReturning<T>(table, mapping::dialectOf(s));
        const common::Params p = mapping::paramsOf(entity);
        r.status = s.execute(sql, p, r.affected, r.keys);
        if (r.status.ok()) r.status = mapping::applyGeneratedKeys(r.keys, entity);
        return r;
    }

    template<class T>
    WriteResult<T> updateAs(Client &client, std::string table, const T &entity) {
        WriteResult<T> r;
        const std::string sql = mapping::updateSql<T>(table, mapping::defaultDialect(client));
        if (sql.empty()) {
            r.status = mapping::mapError(
                "updateAs: entity has no PrimaryKey field or no updatable fields; "
                "refusing to generate UPDATE");
            return r;
        }
        r.status = client.execute(sql, mapping::updateParamsOf(entity), r.affected);
        return r;
    }

    template<class T>
    WriteResult<T> updateAs(core::Session &s, std::string table, const T &entity) {
        WriteResult<T> r;
        const std::string sql = mapping::updateSql<T>(table, mapping::dialectOf(s));
        if (sql.empty()) {
            r.status = mapping::mapError(
                "updateAs: entity has no PrimaryKey field or no updatable fields; "
                "refusing to generate UPDATE");
            return r;
        }
        r.status = s.execute(sql, mapping::updateParamsOf(entity), r.affected);
        return r;
    }

    template<class T>
    BatchWriteResult<T> insertBatchAs(Client &client, std::string table,
                                      const std::vector<T> &entities) {
        BatchWriteResult<T> r;
        r.status = client.executeBatch(
            mapping::insertSql<T>(table, mapping::defaultDialect(client)),
            mapping::batchOf(entities), r.batch);
        return r;
    }

    template<class T>
    BatchWriteResult<T> insertBatchAs(core::Session &s, std::string table,
                                      const std::vector<T> &entities) {
        BatchWriteResult<T> r;
        r.status = s.executeBatch(mapping::insertSql<T>(table, mapping::dialectOf(s)),
                                  mapping::batchOf(entities), r.batch);
        return r;
    }

    namespace detail {
        template<class T>
        common::Status applyBatchKeys(const common::BatchResult &batch, std::vector<T> &entities) {
            if (batch.keys.empty()) return common::Status::OK();
            const std::size_t n = batch.keys.size() < entities.size()
                                      ? batch.keys.size()
                                      : entities.size();
            for (std::size_t i = 0; i < n; ++i) {
                if (const auto s = mapping::applyGeneratedKeys(batch.keys[i], entities[i]); !s.ok())
                    return s;
            }
            return common::Status::OK();
        }
    }

    template<class T>
    BatchWriteResult<T> insertBatchAs(Client &client, std::string table,
                                      std::vector<T> &entities) {
        BatchWriteResult<T> r;
        r.status = client.executeBatch(
            mapping::insertSqlReturning<T>(table, mapping::defaultDialect(client)),
            mapping::batchOf(entities), r.batch);
        if (r.status.ok()) r.status = detail::applyBatchKeys<T>(r.batch, entities);
        return r;
    }

    template<class T>
    BatchWriteResult<T> insertBatchAs(core::Session &s, std::string table,
                                      std::vector<T> &entities) {
        BatchWriteResult<T> r;
        r.status = s.executeBatch(mapping::insertSqlReturning<T>(table, mapping::dialectOf(s)),
                                  mapping::batchOf(entities), r.batch);
        if (r.status.ok()) r.status = detail::applyBatchKeys<T>(r.batch, entities);
        return r;
    }
}


namespace sqlconduit {
    namespace detail {
        template<class T>
        EntityResult<T> directEntities(common::Status status, const common::ResultSet &rows,
                                      async::ExecutionMode mode) {
            EntityResult<T> result;
            result.status = std::move(status);
            result.mode = mode;
            if (result.ok()) result.status = mapping::fromRows<T>(rows, result.items);
            if (!result.ok()) result.items.clear();
            return result;
        }

        template<class T>
        EntityOne<T> directOne(common::Status status, const common::ResultSet &rows,
                               async::ExecutionMode mode) {
            EntityOne<T> result;
            result.status = std::move(status);
            result.mode = mode;
            if (!result.ok()) return result;
            if (rows.rowCount() > 1) {
                result.status = mapping::mapError("queryOne: expected at most one row");
            } else if (!rows.empty()) {
                T item{};
                result.status = mapping::fromRow<T>(rows.rows().front(), item);
                if (result.ok()) result.value = std::move(item);
            }
            return result;
        }

        template<class T>
        ScalarResult<T> directScalar(common::Status status, const common::ResultSet &rows,
                                      async::ExecutionMode mode) {
            ScalarResult<T> result;
            result.status = std::move(status);
            result.mode = mode;
            if (!result.ok()) return result;
            if (rows.rowCount() > 1 || rows.fields().size() > 1 ||
                (!rows.empty() && rows.rows().front().size() != 1)) {
                result.status = mapping::mapError("queryScalar: expected at most one row and exactly one column");
                return result;
            }
            if (rows.empty()) return result;
            const auto &row = rows.rows().front();
            if (!rows.fields().empty() && !row.has(rows.fields().front())) {
                result.status = mapping::mapError("queryScalar: result column metadata mismatch");
                return result;
            }
            result.hasRow = true;
            const auto &value = row.data().begin()->second;
            result.isNull = std::holds_alternative<std::nullptr_t>(value);
            if (!result.isNull) {
                T item{};
                result.status = mapping::ValueConverter<T>::fromValue(value, item, mapping::FieldFlags::None);
                if (result.ok()) result.value = std::move(item);
            }
            return result;
        }
    }

    template<class Result, class Convert>
    std::future<Result> Client::queryMappedAsync(const std::string &source, const std::string &sql,
                                                const common::Params &params, Convert convert) const {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        // Map at completion; never block an executor worker on another future.
        queryAsyncComplete(source, sql, params,
            [promise, convert = std::move(convert)](async::QueryResult raw) mutable {
                try {
                    promise->set_value(convert(std::move(raw)));
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            });
        return future;
    }

    template<class T>
    EntityResult<T> Client::query(const std::string &sql, const common::Params &params) const {
        return query<T>({}, sql, params);
    }

    template<class T>
    EntityResult<T> Client::query(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        auto raw = query(source, sql, params);
        return sqlconduit::detail::directEntities<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }

    template<class T>
    EntityResult<T> Client::queryNamed(const std::string &sql, const common::NamedParams &params) const {
        return queryNamed<T>({}, sql, params);
    }

    template<class T>
    EntityResult<T> Client::queryNamed(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return query<T>(source, bound.sql, bound.params);
        EntityResult<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        return result;
    }

    template<class T>
    std::future<EntityResult<T>> Client::queryAsync(const std::string &sql, const common::Params &params) const {
        return queryAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<EntityResult<T>> Client::queryAsync(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        return queryMappedAsync<EntityResult<T>>(source, sql, params, [](async::QueryResult raw) {
            return sqlconduit::detail::directEntities<T>(std::move(raw.status), raw.rows, raw.mode);
        });
    }

    template<class T>
    std::future<EntityResult<T>> Client::queryNamedAsync(const std::string &sql, const common::NamedParams &params) const {
        return queryNamedAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<EntityResult<T>> Client::queryNamedAsync(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryAsync<T>(source, bound.sql, bound.params);
        EntityResult<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        std::promise<EntityResult<T>> promise;
        auto future = promise.get_future();
        promise.set_value(std::move(result));
        return future;
    }

    template<class T>
    EntityOne<T> Client::queryOne(const std::string &sql, const common::Params &params) const {
        return queryOne<T>({}, sql, params);
    }

    template<class T>
    EntityOne<T> Client::queryOne(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        auto raw = query(source, sql, params);
        return sqlconduit::detail::directOne<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }

    template<class T>
    EntityOne<T> Client::queryOneNamed(const std::string &sql, const common::NamedParams &params) const {
        return queryOneNamed<T>({}, sql, params);
    }

    template<class T>
    EntityOne<T> Client::queryOneNamed(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryOne<T>(source, bound.sql, bound.params);
        EntityOne<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        return result;
    }

    template<class T>
    std::future<EntityOne<T>> Client::queryOneAsync(const std::string &sql, const common::Params &params) const {
        return queryOneAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<EntityOne<T>> Client::queryOneAsync(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        return queryMappedAsync<EntityOne<T>>(source, sql, params, [](async::QueryResult raw) {
            return sqlconduit::detail::directOne<T>(std::move(raw.status), raw.rows, raw.mode);
        });
    }

    template<class T>
    std::future<EntityOne<T>> Client::queryOneNamedAsync(const std::string &sql, const common::NamedParams &params) const {
        return queryOneNamedAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<EntityOne<T>> Client::queryOneNamedAsync(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryOneAsync<T>(source, bound.sql, bound.params);
        EntityOne<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        std::promise<EntityOne<T>> promise;
        auto future = promise.get_future();
        promise.set_value(std::move(result));
        return future;
    }

    template<class T>
    ScalarResult<T> Client::queryScalar(const std::string &sql, const common::Params &params) const {
        return queryScalar<T>({}, sql, params);
    }

    template<class T>
    ScalarResult<T> Client::queryScalar(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        auto raw = query(source, sql, params);
        return sqlconduit::detail::directScalar<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }

    template<class T>
    ScalarResult<T> Client::queryScalarNamed(const std::string &sql, const common::NamedParams &params) const {
        return queryScalarNamed<T>({}, sql, params);
    }

    template<class T>
    ScalarResult<T> Client::queryScalarNamed(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryScalar<T>(source, bound.sql, bound.params);
        ScalarResult<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        return result;
    }

    template<class T>
    std::future<ScalarResult<T>> Client::queryScalarAsync(const std::string &sql, const common::Params &params) const {
        return queryScalarAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<ScalarResult<T>> Client::queryScalarAsync(const std::string &source, const std::string &sql,
                                   const common::Params &params) const {
        return queryMappedAsync<ScalarResult<T>>(source, sql, params, [](async::QueryResult raw) {
            return sqlconduit::detail::directScalar<T>(std::move(raw.status), raw.rows, raw.mode);
        });
    }

    template<class T>
    std::future<ScalarResult<T>> Client::queryScalarNamedAsync(const std::string &sql, const common::NamedParams &params) const {
        return queryScalarNamedAsync<T>({}, sql, params);
    }

    template<class T>
    std::future<ScalarResult<T>> Client::queryScalarNamedAsync(const std::string &source, const std::string &sql,
                            const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryScalarAsync<T>(source, bound.sql, bound.params);
        ScalarResult<T> result;
        result.status = std::move(bound.status);
        result.mode = async::ExecutionMode::CompatibilityFallback;
        std::promise<ScalarResult<T>> promise;
        auto future = promise.get_future();
        promise.set_value(std::move(result));
        return future;
    }
}


namespace sqlconduit::detail {
    template<class T>
    EntityResult<T> directColumns(common::Status status, const common::ResultSet &rows, async::ExecutionMode mode) {
        EntityResult<T> result; result.status = std::move(status); result.mode = mode;
        if (!result.ok()) return result;
        if (rows.fields().size() > 1) {
            result.status = mapping::mapError("queryColumn: expected exactly one column"); return result;
        }
        result.items.reserve(rows.rowCount());
        for (const auto &row : rows.rows()) {
            if (row.size() != 1 || (!rows.fields().empty() && !row.has(rows.fields()[0]))) {
                result.status = mapping::mapError("queryColumn: expected exactly one matching column");
                result.items.clear(); return result;
            }
            T value{};
            result.status = mapping::ValueConverter<T>::fromValue(row.data().begin()->second, value, mapping::FieldFlags::None);
            if (!result.ok()) { result.items.clear(); return result; }
            result.items.push_back(std::move(value));
        }
        return result;
    }
}
namespace sqlconduit {

    template<class T> EntityResult<T> Client::queryColumn(const std::string &sql, const common::Params &params) const {
        return queryColumn<T>({}, sql, params);
    }
    template<class T> EntityResult<T> Client::queryColumn(const std::string &source, const std::string &sql, const common::Params &params) const {
        auto raw = query(source, sql, params);
        return sqlconduit::detail::directColumns<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }

    template<class T> EntityResult<T> Client::queryColumnNamed(const std::string &sql, const common::NamedParams &params) const {
        return queryColumnNamed<T>({}, sql, params);
    }
    template<class T> EntityResult<T> Client::queryColumnNamed(const std::string &source, const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryColumn<T>(source, bound.sql, bound.params);
        EntityResult<T> result; result.status = std::move(bound.status);
        return result;
    }

    template<class T> std::future<EntityResult<T>> Client::queryColumnAsync(const std::string &sql, const common::Params &params) const {
        return queryColumnAsync<T>({}, sql, params);
    }
    template<class T> std::future<EntityResult<T>> Client::queryColumnAsync(const std::string &source, const std::string &sql, const common::Params &params) const {
        return queryMappedAsync<EntityResult<T>>(source, sql, params, [](async::QueryResult raw) {
            return sqlconduit::detail::directColumns<T>(std::move(raw.status), raw.rows, raw.mode);
        });
    }

    template<class T> std::future<EntityResult<T>> Client::queryColumnNamedAsync(const std::string &sql, const common::NamedParams &params) const {
        return queryColumnNamedAsync<T>({}, sql, params);
    }
    template<class T> std::future<EntityResult<T>> Client::queryColumnNamedAsync(const std::string &source, const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryColumnAsync<T>(source, bound.sql, bound.params);
        EntityResult<T> result; result.status = std::move(bound.status);
        std::promise<EntityResult<T>> promise; auto future = promise.get_future();
        promise.set_value(std::move(result)); return future;
    }

    template<class T> EntityResult<T> core::Session::query(const std::string &sql, const common::Params &params) const {
        auto raw = query(sql, params);
        return sqlconduit::detail::directEntities<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }
    template<class T> EntityResult<T> core::Session::queryNamed(const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return query<T>(bound.sql, bound.params);
        EntityResult<T> result; result.status = std::move(bound.status); return result;
    }

    template<class T> EntityResult<T> Client::query(const sql::BuildResult &build) const {
        if (build.ok()) return query<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityResult<T> Client::query(const sql::Statement &statement) const {
        return query<T>(statement.sql, statement.params);
    }

    template<class T> std::future<EntityResult<T>> Client::queryAsync(const sql::BuildResult &build) const {
        if (build.ok()) return queryAsync<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        std::promise<EntityResult<T>> promise; auto future = promise.get_future();
        promise.set_value(std::move(result)); return future;
    }
    template<class T> std::future<EntityResult<T>> Client::queryAsync(const sql::Statement &statement) const {
        return queryAsync<T>(statement.sql, statement.params);
    }

    template<class T> EntityResult<T> core::Session::query(const sql::BuildResult &build) const {
        if (build.ok()) return query<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityResult<T> core::Session::query(const sql::Statement &statement) const {
        return query<T>(statement.sql, statement.params);
    }

    template<class T> EntityOne<T> core::Session::queryOne(const std::string &sql, const common::Params &params) const {
        auto raw = query(sql, params);
        return sqlconduit::detail::directOne<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }
    template<class T> EntityOne<T> core::Session::queryOneNamed(const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryOne<T>(bound.sql, bound.params);
        EntityOne<T> result; result.status = std::move(bound.status); return result;
    }

    template<class T> EntityOne<T> Client::queryOne(const sql::BuildResult &build) const {
        if (build.ok()) return queryOne<T>(build.statement.sql, build.statement.params);
        EntityOne<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityOne<T> Client::queryOne(const sql::Statement &statement) const {
        return queryOne<T>(statement.sql, statement.params);
    }

    template<class T> std::future<EntityOne<T>> Client::queryOneAsync(const sql::BuildResult &build) const {
        if (build.ok()) return queryOneAsync<T>(build.statement.sql, build.statement.params);
        EntityOne<T> result; result.status = build.status;
        std::promise<EntityOne<T>> promise; auto future = promise.get_future();
        promise.set_value(std::move(result)); return future;
    }
    template<class T> std::future<EntityOne<T>> Client::queryOneAsync(const sql::Statement &statement) const {
        return queryOneAsync<T>(statement.sql, statement.params);
    }

    template<class T> EntityOne<T> core::Session::queryOne(const sql::BuildResult &build) const {
        if (build.ok()) return queryOne<T>(build.statement.sql, build.statement.params);
        EntityOne<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityOne<T> core::Session::queryOne(const sql::Statement &statement) const {
        return queryOne<T>(statement.sql, statement.params);
    }

    template<class T> ScalarResult<T> core::Session::queryScalar(const std::string &sql, const common::Params &params) const {
        auto raw = query(sql, params);
        return sqlconduit::detail::directScalar<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }
    template<class T> ScalarResult<T> core::Session::queryScalarNamed(const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryScalar<T>(bound.sql, bound.params);
        ScalarResult<T> result; result.status = std::move(bound.status); return result;
    }

    template<class T> ScalarResult<T> Client::queryScalar(const sql::BuildResult &build) const {
        if (build.ok()) return queryScalar<T>(build.statement.sql, build.statement.params);
        ScalarResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> ScalarResult<T> Client::queryScalar(const sql::Statement &statement) const {
        return queryScalar<T>(statement.sql, statement.params);
    }

    template<class T> std::future<ScalarResult<T>> Client::queryScalarAsync(const sql::BuildResult &build) const {
        if (build.ok()) return queryScalarAsync<T>(build.statement.sql, build.statement.params);
        ScalarResult<T> result; result.status = build.status;
        std::promise<ScalarResult<T>> promise; auto future = promise.get_future();
        promise.set_value(std::move(result)); return future;
    }
    template<class T> std::future<ScalarResult<T>> Client::queryScalarAsync(const sql::Statement &statement) const {
        return queryScalarAsync<T>(statement.sql, statement.params);
    }

    template<class T> ScalarResult<T> core::Session::queryScalar(const sql::BuildResult &build) const {
        if (build.ok()) return queryScalar<T>(build.statement.sql, build.statement.params);
        ScalarResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> ScalarResult<T> core::Session::queryScalar(const sql::Statement &statement) const {
        return queryScalar<T>(statement.sql, statement.params);
    }

    template<class T> EntityResult<T> core::Session::queryColumn(const std::string &sql, const common::Params &params) const {
        auto raw = query(sql, params);
        return sqlconduit::detail::directColumns<T>(std::move(raw.status), raw.rows, async::ExecutionMode::Native);
    }
    template<class T> EntityResult<T> core::Session::queryColumnNamed(const std::string &sql, const common::NamedParams &params) const {
        auto bound = common::bindNamed(sql, params);
        if (bound.ok()) return queryColumn<T>(bound.sql, bound.params);
        EntityResult<T> result; result.status = std::move(bound.status); return result;
    }

    template<class T> EntityResult<T> Client::queryColumn(const sql::BuildResult &build) const {
        if (build.ok()) return queryColumn<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityResult<T> Client::queryColumn(const sql::Statement &statement) const {
        return queryColumn<T>(statement.sql, statement.params);
    }

    template<class T> std::future<EntityResult<T>> Client::queryColumnAsync(const sql::BuildResult &build) const {
        if (build.ok()) return queryColumnAsync<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        std::promise<EntityResult<T>> promise; auto future = promise.get_future();
        promise.set_value(std::move(result)); return future;
    }
    template<class T> std::future<EntityResult<T>> Client::queryColumnAsync(const sql::Statement &statement) const {
        return queryColumnAsync<T>(statement.sql, statement.params);
    }

    template<class T> EntityResult<T> core::Session::queryColumn(const sql::BuildResult &build) const {
        if (build.ok()) return queryColumn<T>(build.statement.sql, build.statement.params);
        EntityResult<T> result; result.status = build.status;
        return result;
    }
    template<class T> EntityResult<T> core::Session::queryColumn(const sql::Statement &statement) const {
        return queryColumn<T>(statement.sql, statement.params);
    }
}

// Define at namespace scope. Fields are a chain of SQLCONDUIT_FIELD / NAMED_FIELD entries.
// Explicit RowMapper specializations remain available for custom policies/converters.
#define SQLCONDUIT_FIELD(member) .field(&SQLConduitMappedType::member, #member)
#define SQLCONDUIT_NAMED_FIELD(member, column) .field(&SQLConduitMappedType::member, column)
#define SQLCONDUIT_MAP(Type, fields) \
    template<> struct sqlconduit::mapping::RowMapper<Type> { \
        using SQLConduitMappedType = Type; \
        static sqlconduit::mapping::Mapping<Type> describe() { \
            return sqlconduit::mapping::Mapping<Type>() fields \
                .missingColumns(sqlconduit::mapping::MissingColumns::Error); \
        } \
    };

// Convenience registration for 1-32 public members with matching column names.
// Define at global namespace scope, once per entity, instead of SQLCONDUIT_MAP.
#define SQLCONDUIT_DETAIL_EXPAND_ENTITY(x) x
#define SQLCONDUIT_DETAIL_ENTITY_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, NAME, ...) NAME
#define SQLCONDUIT_DETAIL_ENTITY_1(a1) SQLCONDUIT_FIELD(a1)
#define SQLCONDUIT_DETAIL_ENTITY_2(a1, a2) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2)
#define SQLCONDUIT_DETAIL_ENTITY_3(a1, a2, a3) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3)
#define SQLCONDUIT_DETAIL_ENTITY_4(a1, a2, a3, a4) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4)
#define SQLCONDUIT_DETAIL_ENTITY_5(a1, a2, a3, a4, a5) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5)
#define SQLCONDUIT_DETAIL_ENTITY_6(a1, a2, a3, a4, a5, a6) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6)
#define SQLCONDUIT_DETAIL_ENTITY_7(a1, a2, a3, a4, a5, a6, a7) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7)
#define SQLCONDUIT_DETAIL_ENTITY_8(a1, a2, a3, a4, a5, a6, a7, a8) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8)
#define SQLCONDUIT_DETAIL_ENTITY_9(a1, a2, a3, a4, a5, a6, a7, a8, a9) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9)
#define SQLCONDUIT_DETAIL_ENTITY_10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10)
#define SQLCONDUIT_DETAIL_ENTITY_11(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11)
#define SQLCONDUIT_DETAIL_ENTITY_12(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12)
#define SQLCONDUIT_DETAIL_ENTITY_13(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13)
#define SQLCONDUIT_DETAIL_ENTITY_14(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14)
#define SQLCONDUIT_DETAIL_ENTITY_15(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15)
#define SQLCONDUIT_DETAIL_ENTITY_16(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16)
#define SQLCONDUIT_DETAIL_ENTITY_17(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17)
#define SQLCONDUIT_DETAIL_ENTITY_18(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18)
#define SQLCONDUIT_DETAIL_ENTITY_19(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19)
#define SQLCONDUIT_DETAIL_ENTITY_20(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20)
#define SQLCONDUIT_DETAIL_ENTITY_21(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21)
#define SQLCONDUIT_DETAIL_ENTITY_22(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22)
#define SQLCONDUIT_DETAIL_ENTITY_23(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23)
#define SQLCONDUIT_DETAIL_ENTITY_24(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24)
#define SQLCONDUIT_DETAIL_ENTITY_25(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25)
#define SQLCONDUIT_DETAIL_ENTITY_26(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26)
#define SQLCONDUIT_DETAIL_ENTITY_27(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27)
#define SQLCONDUIT_DETAIL_ENTITY_28(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27) SQLCONDUIT_FIELD(a28)
#define SQLCONDUIT_DETAIL_ENTITY_29(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27) SQLCONDUIT_FIELD(a28) SQLCONDUIT_FIELD(a29)
#define SQLCONDUIT_DETAIL_ENTITY_30(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27) SQLCONDUIT_FIELD(a28) SQLCONDUIT_FIELD(a29) SQLCONDUIT_FIELD(a30)
#define SQLCONDUIT_DETAIL_ENTITY_31(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, a31) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27) SQLCONDUIT_FIELD(a28) SQLCONDUIT_FIELD(a29) SQLCONDUIT_FIELD(a30) SQLCONDUIT_FIELD(a31)
#define SQLCONDUIT_DETAIL_ENTITY_32(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30, a31, a32) SQLCONDUIT_FIELD(a1) SQLCONDUIT_FIELD(a2) SQLCONDUIT_FIELD(a3) SQLCONDUIT_FIELD(a4) SQLCONDUIT_FIELD(a5) SQLCONDUIT_FIELD(a6) SQLCONDUIT_FIELD(a7) SQLCONDUIT_FIELD(a8) SQLCONDUIT_FIELD(a9) SQLCONDUIT_FIELD(a10) SQLCONDUIT_FIELD(a11) SQLCONDUIT_FIELD(a12) SQLCONDUIT_FIELD(a13) SQLCONDUIT_FIELD(a14) SQLCONDUIT_FIELD(a15) SQLCONDUIT_FIELD(a16) SQLCONDUIT_FIELD(a17) SQLCONDUIT_FIELD(a18) SQLCONDUIT_FIELD(a19) SQLCONDUIT_FIELD(a20) SQLCONDUIT_FIELD(a21) SQLCONDUIT_FIELD(a22) SQLCONDUIT_FIELD(a23) SQLCONDUIT_FIELD(a24) SQLCONDUIT_FIELD(a25) SQLCONDUIT_FIELD(a26) SQLCONDUIT_FIELD(a27) SQLCONDUIT_FIELD(a28) SQLCONDUIT_FIELD(a29) SQLCONDUIT_FIELD(a30) SQLCONDUIT_FIELD(a31) SQLCONDUIT_FIELD(a32)
#define SQLCONDUIT_DETAIL_ENTITY_FIELDS(...) \
    SQLCONDUIT_DETAIL_EXPAND_ENTITY(SQLCONDUIT_DETAIL_EXPAND_ENTITY(SQLCONDUIT_DETAIL_ENTITY_PICK(__VA_ARGS__, SQLCONDUIT_DETAIL_ENTITY_32, SQLCONDUIT_DETAIL_ENTITY_31, SQLCONDUIT_DETAIL_ENTITY_30, SQLCONDUIT_DETAIL_ENTITY_29, SQLCONDUIT_DETAIL_ENTITY_28, SQLCONDUIT_DETAIL_ENTITY_27, SQLCONDUIT_DETAIL_ENTITY_26, SQLCONDUIT_DETAIL_ENTITY_25, SQLCONDUIT_DETAIL_ENTITY_24, SQLCONDUIT_DETAIL_ENTITY_23, SQLCONDUIT_DETAIL_ENTITY_22, SQLCONDUIT_DETAIL_ENTITY_21, SQLCONDUIT_DETAIL_ENTITY_20, SQLCONDUIT_DETAIL_ENTITY_19, SQLCONDUIT_DETAIL_ENTITY_18, SQLCONDUIT_DETAIL_ENTITY_17, SQLCONDUIT_DETAIL_ENTITY_16, SQLCONDUIT_DETAIL_ENTITY_15, SQLCONDUIT_DETAIL_ENTITY_14, SQLCONDUIT_DETAIL_ENTITY_13, SQLCONDUIT_DETAIL_ENTITY_12, SQLCONDUIT_DETAIL_ENTITY_11, SQLCONDUIT_DETAIL_ENTITY_10, SQLCONDUIT_DETAIL_ENTITY_9, SQLCONDUIT_DETAIL_ENTITY_8, SQLCONDUIT_DETAIL_ENTITY_7, SQLCONDUIT_DETAIL_ENTITY_6, SQLCONDUIT_DETAIL_ENTITY_5, SQLCONDUIT_DETAIL_ENTITY_4, SQLCONDUIT_DETAIL_ENTITY_3, SQLCONDUIT_DETAIL_ENTITY_2, SQLCONDUIT_DETAIL_ENTITY_1, unused))(__VA_ARGS__))
#define SQLCONDUIT_ENTITY(Type, tableName, ...) \
    SQLCONDUIT_MAP(Type, .table(tableName) SQLCONDUIT_DETAIL_ENTITY_FIELDS(__VA_ARGS__))
#define SQLCONDUIT_TABLE(name) .table(name)
#define SQLCONDUIT_FLAGGED_FIELD(member, column, flags) .field(&SQLConduitMappedType::member, column, flags)

#endif
