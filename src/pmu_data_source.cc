// pmu_metrics/src/pmu_data_source.cc
//
// Implements PmuDataSource callbacks and the static-member definition
// required by the Perfetto SDK.
//
// PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS must appear exactly once in the
// entire link unit — it lives here.

#include "pmu_data_source.h"

#include <perfetto.h>

// One-time static-member definition (SDK requirement).
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(pmu_metrics::internal::PmuDataSource);

namespace pmu_metrics::internal {

// ---------------------------------------------------------------------------
// Lifecycle callbacks
// ---------------------------------------------------------------------------

void PmuDataSource::OnSetup(const SetupArgs& /*args*/) {
    // TODO: parse DataSourceConfig fields if the host passes custom config
    //       (e.g. sampling interval, enabled metrics).
}

void PmuDataSource::OnStart(const StartArgs& /*args*/) {
    // Tracing session has started.  PmuSession instances will begin emitting
    // packets on the next Tick() call.
}

void PmuDataSource::OnStop(const StopArgs& /*args*/) {
    // Tracing session is stopping.  Flush any pending state if needed.
    // Currently a no-op because Tick() emits packets synchronously.
}

// ---------------------------------------------------------------------------
// EmitTrackDescriptor
//
// Sends a TrackDescriptor packet for a CounterTrack so trace viewers can
// label the track.  Called once from PmuSession::Create().
// ---------------------------------------------------------------------------
void PmuDataSource::EmitTrackDescriptor(uint64_t track_uuid,
                                         const std::string& track_name,
                                         const std::string& counter_name) {
    PmuDataSource::Trace([&](PmuDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        // Set a synthetic timestamp; descriptor packets don't need a precise one.
        packet->set_timestamp(0);

        auto* track_desc = packet->set_track_descriptor();
        track_desc->set_uuid(track_uuid);
        track_desc->set_name(track_name + "/" + counter_name);

        auto* counter = track_desc->set_counter();
        counter->set_unit_name(counter_name == "ipc" ? "instr/cycle" : "cycle/instr");
        // is_incremental = false: we emit absolute values on each Tick().
        counter->set_is_incremental(false);
    });
}

// ---------------------------------------------------------------------------
// EmitCounterPacket
//
// Writes a single counter sample into the host's trace buffer.
// Called from PmuSession::Tick() on the owning thread.
// ---------------------------------------------------------------------------
void PmuDataSource::EmitCounterPacket(uint64_t track_uuid,
                                       uint64_t timestamp_ns,
                                       const std::string& /*counter_name*/,
                                       double value) {
    PmuDataSource::Trace([&](PmuDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(timestamp_ns);

        auto* track_event = packet->set_track_event();
        track_event->set_track_uuid(track_uuid);
        // Use TYPE_COUNTER so Perfetto renders this as a time-series graph.
        track_event->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
        track_event->set_counter_value_double(value);
    });
}

}  // namespace pmu_metrics::internal
