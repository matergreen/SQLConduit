# 1.1 配置工具与运行时诊断

开发分支当前为 `1.1.0-dev`。默认构建并安装 `sqlconduitctl`，通过
`SQLCONDUIT_BUILD_TOOLS=OFF` 可关闭。

```sh
sqlconduitctl validate datasource.yaml
sqlconduitctl explain datasource.json
sqlconduitctl doctor datasource.yaml
sqlconduitctl doctor datasource.yaml --connect
```

- `validate`：离线检查 JSON/YAML 字段、类型、范围、环境变量和拓扑引用。
- `explain`：输出补全默认值后的脱敏 JSON，包含池、异步、重试、路由、缓存和观测配置。
- `doctor`：检查工具实际链接的驱动与能够获得的客户端能力信息。
- `doctor --connect`：显式连接并 ping 每个叶子，返回结构化状态和实际能力，不执行应用写入。

退出码 0 表示成功，1 表示校验/诊断失败，2 表示参数错误。密码、DSN、私钥路径、Wallet 路径和
所有 `extra` 值均被脱敏。CLI 不回显可能包含敏感值的解析异常和厂商错误原文；诊断报告不能直接
当作生产配置文件使用。离线命令也要求 `password_env` 已设置。连接探测的超时精度受厂商 SDK
限制。Core-only 工具可运行 validate/explain；自定义驱动需要在应用自己的工具里注册。

库内调用：

```cpp
sqlconduit::config::GlobalConfig config;
std::string error;
if (!sqlconduit::config::ConfigLoader::loadFromFile("datasource.yaml", config, error)) return 1;
if (!sqlconduit::config::ConfigLoader::validateTopology(config, error)) return 1;
std::cout << sqlconduit::config::ConfigLoader::explain(config);

auto health = client.diagnostics();
auto report = client.capabilities("read_write_group");
if (!report.status.ok()) return 1;
for (const auto &source : report.sources) {
    if (!source.group && source.capabilities.observed)
        std::cout << source.driver << ": " << source.capabilities.asyncConstraints;
}
```

`diagnostics()` 包含组与叶子、池压力、熔断、连续失败数、最近错误码、重试次数、写缓冲深度、
缓存和执行器统计；`configSource` 区分文件与程序配置，不保留路径或凭据。
`capabilities(name)` 还列出组的主库、副本、故障转移候选和 shadow。

诊断不会借连接或执行 SQL，并忽略请求的 `targetDataSource` 覆盖。能力来自最近一次成功创建
连接的快照：`observed=false` 表示未观测，空版本字符串表示元数据缺失。历史能力不代表当前
连接健康，也不保证每条语句都是 native；实际模式以 Future 的 mode 和阶段事件为准。并发期间
各类快照可来自略有不同的时刻。库解析器返回的错误原文应由应用脱敏后再对外发布。

`setExecutionPhaseObserver` 可观察队列、借连接、Native/Fallback、重试、副本回退、拒绝、取消
和缓存命中。`FallbackReason` 区分驱动不支持、驱动拒绝语句、复合操作和缓存命中。借连接耗时
包含异步回调的交付时间；Retry 是调度事件，不把计划退避时间当成已发生耗时。

OpenTelemetry 需安装 SDK 并启用 `SQLCONDUIT_ENABLE_OPENTELEMETRY=ON`，Core 无此依赖：

```cmake
find_package(sqlconduit REQUIRED COMPONENTS Core Postgres OpenTelemetry)
target_link_libraries(app PRIVATE sqlconduit::postgres sqlconduit::opentelemetry)
```

应用配置 TracerProvider 和 exporter 后安装适配器：

```cpp
#include "sqlconduit/exporters/opentelemetry.h"
client.setObserver(sqlconduit::exporters::openTelemetryObserver());
client.setExecutionPhaseObserver(sqlconduit::exporters::openTelemetryPhaseObserver());
{
    const sqlconduit::common::ContextScope scope(sqlconduit::exporters::openTelemetryContext());
    auto result = client.queryAsync("SELECT id FROM users WHERE id=?", {std::int64_t(42)}).get();
}
```

父 span 在调用线程捕获后随异步上下文传递，没有有效父级时创建 root span；W3C 采样标记保留。
默认不导出 SQL、参数、厂商错误、数据源名或连接串，指纹也需显式启用 `includeFingerprint`。
应用负责 exporter 的批处理、flush 和关闭。设置 observer 会替换同类回调；多个接收端需显式
组合。回调应快速返回，不能在同一 Client 的回调中 reload、shutdown 或销毁 Client。

完整说明见 [英文文档](diagnostics.md)，包管理见 [Package managers](package_managers.md)。
