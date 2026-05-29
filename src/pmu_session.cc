// pmu_metrics/src/pmu_session.cc
//
// Implements the C ABI (pmu_metrics_c.h).
// ZERO Perfetto includes — all trace emission is via the writer_fn callback
// supplied by the host at pmu_metrics_create() time.

#include "pmu_metrics/pmu_metrics_c.h"

#include "perf_group.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Session internals (fully hidden — not in any public header)
// ---------------------------------------------------------------------------
struct pmu_metrics_session {
    pmu_metrics::internal::PerfGroup group;

    // Callbacks supplied by the host.
    pmu_metrics_writer_fn     writer_fn;
    pmu_metrics_descriptor_fn descriptor_fn;
    void*                     userdata;

    // Stable UUIDs for each emitted metric track.
    uint64_t uuid_ipc;
    uint64_t uuid_cpi;

    // Copy of the track name (owned by this struct).
    std::string track_name;

    // Most recent snapshot.
    pmu_metrics_snapshot_t last_snapshot{};

    pmu_metrics_session(pmu_metrics::internal::PerfGroup g,
                        std::string                      name,
                        uint64_t                         u_ipc,
                        uint64_t                         u_cpi,
                        pmu_metrics_writer_fn            wfn,
                        pmu_metrics_descriptor_fn        dfn,
                        void*                            ud)
        : group(std::move(g))
        , writer_fn(wfn)
        , descriptor_fn(dfn)
        , userdata(ud)
        , uuid_ipc(u_ipc)
        , uuid_cpi(u_cpi)
        , track_name(std::move(name)) {}
};

// ---------------------------------------------------------------------------
// UUID derivation — stable per (thread_id, track_name, metric_name).
// ---------------------------------------------------------------------------
static uint64_t make_uuid(std::thread::id    tid,
                           const std::string& name,
                           const std::string& metric) {
    std::size_t h = std::hash<std::thread::id>{}(tid);
    h ^= std::hash<std::string>{}(name)   + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<std::string>{}(metric) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return static_cast<uint64_t>(h);
}

// ---------------------------------------------------------------------------
// pmu_metrics_create
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
pmu_metrics_session_t* pmu_metrics_create(const pmu_metrics_config_t* cfg) {
    if (!cfg || !cfg->writer_fn) return nullptr;

    auto group = pmu_metrics::internal::PerfGroup::Open();
    if (!group) return nullptr;

    auto tid = std::this_thread::get_id();

    std::string name;
    if (cfg->track_name && cfg->track_name[0] != '\0') {
        name = cfg->track_name;
    } else {
        std::ostringstream oss;
        oss << "pmu_metrics/" << tid;
        name = oss.str();
    }

    uint64_t u_ipc = make_uuid(tid, name, "ipc");
    uint64_t u_cpi = make_uuid(tid, name, "cpi");

    auto* s = new pmu_metrics_session(std::move(*group),
                                      std::move(name),
                                      u_ipc, u_cpi,
                                      cfg->writer_fn,
                                      cfg->descriptor_fn,
                                      cfg->userdata);

    // Emit track descriptors once via the host callback.
    if (s->descriptor_fn) {
        s->descriptor_fn(s->userdata, u_ipc, s->track_name.c_str(), "ipc");
        s->descriptor_fn(s->userdata, u_cpi, s->track_name.c_str(), "cpi");
    }

    return s;
}

// ---------------------------------------------------------------------------
// pmu_metrics_tick
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
int pmu_metrics_tick(pmu_metrics_session_t*  session,
                     pmu_metrics_snapshot_t* snap_out) {
    if (!session) return -1;

    pmu_metrics_snapshot_t snap{};

    // Timestamp.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    snap.timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                      + static_cast<uint64_t>(ts.tv_nsec);

    // Read counters.
    pmu_metrics::internal::PerfGroupReadFormat raw{};
    if (!session->group.Read(raw)) {
        session->last_snapshot = snap;
        if (snap_out) *snap_out = snap;
        return -1;
    }

    snap.cycles       = raw.values[0].value;
    snap.instructions = raw.values[1].value;
    snap.scaled       = (raw.time_running > 0) &&
                        (raw.time_running < raw.time_enabled) ? 1 : 0;

    if (snap.scaled && raw.time_running > 0) {
        double f = static_cast<double>(raw.time_enabled)
                 / static_cast<double>(raw.time_running);
        snap.cycles       = static_cast<uint64_t>(snap.cycles       * f);
        snap.instructions = static_cast<uint64_t>(snap.instructions * f);
    }

    if (snap.cycles > 0) {
        snap.ipc = static_cast<double>(snap.instructions)
                 / static_cast<double>(snap.cycles);
        snap.cpi = 1.0 / snap.ipc;
    }

    // Emit via host-provided callbacks — NO Perfetto calls here.
    session->writer_fn(session->userdata,
                       session->uuid_ipc, snap.timestamp_ns, "ipc", snap.ipc);
    session->writer_fn(session->userdata,
                       session->uuid_cpi, snap.timestamp_ns, "cpi", snap.cpi);

    session->group.Reset();
    session->last_snapshot = snap;

    if (snap_out) *snap_out = snap;
    return 0;
}

// ---------------------------------------------------------------------------
// pmu_metrics_last_snapshot
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
const pmu_metrics_snapshot_t* pmu_metrics_last_snapshot(
        const pmu_metrics_session_t* session) {
    if (!session) return nullptr;
    return &session->last_snapshot;
}

// ---------------------------------------------------------------------------
// pmu_metrics_destroy
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
void pmu_metrics_destroy(pmu_metrics_session_t* session) {
    delete session;
}

// ---------------------------------------------------------------------------
// pmu_metrics_is_perf_available
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
int pmu_metrics_is_perf_available(void) {
    FILE* f = std::fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!f) return 0;
    int paranoid = 3;
    std::fscanf(f, "%d", &paranoid);
    std::fclose(f);
    return paranoid <= 1 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// pmu_metrics_host_arch
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
const char* pmu_metrics_host_arch(void) {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

// ---------------------------------------------------------------------------
// pmu_metrics_version_string
// ---------------------------------------------------------------------------
extern "C" PMU_METRICS_API
const char* pmu_metrics_version_string(void) {
    return "0.1.0";
}
