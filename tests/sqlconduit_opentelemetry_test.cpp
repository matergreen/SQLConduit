#include "sqlconduit/exporters/opentelemetry.h"
#include <opentelemetry/exporters/memory/in_memory_span_exporter.h>
#include <opentelemetry/sdk/trace/simple_processor.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/scope.h>
#include <iostream>
#include <thread>

int main() {
    namespace ot = opentelemetry;
    auto previous = ot::trace::Provider::GetTracerProvider();
    auto exporter = std::make_unique<ot::exporter::memory::InMemorySpanExporter>();
    auto data = exporter->GetData();
    auto processor = std::make_unique<ot::sdk::trace::SimpleSpanProcessor>(std::move(exporter));
    ot::nostd::shared_ptr<ot::trace::TracerProvider> provider(
        new ot::sdk::trace::TracerProvider(std::move(processor)));
    ot::trace::Provider::SetTracerProvider(provider);
    auto tracer = provider->GetTracer("test");
    auto parent = tracer->StartSpan("request");
    sqlconduit::common::SqlContext context;
    {
        const ot::trace::Scope scope(parent);
        context = sqlconduit::exporters::openTelemetryContext();
    }
    auto observer = sqlconduit::exporters::openTelemetryObserver();
    sqlconduit::common::OperationEvent event;
    event.type = sqlconduit::common::OperationType::Query;
    event.duration = std::chrono::microseconds(2500);
    event.traceId = context.traceId;
    event.spanId = context.spanId;
    event.traceFlags = context.traceFlags;
    event.sqlTemplate = "SELECT secret FROM private_table";
    event.renderedSql = "secret-parameter";
    event.dataSource = "secret-dsn";
    event.status = sqlconduit::common::Status::error(sqlconduit::common::ErrorCode::QueryTimeout,
                                                  "secret-vendor-error");
    std::thread worker([&] { observer(event); });
    worker.join();
    auto spans = data->GetSpans();
    bool ok = spans.size() == 1;
    if (ok) {
        const auto &s = *spans[0];
        ok = s.GetName() == "query" && s.GetDuration() == std::chrono::microseconds(2500) &&
             s.GetTraceId() == parent->GetContext().trace_id() &&
             s.GetParentSpanId() == parent->GetContext().span_id() &&
             s.GetStatus() == ot::trace::StatusCode::kError && s.GetDescription().empty();
        for (const auto &a : s.GetAttributes()) {
            if (a.first == "db.statement" || a.first == "db.query.text" ||
                a.first == "sqlconduit.datasource" || a.first == "sqlconduit.fingerprint") ok = false;
        }
    }
    sqlconduit::common::ExecutionPhaseEvent phase;
    phase.phase = sqlconduit::common::ExecutionPhase::Fallback;
    phase.fallbackReason = sqlconduit::common::FallbackReason::DriverUnsupported;
    phase.duration = std::chrono::microseconds(700);
    phase.attempt = 2;
    phase.context = context;
    auto phaseObserver = sqlconduit::exporters::openTelemetryPhaseObserver();
    std::thread phaseWorker([&] { phaseObserver(phase); });
    phaseWorker.join();
    auto phaseSpans = data->GetSpans();
    ok = ok && phaseSpans.size() == 1;
    if (phaseSpans.size() == 1) {
        const auto &s = *phaseSpans[0];
        const auto &attributes = s.GetAttributes();
        ok = ok && s.GetName() == "fallback" &&
             s.GetDuration() == std::chrono::microseconds(700) &&
             s.GetTraceId() == parent->GetContext().trace_id() &&
             s.GetParentSpanId() == parent->GetContext().span_id() &&
             attributes.count("sqlconduit.phase") == 1 &&
             attributes.count("sqlconduit.fallback_reason") == 1 &&
             attributes.count("sqlconduit.attempt") == 1;
    }
    parent->End();
    ot::trace::Provider::SetTracerProvider(previous);
    if (!ok) std::cerr << "OpenTelemetry duration/context/safe-attribute contract failed\n";
    return ok ? 0 : 1;
}
