#ifndef SQLCONDUIT_DRIVERS_ORACLE_H
#define SQLCONDUIT_DRIVERS_ORACLE_H

#include "sqlconduit/driver/driver_registry.h"

namespace sqlconduit::drivers {
    // Link sqlconduit::oracle and pass this registration to Client::addDriver().
    driver::DriverRegistration oracle();
}

#endif
