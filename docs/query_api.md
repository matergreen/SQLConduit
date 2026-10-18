# Direct query results (since 1.2)

These additions require SQLConduit 1.2 or later.
Existing output-parameter APIs and free `queryAs` / `queryOneAs` helpers remain supported.

## Map an entity once

Include `sqlconduit/mapping.h` or the full `sqlconduit/sqlconduit.h` umbrella for typed methods.
No ORM inheritance or vendor headers are required.
C++17 cannot reflect field names: register the mapping once, then reuse it for every query.

```cpp
#include <sqlconduit/mapping.h>

struct User {
    std::int64_t id = 0;
    std::string name;
    std::optional<std::string> nickname;
};

namespace sqlconduit::mapping {
    template<> struct RowMapper<User> {
        static Mapping<User> describe() {
            return Mapping<User>().field(&User::id, "id").field(&User::name, "name")
                .field(&User::nickname, "nickname").missingColumns(MissingColumns::Error);
        }
    };
}
```

After registering the driver and initializing a `Client`, obtain usable objects directly:

```cpp
auto users = client.query<User>("SELECT id, name, nickname FROM users WHERE active=?", {true});
if (!users.ok()) return users.status;
for (const auto &user : users.items) { /* user.id, user.name, user.nickname */ }

auto user = client.queryOneNamed<User>(
    "SELECT id, name, nickname FROM users WHERE id=:id", {{"id", 42}});
if (!user.ok()) return user.status;
if (user.value) { /* user.value->name; no ResultSet conversion needed */ }

auto count = client.queryScalar<std::int64_t>("SELECT COUNT(*) FROM users");
if (!count.ok()) return count.status;
if (count.value) { /* *count.value */ }
```

Database errors are returned in `status`, including SQLSTATE/native code. Do not consume an error's
empty container as if it were a successful empty query. Built-in conversion errors return
`MappingError`; no partial entity list is returned. Custom mapper/converter exceptions are not
silently converted to defaults (synchronous calls propagate; async futures hold the exception).
Mapping column policies are inherited from `RowMapper`; use `MissingColumns::Error` when incomplete
projections must fail. SQL aliases must match mapped column names.

## Method inventory

Every method below supports `(sql, params)` on the default/contextual source and
`(source, sql, params)` on a named source, retaining existing `ContextScope` routing precedence.
`params` defaults to empty only on the first form.

| Method | Result | Async variant |
| --- | --- | --- |
| `query(sql, Params)` | `common::QueryResult` (`status`, `rows`, `ok()`) | Existing `queryAsync` |
| `query<T>` | `EntityResult<T>` (`status`, `items`, `ok()`) | `queryAsync<T>` |
| `queryOne<T>` | `EntityOne<T>` (`status`, `optional<T> value`, `ok()`) | `queryOneAsync<T>` |
| `queryScalar<T>` | `ScalarResult<T>` (`status`, `value`, `hasRow`, `isNull`, `ok()`) | `queryScalarAsync<T>` |
| `queryNamed` / `queryNamed<T>` | Raw rows / entity list, with `NamedParams` | `queryNamedAsync` / `queryNamedAsync<T>` |
| `queryOneNamed<T>` / `queryScalarNamed<T>` | Single entity / scalar, with `NamedParams` | `queryOneNamedAsync<T>` / `queryScalarNamedAsync<T>` |
| `executeNamed` | `common::ExecuteResult` (`status`, `affected`, `ok()`) | `executeNamedAsync` |

For example, `client.queryOneNamed<User>("reporting", sql, {{"id", 42}})` targets a source by name.
For raw results, include only `sqlconduit/client.h` and use `client.query(sql)` or
`client.queryNamed(sql, params)`; no entity mapping is needed.

### Empty, NULL, cardinality and conversion

- `query<T>`: zero rows is success with an empty vector.
- `queryOne<T>`: zero rows is success with an empty optional; more than one row is an error.
- `queryScalar<T>`: at most one row and one column. Zero rows: `hasRow=false`, `isNull=false`.
  One SQL NULL: `hasRow=true`, `isNull=true`, empty `value`. A concrete value sets `value`.
  Multiple rows/columns, integer overflow or incompatible types are errors, not truncation.
- Nullable entity fields use `std::optional<T>`. A NULL assigned to a non-nullable field is an error
  under the existing converter's rules. Custom `ValueConverter<T>` specializations are reused.
- These are materialized queries, not streaming APIs. Large results still use `queryEach` / cursors.

## Named binding

