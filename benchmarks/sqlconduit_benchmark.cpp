#include "sqlconduit/common/observer.h"
#include "sqlconduit/core/connection_pool.h"
#include "sqlconduit/driver/idriver.h"
#include "sqlconduit/mapping.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace sqlconduit;

    volatile std::uint64_t benchmarkSink = 0;

    struct BenchEntity {
        std::int64_t id = 0;
        std::string name;
        double score = 0;
        bool active = false;
    };

    class BenchConnection final : public core::IDatabaseConnection {
    public:
        common::Status connect(const config::DataSourceConfig &) override {
            open_ = true;
            return common::Status::OK();
        }

        common::Status ping() override { return common::Status::OK(); }

        common::Status query(const std::string &, common::ResultSet &out) override {
            out.clear();
            return common::Status::OK();
        }

        common::Status execute(const std::string &sql, std::int64_t &affected) override {
            affected = 1;
            benchmarkSink += sql.size();
            return common::Status::OK();
        }

        common::Status begin() override {
            inTransaction_ = true;
            return common::Status::OK();
        }

        common::Status commit() override {
            inTransaction_ = false;
            return common::Status::OK();
        }

        common::Status rollback() override {
            inTransaction_ = false;
            return common::Status::OK();
        }

        void close() override { open_ = false; }

        [[nodiscard]] bool isOpen() const override { return open_; }

        [[nodiscard]] bool inTransaction() const override { return inTransaction_; }

        [[nodiscard]] bool allowsLiteralInterpolation() const override { return true; }

    private:
        bool open_ = false;
        bool inTransaction_ = false;
    };

    class BenchDriver final : public driver::IDriver {
    public:
        [[nodiscard]] const char *name() const override { return "benchmark"; }

        std::unique_ptr<core::IDatabaseConnection> createConnection() override {
            return std::make_unique<BenchConnection>();
        }
    };

    class BenchCursor final : public core::ICursor {
    public:
        explicit BenchCursor(common::Row row) : row_(std::move(row)) {}

        common::Status fetch(const std::size_t n, common::ResultSet &out) override {
            out.clear();
            for (std::size_t i = 0; i < n; ++i) out.addRow(row_);
            fetched_ += n;
            return common::Status::OK();
        }

        common::Status fetchRow(common::Row &out, bool &ok) override {
            out = row_;
            ok = true;
            ++fetched_;
            return common::Status::OK();
        }

        common::Status close() override {
            open_ = false;
            return common::Status::OK();
        }

        [[nodiscard]] bool isOpen() const override { return open_; }
        [[nodiscard]] bool hasNext() const override { return open_; }
        [[nodiscard]] std::uint64_t rowsFetched() const override { return fetched_; }

    private:
        common::Row row_;
        std::uint64_t fetched_ = 0;
        bool open_ = true;
    };

    struct Result {
        std::string name;
        std::uint64_t iterations = 0;
        std::uint64_t repetitions = 0;
        double nanosecondsPerOperation = 0;
        double operationsPerSecond = 0;
    };

    template<class Fn>
    Result measure(std::string name, const std::uint64_t iterations, Fn &&fn) {
        const std::uint64_t warmup = (std::max<std::uint64_t>)(100, iterations / 20);
        for (std::uint64_t i = 0; i < warmup; ++i) fn();

        constexpr std::uint64_t repetitions = 5;
        std::vector<double> samples;
        samples.reserve(repetitions);
        for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition) {
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t i = 0; i < iterations; ++i) fn();
            const auto elapsed = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count();
            samples.push_back(elapsed / static_cast<double>(iterations));
        }
        std::sort(samples.begin(), samples.end());
        const double medianNs = samples[samples.size() / 2];
        return {std::move(name), iterations, repetitions, medianNs, 1e9 / medianNs};
    }

    void requireOk(const common::Status &status, const char *operation) {
        if (!status.ok()) throw std::runtime_error(std::string(operation) + ": " + status.message);
    }

    std::uint64_t parseIterations(const char *text) {
        const std::string value(text ? text : "");
        std::size_t used = 0;
        const auto parsed = std::stoull(value, &used);
        if (used != value.size() || parsed < 100) throw std::invalid_argument("iterations must be >= 100");
        return parsed;
    }
}

namespace sqlconduit::mapping {
    template<>
    struct RowMapper<::BenchEntity> {
        static Mapping<::BenchEntity> describe() {
            return Mapping<::BenchEntity>()
                .field(&::BenchEntity::id, "id")
                .field(&::BenchEntity::name, "name")
                .field(&::BenchEntity::score, "score")
                .field(&::BenchEntity::active, "active");
        }
    };
}

