// pmu_metrics/include/pmu_metrics/pmu_metrics_host_shim.h
//
// HOST-SIDE ONLY.  Compiled into the host binary, NOT into libpmu_metrics.so.
//
// Provides a minimal bridge between the host's existing Perfetto write path
// and the pmu_metrics C ABI.  The host project already has its own Perfetto
// DataSource and trace session management — this shim does NOT add a second
// DataSource.  It simply implements pmu_metrics_write_fn and
// pmu_metrics_describe_fn in terms of whichever Perfetto writer the host
// already uses.
//
// ── How to use ───────────────────────────────────────────────────────────────
//
//   The host calls pmu_metrics_init() once, passing a pmu_metrics_init_args_t
//   populated with the two helpers below:
//
//     pmu_metrics_init_args_t args{};
//     args.write_fn       = pmu_metrics::host::WriteFn;
//     args.describe_fn    = pmu_metrics::host::DescribeFn;
//     args.userdata       = my_perfetto_context_ptr;  // or nullptr
//     args.track_name_prefix = "pmu_ext";
//     pmu_metrics_init(&args);
//
//   The host is responsible for providing a Perfetto TraceWriter / DataSource
//   context.  This header does not dictate how — it only shows the expected
//   call sites so you can wire them to whatever your project already has.
//
// ── What the shim does NOT do ────────────────────────────────────────────────
//   • Does NOT call perfetto::Tracing::Initialize() — that's the host's job.
//   • Does NOT register a DataSource — use the host's existing one.
//   • Does NOT hold any global Perfetto state.
//
// ── GN ───────────────────────────────────────────────────────────────────────
//   source_set("pmu_metrics_shim") {
//     public = [ "include/pmu_metrics/pmu_metrics_host_shim.h" ]
//     public_deps = [
//       "//third_party/perfetto:perfetto_sdk",
//       "//third_party/pmu_metrics:pmu_metrics_headers",
//     ]
//     deps = [ "//third_party/pmu_metrics:pmu_metrics" ]  # link the .so
//   }

#pragma once

#include <perfetto.h>
#include "pmu_metrics_c.h"

namespace pmu_metrics::host {

// ─────────────────────────────────────────────────────────────────────────────
// WriteFn
//
// Pass as pmu_metrics_init_args_t::write_fn.
//
// `userdata` is whatever pointer your project needs to reach its Perfetto
// DataSource / TraceWriter.  The implementation below shows the pattern using
// a generic DataSource::Trace() call — adapt the DataSource type to your own.
//
// The function is intentionally left as a template / sketch because the host
// project owns the concrete DataSource type.  Copy and specialise it in one
// of your own .cc files.
//
//   template<typename DS>           // DS = your project's DataSource subclass
//   static void WriteFn(void*       /*userdata — not needed for in-proc DS*/,
//                       uint64_t    track_uuid,
//                       uint64_t    timestamp_ns,
//                       const char* /*metric_name*/,
//                       double      value) {
//       DS::Trace([&](typename DS::TraceContext ctx) {
//           auto packet = ctx.NewTracePacket();
//           packet->set_timestamp(timestamp_ns);
//           auto* te = packet->set_track_event();
//           te->set_track_uuid(track_uuid);
//           te->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
//           te->set_counter_value_double(value);
//       });
//   }
//
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// DescribeFn
//
// Pass as pmu_metrics_init_args_t::describe_fn.
// Called once per (tid, metric) at pmu_metrics_attach() time.
// Emits a CounterTrack descriptor into the host's trace so the viewer shows
// the right track name and unit.
//
//   template<typename DS>
//   static void DescribeFn(void*       /*userdata*/,
//                          uint64_t    track_uuid,
//                          const char* metric_name,
//                          const char* unit) {
//       DS::Trace([&](typename DS::TraceContext ctx) {
//           auto packet = ctx.NewTracePacket();
//           packet->set_timestamp(0);
//           auto* td = packet->set_track_descriptor();
//           td->set_uuid(track_uuid);
//           td->set_name(metric_name);
//           auto* ctr = td->set_counter();
//           ctr->set_unit_name(unit);
//           ctr->set_is_incremental(false);
//       });
//   }
//
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// PmuMetricsManager
//
// Optional RAII helper that manages the pmu_metrics_init / pmu_metrics_shutdown
// lifetime and wraps attach / detach / sample in typed calls.
//
// The host constructs one instance at startup, passing its own write / describe
// function pointers.
//
// Example:
//   // At startup (after Perfetto::Initialize):
//   auto pmu = pmu_metrics::host::PmuMetricsManager(
//       &MyDataSource::WriteFn,
//       &MyDataSource::DescribeFn,
//       nullptr,        // userdata
//       "pmu_ext"       // track name prefix
//   );
//
//   // When a worker thread starts:
//   pmu.Attach(gettid());
//
//   // In the host's periodic sampling loop (any thread):
//   pmu.Sample(worker_tid);
//
//   // When a worker thread finishes:
//   pmu.Detach(worker_tid);
//
//   // Destructor calls pmu_metrics_shutdown() automatically.
// ─────────────────────────────────────────────────────────────────────────────
class PmuMetricsManager {
public:
    PmuMetricsManager(pmu_metrics_write_fn    write_fn,
                      pmu_metrics_describe_fn describe_fn,   // may be nullptr
                      void*                   userdata,
                      const char*             track_name_prefix = "pmu_metrics") {
        pmu_metrics_init_args_t args{};
        args.write_fn          = write_fn;
        args.describe_fn       = describe_fn;
        args.userdata          = userdata;
        args.track_name_prefix = track_name_prefix;
        args.flags             = 0;
        ok_ = (pmu_metrics_init(&args) == PMU_METRICS_OK);
    }

    ~PmuMetricsManager() { pmu_metrics_shutdown(); }

    // Non-copyable, non-movable (owns global library state).
    PmuMetricsManager(const PmuMetricsManager&)            = delete;
    PmuMetricsManager& operator=(const PmuMetricsManager&) = delete;

    explicit operator bool() const noexcept { return ok_; }

    // Returns PMU_METRICS_OK or an error code.
    int Attach(uint32_t tid) { return pmu_metrics_attach(tid); }
    int Detach(uint32_t tid) { return pmu_metrics_detach(tid); }
    int Sample(uint32_t tid) { return pmu_metrics_sample(tid); }

private:
    bool ok_{false};
};

}  // namespace pmu_metrics::host
