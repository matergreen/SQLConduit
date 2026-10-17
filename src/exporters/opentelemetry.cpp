#include "sqlconduit/exporters/opentelemetry.h"
#include "sqlconduit/version.h"
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace sqlconduit::exporters {
    namespace {
        const char *operation(common::OperationType type) {
            switch (type) {
                case common::OperationType::Query: return "query";
                case common::OperationType::Execute: return "execute";
                case common::OperationType::Begin: return "begin";
                case common::OperationType::Commit: return "commit";
                case common::OperationType::Rollback: return "rollback";
                case common::OperationType::Cancel: return "cancel";
                case common::OperationType::Stream: return "stream";
                case common::OperationType::Batch: return "batch";
                case common::OperationType::Savepoint: return "savepoint";
                case common::OperationType::Select: return "select";
                case common::OperationType::Routine: return "routine";
            }
            return "unknown";
        }

        template<std::size_t N>
        bool decode(const std::string &hex, std::array<std::uint8_t, N> &out) {
            if (hex.size() != N * 2) return false;
            auto digit = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            for (std::size_t i = 0; i < N; ++i) {
                const int high = digit(hex[2 * i]), low = digit(hex[2 * i + 1]);
                if (high < 0 || low < 0) return false;
                out[i] = static_cast<std::uint8_t>(high * 16 + low);
            }
            return true;
        }
    }

    common::OperationObserver openTelemetryObserver(const OpenTelemetryOptions &options) {
        namespace ot = opentelemetry;
        auto tracer = ot::trace::Provider::GetTracerProvider()->GetTracer("sqlconduit", versionString);
        return [tracer, options](const common::OperationEvent &event) {
            ot::trace::StartSpanOptions start;
            start.kind = ot::trace::SpanKind::kClient;
            const auto steadyEnd = std::chrono::steady_clock::now();
            const auto systemEnd = std::chrono::system_clock::now();
            const auto elapsed = (std::max)(event.duration, std::chrono::microseconds(0));
            start.start_system_time = ot::common::SystemTimestamp(systemEnd - elapsed);
            start.start_steady_time = ot::common::SteadyTimestamp(steadyEnd - elapsed);
            // Do not inherit a random reactor worker's current span when event
            // context is absent or malformed. Use an explicit root instead.
            ot::context::Context root;
            start.parent = root.SetValue(ot::trace::kIsRootSpanKey, true);
            std::array<std::uint8_t, 16> trace{};
            std::array<std::uint8_t, 8> span{};
            if (decode(event.traceId, trace) && decode(event.spanId, span)) {
                ot::trace::SpanContext parent(
                    ot::trace::TraceId(ot::nostd::span<const std::uint8_t, 16>(trace.data(), trace.size())),
                    ot::trace::SpanId(ot::nostd::span<const std::uint8_t, 8>(span.data(), span.size())),
                    ot::trace::TraceFlags(event.traceFlags), false);
                if (parent.IsValid()) start.parent = parent;
            }
            constexpr const char *phases[] = {"queue", "pool_wait", "native", "fallback", "retry",
                "replica_fallback", "rejected", "cancel", "cache_hit"};
            const bool phase = event.diagnosticPhase >= 0 && event.diagnosticPhase < 9;
            auto result = tracer->StartSpan(phase ? phases[event.diagnosticPhase] : operation(event.type), start);
            if (phase) {
                result->SetAttribute("sqlconduit.phase", phases[event.diagnosticPhase]);
                result->SetAttribute("sqlconduit.fallback_reason", static_cast<std::int64_t>(event.fallbackReason));
                result->SetAttribute("sqlconduit.attempt", static_cast<std::int64_t>(event.attempt));
            } else result->SetAttribute("db.operation.name", operation(event.type));
            result->SetAttribute("sqlconduit.error_code", static_cast<std::int64_t>(event.status.code));
            result->SetAttribute("sqlconduit.rows", static_cast<std::int64_t>(event.rowCount));
            result->SetAttribute("sqlconduit.shadow", event.shadow);
            result->SetAttribute("sqlconduit.slow", event.slow);
            if (options.includeFingerprint)
                result->SetAttribute("sqlconduit.fingerprint", std::to_string(event.sqlFingerprint));
            if (!event.status.ok()) result->SetStatus(ot::trace::StatusCode::kError);
            ot::trace::EndSpanOptions end;
            end.end_steady_time = ot::common::SteadyTimestamp(steadyEnd);
            result->End(end);
        };
    }

    common::SqlContext openTelemetryContext() {
        auto out = common::ContextScope::current();
        const auto context = opentelemetry::trace::Tracer::GetCurrentSpan()->GetContext();
        if (context.IsValid()) {
            char trace[32], span[16];
            context.trace_id().ToLowerBase16(trace);
            context.span_id().ToLowerBase16(span);
            out.traceId.assign(trace, sizeof(trace));
            out.spanId.assign(span, sizeof(span));
            out.traceFlags = context.trace_flags().flags();
        }
        return out;
    }

    common::ExecutionPhaseObserver openTelemetryPhaseObserver() {
        auto exportOperation = openTelemetryObserver();
        return [exportOperation](const common::ExecutionPhaseEvent &phase) {
            common::OperationEvent event;
            event.duration = phase.duration;
            event.status.code = phase.errorCode;
            event.traceId = phase.context.traceId;
            event.spanId = phase.context.spanId;
            event.traceFlags = phase.context.traceFlags;
            event.diagnosticPhase = static_cast<int>(phase.phase);
            event.fallbackReason = static_cast<int>(phase.fallbackReason);
            event.attempt = phase.attempt;
            exportOperation(event);
        };
    }
}
