#ifndef DBMW_DRIVER_DRIVER_FACTORY_H
#define DBMW_DRIVER_DRIVER_FACTORY_H

#include "dbmw/driver/idriver.h"

#include <memory>
#include <string>

namespace dbmw::driver {
    std::unique_ptr<IDriver> createDriver(const std::string &type);

    void registerBuiltinDrivers();
}

#endif
