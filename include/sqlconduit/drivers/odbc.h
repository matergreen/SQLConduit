#ifndef SQLCONDUIT_DRIVERS_ODBC_H
#define SQLCONDUIT_DRIVERS_ODBC_H

#include "sqlconduit/driver/driver_registry.h"

namespace sqlconduit::drivers {
    // Link sqlconduit::odbc and pass this registration to Client::addDriver().
    driver::DriverRegistration odbc();
}

#endif
