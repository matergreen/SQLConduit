#ifndef DBMW_DRIVER_IDRIVER_H
#define DBMW_DRIVER_IDRIVER_H

#include "dbmw/core/idatabase_connection.h"
#include <memory>

namespace dbmw::driver {
    class IDriver {
    public:
        virtual ~IDriver() = default;

        virtual const char *name() const = 0;

        virtual std::unique_ptr<core::IDatabaseConnection> createConnection() = 0;
    };
}

#endif
