// pmu_metrics/src/perf_group.cc
//
// STUB implementation of PerfGroup.
// TODO: replace stub bodies with real perf_event_open(2) syscalls.

#include "perf_group.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>

// Linux perf_event_open header — only needed in this TU.
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

namespace pmu_metrics::internal {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static long perf_event_open(struct perf_event_attr* attr,
                             pid_t pid, int cpu,
                             int group_fd, unsigned long flags) noexcept {
    return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// ---------------------------------------------------------------------------
// PerfGroup::Open  — STUB
//
// TODO: fill in the perf_event_attr fields for aarch64:
//   leader : PERF_TYPE_HARDWARE / PERF_COUNT_HW_CPU_CYCLES
//   member : PERF_TYPE_HARDWARE / PERF_COUNT_HW_INSTRUCTIONS
//   Both   : .exclude_kernel = 1 (userspace only, no CAP_SYS_ADMIN needed)
//             .disabled      = 1 (start explicitly via ioctl ENABLE)
//             .read_format   = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED
//                              | PERF_FORMAT_TOTAL_TIME_RUNNING
// ---------------------------------------------------------------------------
std::optional<PerfGroup> PerfGroup::Open() noexcept {
    // --- STUB: always return nullopt until the real implementation lands ---
    // Real implementation sketch:
    //
    //   struct perf_event_attr leader_attr{};
    //   leader_attr.type           = PERF_TYPE_HARDWARE;
    //   leader_attr.config         = PERF_COUNT_HW_CPU_CYCLES;
    //   leader_attr.size           = sizeof(leader_attr);
    //   leader_attr.disabled       = 1;
    //   leader_attr.exclude_kernel = 1;
    //   leader_attr.exclude_hv     = 1;
    //   leader_attr.read_format    = PERF_FORMAT_GROUP
    //                              | PERF_FORMAT_TOTAL_TIME_ENABLED
    //                              | PERF_FORMAT_TOTAL_TIME_RUNNING;
    //
    //   int leader_fd = perf_event_open(&leader_attr,
    //                                   /*pid=*/0, /*cpu=*/-1,
    //                                   /*group_fd=*/-1, /*flags=*/0);
    //   if (leader_fd < 0) return std::nullopt;
    //
    //   struct perf_event_attr member_attr{};
    //   member_attr.type     = PERF_TYPE_HARDWARE;
    //   member_attr.config   = PERF_COUNT_HW_INSTRUCTIONS;
    //   member_attr.size     = sizeof(member_attr);
    //   member_attr.disabled = 1;
    //   // read_format NOT set on members — only the leader is read.
    //
    //   int member_fd = perf_event_open(&member_attr,
    //                                   /*pid=*/0, /*cpu=*/-1,
    //                                   /*group_fd=*/leader_fd, /*flags=*/0);
    //   if (member_fd < 0) { close(leader_fd); return std::nullopt; }
    //
    //   PerfGroup g(leader_fd, member_fd);
    //   if (!g.Reset()) return std::nullopt;
    //   return g;

    errno = ENOSYS;  // stub signal
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// PerfGroup::Reset  — STUB
// ---------------------------------------------------------------------------
bool PerfGroup::Reset() noexcept {
    if (leader_fd_ < 0) return false;
    // TODO:
    //   ioctl(leader_fd_, PERF_EVENT_IOC_RESET,  PERF_IOC_FLAG_GROUP);
    //   ioctl(leader_fd_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    return false;  // stub
}

// ---------------------------------------------------------------------------
// PerfGroup::Read  — STUB
// ---------------------------------------------------------------------------
bool PerfGroup::Read(PerfGroupReadFormat& out) const noexcept {
    if (leader_fd_ < 0) return false;
    // TODO:
    //   ssize_t n = ::read(leader_fd_, &out, sizeof(out));
    //   return n == sizeof(out);
    (void)out;
    return false;  // stub
}

// ---------------------------------------------------------------------------
// Destructor / move
// ---------------------------------------------------------------------------
PerfGroup::~PerfGroup() {
    if (member_fd_ >= 0) ::close(member_fd_);
    if (leader_fd_ >= 0) ::close(leader_fd_);
}

PerfGroup::PerfGroup(PerfGroup&& o) noexcept
    : leader_fd_(o.leader_fd_), member_fd_(o.member_fd_) {
    o.leader_fd_ = -1;
    o.member_fd_ = -1;
}

PerfGroup& PerfGroup::operator=(PerfGroup&& o) noexcept {
    if (this != &o) {
        if (member_fd_ >= 0) ::close(member_fd_);
        if (leader_fd_ >= 0) ::close(leader_fd_);
        leader_fd_   = o.leader_fd_;
        member_fd_   = o.member_fd_;
        o.leader_fd_ = -1;
        o.member_fd_ = -1;
    }
    return *this;
}

}  // namespace pmu_metrics::internal
