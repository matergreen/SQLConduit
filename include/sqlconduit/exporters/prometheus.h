#ifndef SQLCONDUIT_EXPORTERS_PROMETHEUS_H
#define SQLCONDUIT_EXPORTERS_PROMETHEUS_H

#include "sqlconduit/common/observer.h"

#include <cstddef>
#include <string>
#include <vector>

namespace sqlconduit::exporters {
    std::string toPrometheusText(const common::PoolMetricsEvent &pools,
                                 const std::vector<common::SlowSqlStats> &slow,
                                 const std::string &prefix = "sqlconduit",
                                 std::size_t maxFingerprintLabels = 0);
}

#endif
