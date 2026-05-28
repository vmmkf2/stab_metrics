// pmu_metrics/src/perf_group.h  (PRIVATE)
//
// Thin RAII wrapper around a perf_event_open(2) file descriptor group.
// Only the two events needed for IPC/CPI on aarch64 are opened initially:
//   leader : PERF_COUNT_HW_CPU_CYCLES
//   member : PERF_COUNT_HW_INSTRUCTIONS
//
// The group leader is opened with PERF_FORMAT_GROUP | PERF_FORMAT_SCALE so a
// single read(2) returns all members plus time_enabled / time_running for
// multiplexing detection.

#pragma once

#include <cstdint>
#include <optional>

// Forward-declare the kernel struct to avoid pulling linux/perf_event.h into
// every TU that includes this header.
struct perf_event_attr;

namespace pmu_metrics::internal {

// Layout of the buffer returned by read() on a PERF_FORMAT_GROUP fd.
// Matches the kernel ABI exactly (little-endian, no padding).
struct PerfGroupReadFormat {
    uint64_t nr;           // number of events in the group
    uint64_t time_enabled; // ns the group was enabled
    uint64_t time_running; // ns counters actually ran (< time_enabled → multiplexed)
    struct {
        uint64_t value;
        uint64_t id;       // only present with PERF_FORMAT_ID — omitted for simplicity
    } values[2];           // [0] = cycles, [1] = instructions
};

// ---------------------------------------------------------------------------
// PerfGroup
// ---------------------------------------------------------------------------
class PerfGroup {
public:
    // Open the counter group for the calling thread (pid=0, cpu=-1).
    // Returns nullopt on failure (sets errno).
    [[nodiscard]] static std::optional<PerfGroup> Open() noexcept;

    ~PerfGroup();

    PerfGroup(const PerfGroup&)            = delete;
    PerfGroup& operator=(const PerfGroup&) = delete;
    PerfGroup(PerfGroup&&) noexcept;
    PerfGroup& operator=(PerfGroup&&) noexcept;

    // Reset all counters (ioctl PERF_EVENT_IOC_RESET + ENABLE).
    bool Reset() noexcept;

    // Read the current accumulated values since the last Reset().
    // Returns false if the read(2) syscall fails.
    [[nodiscard]] bool Read(PerfGroupReadFormat& out) const noexcept;

    bool IsValid() const noexcept { return leader_fd_ >= 0; }

private:
    int leader_fd_{-1};    // PERF_COUNT_HW_CPU_CYCLES  (group leader)
    int member_fd_{-1};    // PERF_COUNT_HW_INSTRUCTIONS

    PerfGroup(int leader_fd, int member_fd) noexcept
        : leader_fd_(leader_fd), member_fd_(member_fd) {}
};

}  // namespace pmu_metrics::internal