```cpp
auto users = client.queryNamed<User>(
    "SELECT id, name, nickname FROM users WHERE tenant_id=:tenant AND id>=:first",
    {{"first", 100}, {"tenant", "acme"}}); // caller order does not matter
auto updated = client.executeNamed("UPDATE users SET name=:name WHERE id=:id",
                                   {{"id", 42}, {"name", "Ada"}});
```

Names are case-sensitive ASCII `[A-Za-z_][A-Za-z0-9_]*`, without the leading colon in `NamedParams`.
Repeated placeholders reuse the same value; duplicate input names, missing and unused values fail
before execution. Strings, quoted identifiers, dollar literals, Oracle alternative literals,
nested block comments, line comments and PostgreSQL `::` casts are not rewritten as parameters.
Values are copied/owned and passed through the existing `?` binding pipeline for each driver;
they never become executable SQL text. Parameters represent values, not identifiers or SQL fragments.

Use explicit `*Named*` methods rather than an ambiguous overload of positional `Params`; existing
calls with `{}` retain their meaning. Do not mix `?`, `$1`, `:1` and named placeholders.
This portable lexer is not a complete dialect parser: hash comments/operators are rejected because
their meanings differ by database, and raw `?` operators are reserved for positional binding.
Use portable `--` comments and function equivalents such as PostgreSQL `jsonb_exists` where needed.
Session supports the same synchronous raw/typed/named helpers on its existing connection. For a
custom adapter, `common::bindNamed` exposes the validated rewrite without execution.

## Async without a waiting thread

```cpp
auto pending = client.queryOneNamedAsync<User>(
    "SELECT id, name, nickname FROM users WHERE id=:id", {{"id", 42}});
// Do other work. Do not call get() on an executor/completion callback.
auto user = pending.get();
if (!user.ok()) return user.status;
if (user.value) { /* user.value->name */ }
```

Mapping occurs at query completion, without `std::async`, a detached waiting thread, or a worker
blocked on another future. `mode` preserves `Native` versus `CompatibilityFallback`; a typed helper
does not make an unsupported driver/statement native. SQL and parameters are owned while in flight.
Synchronous typed results do not use `mode` to claim asynchronous execution. Custom mapping should
be quick and must not wait for another request or reload/shutdown the same Client from completion.

## Builder, sessions, columns and shorter mappings

```cpp
SQLCONDUIT_MAP(User,
    SQLCONDUIT_FIELD(id)
    SQLCONDUIT_FIELD(name)
    SQLCONDUIT_FIELD(nickname))
```

Define this at global namespace scope, once per entity (normally in a shared header), instead of
the explicit `RowMapper<User>` specialization above, not in addition to it. Use
`SQLCONDUIT_NAMED_FIELD(member, "column_alias")` for different names. This shorthand requires every
mapped column; retain explicit mappings for custom missing-column policies or converters.

```cpp
auto built = sqlconduit::sql::Builder::select("users").columns({"id", "name", "nickname"})
    .where(sqlconduit::sql::eq("id", 42)).build();
auto user = client.queryOne<User>(built); // also accepts a sql::Statement
auto pending = client.queryOneAsync<User>(built);
auto names = client.queryColumn<std::string>("SELECT name FROM users");
auto nicknames = client.queryColumn<std::optional<std::string>>("SELECT nickname FROM users");
auto status = client.withSession([&](sqlconduit::core::Session &session) {
    auto row = session.queryOne<User>(built);
    if (!row.ok()) return row.status;
    return session.executeNamed("UPDATE users SET name=:name WHERE id=:id",
                                {{"id", 42}, {"name", "Ada"}}).status;
});
```

Client and Session raw query/execute and typed query/one/scalar/column accept `BuildResult` or
`Statement`; Client async variants do too. A failed build returns its error without executing SQL.
Session calls do not borrow a second connection, commit, or start a new transaction; Session stays
synchronous. Use the existing transaction callback to obtain transactional behavior.

`queryColumn<T>` / `queryColumnNamed<T>` and their Client async variants return `EntityResult<T>`:
exactly one projected column, any number of rows, strict conversion, no partial list on failure.
Use `optional<T>` for nullable column values; NULL into a non-nullable type is an error.

All result types expose `ok()`. Status checking remains the default; exception-oriented callers
may explicitly opt into `rowsOrThrow()`, `itemsOrThrow()` or `valueOrThrow()` (typed single/scalar).
These throw `common::StatusError` with `status()` containing the original structured failure;
single/scalar access also throws `MappingError` for no value/NULL, never a default object.

