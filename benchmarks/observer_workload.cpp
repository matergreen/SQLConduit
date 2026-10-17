#include "observer_workload.h"
#include <utility>

namespace sqlconduit::benchmarks {
    // A separate translation unit keeps this measured workload independent of
    // GCC's inlining/register-allocation decisions for unrelated benchmark cases.
    void emitDisabledLogEvent(const std::string &statement, const common::SqlRenderer &renderer) {
        common::OperationEvent event;
        event.dataSource = "benchmark";
        event.type = common::OperationType::Execute;
        event.status = common::Status::OK();
        common::Observability::emitSql(std::move(event), statement, renderer);
    }
}
