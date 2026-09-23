#ifndef SQLCONDUIT_DRIVERS_MYSQL_H
#define SQLCONDUIT_DRIVERS_MYSQL_H

#include "sqlconduit/driver/driver_registry.h"

namespace sqlconduit::drivers {
    // Link sqlconduit::mysql and pass this registration to Client::addDriver().
    driver::DriverRegistration mysql();
}

#endif
