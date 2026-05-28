// pmu_metrics/src/pmu_data_source.h  (PRIVATE — not installed)
//
// Declares the Perfetto DataSource that writes PMU CounterTrack packets into
// the host project's existing trace buffer.
//
// Registration contract:
//   - PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS is placed in
//     pmu_data_source.cc; this triggers registration before main() when the
//     host calls perfetto::Tracing::Initialize().
//   - The host MUST NOT call Initialize() a second time.

#pragma once

#include <perfetto.h>  // Provided by the host CMake target "perfetto"

#include <cstdint>
#include <string>

namespace pmu_metrics::internal {

// Perfetto DataSource descriptor name.
inline constexpr char kDataSourceName[] = "dev.pmu_metrics";

// ---------------------------------------------------------------------------
// PmuDataSource
//
// Receives OnSetup / OnStart / OnStop callbacks from the Perfetto SDK.
// Individual PmuSession instances call EmitCounterPacket() directly.
// ---------------------------------------------------------------------------
class PmuDataSource : public perfetto::DataSource<PmuDataSource> {
public:
    // Called by the Perfetto SDK when a tracing session that includes
    // "dev.pmu_metrics" in its data-source config is started.
    void OnSetup(const SetupArgs&) override;
    void OnStart(const StartArgs&) override;
    void OnStop(const StopArgs&) override;

    // ---------------------------------------------------------------------------
    // Emit one counter packet for a single (track_uuid, metric_name) pair.
    //
    // Called from PmuSession::Tick() on the owning thread.
    // Internally calls DataSource::Trace([...](auto ctx){ ctx.NewTracePacket() }).
    //
    // Parameters:
    //   track_uuid   — stable UUID derived from thread_id + track_name at
    //                  session creation; used as Perfetto CounterTrack id.
    //   timestamp_ns — CLOCK_MONOTONIC_RAW value at the sample point.
    //   counter_name — human-readable name, e.g. "ipc" or "cpi".
    //   value        — the floating-point metric value.
    // ---------------------------------------------------------------------------
    static void EmitCounterPacket(uint64_t track_uuid,
                                  uint64_t timestamp_ns,
                                  const std::string& counter_name,
                                  double value);

    // Emit a CounterTrack descriptor packet (sent once per track at
    // PmuSession::Create() time so the trace viewer knows the track name).
    static void EmitTrackDescriptor(uint64_t track_uuid,
                                    const std::string& track_name,
                                    const std::string& counter_name);
};

}  // namespace pmu_metrics::internal

// Required by Perfetto SDK — placed in the header so the macro sees the
// full class definition.  The companion PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS
// must appear exactly once in pmu_data_source.cc.
PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(pmu_metrics::internal::PmuDataSource);
