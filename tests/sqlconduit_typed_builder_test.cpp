#include "sqlconduit/typed_sql_builder.h"
#include <iostream>
#include <limits>

struct TypedUser {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    std::optional<std::string> nickname;
    bool active = false;
    int unmapped = 0;
};
SQLCONDUIT_ENTITY(TypedUser, "app.users", id, name, age, nickname, active)
struct OtherUser { int age = 0; };
namespace registered_example { struct One { int id = 0; }; }
SQLCONDUIT_ENTITY(registered_example::One, "one", id)
struct WideEntity { int f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0, f6 = 0, f7 = 0, f8 = 0, f9 = 0, f10 = 0, f11 = 0, f12 = 0, f13 = 0, f14 = 0, f15 = 0, f16 = 0, f17 = 0, f18 = 0, f19 = 0, f20 = 0, f21 = 0, f22 = 0, f23 = 0, f24 = 0, f25 = 0, f26 = 0, f27 = 0, f28 = 0, f29 = 0, f30 = 0, f31 = 0, f32 = 0; };
SQLCONDUIT_ENTITY(WideEntity, "wide", f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32)
struct AliasUser { std::int64_t id = 0; std::string name; };
SQLCONDUIT_MAP(AliasUser,
    SQLCONDUIT_TABLE("users")
    SQLCONDUIT_FLAGGED_FIELD(id, "user_id", sqlconduit::mapping::FieldFlags::PrimaryKey |
                            sqlconduit::mapping::FieldFlags::Generated)
    SQLCONDUIT_NAMED_FIELD(name, "display_name"))
struct LegacyUser { int id = 0; };
SQLCONDUIT_MAP(LegacyUser, SQLCONDUIT_FIELD(id))
struct DuplicateUser { int a = 0; int b = 0; };
SQLCONDUIT_MAP(DuplicateUser, SQLCONDUIT_TABLE("users")
    SQLCONDUIT_NAMED_FIELD(a, "id") SQLCONDUIT_NAMED_FIELD(b, "id"))
struct AmbiguousUser { int a = 0; };
SQLCONDUIT_MAP(AmbiguousUser, SQLCONDUIT_TABLE("users")
    SQLCONDUIT_NAMED_FIELD(a, "a") SQLCONDUIT_NAMED_FIELD(a, "b"))

template<class B, class P, class V, class = void> struct CanEq : std::false_type {};
template<class B, class P, class V>
struct CanEq<B, P, V, std::void_t<decltype(std::declval<B &>().eq(std::declval<P>(), std::declval<V>()))>>
    : std::true_type {};
static_assert(CanEq<sqlconduit::sql::TypedBuilder<TypedUser>, int TypedUser::*, int>::value);
static_assert(!CanEq<sqlconduit::sql::TypedBuilder<TypedUser>, int OtherUser::*, int>::value);
static_assert(!CanEq<sqlconduit::sql::TypedBuilder<TypedUser>, int TypedUser::*, std::string>::value);
static_assert(!CanEq<sqlconduit::sql::TypedBuilder<TypedUser>, std::string TypedUser::*, int>::value);

namespace {
    void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
}

