#ifndef SQLCONDUIT_BENCHMARK_OBSERVER_WORKLOAD_H
#define SQLCONDUIT_BENCHMARK_OBSERVER_WORKLOAD_H

#include "sqlconduit/common/observer.h"

namespace sqlconduit::benchmarks {
    void emitDisabledLogEvent(const std::string &statement, const common::SqlRenderer &renderer);
}

#endif
