// pmu_metrics/src/pmu_session.cc
//
// Implements PmuSession, IsPerfAvailable(), and HostArchitecture().
// All perf_event fd management is delegated to PerfGroup (perf_group.h).
// All Perfetto I/O is delegated to PmuDataSource (pmu_data_source.h).

#include "pmu_metrics/pmu_metrics.h"

#include "perf_group.h"
#include "pmu_data_source.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <thread>

namespace pmu_metrics {

// ---------------------------------------------------------------------------
// Pimpl
// ---------------------------------------------------------------------------
struct PmuSession::Impl {
    internal::PerfGroup   group;
    MetricsSnapshot       last_snapshot;
    uint64_t              track_uuid_ipc{0};
    uint64_t              track_uuid_cpi{0};
    std::string           track_name;

    explicit Impl(internal::PerfGroup g, std::string name,
                  uint64_t uuid_ipc, uint64_t uuid_cpi)
        : group(std::move(g))
        , track_name(std::move(name))
        , track_uuid_ipc(uuid_ipc)
        , track_uuid_cpi(uuid_cpi) {}
};

// ---------------------------------------------------------------------------
// UUID derivation — cheap, stable per (thread, name) pair.
// Uses std::hash to combine thread::id and the track name string.
// NOT cryptographic; collision probability is negligible for typical use.
// ---------------------------------------------------------------------------
static uint64_t MakeTrackUuid(std::thread::id tid, const std::string& name,
                               const std::string& counter) {
    std::size_t h = std::hash<std::thread::id>{}(tid);
    h ^= std::hash<std::string>{}(name)    + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<std::string>{}(counter) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return static_cast<uint64_t>(h);
}

// ---------------------------------------------------------------------------
// PmuSession::Create
// ---------------------------------------------------------------------------
std::optional<PmuSession> PmuSession::Create(const PmuConfig& cfg) {
    auto group = internal::PerfGroup::Open();
    if (!group) return std::nullopt;

    auto tid = std::this_thread::get_id();

    std::string name;
    if (cfg.track_name.empty()) {
        // Build a default name from the thread id numeric value.
        std::ostringstream oss;
        oss << "pmu_metrics/" << tid;
        name = oss.str();
    } else {
        name = std::string(cfg.track_name);
    }

    uint64_t uuid_ipc = MakeTrackUuid(tid, name, "ipc");
    uint64_t uuid_cpi = MakeTrackUuid(tid, name, "cpi");

    // Emit CounterTrack descriptor packets once so trace viewers label them.
    internal::PmuDataSource::EmitTrackDescriptor(uuid_ipc, name, "ipc");
    internal::PmuDataSource::EmitTrackDescriptor(uuid_cpi, name, "cpi");

    auto* impl = new Impl(std::move(*group), name, uuid_ipc, uuid_cpi);
    return PmuSession(impl);
}

// ---------------------------------------------------------------------------
// PmuSession::Tick
// ---------------------------------------------------------------------------
MetricsSnapshot PmuSession::Tick() {
    MetricsSnapshot snap;

    // Wall-clock timestamp.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    snap.timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                      + static_cast<uint64_t>(ts.tv_nsec);

    // Read counters from the kernel.
    internal::PerfGroupReadFormat raw{};
    if (!impl_ || !impl_->group.Read(raw)) {
        // Counters unavailable (stub or fd error) — return zero snapshot.
        if (impl_) impl_->last_snapshot = snap;
        return snap;
    }

    snap.instructions = raw.values[1].value;
    snap.cycles       = raw.values[0].value;

    // Detect kernel multiplexing.
    snap.scaled = (raw.time_running > 0) &&
                  (raw.time_running < raw.time_enabled);

    // Scale raw values if multiplexed.
    if (snap.scaled && raw.time_running > 0) {
        double factor = static_cast<double>(raw.time_enabled)
                      / static_cast<double>(raw.time_running);
        snap.instructions = static_cast<uint64_t>(snap.instructions * factor);
        snap.cycles       = static_cast<uint64_t>(snap.cycles       * factor);
    }

    // Derive IPC / CPI.
    if (snap.cycles > 0) {
        snap.ipc = static_cast<double>(snap.instructions)
                 / static_cast<double>(snap.cycles);
        snap.cpi = 1.0 / snap.ipc;
    }

    // Emit Perfetto counter packets.
    internal::PmuDataSource::EmitCounterPacket(
        impl_->track_uuid_ipc, snap.timestamp_ns, "ipc", snap.ipc);
    internal::PmuDataSource::EmitCounterPacket(
        impl_->track_uuid_cpi, snap.timestamp_ns, "cpi", snap.cpi);

    // Reset the counter group for the next interval.
    impl_->group.Reset();

    impl_->last_snapshot = snap;
    return snap;
}

// ---------------------------------------------------------------------------
// PmuSession::LastSnapshot
// ---------------------------------------------------------------------------
const MetricsSnapshot& PmuSession::LastSnapshot() const noexcept {
    static const MetricsSnapshot kEmpty{};
    return impl_ ? impl_->last_snapshot : kEmpty;
}

// ---------------------------------------------------------------------------
// Destructor / move
// ---------------------------------------------------------------------------
PmuSession::PmuSession(Impl* impl) noexcept : impl_(impl) {}

PmuSession::~PmuSession() {
    delete impl_;
}

PmuSession::PmuSession(PmuSession&& o) noexcept : impl_(o.impl_) {
    o.impl_ = nullptr;
}

PmuSession& PmuSession::operator=(PmuSession&& o) noexcept {
    if (this != &o) {
        delete impl_;
        impl_   = o.impl_;
        o.impl_ = nullptr;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// IsPerfAvailable
// ---------------------------------------------------------------------------
bool IsPerfAvailable() noexcept {
    // Read /proc/sys/kernel/perf_event_paranoid.
    // Values <= 1 allow unprivileged use; 2+ blocks most events.
    FILE* f = std::fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!f) return false;
    int paranoid = 3;
    std::fscanf(f, "%d", &paranoid);
    std::fclose(f);
    return paranoid <= 1;
}

// ---------------------------------------------------------------------------
// HostArchitecture
// ---------------------------------------------------------------------------
std::string_view HostArchitecture() noexcept {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

}  // namespace pmu_metrics
