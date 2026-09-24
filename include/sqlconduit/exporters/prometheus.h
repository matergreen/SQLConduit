#ifndef SQLCONDUIT_EXPORTERS_PROMETHEUS_H
#define SQLCONDUIT_EXPORTERS_PROMETHEUS_H

#include "sqlconduit/common/observer.h"

#include <cstddef>
#include <string>
#include <vector>

namespace sqlconduit::exporters {
    // Fingerprint is the only intentionally high-cardinality label exported by
    // SQLConduit. The hard limit applies even when maxFingerprintLabels is 0.
    inline constexpr std::size_t kPrometheusFingerprintSeriesHardLimit = 1000;

    std::string toPrometheusText(const common::PoolMetricsEvent &pools,
                                 const std::vector<common::SlowSqlStats> &slow,
                                 const std::string &prefix = "sqlconduit",
                                 std::size_t maxFingerprintLabels = 0);
}

#endif
