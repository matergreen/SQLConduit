#ifndef DBMW_EXPORTERS_PROMETHEUS_H
#define DBMW_EXPORTERS_PROMETHEUS_H

#include "dbmw/common/observer.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dbmw::exporters {
    std::string toPrometheusText(const common::PoolMetricsEvent &pools,
                                 const std::vector<common::SlowSqlStats> &slow,
                                 const std::string &prefix = "dbmw",
                                 std::size_t maxFingerprintLabels = 0);
}

#endif
