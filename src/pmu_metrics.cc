// pmu_metrics/src/pmu_metrics.cc
//
// Stub implementation.  No perf_event_open, no Perfetto, no pthreads.
//
// Sample() returns a vector<pair<int,int>> of (counter_id, value).
// Values are monotonic integers derived from a per-call counter so the
// host can verify plumbing without real PMU hardware.
//
// Stub formulas (replace with real perf reads next iteration):
//   IPC = stub_call_count * 2    (pretend IPC grows by 2 each interval)
//   CPI = 1                      (constant until real counters land)

#include "pmu_metrics/pmu_metrics.h"

namespace pmu_metrics {

bool PmuMetricsManager::Init(const InitConfig& cfg) {
    cfg_             = cfg;
    ready_           = true;
    stub_call_count_ = 0;
    // TODO: open perf_event_open fd group for cfg.tid / cfg.cpu
    return true;
}

Sample PmuMetricsManager::Sample() {
    Sample result;
    if (!ready_) return result;

    ++stub_call_count_;

    // TODO: read real fd group here; compute from actual instructions/cycles.
    if (cfg_.counters & Counter::IPC) {
        result.emplace_back(static_cast<int>(Counter::IPC),
                            static_cast<int>(stub_call_count_ * 2));
    }
    if (cfg_.counters & Counter::CPI) {
        result.emplace_back(static_cast<int>(Counter::CPI),
                            1);
    }

    return result;
}

std::string_view CounterName(Counter c) noexcept {
    switch (c) {
        case Counter::IPC: return "ipc";
        case Counter::CPI: return "cpi";
        default:           return "unknown";
    }
}

}  // namespace pmu_metrics
