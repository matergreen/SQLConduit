#include "sqlconduit/mapping.h"
#include "sqlconduit/typed_sql_builder.h"
#include "sqlconduit/driver/idriver.h"
#include "sqlconduit/core/heartbeat_manager.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace sqlconduit;

struct QueryUser { std::int64_t id = 0; std::string name; std::optional<std::string> nickname; };
struct CompactUser { std::int64_t id = 0; std::string name; };
struct RegisteredUser { std::int64_t id = 0; std::string name; std::optional<std::string> nickname; };
SQLCONDUIT_ENTITY(RegisteredUser, "users", id, name, nickname)
SQLCONDUIT_MAP(CompactUser, SQLCONDUIT_FIELD(id) SQLCONDUIT_NAMED_FIELD(name, "name"))
namespace sqlconduit::mapping {
    template<> struct RowMapper<QueryUser> {
        static Mapping<QueryUser> describe() {
            return Mapping<QueryUser>().field(&QueryUser::id, "id").field(&QueryUser::name, "name")
                .field(&QueryUser::nickname, "nickname").missingColumns(MissingColumns::Error);
        }
    };
}

namespace {
    void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
    std::atomic<int> calls{0};
    std::atomic<bool> delay{false};
    std::mutex pendingMutex;
    std::function<void()> pending;
    std::function<common::Status()> pingHook;

    class Connection final : public core::IDatabaseConnection {
    public:
        common::Status connect(const config::DataSourceConfig &cfg) override {
            open = true; native = cfg.database == "native"; return {};
        }
        common::Status ping() override { return pingHook ? pingHook() : common::Status{}; }
        bool supportsParams() const override { return true; }
        core::AsyncCapability asyncCapability() const override {
            return native ? core::AsyncCapability::Native : core::AsyncCapability::ThreadPoolFallback;
        }
        common::Status query(const std::string &sql, common::ResultSet &out) override {
            return query(sql, {}, out);
        }
        common::Status query(const std::string &sql, const common::Params &params, common::ResultSet &out) override {
            ++calls;
            out.clear();
            if (sql.find("error") != std::string::npos)
                return common::Status::databaseError(common::ErrorCode::QueryError, "test failure", "42000", 7);
            const bool scalar = sql.find("scalar") != std::string::npos;
            out.setFields(scalar ? std::vector<std::string>{"value"} : std::vector<std::string>{"id", "name", "nickname"});
            if (sql.find("empty") != std::string::npos) return {};
            common::Row row;
            if (scalar) row.set("value", sql.find("null") != std::string::npos ? common::Value{nullptr} :
                sql.find("bad") != std::string::npos ? common::Value{std::string("wrong")} :
                params.empty() ? common::Value{std::int64_t{42}} : params.front());
            else {
                row.set("id", sql.find("bad") != std::string::npos ? common::Value{std::string("wrong")} :
                    params.empty() ? common::Value{std::int64_t{42}} : params.front());
                row.set("name", std::string("Ada")); row.set("nickname", nullptr);
            }
            out.addRow(row);
            if (sql.find("many") != std::string::npos) out.addRow(row);
            return {};
        }
        bool queryAsync(const std::string &sql, const common::Params &params, AsyncQueryCompletion completion) override {
            if (!native) return false;
            auto work = [this, sql, params, completion = std::move(completion)]() mutable {
                common::ResultSet rows;
                auto status = query(sql, params, rows);
                completion(std::move(status), std::move(rows));
            };
            if (delay) { std::lock_guard<std::mutex> lock(pendingMutex); pending = std::move(work); }
            else work();
            return true;
        }
        common::Status execute(const std::string &, std::int64_t &affected) override { ++calls; affected = 1; return {}; }
        common::Status execute(const std::string &sql, const common::Params &, std::int64_t &affected) override {
            return execute(sql, affected);
        }
        common::Status begin() override { return {}; }
        common::Status commit() override { return {}; }
        common::Status rollback() override { return {}; }
        void close() override { open = false; }
        bool isOpen() const override { return open; }
        bool inTransaction() const override { return false; }
        using IDatabaseConnection::replacePlaceholders;
    private:
        bool open = false, native = true;
    };
    class Driver final : public driver::IDriver {
    public:
        const char *name() const override { return "query-test"; }
        std::unique_ptr<core::IDatabaseConnection> createConnection() override { return std::make_unique<Connection>(); }
    };
    void init(Client &client, bool native = true) {
        require(client.addDriver({"query-test", [] { return std::make_unique<Driver>(); }}).ok(), "register mock");
        config::GlobalConfig cfg;
        cfg.default_datasource = "db"; cfg.pool.min = 0; cfg.pool.max = 1;
        cfg.heartbeat_interval_ms = 0; cfg.async.threads = 1;
        config::DataSourceConfig ds; ds.name = "db"; ds.type = "query-test";
        ds.database = native ? "native" : "fallback"; cfg.datasources.push_back(ds);
        require(client.init(cfg).ok(), "init mock");
    }
    template<class T> T await(std::future<T> future) {
        require(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "async completion must not deadlock");
        return future.get();
    }

