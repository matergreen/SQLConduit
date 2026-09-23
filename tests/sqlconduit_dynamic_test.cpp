#include "sqlconduit/sqlconduit.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/core/database_manager.h"
#include "sqlconduit/core/idatabase_connection.h"
#include "sqlconduit/config/datasource_config.h"
#include "sqlconduit/driver/driver_registry.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sqlconduit;
using common::Status;
static Client g_client;

static int g_failed = 0;
static int g_passed = 0;

static void check(bool cond, const std::string &name) {
    if (cond) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

class MockConnection : public core::IDatabaseConnection {
public:
    static std::atomic<int> alive;
    static std::atomic<bool> connectFails;

    common::Status connect(const config::DataSourceConfig &cfg) override {
        (void) cfg;
        if (connectFails.load())
            return Status::error(common::ErrorCode::ConnectionFailed, "mock connect failed");
        open_ = true;
        ++alive;
        return Status::OK();
    }

    common::Status ping() override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        return Status::OK();
    }

    common::Status query(const std::string &sql, common::ResultSet &out) override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        common::Row r;
        r.set("echo", std::string(sql));
        out.addRow(std::move(r));
        return Status::OK();
    }

    common::Status execute(const std::string &, std::int64_t &affected) override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        affected = 1;
        return Status::OK();
    }

    common::Status begin() override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        return Status::OK();
    }

    common::Status commit() override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        return Status::OK();
    }

    common::Status rollback() override {
        if (!open_) return Status::error(common::ErrorCode::NotConnected, "closed");
        return Status::OK();
    }

    void close() override {
        open_ = false;
        --alive;
    }

    bool isOpen() const override { return open_; }

private:
    bool open_ = false;
};

std::atomic<int> MockConnection::alive{0};
std::atomic<bool> MockConnection::connectFails{false};

class MockDriver : public driver::IDriver {
public:
    const char *name() const override { return "mock"; }

    std::unique_ptr<core::IDatabaseConnection> createConnection() override {
        return std::make_unique<MockConnection>();
    }
};

static void resetMock() {
    MockConnection::alive = 0;
    MockConnection::connectFails = false;
}

static config::DataSourceConfig mockLeafCfg(const std::string &name) {
    config::DataSourceConfig c;
    c.name = name;
    c.type = "mock";
    c.host = "localhost";
    c.connection_timeout_ms = 100;
    return c;
}

static config::PoolConfig smallPool() {
    config::PoolConfig p;
    p.min = 0;
    p.max = 2;
    p.borrow_timeout_ms = 100;
    return p;
}

static config::GlobalConfig makeBaseGlobal(const std::string &defaultName = "anchor") {
    config::GlobalConfig g;
    g.default_datasource = defaultName;
    g.pool = smallPool();
    g.datasources.push_back(mockLeafCfg("anchor"));
    return g;
}

