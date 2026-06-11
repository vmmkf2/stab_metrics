// pmu_metrics/src/pmu_metrics.cc
//
// Stub implementation.  No perf_event_open, no Perfetto, no pthreads.
//
// Counter values are monotonic integers that increment by a fixed delta on
// every Sample() call so the host can verify plumbing without real hardware.

#include "pmu_metrics/pmu_metrics.h"

#include <ctime>

namespace pmu_metrics {

// Fixed increments per Sample() call used by the stub.
// Replace with real perf_event_open reads in the next iteration.
static constexpr uint64_t kStubInstrDelta  = 1'000'000;   // 1M instructions
static constexpr uint64_t kStubCyclesDelta =   500'000;   // 500k cycles  → IPC≈2

// ── PmuMetricsManager::Init ──────────────────────────────────────────────────

bool PmuMetricsManager::Init(const InitConfig& cfg) {
    cfg_   = cfg;
    ready_ = true;

    // Reset stub counters so a fresh sequence starts after each Init().
    stub_instructions_ = 0;
    stub_cycles_       = 0;

    // TODO (next iteration): open perf_event_open fd group here.
    //   pid = (cfg.tid == 0) ? gettid() : cfg.tid
    //   cpu = cfg.cpu

    return true;
}

// ── PmuMetricsManager::Sample ────────────────────────────────────────────────

pmu_metrics::Sample PmuMetricsManager::Sample() {
    pmu_metrics::Sample s;

    if (!ready_) return s;

    // Timestamp.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    s.timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                   + static_cast<uint64_t>(ts.tv_nsec);

    // Stub: advance monotonic accumulators.
    // TODO: replace with a real read(fd, &perf_group_read_format, sizeof(...)).
    stub_instructions_ += kStubInstrDelta;
    stub_cycles_       += kStubCyclesDelta;

    // Populate raw counters.
    s.instructions = stub_instructions_;
    s.cycles       = stub_cycles_;

    // Derive requested metrics.
    if (cfg_.counters & Counter::IPC && stub_cycles_ > 0) {
        s.ipc = static_cast<double>(stub_instructions_)
              / static_cast<double>(stub_cycles_);
    }
    if (cfg_.counters & Counter::CPI && stub_cycles_ > 0) {
        s.cpi = static_cast<double>(stub_cycles_)
              / static_cast<double>(stub_instructions_);
    }

    return s;
}

// ── CounterName ──────────────────────────────────────────────────────────────

std::string_view CounterName(Counter c) noexcept {
    switch (c) {
        case Counter::IPC: return "ipc";
        case Counter::CPI: return "cpi";
        default:           return "unknown";
    }
}

}  // namespace pmu_metrics
