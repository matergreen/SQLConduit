#ifndef SQLCONDUIT_EXPORTERS_OPENTELEMETRY_H
#define SQLCONDUIT_EXPORTERS_OPENTELEMETRY_H

#include "sqlconduit/common/observer.h"
#include "sqlconduit/common/context.h"
#include "sqlconduit/common/diagnostics.h"

namespace sqlconduit::exporters {
    struct OpenTelemetryOptions {
        // Fingerprints are safe from raw values but can still be high-cardinality.
        bool includeFingerprint = false;
    };

    // Link sqlconduit::opentelemetry. Configure the application's OpenTelemetry
    // TracerProvider before creating the observer. The application owns exporters.
    // Event context is propagated explicitly across worker/reactor completions.
    [[nodiscard]] common::OperationObserver openTelemetryObserver(
        const OpenTelemetryOptions &options = {});

    // Capture on the submitting thread, then install a ContextScope around the
    // operation. Async calls copy that context into their continuations.
    [[nodiscard]] common::SqlContext openTelemetryContext();

    [[nodiscard]] common::ExecutionPhaseObserver openTelemetryPhaseObserver();
}
#endif
