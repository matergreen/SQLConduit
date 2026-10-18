#ifndef SQLCONDUIT_SQLCONDUIT_H
#define SQLCONDUIT_SQLCONDUIT_H

// Convenience umbrella for the instance-owned public API.
#include "sqlconduit/api.h"
#include "sqlconduit/client.h"
#include "sqlconduit/public.h"
#include "sqlconduit/sql_builder.h"
#include "sqlconduit/version.h"
// Keep mapping last: it builds on the complete Client convenience surface.
#include "sqlconduit/mapping.h"
#include "sqlconduit/typed_sql_builder.h"

#endif
