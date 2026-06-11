// pmu_metrics/include/pmu_metrics/pmu_metrics.h
//
// Minimal stub library for integrating PMU counter collection into the host
// project and determining the right interface.
//
// Goals of this iteration:
//   - Zero dependencies (no Perfetto, no pthreads, no linux/perf_event.h).
//   - Caller decides which thread and CPU to measure — library is passive.
//   - sample() returns a plain struct; no callbacks, no write-back.
//   - Counter values are monotonic integers (stub implementation increments
//     them by a fixed delta each call so the host can verify plumbing end-to-end).
//
// Non-goals of this iteration:
//   - Real perf_event_open calls.
//   - Perfetto integration.
//   - Thread safety / concurrent sessions.

#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace pmu_metrics {

// ── Counter selector ─────────────────────────────────────────────────────────
//
// Bitmask passed to PmuMetricsManager::Init() to choose which counters
// the session should collect.  Combine with |.
//
enum class Counter : uint32_t {
    IPC = 1 << 0,   // Instructions Per Cycle   (instructions / cycles)
    CPI = 1 << 1,   // Cycles Per Instruction   (cycles / instructions)
    // Reserved for future expansion:
    // MPKI          = 1 << 2,
    // BRANCH_MISS   = 1 << 3,
};

inline Counter operator|(Counter a, Counter b) {
    return static_cast<Counter>(
        static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(Counter a, Counter b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// ── Init config ──────────────────────────────────────────────────────────────

struct InitConfig {
    // Which derived metrics to collect.
    Counter counters = Counter::IPC | Counter::CPI;

    // Linux thread id to measure.
    // 0 means "the calling thread" (resolved at Init() time via gettid()).
    uint32_t tid = 0;

    // CPU to pin measurement to.
    // -1 means "any CPU the thread runs on" (follow the thread).
    int cpu = -1;
};

// ── Sample result ────────────────────────────────────────────────────────────
//
// A flat list of (counter_id, value) pairs — one entry per requested metric.
// counter_id matches the Counter enum values (cast to int).
// Only the metrics that were enabled in InitConfig are present.
//
// Example for Counter::IPC | Counter::CPI:
//   { {1, 2}, {2, 0} }   →  IPC=2, CPI=0  (stub iteration 1)

using Sample = std::vector<std::pair<int, int>>;

// ── PmuMetricsManager ────────────────────────────────────────────────────────
//
// Owns one measurement session.  Non-copyable.
//
// Typical usage:
//
//   pmu_metrics::PmuMetricsManager mgr;
//   mgr.Init({ .counters = pmu_metrics::Counter::IPC | pmu_metrics::Counter::CPI,
//              .tid = gettid(),
//              .cpu = -1 });
//
//   // ... do work ...
//
//   auto s = mgr.Sample();
//   printf("IPC=%.2f  CPI=%.2f\n", s.ipc.value_or(0), s.cpi.value_or(0));
//
class PmuMetricsManager {
public:
    PmuMetricsManager()  = default;
    ~PmuMetricsManager() = default;

    PmuMetricsManager(const PmuMetricsManager&)            = delete;
    PmuMetricsManager& operator=(const PmuMetricsManager&) = delete;

    PmuMetricsManager(PmuMetricsManager&&)            = default;
    PmuMetricsManager& operator=(PmuMetricsManager&&) = default;

    // Configure and arm the session.
    // May be called multiple times to reconfigure (implicitly resets counters).
    // Returns false if the configuration is invalid.
    bool Init(const InitConfig& cfg = {});

    // Read counters, compute derived metrics, reset accumulators.
    // Returns an empty vector if Init() has not been called.
    // Each pair: { static_cast<int>(Counter::XXX), integer_value }.
    [[nodiscard]] Sample Sample();

    // True after a successful Init().
    bool IsReady() const noexcept { return ready_; }

    // The config that was passed to the last successful Init().
    const InitConfig& Config() const noexcept { return cfg_; }

private:
    InitConfig cfg_{};
    bool       ready_{false};

    // Stub state: call counter incremented on each Sample() call.
    uint64_t stub_call_count_{0};
};

// ── Utility ──────────────────────────────────────────────────────────────────

// Human-readable name for a Counter bit, e.g. "ipc".
std::string_view CounterName(Counter c) noexcept;

}  // namespace pmu_metrics