int main(int argc, char **argv) {
    std::uint64_t iterations = 100000;
    std::string jsonPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) iterations = parseIterations(argv[++i]);
        else if (arg == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else {
            std::cerr << "usage: sqlconduit_benchmark [--iterations N] [--json FILE]\n";
            return 2;
        }
    }

    using namespace sqlconduit;
    std::vector<Result> results;

    config::DataSourceConfig poolConfig;
    poolConfig.name = "benchmark";
    poolConfig.type = "benchmark";
    auto pool = std::make_shared<core::ConnectionPool>(
        std::make_unique<BenchDriver>(), poolConfig, 1, 1,
        std::chrono::milliseconds(1000), std::chrono::milliseconds(0),
        std::chrono::milliseconds(0), std::chrono::milliseconds(0),
        std::chrono::hours(1), false, true);
    results.push_back(measure("connection_borrow_return", iterations, [&] {
        common::ErrorCode code = common::ErrorCode::Unknown;
        std::string error;
        auto handle = pool->borrow(code, error);
        if (!handle) throw std::runtime_error("borrow: " + error);
        benchmarkSink += (*handle)->isOpen() ? 1 : 0;
    }));

    BenchConnection connection;
    requireOk(connection.connect(poolConfig), "connect");
    core::IDatabaseConnection &database = connection;
    const common::Params params{
        std::int64_t{42}, std::string("benchmark-user"), 98.5, true, nullptr,
    };
    const std::string statement =
        "UPDATE bench SET name=?, score=?, active=?, note=? WHERE id=?";
    results.push_back(measure("parameter_binding", iterations, [&] {
        std::int64_t affected = 0;
        requireOk(database.execute(statement, params, affected), "parameter binding");
        benchmarkSink += static_cast<std::uint64_t>(affected);
    }));

    common::Row row;
    row.set("id", std::int64_t{42});
    row.set("name", std::string("benchmark-user"));
    row.set("score", 98.5);
    row.set("active", true);
    results.push_back(measure("row_mapping", iterations, [&] {
        BenchEntity entity;
        requireOk(mapping::fromRow(row, entity), "row mapping");
        benchmarkSink += static_cast<std::uint64_t>(entity.id + entity.name.size());
    }));

    common::ParamBatch batch(32, params);
    const std::uint64_t batchIterations = (std::max<std::uint64_t>)(100, iterations / 10);
    results.push_back(measure("batch_execute_32_rows", batchIterations, [&] {
        common::BatchResult output;
        requireOk(database.executeBatch(statement, batch, output), "batch execute");
        benchmarkSink += static_cast<std::uint64_t>(output.totalAffected());
    }));

    BenchCursor cursor(row);
    const std::uint64_t cursorIterations = (std::max<std::uint64_t>)(100, iterations / 10);
    results.push_back(measure("cursor_fetch_64_rows", cursorIterations, [&] {
        common::ResultSet output;
        requireOk(cursor.fetch(64, output), "cursor fetch");
        benchmarkSink += output.rowCount();
    }));

    config::ObservabilityConfig observability;
    observability.sql_log.enabled = false;
    observability.slow_sql.enabled = false;
    observability.pool_metrics.enabled = false;
    observability.stats_report.enabled = false;
    common::Observability::configure(observability);
    common::Observability::setObserver({});
    std::uint64_t rendererCalls = 0;
    const common::SqlRenderer renderer = [&](const common::SqlRenderOptions &, std::string &out) {
        ++rendererCalls;
        out = statement;
        return common::Status::OK();
    };
    results.push_back(measure("logging_disabled_emit", iterations, [&] {
        common::OperationEvent event;
        event.dataSource = "benchmark";
        event.type = common::OperationType::Execute;
        event.status = common::Status::OK();
        common::Observability::emitSql(std::move(event), statement, renderer);
    }));
    if (rendererCalls != 0) throw std::runtime_error("disabled logging invoked SQL renderer");

    pool->shutdown();

    std::cout << std::left << std::setw(30) << "benchmark"
              << std::right << std::setw(14) << "iterations"
              << std::setw(18) << "ns/op"
              << std::setw(18) << "ops/s" << '\n';
    for (const auto &result: results) {
        std::cout << std::left << std::setw(30) << result.name
                  << std::right << std::setw(14) << result.iterations
                  << std::setw(18) << std::fixed << std::setprecision(2)
                  << result.nanosecondsPerOperation
                  << std::setw(18) << std::fixed << std::setprecision(0)
                  << result.operationsPerSecond << '\n';
    }

    if (!jsonPath.empty()) {
        std::ofstream json(jsonPath);
        if (!json) throw std::runtime_error("cannot open benchmark JSON output: " + jsonPath);
        json << "{\n  \"schema_version\": 1,\n  \"results\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            const auto &result = results[i];
            json << "    {\"name\": \"" << result.name << "\", \"iterations\": "
                 << result.iterations << ", \"repetitions\": " << result.repetitions
                 << ", \"nanoseconds_per_operation_median\": "
                 << std::fixed << std::setprecision(3) << result.nanosecondsPerOperation
                 << ", \"operations_per_second\": " << std::setprecision(3)
                 << result.operationsPerSecond << "}";
            if (i + 1 != results.size()) json << ',';
            json << '\n';
        }
        json << "  ],\n  \"sink\": " << benchmarkSink << "\n}\n";
    }

    return 0;
}
