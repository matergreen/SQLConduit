#ifndef SQLCONDUIT_COMMON_NAMED_PARAMS_H
#define SQLCONDUIT_COMMON_NAMED_PARAMS_H

#include "sqlconduit/common/types.h"
#include <initializer_list>

namespace sqlconduit::common {
    // Keep entries (rather than a map) so duplicate names are diagnosed.
    struct NamedParams {
        std::vector<std::pair<std::string, Value>> values;
        NamedParams() = default;
        NamedParams(std::initializer_list<std::pair<std::string, Value>> entries) : values(entries) {}
    };

    struct BoundQuery {
        Status status;
        std::string sql;
        Params params;
        [[nodiscard]] bool ok() const { return status.ok(); }
    };

    // Rewrites :name to the portable ? binding syntax; never interpolates values.
    // Missing, duplicate, unused and mixed positional parameters are errors.
    [[nodiscard]] BoundQuery bindNamed(const std::string &sql, const NamedParams &params);
}

#endif