int main() {
    driver::DriverRegistry::instance().registerDriver(
        "mock", [] { return std::make_unique<MockDriver>(); });

    std::cout << "== M4.1  addDataSource 正常路径 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功（含 anchor 锚点）");
        check(mgr.dataSourceCount() == 1, "init 后 dataSourceCount == 1");

        auto leaf = mockLeafCfg("leaf1");
        const auto before = MockConnection::alive.load();
        check(mgr.addDataSource(leaf).ok(),
              "addDataSource(leaf1) 成功");
        check(mgr.dataSourceCount() == 2, "dataSourceCount 加一（2）");
        check(MockConnection::alive.load() > before,
              "新建叶子触发了连接预热/首次借出");

        const auto ds = mgr.getDataSource("leaf1");
        check(ds != nullptr, "getDataSource(leaf1) 非空");
        if (ds) {
            common::ResultSet rs;
            check(ds->query("select 1", rs).ok(), "leaf1 可正常执行 query");
            check(rs.rowCount() == 1, "返回 1 行");
        }
        mgr.shutdown(std::chrono::milliseconds(0));
        check(MockConnection::alive.load() == 0, "shutdown 后所有 mock 连接都已关闭");
    }

    std::cout << "== M4.2  addDataSource 重名拒绝，旧池不被破坏 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("dup")).ok(), "首次 addDataSource(dup) 成功");
        const auto cnt1 = mgr.dataSourceCount();
        const auto st = mgr.addDataSource(mockLeafCfg("dup"));
        check(!st.ok(), "重名 addDataSource 返回错误");
        check(st.code == common::ErrorCode::ConfigError, "错误码为 ConfigError");
        check(mgr.dataSourceCount() == cnt1, "重名失败后 dataSourceCount 不变");

        const auto ds = mgr.getDataSource("dup");
        check(ds != nullptr, "原 dup 仍可见");
        if (ds) {
            common::ResultSet rs;
            check(ds->query("SELECT 1", rs).ok(), "原 dup 仍可查询（未被新请求破坏）");
        }
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.3  addDataSource 未知驱动类型 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        config::DataSourceConfig bogus = mockLeafCfg("bogus");
        bogus.type = "no_such_driver_xyz";
        const auto st = mgr.addDataSource(bogus);
        check(!st.ok(), "未知驱动类型 addDataSource 返回错误");
        check(mgr.dataSourceCount() == 1, "未知驱动失败后 dataSourceCount 不变（仍是 anchor）");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.4  addDataSource 空名 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        config::DataSourceConfig empty;
        empty.name = "";
        empty.type = "mock";
        const auto st = mgr.addDataSource(empty);
        check(!st.ok() && st.code == common::ErrorCode::ConfigError,
              "空名 addDataSource 返回 ConfigError");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.5  removeDataSource 正常路径 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("removable")).ok(), "addDataSource(removable) 成功");
        const auto cnt = mgr.dataSourceCount();
        check(mgr.removeDataSource("removable").ok(), "removeDataSource 成功");
        check(mgr.dataSourceCount() == cnt - 1, "dataSourceCount 减一");
        check(mgr.getDataSource("removable") == nullptr,
              "remove 后 getDataSource 不可见");
        mgr.shutdown(std::chrono::milliseconds(0));
        check(MockConnection::alive.load() == 0,
              "remove + shutdown 后所有 mock 连接都已关闭");
    }

    std::cout << "== M4.6  removeDataSource 未知名 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        const auto st = mgr.removeDataSource("never_existed");
        check(!st.ok() && st.code == common::ErrorCode::ConfigError,
              "未知名 removeDataSource 返回 ConfigError");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.7  removeDataSource 拒绝注销'被组引用'的叶子 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("leafA")).ok(), "addDataSource(leafA)");
        check(mgr.addDataSource(mockLeafCfg("leafB")).ok(), "addDataSource(leafB)");

        config::DataSourceGroupConfig gcfg;
        gcfg.name = "AB";
        gcfg.primary = "leafA";
        config::ReplicaConfig r;
        r.name = "leafB";
        r.weight = 1;
        gcfg.replicas.push_back(r);

        core::GroupOptions gopts;
        check(mgr.addGroup(gcfg, gopts).ok(), "addGroup(AB) 成功");

        const auto st = mgr.removeDataSource("leafA");
        check(!st.ok(), "removeDataSource(leafA) 被组引用，应失败");
        check(st.code == common::ErrorCode::ConfigError,
              "错误码为 ConfigError");
        check(st.message.find("AB") != std::string::npos,
              "错误消息包含冲突组名 AB（got: " + st.message + ")");

        const auto st2 = mgr.removeDataSource("leafB");
        check(!st2.ok(), "removeDataSource(leafB) 在副本里，也应失败");

        check(mgr.removeGroup("AB").ok(), "removeGroup(AB) 成功");
        check(mgr.removeDataSource("leafA").ok(),
              "removeGroup 后再 removeDataSource(leafA) 成功");
        check(mgr.removeDataSource("leafB").ok(),
              "removeDataSource(leafB) 也成功");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.8  addGroup 引用完整性：未知 primary ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        config::DataSourceGroupConfig g;
        g.name = "bad_group";
        g.primary = "ghost";
        const auto st = mgr.addGroup(g);
        check(!st.ok(), "未知 primary 的 addGroup 返回错误");
        check(mgr.getDataSource("bad_group") == nullptr, "未成功插入 group");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.9  addGroup 重名拒绝 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        config::GlobalConfig gc = makeBaseGlobal("anchor");
        config::DataSourceGroupConfig g0;
        g0.name = "dupg";
        g0.primary = "anchor";
        gc.groups.push_back(g0);
        check(mgr.init(gc).ok(), "init（含 dupg 组）成功");

        config::DataSourceGroupConfig g1;
        g1.name = "dupg";
        g1.primary = "anchor";
        const auto st = mgr.addGroup(g1);
        check(!st.ok() && st.code == common::ErrorCode::ConfigError,
              "重名 addGroup 返回 ConfigError");
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.10 addGroup ack 校验：failover.primaries 必须 ack ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("fp1")).ok(), "addDataSource(fp1)");

        config::DataSourceGroupConfig g;
        g.name = "failgroup";
        g.primary = "anchor";
        g.failover.primaries.push_back("fp1");
        g.failover.acknowledge_external_fencing = false;

        core::GroupOptions opts;
        const auto st1 = mgr.addGroup(g, opts);
        check(!st1.ok() && st1.code == common::ErrorCode::ConfigError,
              "未 ack 自动写切换 → ConfigError");

        opts.acknowledge_external_fencing = true;
        const auto st2 = mgr.addGroup(g, opts);
        check(st2.ok(), "显式 ack 后 addGroup 成功");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.11 addGroup 写缓冲启用但未 ack / 显式 ack 后通过 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("wb1")).ok(), "addDataSource(wb1)");

        config::DataSourceGroupConfig g;
        g.name = "wbgroup";
        g.primary = "anchor";
        config::ReplicaConfig r;
        r.name = "wb1";
        g.replicas.push_back(r);
        g.failover.write_buffer.enabled = true;
        g.failover.write_buffer.acknowledge_data_loss_and_duplicates = false;

        core::GroupOptions opts;
        const auto st1 = mgr.addGroup(g, opts);
        check(!st1.ok() && st1.code == common::ErrorCode::ConfigError,
              "写缓冲启用但未 ack → ConfigError");

        g.failover.write_buffer.acknowledge_data_loss_and_duplicates = true;
        opts.acknowledge_data_loss_and_duplicates = true;
        check(mgr.addGroup(g, opts).ok(), "显式 ack 后 addGroup 成功");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.12 removeGroup 正常路径 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        check(mgr.addDataSource(mockLeafCfg("rg1")).ok(), "addDataSource(rg1)");

        config::DataSourceGroupConfig g;
        g.name = "rg";
        g.primary = "anchor";
        config::ReplicaConfig r;
        r.name = "rg1";
        g.replicas.push_back(r);

        check(mgr.addGroup(g).ok(), "addGroup(rg) 成功");
        check(mgr.getDataSource("rg") != nullptr, "rg 已可见");

        check(mgr.removeGroup("rg").ok(), "removeGroup(rg) 成功");
        check(mgr.getDataSource("rg") == nullptr, "rg 已不可见");
        check(mgr.getDataSource("rg1") != nullptr, "叶子 rg1 仍可见");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.13 removeGroup 未知名 / 非组名 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        const auto st1 = mgr.removeGroup("never_group");
        check(!st1.ok() && st1.code == common::ErrorCode::ConfigError,
              "未知名 removeGroup 返回 ConfigError");

        const auto st2 = mgr.removeGroup("anchor");
        check(!st2.ok() && st2.code == common::ErrorCode::ConfigError,
              "对叶子名调用 removeGroup 返回 ConfigError");

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.14 并发 addDataSource（不同名）==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");

        constexpr int kThreads = 8;
        constexpr int kPerThread = 5;
        std::vector<std::thread> workers;
        std::atomic<int> ok{0};
        std::atomic<int> fail{0};
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    const std::string name = "conc_" + std::to_string(t) + "_" + std::to_string(i);
                    auto cfg = mockLeafCfg(name);
                    if (mgr.addDataSource(cfg).ok()) {
                        ++ok;
                    } else {
                        ++fail;
                    }
                }
            });
        }
        for (auto &w: workers) w.join();

        const int expected = kThreads * kPerThread;
        check(ok.load() == expected,
              "并发 addDataSource 全部成功（" + std::to_string(ok.load()) +
              "/" + std::to_string(expected) + "）");
        check(fail.load() == 0, "并发 addDataSource 失败数 = 0");
        check(mgr.dataSourceCount() == static_cast<size_t>(1 + expected),
              "dataSourceCount == anchor + 并发成功数");

        common::ResultSet rs;
        check(mgr.getDataSource("conc_0_0") != nullptr, "conc_0_0 可见");
        if (auto ds = mgr.getDataSource("conc_0_0")) {
            check(ds->query("SELECT 42", rs).ok(), "conc_0_0 可查询");
        }

        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.15 Client 入口与 DatabaseManager 行为一致 ==\n";
    {
        resetMock();
        const std::string cfgPath =
                (std::filesystem::temp_directory_path() / "sqlconduit_m4_facade.json").string();
        {
            std::ofstream f(cfgPath);
            f << R"({
                "default_datasource": "anchor",
                "datasources": [
                    {"name": "anchor", "type": "mock", "host": "localhost",
                     "connection_timeout_ms": 100}
                ],
                "pool": {"min": 0, "max": 2, "borrow_timeout_ms": 100}
            })";
        }
        check(g_client.init(cfgPath).ok(), "g_client.init 成功");

        const auto st = g_client.addDataSource(mockLeafCfg("via_facade"));
        check(st.ok(), "g_client.addDataSource 成功");
        check(g_client.dataSource("via_facade") != nullptr,
              "g_client.dataSource(via_facade) 可见");

        const auto routed = g_client.dataSource("via_facade");
        common::SqlContext routeContext;
        routeContext.targetDataSource = "via_facade";
        {
            common::ContextScope scope(routeContext);
            check(g_client.dataSource() == routed && g_client.dataSource("anchor") == routed,
                  "SqlContext.targetDataSource 覆盖默认与显式 Client 路由");
        }
        check(g_client.dataSource() != routed,
              "ContextScope 退出后恢复默认数据源");

        const auto removeDefault = g_client.removeDataSource("anchor");
        check(!removeDefault.ok() && removeDefault.code == common::ErrorCode::ConfigError,
              "运行期拒绝删除当前默认数据源");

        const auto dup = g_client.addDataSource(mockLeafCfg("via_facade"));
        check(!dup.ok() && dup.code == common::ErrorCode::ConfigError,
              "g_client.addDataSource 重名拒绝");

        check(g_client.removeDataSource("via_facade").ok(),
              "g_client.removeDataSource 成功");
        check(g_client.dataSource("via_facade") == nullptr,
              "删除后不可见");

        g_client.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "== M4.16 removeDataSource 在途连接宽限期 ==\n";
    {
        resetMock();
        core::DatabaseManager mgr;
        check(mgr.init(makeBaseGlobal("anchor")).ok(), "init 成功");
        check(mgr.addDataSource(mockLeafCfg("g1")).ok(), "addDataSource(g1)");

        std::thread worker([&] {
            (void) mgr.getDataSource("g1")->withSession(
                [&](core::Session &) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    return Status::OK();
                });
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto t0 = std::chrono::steady_clock::now();
        const auto st = mgr.removeDataSource("g1", std::chrono::milliseconds(0));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(st.ok(), "在途连接存在时 removeDataSource 仍 ok（grace=0）");
        check(ms < 100, "grace=0 不应等在途归还（实测 " + std::to_string(ms) + "ms）");

        worker.join();
        mgr.shutdown(std::chrono::milliseconds(0));
    }

    std::cout << "\n--- 总计 " << g_passed << " 通过 / " << g_failed << " 失败 ---\n";
    return g_failed == 0 ? 0 : 1;
}
