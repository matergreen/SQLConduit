#ifndef SQLCONDUIT_ASYNC_ASYNC_TYPES_H
#define SQLCONDUIT_ASYNC_ASYNC_TYPES_H

#include "sqlconduit/common/types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sqlconduit::async {
    enum class ExecutionMode {
        Native = 0,
        CompatibilityFallback = 1
    };

    struct QueryResult {
        common::Status status;
        common::ResultSet rows;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct MultiQueryResult {
        common::Status status;
        std::vector<common::ResultSet> sets;
        ExecutionMode mode = ExecutionMode::Native;

        [[nodiscard]] std::size_t rowCount() const {
            std::size_t n = 0;
            for (const auto &s: sets) n += s.rowCount();
            return n;
        }
    };

    struct ExecResult {
        common::Status status;
        std::int64_t affected = 0;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct ExecKeysResult {
        common::Status status;
        std::int64_t affected = 0;
        common::GeneratedKeys keys;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct EachResult {
        common::Status status;
        std::uint64_t rows = 0;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct BatchResult {
        common::Status status;
        common::BatchResult batch;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct OpResult {
        common::Status status;
        ExecutionMode mode = ExecutionMode::Native;
    };

    struct ExecutorStats {
        std::size_t threads = 0;
        std::size_t queueDepth = 0;
        std::size_t active = 0;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t rejected = 0;
        std::uint64_t delayedPending = 0;
        std::uint64_t nativeOperations = 0;
        std::uint64_t fallbackOperations = 0;
        std::uint64_t timedOutOperations = 0;
    };
}

#endif
