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
#include <optional>
#include <string_view>

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

struct Sample {
    // Monotonic wall-clock timestamp at the moment of sampling
    // (CLOCK_MONOTONIC_RAW, nanoseconds).
    uint64_t timestamp_ns = 0;

    // Raw counter accumulations since the last sample() call (or Init()).
    // Present only when the corresponding Counter bit was set in InitConfig.
    // In the stub: incremented by a fixed delta each call so the host can
    // verify the plumbing without real hardware counters.
    std::optional<uint64_t> instructions;  // raw retired instructions
    std::optional<uint64_t> cycles;        // raw CPU cycles

    // Derived metrics.  Computed from instructions/cycles above.
    // NaN (0.0) when the underlying counters are unavailable.
    std::optional<double> ipc;  // instructions / cycles
    std::optional<double> cpi;  // cycles / instructions
};

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
    // Returns a zeroed Sample if Init() has not been called.
    [[nodiscard]] Sample Sample();

    // True after a successful Init().
    bool IsReady() const noexcept { return ready_; }

    // The config that was passed to the last successful Init().
    const InitConfig& Config() const noexcept { return cfg_; }

private:
    InitConfig cfg_{};
    bool       ready_{false};

    // Stub state: monotonic counters incremented on each Sample() call.
    uint64_t stub_instructions_{0};
    uint64_t stub_cycles_{0};
};

// ── Utility ──────────────────────────────────────────────────────────────────

// Human-readable name for a Counter bit, e.g. "ipc".
std::string_view CounterName(Counter c) noexcept;

}  // namespace pmu_metrics
