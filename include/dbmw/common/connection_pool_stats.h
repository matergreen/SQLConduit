#ifndef DBMW_COMMON_CONNECTION_POOL_STATS_H
#define DBMW_COMMON_CONNECTION_POOL_STATS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dbmw::common {
    struct ConnectionPoolStats {
        std::size_t minConnections = 0;
        std::size_t maxConnections = 0;
        std::size_t idle = 0;
        std::size_t total = 0;
        std::size_t borrowed = 0;
        std::size_t waiting = 0;
        std::uint64_t connectionsCreated = 0;
        std::uint64_t connectionsClosed = 0;
        std::uint64_t borrowTimeouts = 0;
        std::uint64_t validationFailures = 0;
        std::uint64_t leakWarnings = 0;
        std::size_t maxBorrowed = 0;
        std::size_t maxWaiting = 0;
        std::uint64_t borrowRequests = 0;
        std::uint64_t borrowSuccesses = 0;
        std::uint64_t connectionCreateFailures = 0;
        std::uint64_t invalidatedConnections = 0;
        std::uint64_t idleEvictions = 0;
        std::uint64_t lifetimeEvictions = 0;
        std::chrono::microseconds totalBorrowWait{0};
        std::chrono::microseconds maxBorrowWait{0};
        std::size_t asyncWaiting = 0;
        [[nodiscard]] double utilization() const {
            return maxConnections == 0 ? 0.0
                : static_cast<double>(borrowed) / static_cast<double>(maxConnections);
        }
    };

    struct NamedPoolStats {
        std::string dataSource;
        ConnectionPoolStats stats;
    };
}

#endif