int main() {
    using namespace sqlconduit;
    try {
        require(mapping::mappingFor<registered_example::One>().size() == 1 &&
                mapping::mappingFor<WideEntity>().size() == 32, "entity macro supports one and 32 members and qualified types");
        auto query = sql::select<TypedUser>().eq(&TypedUser::active, true)
            .ge(false, &TypedUser::age, 99).ge(&TypedUser::age, 18)
            .like(&TypedUser::name, "%Ada%")
            .in(&TypedUser::id, {std::int64_t{1}, std::int64_t{2}})
            .isNull(&TypedUser::nickname).orderByDesc(&TypedUser::id).build();
        require(query.ok(), "typed select builds");
        require(query.statement.sql == "SELECT \"id\", \"name\", \"age\", \"nickname\", \"active\" FROM \"app\".\"users\" WHERE \"active\" = ? AND \"age\" >= ? AND \"name\" LIKE ? AND \"id\" IN (?, ?) AND \"nickname\" IS NULL ORDER BY \"id\" DESC",
                "typed select uses mapping and deterministic parameter order");
        require(query.statement.params == common::Params{true, std::int64_t{18}, std::string("%Ada%"), std::int64_t{1}, std::int64_t{2}},
                "conditions remain bound values");
        auto projected = sql::select<AliasUser>(common::util::Dialect::MySQL)
            .columns(&AliasUser::name, &AliasUser::id).eq(&AliasUser::id, 42).build();
        require(projected.ok() && projected.statement.sql ==
            "SELECT `display_name`, `user_id` FROM `users` WHERE `user_id` = ?", "aliases and dialect reused");
        auto update = sql::update<AliasUser>().value(&AliasUser::name, "O'Brien").eq(&AliasUser::id, 42).build();
        require(update.ok() && update.statement.sql == "UPDATE \"users\" SET \"display_name\" = ? WHERE \"user_id\" = ?" &&
            update.statement.params == common::Params{std::string("O'Brien"), std::int64_t{42}}, "typed update binds values");
        require(!sql::update<AliasUser>().value(&AliasUser::id, 42).eq(&AliasUser::id, 1).build().ok(),
                "generated field assignment rejected");
        require(sql::insert<AliasUser>().value(&AliasUser::name, "Ada").build().ok(), "typed insert");
        require(!sql::update<TypedUser>().value(&TypedUser::name, "Ada").eq(false, &TypedUser::id, 42).build().ok(),
                "skipped conditions cannot bypass full-table update protection");
        require(!sql::deleteFrom<TypedUser>().eq(false, &TypedUser::id, 42).build().ok(), "full-table delete protected");
        require(sql::deleteFrom<TypedUser>().allowAllRows().build().ok(), "explicit full-table opt-in retained");
        require(sql::deleteFrom<TypedUser>().between(&TypedUser::age, 18, 30).build().ok(), "typed between");
        require(sql::select<TypedUser>().ne(&TypedUser::age, 1).gt(&TypedUser::age, 2).lt(&TypedUser::age, 99)
            .le(&TypedUser::age, 90).notIn(&TypedUser::age, {3, 4}).isNotNull(&TypedUser::nickname)
            .orderByAsc(&TypedUser::age).build().ok(), "all comparison/list/null methods build");
        int TypedUser::*nullMember = nullptr;
        require(!sql::select<TypedUser>().eq(nullMember, 1).build().ok(), "null member rejected");
        require(!sql::select<TypedUser>().eq(&TypedUser::unmapped, 1).build().ok(), "unmapped member rejected");
        require(!sql::select<DuplicateUser>().build().ok(), "duplicate mapped columns rejected");
        require(!sql::select<AmbiguousUser>().eq(&AmbiguousUser::a, 1).build().ok(), "ambiguous member rejected");
        auto overflow = sql::select<TypedUser>().eq(&TypedUser::age, std::numeric_limits<std::uint64_t>::max()).build();
        require(!overflow.ok() && overflow.statement.sql.empty() && overflow.statement.params.empty(), "overflow rejected atomically");
        require(!sql::select<TypedUser>().eq(&TypedUser::age, 1.5).build().ok(), "no implicit narrowing");
        require(!sql::select<TypedUser>().in(&TypedUser::age, {1, std::string("wrong")}).build().ok(), "IN list validates every value");
        require(sql::select<TypedUser>().eq(&TypedUser::nickname, std::nullopt).build().ok(), "optional NULL binding");
        require(!sql::select<LegacyUser>().build().ok(), "missing table not guessed");
        require(sql::select<LegacyUser>("users").eq(&LegacyUser::id, 1).build().ok(), "old mappings accept explicit table");
        auto reusable = sql::select<TypedUser>().eq(&TypedUser::id, 1);
        require(reusable.build().statement.params == reusable.build().statement.params, "build is reusable and read-only");
        common::Row row; row.set("id", std::int64_t{1}); row.set("name", std::string("Ada"));
        row.set("age", std::int64_t{20}); row.set("nickname", nullptr); row.set("active", true);
        TypedUser entity;
        require(mapping::fromRow(row, entity).ok() && entity.age == 20 && !entity.nickname, "one-line metadata maps query rows");
        common::Row missing; missing.set("id", std::int64_t{1});
        require(!mapping::fromRow(missing, entity).ok(), "one-line mapping remains strict");
        std::cout << "Typed Builder, entity macros, strict binding and safety checks passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
