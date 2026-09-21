#ifndef SQLCONDUIT_DRIVER_DRIVER_FACTORY_H
#define SQLCONDUIT_DRIVER_DRIVER_FACTORY_H

#include "sqlconduit/driver/idriver.h"

#include <memory>
#include <string>

namespace sqlconduit::driver
{
    std::unique_ptr<IDriver> createDriver(const std::string& type);

    void registerBuiltinDrivers();
}

#endif