    void heartbeatHandoff(bool alive) {
        config::DataSourceConfig cfg; cfg.name = "heartbeat-test";
        auto pool = std::make_shared<core::ConnectionPool>(std::make_unique<Driver>(), cfg, 1, 1,
            std::chrono::milliseconds(2000), std::chrono::milliseconds(0), std::chrono::milliseconds(0),
            std::chrono::milliseconds(0), std::chrono::milliseconds(0));
        std::promise<void> entered, resume;
        auto enteredFuture = entered.get_future();
        auto resumeFuture = resume.get_future().share();
        pingHook = [&] {
            entered.set_value();
            resumeFuture.wait();
            return alive ? common::Status{} : common::Status::error(common::ErrorCode::PingFailed, "test disconnect");
        };
        std::thread heartbeat([&] { pool->healthCheck(); });
        const bool enteredPing = enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        std::promise<std::unique_ptr<core::ConnectionPool::Handle>> result;
        auto future = result.get_future();
        core::AsyncIo io;
        io.post = io.deliver = [](std::function<void()> task) { task(); return true; };
        io.postAfter = [](std::function<void()>, std::chrono::milliseconds) {};
        if (enteredPing) pool->borrowAsync(std::chrono::milliseconds(2000), io,
            [&](std::unique_ptr<core::ConnectionPool::Handle> handle, common::Status status) {
                if (status.ok()) result.set_value(std::move(handle));
                else result.set_value(nullptr);
            });
        resume.set_value();
        heartbeat.join();
        pingHook = {};
        require(enteredPing, "heartbeat entered gated validation");
        auto handle = await(std::move(future));
        require(static_cast<bool>(handle), "heartbeat wakes borrower after restore or replacement");
        handle.reset();
        pool->shutdown();
    }
}

