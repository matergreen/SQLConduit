#ifndef SQLCONDUIT_BENCHMARK_CACHE_WORKLOAD_H
#define SQLCONDUIT_BENCHMARK_CACHE_WORKLOAD_H

#include "sqlconduit/core/query_cache.h"

namespace sqlconduit::benchmarks {
    bool getCacheEntry(core::detail::QueryCacheState &cache, const char *key,
                       common::ResultSet &out);
    void putCacheEntry(core::detail::QueryCacheState &cache, const char *key,
                       const common::ResultSet &rows);
}

#endif
