#ifndef SQLCONDUIT_SQL_BUILDER_H
#define SQLCONDUIT_SQL_BUILDER_H

#include "sqlconduit/common/types.h"
#include "sqlconduit/common/sql_dialect.h"

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sqlconduit::sql {
    namespace detail {
        template<class T>
        common::Value makeValue(T &&input) {
            using D = std::decay_t<T>;
            if constexpr (std::is_same_v<D, common::Value>) {
                return std::forward<T>(input);
            } else if constexpr (std::is_same_v<D, std::nullptr_t>) {
                return common::Value{nullptr};
            } else if constexpr (std::is_same_v<D, bool>) {
                return common::Value{input};
            } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
                return common::Value{static_cast<std::int64_t>(input)};
            } else if constexpr (std::is_integral_v<D> && std::is_unsigned_v<D>) {
                return common::Value{static_cast<std::uint64_t>(input)};
            } else if constexpr (std::is_floating_point_v<D>) {
                return common::Value{static_cast<double>(input)};
            } else if constexpr (std::is_convertible_v<T, const char *>) {
                const char *text = input;
                return common::Value{std::string(text ? text : "")};
            } else {
                return common::Value{std::forward<T>(input)};
            }
        }
    }

    enum class Operation {
        Select = 0,
        Insert = 1,
        Update = 2,
        Delete = 3
    };

    enum class CompareOperator {
        Equal = 0,
        NotEqual = 1,
        Less = 2,
        LessOrEqual = 3,
        Greater = 4,
        GreaterOrEqual = 5,
        Like = 6
    };

    enum class SortDirection {
        Ascending = 0,
        Descending = 1
    };

    struct Statement {
        std::string sql;
        common::Params params;
    };

    struct BuildResult {
        common::Status status;
        Statement statement;

        [[nodiscard]] bool ok() const { return status.ok(); }
    };

    struct FieldValue {
        std::string field;
        common::Value value{nullptr};

        FieldValue(std::string fieldName, common::Value fieldValue)
            : field(std::move(fieldName)), value(std::move(fieldValue)) {
        }

        template<class T,
                 std::enable_if_t<!std::is_same_v<std::decay_t<T>, common::Value>, int> = 0>
        FieldValue(std::string fieldName, T &&fieldValue)
            : field(std::move(fieldName)),
              value(detail::makeValue(std::forward<T>(fieldValue))) {
        }
    };

    class Condition final {
    public:
        enum class Kind {
            Invalid = 0,
            Compare = 1,
            In = 2,
            NotIn = 3,
            Between = 4,
            IsNull = 5,
            IsNotNull = 6,
            All = 7,
            Any = 8,
            Not = 9
        };

        Condition() = default;

        static Condition compare(std::string field, CompareOperator op, common::Value value);

        static Condition in(std::string field, common::Params values);

        static Condition notIn(std::string field, common::Params values);

        static Condition between(std::string field, common::Value lower, common::Value upper);

        static Condition isNull(std::string field);

        static Condition isNotNull(std::string field);

        static Condition all(std::vector<Condition> conditions);

        static Condition any(std::vector<Condition> conditions);

        static Condition negate(Condition condition);

        [[nodiscard]] Kind kind() const noexcept { return kind_; }
        [[nodiscard]] const std::string &field() const noexcept { return field_; }
        [[nodiscard]] CompareOperator compareOperator() const noexcept { return compareOperator_; }
        [[nodiscard]] const common::Params &values() const noexcept { return values_; }
        [[nodiscard]] const std::vector<Condition> &children() const noexcept { return children_; }

    private:
        explicit Condition(const Kind kind) : kind_(kind) {
        }

        Kind kind_ = Kind::Invalid;
        std::string field_;
        CompareOperator compareOperator_ = CompareOperator::Equal;
        common::Params values_;
        std::vector<Condition> children_;
    };

    template<class T>
    Condition eq(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::Equal,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition ne(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::NotEqual,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition lt(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::Less,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition le(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::LessOrEqual,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition gt(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::Greater,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition ge(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::GreaterOrEqual,
                                  detail::makeValue(std::forward<T>(value)));
    }

    template<class T>
    Condition like(std::string field, T &&value) {
        return Condition::compare(std::move(field), CompareOperator::Like,
                                  detail::makeValue(std::forward<T>(value)));
    }

    inline Condition in(std::string field, common::Params values) {
        return Condition::in(std::move(field), std::move(values));
    }

    inline Condition notIn(std::string field, common::Params values) {
        return Condition::notIn(std::move(field), std::move(values));
    }

    template<class Lower, class Upper>
    Condition between(std::string field, Lower &&lower, Upper &&upper) {
        return Condition::between(std::move(field),
                                  detail::makeValue(std::forward<Lower>(lower)),
                                  detail::makeValue(std::forward<Upper>(upper)));
    }

    inline Condition isNull(std::string field) {
        return Condition::isNull(std::move(field));
    }

    inline Condition isNotNull(std::string field) {
        return Condition::isNotNull(std::move(field));
    }

    inline Condition all(std::vector<Condition> conditions) {
        return Condition::all(std::move(conditions));
    }

    inline Condition any(std::vector<Condition> conditions) {
        return Condition::any(std::move(conditions));
    }

    inline Condition not_(Condition condition) {
        return Condition::negate(std::move(condition));
    }

    class Builder final {
    public:
        explicit Builder(Operation operation, std::string table,
                         common::util::Dialect dialect = common::util::Dialect::Auto);

        static Builder select(std::string table,
                              common::util::Dialect dialect = common::util::Dialect::Auto);

        static Builder insert(std::string table,
                              common::util::Dialect dialect = common::util::Dialect::Auto);

        static Builder update(std::string table,
                              common::util::Dialect dialect = common::util::Dialect::Auto);

        static Builder deleteFrom(std::string table,
                                  common::util::Dialect dialect = common::util::Dialect::Auto);

        Builder &columns(std::vector<std::string> fields);

        Builder &value(std::string field, common::Value value);

        template<class T,
                 std::enable_if_t<!std::is_same_v<std::decay_t<T>, common::Value>, int> = 0>
        Builder &value(std::string field, T &&input) {
            return value(std::move(field), detail::makeValue(std::forward<T>(input)));
        }

        Builder &values(std::vector<FieldValue> values);

        Builder &values(std::vector<std::string> fields, common::Params params);

        Builder &where(Condition condition);

        Builder &orderBy(std::string field, SortDirection direction = SortDirection::Ascending);

        Builder &allowAllRows(bool allow = true) noexcept;

        [[nodiscard]] BuildResult build() const;

        [[nodiscard]] Operation operation() const noexcept { return operation_; }
        [[nodiscard]] common::util::Dialect dialect() const noexcept { return dialect_; }

    private:
        struct Ordering {
            std::string field;
            SortDirection direction = SortDirection::Ascending;
        };

        Operation operation_;
        std::string table_;
        common::util::Dialect dialect_;
        std::vector<std::string> columns_;
        std::vector<FieldValue> values_;
        std::vector<Condition> conditions_;
        std::vector<Ordering> orderings_;
        std::string specificationError_;
        bool allowAllRows_ = false;
    };
}

#endif
