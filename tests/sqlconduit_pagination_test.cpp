#include "sqlconduit/query_page.h"
#include "sqlconduit/driver/idriver.h"
#include <atomic>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

using namespace sqlconduit;
using namespace std::chrono_literals;
struct PageUser { std::int64_t id = 0; std::string name; };
SQLCONDUIT_ENTITY(PageUser, "users", id, name)

namespace {
    std::atomic<int> dataCalls{0}, countCalls{0};
    std::atomic<bool> hold{false};
    std::atomic<bool> declineCount{false};
    std::mutex tasksMutex;
    std::vector<std::function<void()>> tasks;
    void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
    template<class Predicate> void waitFor(Predicate test, const char *message) {
        const auto end = std::chrono::steady_clock::now() + 3s;
        do {
            if (test()) return;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < end);
        require(false, message);
    }
    template<class T> T await(std::future<T> future) {
        require(future.wait_for(3s) == std::future_status::ready, "page future resolves without waiting threads or pool deadlock");
        return future.get();
    }
    void flush() {
        hold = false;
        std::vector<std::function<void()>> ready;
        { std::lock_guard<std::mutex> lock(tasksMutex); ready.swap(tasks); }
        for (auto &task : ready) task();
    }
    class Connection final : public core::IDatabaseConnection {
        bool open = false, native = true;
    public:
        common::Status connect(const config::DataSourceConfig &cfg) override { open = true; native = cfg.database != "fallback"; return {}; }
        common::Status ping() override { return {}; }
        bool supportsParams() const override { return true; }
        common::Status query(const std::string &sql, common::ResultSet &rows) override { return query(sql, {}, rows); }
        common::Status query(const std::string &sql, const common::Params &params, common::ResultSet &rows) override {
            rows.clear();
            if (sql.find("COUNT") != std::string::npos) {
                ++countCalls;
                if (sql.find("fail_count") != std::string::npos)
                    return common::Status::databaseError(common::ErrorCode::QueryError, "count failed", "42000", 7);
                rows.setFields({"total"}); common::Row row;
                row.set("total", sql.find("bad_count") != std::string::npos ? common::Value{nullptr} : common::Value{std::int64_t{5}});
                rows.addRow(std::move(row)); return {};
            }
            ++dataCalls;
            if (sql.find("fail_data") != std::string::npos) return common::Status::error(common::ErrorCode::QueryError, "data failed");
            require(params.size() >= 2, "pagination binds limit and offset");
            const bool limitFirst = sql.find("LIMIT") != std::string::npos;
            const auto limit = std::get<std::int64_t>(params[params.size() - (limitFirst ? 2 : 1)]);
            const auto offset = std::get<std::int64_t>(params[params.size() - (limitFirst ? 1 : 2)]);
            rows.setFields({"id", "name"});
            for (auto id = offset + 1; id <= 5 && id <= offset + limit; ++id) {
                common::Row row; row.set("id", id);
                row.set("name", sql.find("bad_mapping") == std::string::npos ? common::Value{std::string("Ada")} : common::Value{nullptr});
                rows.addRow(std::move(row));
            }
            return {};
        }
        core::AsyncCapability asyncCapability() const override { return native ? core::AsyncCapability::Native : core::AsyncCapability::ThreadPoolFallback; }
        bool queryAsync(const std::string &sql, const common::Params &params, AsyncQueryCompletion complete) override {
            if (!native || (declineCount && sql.find("COUNT") != std::string::npos)) return false;
            auto task = [this, sql, params, complete = std::move(complete)]() mutable {
                common::ResultSet rows; auto status = query(sql, params, rows); complete(std::move(status), std::move(rows));
            };
            if (hold) { std::lock_guard<std::mutex> lock(tasksMutex); tasks.push_back(std::move(task)); }
            else task();
            return true;
        }
        common::Status execute(const std::string &, std::int64_t &) override { return {}; }
        common::Status begin() override { return {}; }
        common::Status commit() override { return {}; }
        common::Status rollback() override { return {}; }
        void close() override { open = false; }
        bool isOpen() const override { return open; }
    };
    class Driver final : public driver::IDriver {
    public:
        const char *name() const override { return "page-test"; }
        std::unique_ptr<core::IDatabaseConnection> createConnection() override { return std::make_unique<Connection>(); }
    };
    void init(Client &client, bool native = true) {
        require(client.addDriver({"page-test", [] { return std::make_unique<Driver>(); }}).ok(), "driver registration");
        config::GlobalConfig cfg; cfg.default_datasource = "db"; cfg.pool.min = 0; cfg.pool.max = 1;
        cfg.heartbeat_interval_ms = 0; cfg.async.threads = 1; cfg.retry.max_attempts = 1;
        config::DataSourceConfig ds; ds.name = "db"; ds.type = "page-test"; ds.database = native ? "native" : "fallback";
        cfg.datasources.push_back(ds); require(client.init(cfg).ok(), "client init");
    }
}

