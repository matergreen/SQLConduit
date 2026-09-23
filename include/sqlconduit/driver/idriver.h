#ifndef SQLCONDUIT_DRIVER_IDRIVER_H
#define SQLCONDUIT_DRIVER_IDRIVER_H

#include "sqlconduit/core/idatabase_connection.h"
#include <memory>

namespace sqlconduit::driver {
    class IDriver {
    public:
        virtual ~IDriver() = default;

        [[nodiscard]] virtual const char *name() const = 0;

        virtual std::unique_ptr<core::IDatabaseConnection> createConnection() = 0;
    };
}

#endif
