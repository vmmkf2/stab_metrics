// pmu_metrics/include/pmu_metrics/pmu_metrics_host_shim.h
//
// HOST-SIDE ONLY.  Include this in the host project, NOT in libpmu_metrics.so.
//
// Provides:
//   1. PmuMetricsDataSource — Perfetto DataSource that must be registered in
//      the host before perfetto::Tracing::Initialize().
//   2. PmuMetricsShim — C++ wrapper that creates a pmu_metrics_session_t,
//      wires the writer callback to PmuMetricsDataSource, and exposes a
//      typed Tick() returning pmu_metrics_snapshot_t.
//
// ── Dependency graph ────────────────────────────────────────────────────────
//
//   host binary
//     ├── Perfetto SDK          (host-owned, single instance)
//     ├── pmu_metrics_host_shim.h  (header-only, compiled into host)
//     │     └── #include <perfetto.h>
//     │     └── #include "pmu_metrics_c.h"   (C ABI from .so)
//     └── libpmu_metrics.so     (NO Perfetto symbols)
//
// ── GN usage ────────────────────────────────────────────────────────────────
//
//   # In the host BUILD.gn:
//   source_set("pmu_metrics_shim") {
//     sources = []   # header-only
//     public = [ "//third_party/pmu_metrics/include/pmu_metrics/pmu_metrics_host_shim.h" ]
//     public_deps = [
//       "//third_party/perfetto:perfetto_sdk",
//       "//third_party/pmu_metrics:pmu_metrics_headers",   # just the C header
//     ]
//   }
//
// ── Registration (before main via static init) ───────────────────────────────
//
//   In exactly ONE host .cc file:
//     #define PMU_METRICS_DEFINE_DATA_SOURCE
//     #include "pmu_metrics/pmu_metrics_host_shim.h"
//
//   In all other host .cc files that use the shim:
//     #include "pmu_metrics/pmu_metrics_host_shim.h"

#pragma once

#include <perfetto.h>
#include "pmu_metrics_c.h"

#include <atomic>
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PmuMetricsDataSource
//
// Perfetto DataSource registered in the host.  It provides static helpers
// that the writer callback calls to emit CounterTrack packets.
// ─────────────────────────────────────────────────────────────────────────────
class PmuMetricsDataSource
    : public perfetto::DataSource<PmuMetricsDataSource> {
public:
    static constexpr char kName[] = "dev.pmu_metrics";

    void OnSetup(const SetupArgs&) override {}
    void OnStart(const StartArgs&) override {}
    void OnStop(const StopArgs&) override {}

    // Emit a CounterTrack descriptor (call once per track at session init).
    static void EmitDescriptor(uint64_t    track_uuid,
                               const char* track_name,
                               const char* metric_name) {
        PmuMetricsDataSource::Trace(
            [&](PmuMetricsDataSource::TraceContext ctx) {
                auto packet = ctx.NewTracePacket();
                packet->set_timestamp(0);
                auto* td = packet->set_track_descriptor();
                td->set_uuid(track_uuid);
                td->set_name(std::string(track_name) + "/" + metric_name);
                auto* ctr = td->set_counter();
                ctr->set_unit_name(
                    std::string(metric_name) == "ipc" ? "instr/cycle"
                                                      : "cycle/instr");
                ctr->set_is_incremental(false);
            });
    }

    // Emit one counter sample.
    static void EmitSample(uint64_t    track_uuid,
                           uint64_t    timestamp_ns,
                           double      value) {
        PmuMetricsDataSource::Trace(
            [&](PmuMetricsDataSource::TraceContext ctx) {
                auto packet = ctx.NewTracePacket();
                packet->set_timestamp(timestamp_ns);
                auto* te = packet->set_track_event();
                te->set_track_uuid(track_uuid);
                te->set_type(
                    perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
                te->set_counter_value_double(value);
            });
    }
};

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(PmuMetricsDataSource);

// In exactly ONE translation unit the host must define:
//   #define PMU_METRICS_DEFINE_DATA_SOURCE
//   #include "pmu_metrics_host_shim.h"
#ifdef PMU_METRICS_DEFINE_DATA_SOURCE
PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(PmuMetricsDataSource);

namespace {
// Triggers DataSource::Register() before main().
struct PmuMetricsRegistrar {
    PmuMetricsRegistrar() {
        perfetto::DataSourceDescriptor dsd;
        dsd.set_name(PmuMetricsDataSource::kName);
        PmuMetricsDataSource::Register(dsd);
    }
};
[[maybe_unused]] volatile PmuMetricsRegistrar g_pmu_registrar;
}  // namespace
#endif  // PMU_METRICS_DEFINE_DATA_SOURCE

// ─────────────────────────────────────────────────────────────────────────────
// Writer callbacks (C linkage) wired to PmuMetricsDataSource.
// These are passed into pmu_metrics_config_t by PmuMetricsShim.
// ─────────────────────────────────────────────────────────────────────────────
namespace pmu_metrics_shim_detail {

inline void WriterFn(void*       /*userdata*/,
                     uint64_t    track_uuid,
                     uint64_t    timestamp_ns,
                     const char* /*metric_name*/,
                     double      value) {
    PmuMetricsDataSource::EmitSample(track_uuid, timestamp_ns, value);
}

inline void DescriptorFn(void*       /*userdata*/,
                         uint64_t    track_uuid,
                         const char* track_name,
                         const char* metric_name) {
    PmuMetricsDataSource::EmitDescriptor(track_uuid, track_name, metric_name);
}

}  // namespace pmu_metrics_shim_detail

// ─────────────────────────────────────────────────────────────────────────────
// PmuMetricsShim
//
// Thin C++ RAII wrapper.  Creates a pmu_metrics_session_t, wires callbacks,
// and exposes a typed interface.
//
// Usage:
//   PmuMetricsShim pmu("my_benchmark");
//   if (!pmu) { /* perf unavailable */ }
//
//   auto snap = pmu.Tick();
//   use(snap.ipc, snap.cpi);
// ─────────────────────────────────────────────────────────────────────────────
class PmuMetricsShim {
public:
    explicit PmuMetricsShim(const char* track_name = nullptr) {
        pmu_metrics_config_t cfg{};
        cfg.track_name    = track_name;
        cfg.writer_fn     = pmu_metrics_shim_detail::WriterFn;
        cfg.descriptor_fn = pmu_metrics_shim_detail::DescriptorFn;
        cfg.userdata      = nullptr;
        cfg.flags         = 0;
        session_ = pmu_metrics_create(&cfg);
    }

    ~PmuMetricsShim() { pmu_metrics_destroy(session_); }

    // Non-copyable, movable.
    PmuMetricsShim(const PmuMetricsShim&)            = delete;
    PmuMetricsShim& operator=(const PmuMetricsShim&) = delete;

    PmuMetricsShim(PmuMetricsShim&& o) noexcept : session_(o.session_) {
        o.session_ = nullptr;
    }
    PmuMetricsShim& operator=(PmuMetricsShim&& o) noexcept {
        if (this != &o) {
            pmu_metrics_destroy(session_);
            session_   = o.session_;
            o.session_ = nullptr;
        }
        return *this;
    }

    explicit operator bool() const noexcept { return session_ != nullptr; }

    pmu_metrics_snapshot_t Tick() {
        pmu_metrics_snapshot_t snap{};
        if (session_) pmu_metrics_tick(session_, &snap);
        return snap;
    }

    const pmu_metrics_snapshot_t* LastSnapshot() const noexcept {
        return session_ ? pmu_metrics_last_snapshot(session_) : nullptr;
    }

private:
    pmu_metrics_session_t* session_{nullptr};
};