```cpp
auto names = client.queryColumn<std::string>("SELECT name FROM users").itemsOrThrow();
auto user = client.queryOneNamed<User>(sql, {{"id", 42}}).valueOrThrow();
```

Rvalue accessors move values out; references from lvalue accessors must not outlive their result.

## Member-pointer Builder and one-line entities

Include `sqlconduit/typed_sql_builder.h` or the full `sqlconduit/sqlconduit.h` umbrella.
Register an entity once at global namespace scope (usually in a shared header):

```cpp
struct Account {
    std::int64_t id = 0;
    std::string name;
    int age = 0;
    std::optional<std::string> nickname;
};
SQLCONDUIT_ENTITY(Account, "accounts", id, name, age, nickname)

std::string nameFilter = "Ada";
auto statement = sqlconduit::sql::select<Account>()
    .ge(&Account::age, 18)
    .like(&Account::name, "%Ada%")
    .eq(!nameFilter.empty(), &Account::name, nameFilter)
    .orderByDesc(&Account::id)
    .build();
auto accounts = client.query<Account>(statement);
if (!accounts.ok()) return accounts.status;
```

`SQLCONDUIT_ENTITY` supports 1-32 public members with column names matching member names. It
generates the same strict RowMapper used by raw SQL and typed queries, plus table/member metadata.
Do not combine it with another mapping specialization for the same entity. C++17 reflection,
getter/lambda parsing, implicit table-name inference and automatic camelCase conversion are not used.
For more than 32 fields or special policies, use an explicit Mapping or the field-chain macro.

Advanced mappings keep real column names and flags in one place:

```cpp
// Alternative registration for Account; use this OR SQLCONDUIT_ENTITY, never both.
SQLCONDUIT_MAP(Account,
    SQLCONDUIT_TABLE("accounts")
    SQLCONDUIT_FLAGGED_FIELD(id, "account_id",
        sqlconduit::mapping::FieldFlags::PrimaryKey | sqlconduit::mapping::FieldFlags::Generated)
    SQLCONDUIT_NAMED_FIELD(name, "display_name")
    SQLCONDUIT_FIELD(age)
    SQLCONDUIT_FIELD(nickname))

auto changed = sqlconduit::sql::update<Account>()
    .value(&Account::name, "Ada").eq(&Account::id, 42).build();
auto result = client.execute(changed);
```

Explicit `Mapping<Account>().table("accounts").field(...)` remains available. Old mappings
without a table may use `sql::select<Account>("accounts")` (also insert/update/deleteFrom).
There is no guessed table name. Never accept table names from untrusted callers as authorization.

| Typed Builder methods | Behavior |
| --- | --- |
| `select<T>()`, `insert<T>()`, `update<T>()`, `deleteFrom<T>()` | Read mapped table; optional explicit table and/or identifier dialect |
| `columns(&T::a, &T::b)` | Explicit projection; default SELECT includes all mapped columns |
| `eq`, `ne`, `gt`, `ge`, `lt`, `le`, `like` | `(member, value)` or `(enabled, member, value)` |
| `in`, `notIn` | `(member, common::Params)` or `(enabled, member, values)` |
| `between` | `(member, lower, upper)` or `(enabled, member, lower, upper)` |
| `isNull`, `isNotNull` | `(member)` or `(enabled, member)` |
| `orderByAsc`, `orderByDesc`, `orderBy` | Member-pointer ordering; `orderBy` accepts SortDirection |
| `value` | Bound INSERT/UPDATE assignment; optional leading `enabled` |
| `where(Condition)`, `allowAllRows()` | Existing structured-condition escape hatch and explicit full-table write opt-in |
| `build()` | Existing BuildResult; Client/Session, async and withOptions APIs need no new execution path |

Methods are AND-combined; grouped predicates can use existing `sql::all/any/not_` with `where`.
The escape hatch takes string column names and does not add member-pointer validation.
Conditional methods skip resolution/binding when disabled, but C++ still evaluates their arguments:
use `filter.value_or(defaultValue)`, not `*filter` on an empty optional. Conditions that are all
skipped do NOT bypass existing UPDATE/DELETE full-table guards.

Wrong entity members and clearly incompatible value types are rejected at compile time.
Unregistered/null/ambiguous members and duplicate column mappings fail `build()` with MappingError.
Values use existing ValueConverter rules (including flags/custom converters); numeric overflow
or narrowing is an error, not an implicit cast. `common::Value` and IN lists are checked at runtime.
Generated/read-only fields may be selected/filtered but not assigned by typed `value`.
Conversion exceptions still propagate; these APIs do not hide custom converter exceptions.

