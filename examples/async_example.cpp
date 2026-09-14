#include "dbmw/dbmw.h"
#include "dbmw/async/dbmw_async.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#if defined(DBMW_ENABLE_ASYNC_CORO)
#include "dbmw/async/task.h"
#endif

namespace {
    void report(const char *tag, const dbmw::common::Status &st) {
        std::cout << (st.ok() ? "[OK]   " : "[NOTE] ")
                  << tag << ": " << st.message
                  << " (code=" << dbmw::common::errorCodeToString(st.code) << ")"
                  << std::endl;
    }

    template <class R>
    R awaitCallback(void (*launch)(std::function<void(R &&)>)) {
        std::promise<R> pr;
        auto fut = pr.get_future();
        launch([&pr](R &&r) { pr.set_value(std::move(r)); });
        return fut.get();
    }
}

#if defined(DBMW_ENABLE_ASYNC_CORO)
dbmw::async::Task<void> coroDemo() {
    dbmw::common::Params params;
    params.push_back(dbmw::common::Value(std::int64_t(1)));

    auto q = co_await dbmw::async::queryAsync(
        "SELECT id, name FROM users WHERE id = ?", params);

    if (q.status.ok()) {
        std::cout << "[OK]   协程式 query 返回 " << q.rows.rowCount() << " 行" << std::endl;
    } else {
        report("协程式 query", q.status);
    }

    dbmw::common::TransactionOptions txOpts;
    auto tx = co_await dbmw::async::transactionAsync(txOpts,
        [](dbmw::core::Session &s) -> dbmw::common::Status {
            std::int64_t affected = 0;
            if (const auto st = s.execute("UPDATE users SET active = 1", affected); !st.ok())
                return st;
            return dbmw::common::Status::OK();
        });
    report("协程式 transaction", tx.status);
}
#endif

int main(int argc, char **argv) {
    const std::string configPath =
        (argc > 1) ? argv[1] : "config/datasources.json.example";

    auto st = dbmw::DBMW::init(configPath);
    if (!st.ok()) {
        std::cerr << "[FAIL] init: " << st.message << std::endl;
        return 1;
    }
    std::cout << "[OK]   init from " << configPath << std::endl;

    {
        auto h = dbmw::async::query(
            "SELECT id, name FROM users WHERE id = ?",
            {dbmw::common::Value(std::int64_t(1))},
            [](dbmw::async::QueryResult &&r) {
                if (r.status.ok())
                    std::cout << "[OK]   回调式 query 返回 " << r.rows.rowCount()
                              << " 行（回调线程: " << "完成调度器"
                              << "）" << std::endl;
                else
                    std::cout << "[NOTE] 回调式 query: " << r.status.message << std::endl;
            });
        (void) h;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    {
        auto fut = dbmw::async::execute(
            "UPDATE users SET active = 1 WHERE id = ?",
            {dbmw::common::Value(std::int64_t(1))});
        auto r = fut.get();
        if (r.status.ok())
            std::cout << "[OK]   future 式 execute 受影响 " << r.affected << " 行" << std::endl;
        else
            report("future 式 execute", r.status);
    }

#if defined(DBMW_ENABLE_ASYNC_CORO)
    {
        dbmw::async::run(coroDemo());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
#else
    std::cout << "[NOTE] 协程层未启用：加 -DDBMW_ENABLE_ASYNC_CORO=ON 重新构建"
              << "（且本 TU 以 C++20 编译）后可见第 3 段演示" << std::endl;
#endif

    {
        dbmw::async::Options opts;
        opts.timeout = std::chrono::milliseconds(2000);
        auto h = dbmw::async::query("SELECT report_all_users()", dbmw::common::Params{},
            [](dbmw::async::QueryResult &&r) {
                std::cout << "[NOTE] 被取消/完成的慢查询: code="
                          << dbmw::common::errorCodeToString(r.status.code)
                          << std::endl;
            }, opts);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (const auto c = h.cancel(); c.ok())
            std::cout << "[OK]   已请求取消（Handle 状态 Running/Queued）" << std::endl;
        else
            std::cout << "[NOTE] cancel: " << c.message << "（已完成则无事可做）" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    dbmw::DBMW::shutdown(std::chrono::milliseconds(3000));
    std::cout << "[OK]   shutdown（在途操作已排空）" << std::endl;
    return 0;
}
