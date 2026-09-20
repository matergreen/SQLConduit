#ifndef DBMW_ASYNC_ASYNC_TYPES_H
#define DBMW_ASYNC_ASYNC_TYPES_H

#include "dbmw/common/types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dbmw::async {
    struct QueryResult {
        common::Status status;
        common::ResultSet rows;
    };

    struct MultiQueryResult {
        common::Status status;
        std::vector<common::ResultSet> sets;

        [[nodiscard]] std::size_t rowCount() const {
            std::size_t n = 0;
            for (const auto &s: sets) n += s.rowCount();
            return n;
        }
    };

    struct ExecResult {
        common::Status status;
        std::int64_t affected = 0;
    };

    struct ExecKeysResult {
        common::Status status;
        std::int64_t affected = 0;
        common::GeneratedKeys keys;
    };

    struct EachResult {
        common::Status status;
        std::uint64_t rows = 0;
    };

    struct BatchResult {
        common::Status status;
        common::BatchResult batch;
    };

    struct OpResult {
        common::Status status;
    };

    using QueryCallback = std::function<void(QueryResult &&)>;
    using MultiQueryCallback = std::function<void(MultiQueryResult &&)>;
    using ExecCallback = std::function<void(ExecResult &&)>;
    using ExecKeysCallback = std::function<void(ExecKeysResult &&)>;
    using EachCallback = std::function<void(EachResult &&)>;
    using BatchCallback = std::function<void(BatchResult &&)>;
    using OpCallback = std::function<void(OpResult &&)>;

    struct Options {
        std::chrono::milliseconds borrowTimeout{-1};
        std::chrono::milliseconds timeout{0};
    };

    namespace detail {
        struct OpState;
        class AsyncEngine;
    }

    class Handle {
    public:
        Handle() = default;

        [[nodiscard]] bool valid() const { return s_ != nullptr; }

        enum class State { Queued, Running, Done };

        [[nodiscard]] State state() const;

        common::Status cancel() const;

    private:
        friend class detail::AsyncEngine;

        explicit Handle(std::shared_ptr<detail::OpState> s) : s_(std::move(s)) {
        }

        std::shared_ptr<detail::OpState> s_;
    };
}

#endif
