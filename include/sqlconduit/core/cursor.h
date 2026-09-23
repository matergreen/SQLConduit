#ifndef SQLCONDUIT_CORE_CURSOR_H
#define SQLCONDUIT_CORE_CURSOR_H

#include "sqlconduit/common/types.h"

#include <chrono>
#include <cstdint>

namespace sqlconduit::core {
    class ICursor {
    public:
        virtual ~ICursor() = default;

        virtual common::Status fetch(std::size_t n, common::ResultSet &out) = 0;

        virtual common::Status fetchRow(common::Row &out, bool &ok) = 0;

        virtual common::Status close() = 0;

        [[nodiscard]] virtual bool isOpen() const = 0;

        [[nodiscard]] virtual bool hasNext() const = 0;

        [[nodiscard]] virtual std::uint64_t rowsFetched() const = 0;
    };

    struct CursorOptions {
        std::size_t batch_size = 256;
        bool scrollable = false;
        std::chrono::milliseconds timeout{0};
        bool auto_transaction = true;
    };
}

#endif