int main() {
    try {
        PageOptions options; options.page = 2; options.pageSize = 2;
        auto builder = sql::select<PageUser>(common::util::Dialect::Postgres).ge(&PageUser::id, 1).orderByAsc(&PageUser::id);
        const auto plan = builder.buildPage();
        const auto serverPlan = sql::Builder::select("users", common::util::Dialect::SqlServer).orderBy("id").buildPage();
        require(serverPlan.ok() && serverPlan.countQuery && serverPlan.countQuery->sql.find("COUNT_BIG(*)") != std::string::npos,
                "SQL Server generated totals use bigint COUNT_BIG");
        require(plan.ok() && plan.countQuery && plan.countQuery->sql == "SELECT COUNT(*) FROM \"users\" WHERE \"id\" >= ?" &&
                plan.countQuery->params == common::Params{std::int64_t{1}}, "Builder COUNT is structural and independently bound");
        for (auto dialect : {common::util::Dialect::MySQL, common::util::Dialect::Postgres,
                             common::util::Dialect::SqlServer, common::util::Dialect::Oracle}) {
            options.dialect = dialect;
            auto prepared = detail::preparePage(plan, options, common::util::Dialect::Auto);
            require(prepared.status.ok() && prepared.data.params.front() == common::Value{std::int64_t{1}}, "preserve original parameters");
            const bool limit = dialect == common::util::Dialect::MySQL || dialect == common::util::Dialect::Postgres;
            require(prepared.data.sql.find(limit ? "\nLIMIT ? OFFSET ?" : "\nOFFSET ? ROWS FETCH NEXT ? ROWS ONLY") != std::string::npos,
                    "dialect pagination syntax");
            require(prepared.data.params[1] == common::Value{std::int64_t{limit ? 3 : 2}} &&
                    prepared.data.params[2] == common::Value{std::int64_t{limit ? 2 : 3}}, "dialect parameter order");
        }
        options.dialect = common::util::Dialect::Postgres;
        Client client; init(client);
        auto page = client.queryPage<PageUser>(builder, options);
        require(page.ok() && page.items.size() == 2 && page.items[0].id == 3 && page.hasNext && !page.total && countCalls == 0,
                "probe one extra row, no default COUNT");
        options.includeTotal = true;
        page = client.queryPage<PageUser>(plan, options);
        require(page.ok() && page.total == 5 && page.hasNext && page.items.size() == 2, "optional total");
        options.page = 3; page = client.queryPage<PageUser>(plan, options);
        require(page.ok() && page.items.size() == 1 && page.items[0].id == 5 && !page.hasNext, "last page");
        options.page = 4; page = client.queryPage<PageUser>(plan, options);
        require(page.ok() && page.items.empty() && page.total == 5 && !page.hasNext, "beyond last page retains total");
        options.page = 1;
        auto native = await(client.queryPageAsync<PageUser>(plan, options));
        require(native.ok() && native.mode == async::ExecutionMode::Native && native.total == 5 && native.hasNext,
                "native async page and COUNT work with one connection and one executor worker");
        declineCount = true;
        const auto mixedMode = await(client.queryPageAsync<PageUser>(plan, options));
        declineCount = false;
        require(mixedMode.ok() && mixedMode.mode == async::ExecutionMode::CompatibilityFallback,
                "combined page never claims native when COUNT fell back");
        require(client.withSession([&](core::Session &session) {
            auto sessionPage = session.queryPage<PageUser>(builder, options);
            require(sessionPage.ok() && sessionPage.total == 5, "Session reuses transaction connection for both statements");
            return common::Status{};
        }).ok(), "session page");
        options.includeTotal = false;
        options.pageSize = 5;
        require(!client.queryPage<PageUser>(plan, options).hasNext, "exactly full final page has no next page");
        options.pageSize = 2;
        require(client.queryPage<PageUser>("SELECT id, name, 'FOR UPDATE; LIMIT ?' AS note FROM users ORDER BY id -- tail ?", {}, options).ok(),
                "quoted clauses stay opaque and trailing comment cannot swallow pagination");
        require(client.queryPageNamed<PageUser>("SELECT id, name FROM users WHERE id>=:id ORDER BY id", {{"id", 1}}, options).ok(),
                "named page query");
        require(await(client.withOptions({}).queryPageNamedAsync<PageUser>("SELECT id, name FROM users WHERE id>=:id ORDER BY id", {{"id", 1}}, options)).ok(),
                "options view accepts named page braced parameters");
        const int before = dataCalls + countCalls;
        for (const auto &sql : {"SELECT id FROM users", "DELETE FROM users ORDER BY id", "SELECT id FROM users ORDER BY id LIMIT 1",
             "SELECT id FROM users ORDER BY id;", "SELECT id FROM users ORDER BY id FOR UPDATE", "SELECT TOP 1 id FROM users ORDER BY id",
             "SELECT id FROM users ORDER BY id OFFSET 1 ROWS", "SELECT id INTO t FROM users ORDER BY id", "SELECT 'bad ORDER BY id"})
            require(!client.queryPage<PageUser>(sql, {}, options).ok(), "unsafe or already paginated SQL rejected before execution");
        require(!client.queryPageNamed<PageUser>("SELECT id, name FROM users WHERE id=:id ORDER BY id", {}, options).ok(),
                "missing named parameter rejected before SQL");
        const std::string nulSql = "SELECT id, name, 'a" + std::string(1, '\0') + "b' FROM users ORDER BY id";
        require(!client.queryPage<PageUser>(nulSql, {}, options).ok(), "NUL inside a literal cannot bypass validation");
        require(!client.queryPage<PageUser>("SELECT * FROM (SELECT id FROM users ORDER BY id) nested", {}, options).ok(),
                "inner ORDER BY does not count as page ordering");
        options.page = 0; require(!client.queryPage<PageUser>(plan, options).ok(), "zero page rejected");
        options.page = (std::numeric_limits<std::int64_t>::max)(); require(!client.queryPage<PageUser>(plan, options).ok(), "offset overflow rejected");
        options.page = 1; options.pageSize = 0; require(!client.queryPage<PageUser>(plan, options).ok(), "zero page size rejected");
        options.pageSize = 2; options.includeTotal = true;
        require(!client.queryPage<PageUser>(builder.build(), options).ok(), "raw built SQL requires explicit COUNT when requested");
        require(dataCalls + countCalls == before, "invalid pagination performs no SQL");
        options.countQuery = sql::Statement{"SELECT COUNT(*) FROM users /* fail_count */", {}};
        auto failed = await(client.queryPageAsync<PageUser>(plan, options));
        require(!failed.ok() && failed.status.sqlState == "42000" && failed.items.empty() && !failed.total && !failed.hasNext,
                "COUNT failure is atomic and retains driver error");
        options.countQuery = sql::Statement{"SELECT COUNT(*) FROM users /* bad_count */", {}};
        require(!client.queryPage<PageUser>(plan, options).ok(), "invalid COUNT value rejected");
        options.countQuery = sql::Statement{"SELECT COUNT(*) FROM users", {}};
        require(client.queryPage<PageUser>(builder.build(), options).ok(), "explicit COUNT supports raw SQL");
        require(!client.queryPage<PageUser>("SELECT id, name FROM bad_mapping ORDER BY id", {}, options).ok(), "mapping errors propagate");
        common::ResultSet decimalCount; decimalCount.setFields({"n"}); common::Row row; row.set("n", common::Decimal{"5.00"});
        decimalCount.addRow(std::move(row)); std::int64_t total = 0;
        require(detail::readPageTotal(decimalCount, total).ok() && total == 5, "Oracle exact aggregate NUMBER supported");
        for (auto value : {common::Value{common::Decimal{"5.10"}}, common::Value{std::string("5")},
                           common::Value{std::int64_t{-1}}, common::Value{nullptr},
                           common::Value{(std::numeric_limits<std::uint64_t>::max)()}}) {
            common::ResultSet bad; common::Row badRow; badRow.set("n", std::move(value)); bad.addRow(std::move(badRow));
            require(!detail::readPageTotal(bad, total).ok(), "COUNT is exact, nonnegative and nonnullable");
        }
        auto failedData = await(client.queryPageAsync<PageUser>("SELECT id, name FROM fail_data ORDER BY id", {}, options));
        require(!failedData.ok() && failedData.items.empty() && !failedData.total, "data failure is atomic");

        // Cancellation resolves the combined result while a driver callback is still held.
        // A failed earlier page can still have a submitted sibling cleaning up. Use
        // an independent pool rather than treating its ready future as pool idleness.
        Client cancelClient; init(cancelClient);
        hold = true; common::CancellationSource cancel;
        common::OperationOptions operation; operation.cancellation = cancel.token();
        auto pending = cancelClient.withOptions(operation).queryPageAsync<PageUser>(plan, options);
        waitFor([] { std::lock_guard<std::mutex> lock(tasksMutex); return !tasks.empty(); }, "native driver callback held");
        waitFor([&] { core::ConnectionPool::Stats stats; return cancelClient.poolStats(stats) && stats.asyncWaiting == 1; },
                "second page statement queues nonblockingly on the single-connection pool");
        cancel.requestCancellation();
        const auto cancelled = await(std::move(pending)); flush();
        require(cancelled.status.code == common::ErrorCode::Cancelled && cancelled.items.empty(), "page cancellation is atomic");
        Client fallback; init(fallback, false);
        const auto compatible = await(fallback.queryPageAsync<PageUser>(plan, options));
        require(compatible.ok() && compatible.total == 5 && compatible.mode == async::ExecutionMode::CompatibilityFallback,
                "fallback mode preserved for both statements");
        std::cout << "Pagination dialects, boundaries, totals, mapping, async, Session and cancellation passed\n";
        return 0;
    } catch (const std::exception &error) { flush(); std::cerr << error.what() << '\n'; return 1; }
}