Typed SELECT does not imply projection-specific entity mappings: querying a partial projection as
the full strict entity still fails on missing columns. Use a dedicated DTO mapping, raw query or
`queryColumn<T>` for partial results. Existing string Builders, joins via hand-written SQL and
explicit mappings remain supported. This is a lightweight SQL convenience layer, not an ORM.

## Per-call deadlines and cancellation

Typed pagination, optional totals and Builder-generated COUNT queries are covered in
[Pagination](pagination.md). Include `query_page.h` or the full umbrella.

```cpp
sqlconduit::common::CancellationSource cancel;
sqlconduit::common::OperationOptions options;
options.timeout = std::chrono::milliseconds(250);
options.cancellation = cancel.token();
// Optional: options.dataSource = "reporting";
// Optional: options.allowCompatibilityFallback = false; // native async only
auto pending = client.withOptions(options).queryOneNamedAsync<User>(
    "SELECT id, name, nickname FROM users WHERE id=:id", {{"id", 42}});
cancel.requestCancellation(); // idempotent, may also be called by another thread
auto user = pending.get();
if (!user.ok()) { /* inspect user.status.code and user.status.outcomeUnknown */ }
```

`withOptions` supports raw/typed/named query, column, Builder and execute methods. Session supports
their synchronous forms. A view is non-owning: keep its Client/Session alive. Reusing a view creates
a fresh timeout for each call. `timeout=0` adds no relative limit; negative values fail validation.
An optional steady-clock `deadline` is absolute. Nested scopes take the earlier parent/child
deadline and inherit cancellation. `OperationScope` is also available for explicit scoped context.
Source selection follows existing context routing precedence; Session cannot change its connection
by setting `dataSource`. These options do not add cancellable cursor/transaction lifecycle APIs.

The request budget covers scheduling, pool waiting, retry/backoff and SQL execution. A stopped
request does not issue another retry. `allowCompatibilityFallback=false` rejects unsupported async
execution with `NotSupported`; it does not prohibit synchronous calls.

Cancellation is cooperative, not a guarantee of server-side rollback. Synchronous calls cannot
preempt an SDK connect or an uncancellable blocking call. Async futures can complete before the
driver does; the connection remains owned until physical completion and in-progress cancel cleanup,
and stopped in-flight connections are discarded. Driver cancellation runs on a shared bounded
worker pool, best effort under saturation, never on the shared deadline clock. Custom conversion
work is not preempted. Existing driver/configured limits can still fire earlier.

For a write that has started, an early timeout/cancel result sets `status.outcomeUnknown=true` and
is not retryable: the server may already have committed. Never interpret cancellation as rollback
or blindly replay it. A synchronous write with confirmed success retains that success, even if its
budget elapsed during the blocking call. A cancelled pool wait has no such write ambiguity.
The flag is specifically for these per-call stop results, not an exhaustive classifier of all
possible driver/commit failures; retain existing ambiguous-write handling for other errors.

## Configuration starter and safe preview

```sh
sqlconduitctl init postgres > datasource.yaml
sqlconduitctl init mysql --format json > datasource.json
# Also supported: odbc (SQL Server Driver 18 starter), oracle (FREEPDB1 starter).
# Set SQLCONDUIT_DB_PASSWORD securely in your environment; no password is generated or printed.
sqlconduitctl validate datasource.yaml
sqlconduitctl doctor datasource.yaml --connect
sqlconduitctl reload current.yaml candidate.yaml --dry-run
```

`init` writes a template to stdout, not a file. Shell redirection can overwrite a file: choose a new
path. Replace sample endpoints/users/database/service and configure TLS before production.
`pool.min=0` avoids eager warmup; it is not proof that the database is reachable.

Dry-run loads and validates both files offline, detects secret rotations, and returns only change
operations/paths, source additions/removals and pool impact. No credential values or hashes appear.
It does not apply anything, contact a running application, test connectivity or reserve a topology.
The current reload implementation rebuilds **all** candidate pools and retires all old pools, even
when only one setting changes. The grace period is supplied to the real `Client::reload` call;
do not assume unlimited request completion or automatic replay. Candidate construction failure
preserves the prior topology. Previewing a file cannot see runtime `add/removeDataSource` changes.

`heartbeat_interval_ms: 0` now explicitly disables periodic heartbeat; positive intervals keep it on.
