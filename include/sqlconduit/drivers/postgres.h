#ifndef SQLCONDUIT_DRIVERS_POSTGRES_H
#define SQLCONDUIT_DRIVERS_POSTGRES_H

#include "sqlconduit/driver/driver_registry.h"

namespace sqlconduit::drivers {
    // Link sqlconduit::postgres and pass this registration to Client::addDriver().
    driver::DriverRegistration postgres();
}

#endif
