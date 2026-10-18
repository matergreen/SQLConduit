#ifndef SQLCONDUIT_PAGINATION_H
#define SQLCONDUIT_PAGINATION_H

#include "sqlconduit/sql_builder.h"
#include "sqlconduit/async/async_types.h"
#include <optional>

namespace sqlconduit {
    struct PageOptions {
        std::int64_t page = 1;
        std::int64_t pageSize = 20;
        bool includeTotal = false;
        common::util::Dialect dialect = common::util::Dialect::Auto;
        // Explicit COUNT for arbitrary SQL, or an override of Builder's generated COUNT.
        std::optional<sql::Statement> countQuery;
    };

    template<class T> struct PageResult {
        common::Status status;
        std::vector<T> items;
        std::int64_t page = 1;
        std::int64_t pageSize = 20;
        bool hasNext = false;
        std::optional<std::int64_t> total;
        async::ExecutionMode mode = async::ExecutionMode::CompatibilityFallback;
        [[nodiscard]] bool ok() const { return status.ok(); }
        std::vector<T> &itemsOrThrow() & { common::throwIfError(status); return items; }
        const std::vector<T> &itemsOrThrow() const & { common::throwIfError(status); return items; }
        std::vector<T> itemsOrThrow() && { common::throwIfError(status); return std::move(items); }
    };
}

namespace sqlconduit::detail {
    struct PreparedPage {
        common::Status status;
        sql::Statement data;
        std::optional<sql::Statement> count;
    };
    struct PageRows {
        common::Status status;
        common::ResultSet rows;
        std::int64_t page = 1, pageSize = 20;
        bool hasNext = false;
        std::optional<std::int64_t> total;
        async::ExecutionMode mode = async::ExecutionMode::CompatibilityFallback;
    };
    PreparedPage preparePage(const sql::PageQuery &, const PageOptions &, common::util::Dialect);
    void finishPageRows(PageRows &);
    common::Status readPageTotal(const common::ResultSet &, std::int64_t &);
}
#endif
