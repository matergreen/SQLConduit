#ifndef SQLCONDUIT_TYPED_SQL_BUILDER_H
#define SQLCONDUIT_TYPED_SQL_BUILDER_H

#include "sqlconduit/mapping.h"
#include "sqlconduit/sql_builder.h"
#include <unordered_set>

namespace sqlconduit::sql {
    namespace detail {
        template<class M, class V>
        using AcceptsMemberValue = std::enable_if_t<
            std::is_constructible_v<M, V &&> || std::is_same_v<std::decay_t<V>, common::Value>, int>;
    }

    // Uses the same mapping for predicates, projections, writes and result conversion.
    // build() deliberately returns the existing BuildResult accepted by Client/Session.
    template<class T>
    class TypedBuilder final {
        using Column = typename mapping::Mapping<T>::Column;
    public:
        explicit TypedBuilder(Operation operation, common::util::Dialect dialect = common::util::Dialect::Auto)
            : TypedBuilder(operation, mapping::mappingFor<T>().tableName(), dialect) {}

        TypedBuilder(Operation operation, std::string table,
                     common::util::Dialect dialect = common::util::Dialect::Auto)
            : builder_(operation, std::move(table), dialect) {
            std::unordered_set<std::string> names;
            for (const auto &column : mapping::mappingFor<T>().columns()) {
                if (!names.insert(column.name).second) fail("duplicate mapped column: " + column.name);
            }
            if (operation == Operation::Select)
                builder_.columns(mapping::mappingFor<T>().columnNames());
        }

        template<class... M>
        TypedBuilder &columns(M T::*...members) {
            static_assert(sizeof...(M) > 0, "typed projection requires at least one member");
            std::vector<std::string> names;
            auto add = [&](const auto member) {
                if (const auto *column = resolve(member)) names.push_back(column->name);
            };
            (add(members), ...);
            builder_.columns(std::move(names));
            return *this;
        }

#define SQLCONDUIT_DETAIL_TYPED_COMPARE(method, op) \
        template<class M, class V, detail::AcceptsMemberValue<M, V> = 0> \
        TypedBuilder &method(M T::*member, V &&value) { \
            const auto *column = resolve(member); \
            common::Value bound; \
            if (column && encode<M>(*column, std::forward<V>(value), bound)) \
                builder_.where(Condition::compare(column->name, CompareOperator::op, std::move(bound))); \
            return *this; \
        } \
        template<class M, class V, detail::AcceptsMemberValue<M, V> = 0> \
        TypedBuilder &method(bool enabled, M T::*member, V &&value) { \
            if (enabled) method(member, std::forward<V>(value)); \
            return *this; \
        }
        SQLCONDUIT_DETAIL_TYPED_COMPARE(eq, Equal)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(ne, NotEqual)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(gt, Greater)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(ge, GreaterOrEqual)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(lt, Less)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(le, LessOrEqual)
        SQLCONDUIT_DETAIL_TYPED_COMPARE(like, Like)
#undef SQLCONDUIT_DETAIL_TYPED_COMPARE

        template<class M> TypedBuilder &in(M T::*member, common::Params values) {
            return list(member, std::move(values), false);
        }
        template<class M> TypedBuilder &notIn(M T::*member, common::Params values) {
            return list(member, std::move(values), true);
        }
        template<class M> TypedBuilder &in(bool enabled, M T::*member, common::Params values) {
            if (enabled) in(member, std::move(values));
            return *this;
        }
        template<class M> TypedBuilder &notIn(bool enabled, M T::*member, common::Params values) {
            if (enabled) notIn(member, std::move(values));
            return *this;
        }
        template<class M, class L, class U,
                 detail::AcceptsMemberValue<M, L> = 0, detail::AcceptsMemberValue<M, U> = 0>
        TypedBuilder &between(M T::*member, L &&lower, U &&upper) {
            const auto *column = resolve(member);
            common::Value low, high;
            if (column && encode<M>(*column, std::forward<L>(lower), low) &&
                encode<M>(*column, std::forward<U>(upper), high))
                builder_.where(Condition::between(column->name, std::move(low), std::move(high)));
            return *this;
        }
        template<class M, class L, class U,
                 detail::AcceptsMemberValue<M, L> = 0, detail::AcceptsMemberValue<M, U> = 0>
        TypedBuilder &between(bool enabled, M T::*member, L &&lower, U &&upper) {
            if (enabled) between(member, std::forward<L>(lower), std::forward<U>(upper));
            return *this;
        }
        template<class M> TypedBuilder &isNull(M T::*member) {
            if (const auto *column = resolve(member)) builder_.where(Condition::isNull(column->name));
            return *this;
        }
        template<class M> TypedBuilder &isNotNull(M T::*member) {
            if (const auto *column = resolve(member)) builder_.where(Condition::isNotNull(column->name));
            return *this;
        }
        template<class M> TypedBuilder &isNull(bool enabled, M T::*member) {
            if (enabled) isNull(member);
            return *this;
        }
        template<class M> TypedBuilder &isNotNull(bool enabled, M T::*member) {
            if (enabled) isNotNull(member);
            return *this;
        }
        template<class M> TypedBuilder &orderBy(M T::*member, SortDirection direction = SortDirection::Ascending) {
            if (const auto *column = resolve(member)) builder_.orderBy(column->name, direction);
            return *this;
        }
        template<class M> TypedBuilder &orderByAsc(M T::*member) { return orderBy(member); }
        template<class M> TypedBuilder &orderByDesc(M T::*member) { return orderBy(member, SortDirection::Descending); }

