// pmu_metrics/pmu_metrics.h
// Public API for the pmu_metrics static library.
//
// Design contract:
//   - Raw perf_event_attr structs, file descriptors, and counter values are
//     NEVER exposed through this header.
//   - Perfetto DataSource registration happens automatically during static
//     initialisation (before main()).  The host must NOT call
//     perfetto::Tracing::Initialize(); that remains the host's responsibility.
//   - Counter scope: thread-local  (pid = 0, cpu = -1).
//   - Collection model: explicit host-driven Tick().  No background thread.
//   - Target architecture: aarch64.  Initial metrics: IPC, CPI.
//     (Expand with MPKI / Branch Miss Rate / TMA later.)

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace pmu_metrics {

// ---------------------------------------------------------------------------
// MetricsSnapshot — aggregated, architecture-neutral metrics.
// All raw counter values are hidden inside the library.
// ---------------------------------------------------------------------------
struct MetricsSnapshot {
    // Cycles and instruction counts for the measured interval.
    // Populated after every successful Tick().
    uint64_t instructions{0};
    uint64_t cycles{0};

    // Derived metrics (NaN / 0 when counters are unavailable).
    double ipc{0.0};   // Instructions Per Cycle
    double cpi{0.0};   // Cycles Per Instruction

    // Nanosecond wall-clock timestamp of this snapshot (CLOCK_MONOTONIC_RAW).
    uint64_t timestamp_ns{0};

    // True if the kernel multiplexed this group (scaled values are estimates).
    bool scaled{false};
};

// ---------------------------------------------------------------------------
// PmuConfig — passed once to PmuSession::Create().
// ---------------------------------------------------------------------------
struct PmuConfig {
    // Name embedded in the Perfetto CounterTrack descriptor.
    // Defaults to "pmu_metrics/<thread_id>" if left empty.
    std::string_view track_name{};

    // Reserved for future expansion (MPKI, branch miss rate, TMA …).
    // Ignored in the current stub.
    bool enable_mpki{false};
    bool enable_branch_miss_rate{false};
    bool enable_tma_level1{false};  // aarch64: not yet implemented
};

// ---------------------------------------------------------------------------
// PmuSession — opaque handle owning one set of perf fd groups for the
// calling thread.  Non-copyable, non-movable.
//
// Typical host usage:
//
//   auto session = pmu_metrics::PmuSession::Create(config);
//   if (!session) { /* perf_event_open unavailable */ }
//
//   // … work …
//   auto snap = session->Tick();     // read counters, emit Perfetto packet
//   use(snap.ipc, snap.cpi);
//
//   // session destructor closes fds and cleans up.
// ---------------------------------------------------------------------------
class PmuSession {
public:
    // Factory.  Returns nullopt if perf_event_open fails (e.g. perf_event_paranoid
    // too restrictive, or running on a non-aarch64 host in a future guard).
    [[nodiscard]] static std::optional<PmuSession> Create(const PmuConfig& cfg = {});

    // Destructor closes all perf fds opened during Create().
    ~PmuSession();

    // Non-copyable.
    PmuSession(const PmuSession&)            = delete;
    PmuSession& operator=(const PmuSession&) = delete;

    // Movable so optional<PmuSession> works.
    PmuSession(PmuSession&&) noexcept;
    PmuSession& operator=(PmuSession&&) noexcept;

    // Read counters for the interval since the last Tick() (or Create()),
    // derive IPC/CPI, and emit one CounterTrack packet into the host's
    // Perfetto trace buffer.
    //
    // Thread-safety: must be called from the same thread that called Create().
    // Returns the aggregated snapshot so the host can act on it immediately.
    [[nodiscard]] MetricsSnapshot Tick();

    // Returns the most recent snapshot without reading counters or emitting
    // a Perfetto packet.  Useful for logging / assertions.
    [[nodiscard]] const MetricsSnapshot& LastSnapshot() const noexcept;

private:
    // Pimpl to keep perf internals fully hidden from host TUs.
    struct Impl;
    Impl* impl_{nullptr};

    explicit PmuSession(Impl* impl) noexcept;
};

// ---------------------------------------------------------------------------
// Library-level helpers (no PmuSession needed).
// ---------------------------------------------------------------------------

// Returns true if perf_event_open is expected to succeed on this kernel
// (checks /proc/sys/kernel/perf_event_paranoid).
[[nodiscard]] bool IsPerfAvailable() noexcept;

// Returns a string like "aarch64" or "x86_64" for the detected host CPU.
[[nodiscard]] std::string_view HostArchitecture() noexcept;

}  // namespace pmu_metrics
