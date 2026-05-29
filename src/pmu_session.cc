// pmu_metrics/src/pmu_session.cc
//
// Implements the C ABI (pmu_metrics_c.h).
//
// Internal state:
//   - One global PerfGroup per attached tid, stored in a mutex-protected map.
//   - The library is initialised once; the host never sees fd handles or
//     counter values.
//   - All metric computation (formulas) lives here — hidden from the host.
//
// ZERO Perfetto includes.  All trace emission goes through write_fn.

#include "pmu_metrics/pmu_metrics_c.h"

#include "perf_group.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace pmu_metrics::internal;

// ─────────────────────────────────────────────────────────────────────────────
// Internal types
// ─────────────────────────────────────────────────────────────────────────────

// Per-tid state, fully private.
struct TidEntry {
    PerfGroup group;
    // Pre-computed UUIDs for each metric track (stable, derived at attach time).
    uint64_t uuid_ipc;
    uint64_t uuid_cpi;
    // TODO (proprietary variant): add uuid fields for additional secret metrics.
};

// Global library state — hidden behind the C ABI.
struct LibState {
    pmu_metrics_write_fn    write_fn{nullptr};
    pmu_metrics_describe_fn describe_fn{nullptr};
    void*                   userdata{nullptr};
    std::string             prefix;

    std::mutex                            mu;
    std::unordered_map<uint32_t, TidEntry> tids;  // keyed by Linux tid

    bool initialised{false};
};

// Single instance; lifetime controlled by pmu_metrics_init / shutdown.
static LibState* g_state{nullptr};

// ─────────────────────────────────────────────────────────────────────────────
// UUID derivation — stable per (prefix, tid, metric_name).
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t make_uuid(const std::string& prefix,
                           uint32_t           tid,
                           const std::string& metric) {
    std::size_t h = std::hash<std::string>{}(prefix);
    h ^= std::hash<uint32_t>{}(tid)     + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= std::hash<std::string>{}(metric) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return static_cast<uint64_t>(h);
}

// ─────────────────────────────────────────────────────────────────────────────
// pmu_metrics_init
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
int pmu_metrics_init(const pmu_metrics_init_args_t* args) {
    if (!args || !args->write_fn) return PMU_METRICS_ERR_ARGS;
    if (g_state && g_state->initialised) return PMU_METRICS_ERR_STATE;

    auto* s         = new LibState;
    s->write_fn     = args->write_fn;
    s->describe_fn  = args->describe_fn;
    s->userdata     = args->userdata;
    s->prefix       = args->track_name_prefix ? args->track_name_prefix
                                              : "pmu_metrics";
    s->initialised  = true;
    g_state         = s;
    return PMU_METRICS_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// pmu_metrics_shutdown
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
void pmu_metrics_shutdown(void) {
    delete g_state;
    g_state = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// pmu_metrics_attach
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
int pmu_metrics_attach(uint32_t tid) {
    if (!g_state) return PMU_METRICS_ERR_STATE;

    std::lock_guard<std::mutex> lk(g_state->mu);

    if (g_state->tids.count(tid)) return PMU_METRICS_ERR_ALREADY;

    // Open the perf fd group for this specific tid.
    // pid=tid, cpu=-1 → measures that thread on any CPU.
    auto group = PerfGroup::OpenForTid(static_cast<pid_t>(tid));
    if (!group) return PMU_METRICS_ERR_PERF;

    TidEntry entry;
    entry.group    = std::move(*group);
    entry.uuid_ipc = make_uuid(g_state->prefix, tid, "ipc");
    entry.uuid_cpi = make_uuid(g_state->prefix, tid, "cpi");
    // TODO (proprietary): compute UUIDs for additional secret metrics here.

    // Notify the host about the new tracks so it can emit descriptors.
    if (g_state->describe_fn) {
        g_state->describe_fn(g_state->userdata,
                             entry.uuid_ipc, "ipc", "instr/cycle");
        g_state->describe_fn(g_state->userdata,
                             entry.uuid_cpi, "cpi", "cycle/instr");
        // TODO (proprietary): describe additional metric tracks.
    }

    g_state->tids.emplace(tid, std::move(entry));
    return PMU_METRICS_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// pmu_metrics_detach
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
int pmu_metrics_detach(uint32_t tid) {
    if (!g_state) return PMU_METRICS_ERR_STATE;

    std::lock_guard<std::mutex> lk(g_state->mu);
    auto it = g_state->tids.find(tid);
    if (it == g_state->tids.end()) return PMU_METRICS_ERR_NOTFOUND;

    g_state->tids.erase(it);   // PerfGroup destructor closes fds.
    return PMU_METRICS_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// pmu_metrics_sample
//
// All metric derivation lives here.  The host never sees counter values.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
int pmu_metrics_sample(uint32_t tid) {
    if (!g_state) return PMU_METRICS_ERR_STATE;

    // Take a snapshot-copy of the entry so the lock is held minimally.
    TidEntry* entry_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state->mu);
        auto it = g_state->tids.find(tid);
        if (it == g_state->tids.end()) return PMU_METRICS_ERR_NOTFOUND;
        entry_ptr = &it->second;
    }
    // NOTE: entry_ptr is only valid while the tid remains attached.
    // For a production implementation use a shared_ptr or a per-entry mutex.
    // This stub is sufficient for single-consumer sampling patterns.

    // Timestamp.
    struct timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    uint64_t timestamp_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
                          + static_cast<uint64_t>(ts.tv_nsec);

    // Read counters from the kernel fd group.
    PerfGroupReadFormat raw{};
    if (!entry_ptr->group.Read(raw)) return PMU_METRICS_ERR_PERF;

    uint64_t cycles       = raw.values[0].value;
    uint64_t instructions = raw.values[1].value;

    // Multiplexing correction.
    if (raw.time_running > 0 && raw.time_running < raw.time_enabled) {
        double f   = static_cast<double>(raw.time_enabled)
                   / static_cast<double>(raw.time_running);
        cycles       = static_cast<uint64_t>(cycles       * f);
        instructions = static_cast<uint64_t>(instructions * f);
    }

    // ── Metric derivation (HIDDEN from host) ──────────────────────────────
    // BASE variant: IPC, CPI only.
    // PROPRIETARY variant: extend here with secret event codes and formulas.
    double ipc = 0.0, cpi = 0.0;
    if (cycles > 0) {
        ipc = static_cast<double>(instructions) / static_cast<double>(cycles);
        cpi = 1.0 / ipc;
    }
    // TODO (proprietary): compute additional metrics from secret counters.
    // ─────────────────────────────────────────────────────────────────────

    // Emit via host callback — host sees only (track_uuid, ts, name, value).
    g_state->write_fn(g_state->userdata,
                      entry_ptr->uuid_ipc, timestamp_ns, "ipc", ipc);
    g_state->write_fn(g_state->userdata,
                      entry_ptr->uuid_cpi, timestamp_ns, "cpi", cpi);
    // TODO (proprietary): emit additional metric tracks.

    // Reset counters for the next sampling interval.
    entry_ptr->group.Reset();

    return PMU_METRICS_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────
extern "C" PMU_METRICS_API
int pmu_metrics_is_perf_available(void) {
    FILE* f = std::fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (!f) return 0;
    int v = 3;
    std::fscanf(f, "%d", &v);
    std::fclose(f);
    return v <= 1 ? 1 : 0;
}

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

extern "C" PMU_METRICS_API
const char* pmu_metrics_version(void) {
    return "0.2.0";
}