int main() {
    try {
        const auto bound = common::bindNamed("SELECT :id, :id::bigint, ':skip', \"a:skip\", `:skip`, [:skip], "
            "$tag$:skip ?$tag$, q'[it's :skip ?]', :name /* :skip /* :skip */ :skip */ -- :skip\n",
            {{"name", std::string("'; DROP TABLE users;--")}, {"id", std::int64_t{7}}});
        require(bound.ok() && bound.params.size() == 3 && bound.sql.find("DROP TABLE") == std::string::npos,
            "named binding handles literals, comments, repeated parameters without interpolation");
        require(std::get<std::int64_t>(bound.params[0]) == 7 && std::get<std::int64_t>(bound.params[1]) == 7,
            "parameters follow placeholder order, not caller order");
        std::size_t count = 0;
        (void) Connection::replacePlaceholders(bound.sql, [](std::size_t) { return "X"; }, count);
        require(count == 3, "downstream driver lexer agrees with named binder");
        auto array = common::bindNamed("SELECT values[:limit], :array[:index], [identifier:skip]", {{"limit", 2}, {"array", 3}, {"index", 1}});
        require(array.ok(), "array subscripts are not bracket identifiers");
        (void) Connection::replacePlaceholders(array.sql, [](std::size_t) { return "X"; }, count);
        require(count == 3, "downstream lexer binds array subscripts");
        // Span copying must preserve every non-placeholder byte, including incomplete
        // literals/comments. The replacement scanner is not a SQL syntax validator.
        for (const auto &literal : {"", "plain SQL", "'it''s ?'", "\"a\"\"?\"", "`a``?`", "[a]]?]",
             "q'[it's ?]'", "Q'{it's ?}'", "q'(it's ?)'", "q'<it's ?>'", "q'!it's ?!'",
             "$tag$' ? $tag$", "$$?$$", "/* ? /* nested ? */ ? */", "-- ?\n",
             "'unterminated ?", "\"unterminated ?", "/* unterminated ?", "[unterminated ?",
             "-", "/", "q", "$", "'escaped \\' ?'"}) {
            std::size_t visited = 0;
            const auto rewritten = Connection::replacePlaceholders(literal, [&](std::size_t) { ++visited; return "X"; }, count);
            require(rewritten == literal && count == 0 && visited == 0, "scanner preserves opaque spans byte-for-byte");
        }
        const auto mixed = Connection::replacePlaceholders("? 'literal ?' ?? -- ?\n? /* ? */ q'[?]' ?",
            [](std::size_t index) { return "<" + std::to_string(index) + ">"; }, count);
        require(mixed == "<0> 'literal ?' <1><2> -- ?\n<3> /* ? */ q'[?]' <4>" && count == 5,
                "replacement spans preserve ordering and adjacent placeholders");
        const std::string binarySql("a\0?b", 4);
        require(Connection::replacePlaceholders(binarySql, [](std::size_t) { return "X"; }, count) ==
                std::string("a\0Xb", 4) && count == 1, "scanner preserves embedded NUL bytes");
        require(std::holds_alternative<std::string>(common::NamedParams{{"name", "Ada"}}.values[0].second),
            "string literals bind as strings, not bool or dangling references");
        for (const auto &sql : {"SELECT :missing", "SELECT ?", "SELECT $1", "SELECT :1", "SELECT ':x", "SELECT /* :x", "SELECT $x$:x", "SELECT # :x"})
            require(!common::bindNamed(sql, {}).ok(), "invalid or ambiguous SQL rejected");
        require(!common::bindNamed("SELECT :id", {{"id", 1}, {"id", 2}}).ok(), "duplicate name rejected");
        require(!common::bindNamed("SELECT :id", {{"id", 1}, {"extra", 2}}).ok(), "unused name rejected");

        Client client;
        require(client.query("SELECT entity").status.code == common::ErrorCode::NotInitialized, "uninitialized raw query");
        require(await(client.queryAsync<QueryUser>("SELECT entity")).status.code == common::ErrorCode::NotInitialized,
            "uninitialized typed async");
        init(client);
        auto typedStatement = sql::select<RegisteredUser>().eq(&RegisteredUser::id, 42).build();
        auto registered = client.queryOne<RegisteredUser>(typedStatement);
        require(registered.ok() && registered.value && registered.value->name == "Ada", "registered entity and typed builder query");
        auto registeredAsync = client.queryOneAsync<RegisteredUser>(typedStatement).get();
        require(registeredAsync.ok() && registeredAsync.value && registeredAsync.value->id == 42, "typed builder native async query");
        require(client.withSession([&](core::Session &session) {
            return session.queryOne<RegisteredUser>(typedStatement).status;
        }).ok(), "typed builder session query");
        require(client.query("SELECT entity").rows.rowCount() == 1, "value-returning raw query");
        const auto built = sql::Builder::select("entity").columns({"id", "name", "nickname"}).build();
        require(client.query<CompactUser>(built).itemsOrThrow()[0].name == "Ada", "macro mapping and direct Builder query");
        require(client.query(built.statement).rowsOrThrow().rowCount() == 1, "raw Statement query");
        require(await(client.queryAsync<CompactUser>(built)).ok(), "typed Builder async query");
        require(await(client.queryAsync(built)).ok(), "raw Builder async query has uniform ok");
        sql::BuildResult bad;
        bad.status = common::Status::error(common::ErrorCode::QueryError, "invalid build");
        const auto beforeBad = calls.load();
        require(!client.query<CompactUser>(bad).ok() && !await(client.queryAsync<CompactUser>(bad)).ok() &&
            !client.execute(bad).ok() && !await(client.executeAsync(bad)).ok() && calls == beforeBad,
            "invalid Builder never executes");
        require(client.queryColumn<int>("SELECT scalar many").items == std::vector<int>({42, 42}), "single-column list");
        require(client.queryColumn<std::optional<int>>("SELECT scalar null").items[0] == std::nullopt,
            "nullable column list");
        require(!client.queryColumn<int>("SELECT scalar null").ok() && !client.queryColumn<int>("SELECT entity").ok(),
            "column list rejects NULL into nonnullable and multiple columns");
        require(await(client.queryColumnNamedAsync<int>("SELECT scalar :id", {{"id", 5}})).items[0] == 5,
            "named async column list");
        require(client.withSession([](core::Session &session) {
            require(session.queryOneNamed<CompactUser>("SELECT :id AS id", {{"id", 9}}).valueOrThrow().id == 9,
                "Session typed named result");
            require(session.query<CompactUser>(sql::Builder::select("entity").build()).itemsOrThrow().size() == 1,
                "Session Builder entity result");
            require(session.queryScalar<int>("SELECT scalar").valueOrThrow() == 42 &&
                session.queryColumnNamed<int>("SELECT scalar :id", {{"id", 3}}).items[0] == 3,
                "Session scalar and column helpers");
            require(session.executeNamed("UPDATE t SET id=:id", {{"id", 1}}).ok(), "Session named write");
            return common::Status{};
        }).ok(), "Session convenience does not borrow a second connection");
        try {
            (void) client.queryScalar<int>("SELECT scalar null").valueOrThrow();
            require(false, "NULL must not turn into a default scalar");
        } catch (const common::StatusError &error) {
            require(error.status().code == common::ErrorCode::MappingError, "opt-in throwing preserves structured status");
        }
        auto users = client.query<QueryUser>("SELECT entity many");
        require(users.ok() && users.items.size() == 2 && users.items[0].name == "Ada" && !users.items[0].nickname,
            "direct entities with nullable field");
        auto user = client.queryOneNamed<QueryUser>("db", "SELECT :id AS id", {{"id", std::int64_t{9}}});
        require(user.ok() && user.value && user.value->id == 9, "direct named single entity");
        require(!client.queryOne<QueryUser>("SELECT entity many").ok(), "single entity rejects multiple rows");
        require(!client.queryOne<QueryUser>("SELECT entity empty").value, "single entity not found");
        require(client.query<QueryUser>("SELECT entity bad").items.empty(), "failed conversion returns no partial entities");
        auto scalar = client.queryScalar<int>("SELECT scalar");
        require(scalar.ok() && scalar.hasRow && !scalar.isNull && scalar.value == 42, "scalar converts safely");
        require(client.queryScalarNamed<int>("SELECT scalar :value", {{"value", 7}}).value == 7, "named scalar");
        require(client.queryNamed<QueryUser>("db", "SELECT :id AS id", {{"id", 7}}).items[0].id == 7,
            "named entity list");
        require(!client.queryScalar<std::int8_t>("SELECT scalar", {std::int64_t{1000}}).ok(), "scalar overflow rejected");
        require(!client.queryScalar<int>("SELECT scalar bad").ok(), "scalar type mismatch rejected");
        require(!client.queryScalar<int>("SELECT scalar many").ok(), "scalar multiple rows rejected");
        require(!client.queryScalar<int>("SELECT entity").ok(), "scalar multiple columns rejected");
        auto nullValue = client.queryScalar<int>("SELECT scalar null");
        require(nullValue.ok() && nullValue.hasRow && nullValue.isNull && !nullValue.value, "NULL distinct from not found");
        auto empty = client.queryScalar<int>("SELECT scalar empty");
        require(empty.ok() && !empty.hasRow && !empty.isNull && !empty.value, "empty distinct from NULL");
        auto error = client.queryScalar<int>("SELECT scalar error");
        require(!error.ok() && error.status.sqlState == "42000" && error.status.nativeCode == 7,
            "database error metadata preserved");
        const auto before = calls.load();
        require(!client.queryNamed("SELECT :missing", {}).ok() &&
            !await(client.queryScalarNamedAsync<int>("SELECT :missing", {})).ok() && calls == before,
            "invalid bindings do not reach database");
        require(client.executeNamed("UPDATE t SET id=:id", {{"id", 1}}).affected == 1, "named execute");
        require(await(client.executeNamedAsync("UPDATE t SET id=:id", {{"id", 1}})).affected == 1, "named async execute");
        require(await(client.queryNamedAsync("SELECT scalar :id", {{"id", 6}})).status.ok(), "raw named async");
        auto asyncUsers = await(client.queryAsync<QueryUser>("SELECT entity"));
        require(asyncUsers.ok() && asyncUsers.mode == async::ExecutionMode::Native, "typed async retains native mode");
        require(await(client.queryOneAsync<QueryUser>("db", "SELECT entity", {})).value.has_value(), "async entity one");
        require(await(client.queryScalarNamedAsync<int>("db", "SELECT scalar :id", {{"id", 8}})).value == 8,
            "async named scalar");
        require(await(client.queryNamedAsync<QueryUser>("SELECT :id AS id", {{"id", 8}})).items.size() == 1,
            "async named entity list");
        require(await(client.queryOneNamedAsync<QueryUser>("SELECT :id AS id", {{"id", 8}})).value->id == 8,
            "async named entity one");
        require(!await(client.queryScalarAsync<int>("SELECT scalar bad")).ok(), "async mapping failure");
        delay = true;
        common::NamedParams params{{"id", std::int64_t{11}}};
        auto future = client.queryOneNamedAsync<QueryUser>("SELECT :id AS id", params);
        params.values[0].second = std::int64_t{99};
        require(future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "native query returns before completion");
        std::function<void()> complete;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!complete && std::chrono::steady_clock::now() < deadline) {
            { std::lock_guard<std::mutex> lock(pendingMutex); complete = std::move(pending); }
            if (!complete) std::this_thread::yield();
        }
        require(static_cast<bool>(complete), "driver accepted delayed query");
        Client moved(std::move(client));
        complete();
        require(await(std::move(future)).value->id == 11, "async owns parameters and survives Client move");
        delay = false;
        moved.shutdown();
        require(await(moved.queryAsync<QueryUser>("SELECT entity")).status.code == common::ErrorCode::ClientClosed,
            "closed typed async");
        Client fallback; init(fallback, false);
        auto fallbackResult = await(fallback.queryAsync<QueryUser>("SELECT entity"));
        require(fallbackResult.ok() && fallbackResult.mode == async::ExecutionMode::CompatibilityFallback,
            "typed mapping preserves compatibility fallback without waiting worker");
        fallback.shutdown();
        heartbeatHandoff(true);
        heartbeatHandoff(false);
        core::HeartbeatManager disabled(std::chrono::milliseconds(0));
        disabled.start();
        require(!disabled.running(), "zero interval disables heartbeat rather than busy looping");
        std::cout << "Direct queries, strict scalar semantics, named binding and native/fallback async passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