        template<class M, class V, detail::AcceptsMemberValue<M, V> = 0>
        TypedBuilder &value(M T::*member, V &&value) {
            const auto *column = resolve(member);
            common::Value bound;
            if (column && !mapping::Mapping<T>::writable(*column)) fail("field is generated or read-only: " + column->name);
            else if (column && encode<M>(*column, std::forward<V>(value), bound))
                builder_.value(column->name, std::move(bound));
            return *this;
        }
        template<class M, class V, detail::AcceptsMemberValue<M, V> = 0>
        TypedBuilder &value(bool enabled, M T::*member, V &&value) {
            if (enabled) this->value(member, std::forward<V>(value));
            return *this;
        }
        // Explicit escape hatch for grouped/dialect-specific conditions using existing Condition APIs.
        TypedBuilder &where(Condition condition) { builder_.where(std::move(condition)); return *this; }
        TypedBuilder &allowAllRows(bool allow = true) { builder_.allowAllRows(allow); return *this; }
        [[nodiscard]] BuildResult build() const {
            if (!status_.ok()) return {status_, {}};
            return builder_.build();
        }

    private:
        void fail(std::string message) {
            if (status_.ok()) status_ = common::Status::error(common::ErrorCode::MappingError, std::move(message));
        }
        template<class M> const Column *resolve(M T::*member) {
            const auto *column = mapping::mappingFor<T>().columnFor(member);
            if (!column) fail("null, unmapped or ambiguously mapped entity member");
            return column;
        }
        template<class M, class V> bool encode(const Column &column, V &&value, common::Value &out) {
            common::Value raw;
            using D = std::decay_t<V>;
            if constexpr (std::is_constructible_v<common::Value, V &&> || std::is_arithmetic_v<D> ||
                          std::is_convertible_v<V, const char *>)
                raw = detail::makeValue(std::forward<V>(value));
            else
                raw = mapping::ValueConverter<M>::toValue(M(std::forward<V>(value)));
            M converted{};
            const auto status = mapping::ValueConverter<M>::fromValue(raw, converted, column.flags);
            if (!status.ok()) { fail("invalid value for mapped column: " + column.name); return false; }
            out = mapping::ValueConverter<M>::toValue(converted);
            return true;
        }
        template<class M> TypedBuilder &list(M T::*member, common::Params values, bool negate) {
            const auto *column = resolve(member);
            if (!column) return *this;
            for (auto &value : values) {
                common::Value converted;
                if (!encode<M>(*column, value, converted)) return *this;
                value = std::move(converted);
            }
            builder_.where(negate ? Condition::notIn(column->name, std::move(values)) :
                                    Condition::in(column->name, std::move(values)));
            return *this;
        }
        Builder builder_;
        common::Status status_;
    };

#define SQLCONDUIT_DETAIL_TYPED_FACTORY(method, operation) \
    template<class T> TypedBuilder<T> method(common::util::Dialect dialect = common::util::Dialect::Auto) { \
        return TypedBuilder<T>(Operation::operation, dialect); \
    } \
    template<class T> TypedBuilder<T> method(std::string table, common::util::Dialect dialect = common::util::Dialect::Auto) { \
        return TypedBuilder<T>(Operation::operation, std::move(table), dialect); \
    }
    SQLCONDUIT_DETAIL_TYPED_FACTORY(select, Select)
    SQLCONDUIT_DETAIL_TYPED_FACTORY(insert, Insert)
    SQLCONDUIT_DETAIL_TYPED_FACTORY(update, Update)
    SQLCONDUIT_DETAIL_TYPED_FACTORY(deleteFrom, Delete)
#undef SQLCONDUIT_DETAIL_TYPED_FACTORY
}
#endif
