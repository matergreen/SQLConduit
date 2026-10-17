#include "cache_workload.h"

namespace sqlconduit::benchmarks {
    // Keep literal conversion and API calls independent of unrelated main()
    // inlining/register allocation. Both installed versions use this same TU.
    bool getCacheEntry(core::detail::QueryCacheState &cache, const char *key,
                       common::ResultSet &out) {
        return cache.get("benchmark", key, out);
    }

    void putCacheEntry(core::detail::QueryCacheState &cache, const char *key,
                       const common::ResultSet &rows) {
        cache.put("benchmark", key, rows);
    }
}
