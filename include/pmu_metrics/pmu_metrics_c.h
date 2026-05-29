/* pmu_metrics/include/pmu_metrics/pmu_metrics_c.h
 *
 * Stable C ABI for libpmu_metrics.so.
 *
 * ── Rules ───────────────────────────────────────────────────────────────────
 *  • No C++ types, no Perfetto types, no linux/perf_event.h types.
 *  • All symbols are explicitly exported via PMU_METRICS_API.
 *    Every other symbol in the .so is hidden (-fvisibility=hidden).
 *  • This header is the ONLY file shipped to host consumers alongside the .so.
 *    The host-side Perfetto shim (pmu_metrics_host_shim.h) is separate and
 *    only needed by the host project's own build — not by end consumers.
 *
 * ── Perfetto integration model ──────────────────────────────────────────────
 *  The .so contains ZERO Perfetto symbols.  Instead, the host registers a
 *  writer callback (pmu_metrics_writer_fn) at session creation time.
 *  On every Tick() the library calls this function with:
 *    - the track UUID
 *    - a timestamp
 *    - a metric name string
 *    - the metric value
 *  The host-side shim (pmu_metrics_host_shim.h) provides a ready-made
 *  implementation of this callback that writes into the host's Perfetto
 *  trace buffer.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Export macro ────────────────────────────────────────────────────────── */
#if defined(_WIN32)
#  define PMU_METRICS_API __declspec(dllexport)
#else
#  define PMU_METRICS_API __attribute__((visibility("default")))
#endif

/* ── Version ─────────────────────────────────────────────────────────────── */
#define PMU_METRICS_VERSION_MAJOR 0
#define PMU_METRICS_VERSION_MINOR 1
#define PMU_METRICS_VERSION_PATCH 0

PMU_METRICS_API const char* pmu_metrics_version_string(void);

/* ── Aggregated metrics snapshot ─────────────────────────────────────────── */
typedef struct pmu_metrics_snapshot {
    uint64_t instructions;   /* raw instruction count for the interval     */
    uint64_t cycles;         /* raw cycle count for the interval           */
    double   ipc;            /* instructions per cycle                     */
    double   cpi;            /* cycles per instruction                     */
    uint64_t timestamp_ns;   /* CLOCK_MONOTONIC_RAW at sample time         */
    int      scaled;         /* 1 if kernel multiplexed and values scaled  */
} pmu_metrics_snapshot_t;

/* ── Writer callback ─────────────────────────────────────────────────────── */
/*
 * The host provides one function of this type at session creation time.
 * The library calls it synchronously from pmu_metrics_tick().
 *
 * Parameters:
 *   userdata      — opaque pointer passed through from pmu_metrics_config_t
 *   track_uuid    — stable 64-bit ID for this (session, metric_name) pair;
 *                   use as Perfetto CounterTrack uuid
 *   timestamp_ns  — CLOCK_MONOTONIC_RAW nanoseconds
 *   metric_name   — NUL-terminated ASCII string, e.g. "ipc" or "cpi"
 *   value         — the metric value to record
 */
typedef void (*pmu_metrics_writer_fn)(void*       userdata,
                                      uint64_t    track_uuid,
                                      uint64_t    timestamp_ns,
                                      const char* metric_name,
                                      double      value);

/*
 * Called once per session at creation time so the host can emit a
 * CounterTrack descriptor packet (track name, unit string, etc.) before the
 * first sample arrives.
 *
 * Parameters:
 *   userdata      — same opaque pointer
 *   track_uuid    — same UUID as will be used for subsequent writer_fn calls
 *   track_name    — human-readable name for the track, e.g. "pmu/my_bench"
 *   metric_name   — "ipc", "cpi", etc.
 */
typedef void (*pmu_metrics_descriptor_fn)(void*       userdata,
                                          uint64_t    track_uuid,
                                          const char* track_name,
                                          const char* metric_name);

/* ── Configuration ───────────────────────────────────────────────────────── */
typedef struct pmu_metrics_config {
    /* Human-readable track name embedded in Perfetto CounterTrack descriptor.
     * May be NULL — defaults to "pmu_metrics/<thread_id>".
     * The library copies this string; caller may free after Create(). */
    const char* track_name;

    /* Writer callback — called on every Tick() to emit one counter packet.
     * MUST NOT be NULL. */
    pmu_metrics_writer_fn writer_fn;

    /* Descriptor callback — called once at Create() per metric.
     * May be NULL if the host doesn't need to emit track descriptors. */
    pmu_metrics_descriptor_fn descriptor_fn;

    /* Opaque pointer forwarded to writer_fn and descriptor_fn unchanged. */
    void* userdata;

    /* Reserved for future flags (MPKI, branch miss rate, TMA …).
     * Set to 0 for the current version. */
    uint32_t flags;
} pmu_metrics_config_t;

/* ── Session handle ──────────────────────────────────────────────────────── */
/*
 * Opaque handle.  Obtain via pmu_metrics_create(), release via
 * pmu_metrics_destroy().  Not thread-safe across sessions; each thread
 * should own its own handle.
 */
typedef struct pmu_metrics_session pmu_metrics_session_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * Create a session for the calling thread.
 * Opens perf_event_open(2) file descriptors (pid=0, cpu=-1).
 * Calls cfg->descriptor_fn once per metric (if non-NULL).
 *
 * Returns NULL on failure (perf_event_paranoid too high, unsupported arch,
 * or kernel error — check errno).
 */
PMU_METRICS_API
pmu_metrics_session_t* pmu_metrics_create(const pmu_metrics_config_t* cfg);

/*
 * Read counters for the interval since the last Tick (or Create),
 * derive metrics, call cfg->writer_fn for each metric, reset counters.
 *
 * Fills *snap_out if non-NULL.
 * Returns 0 on success, -1 on read error (check errno).
 */
PMU_METRICS_API
int pmu_metrics_tick(pmu_metrics_session_t*  session,
                     pmu_metrics_snapshot_t* snap_out  /* may be NULL */);

/*
 * Returns a pointer to the most recent snapshot without reading counters.
 * The pointer is valid until the next pmu_metrics_tick() or pmu_metrics_destroy().
 */
PMU_METRICS_API
const pmu_metrics_snapshot_t* pmu_metrics_last_snapshot(
        const pmu_metrics_session_t* session);

/*
 * Destroy a session and close all perf fds.
 * Safe to call with NULL.
 */
PMU_METRICS_API
void pmu_metrics_destroy(pmu_metrics_session_t* session);

/* ── Utility ─────────────────────────────────────────────────────────────── */

/* Returns 1 if perf_event_paranoid <= 1 (unprivileged use allowed). */
PMU_METRICS_API int  pmu_metrics_is_perf_available(void);

/* Returns a string like "aarch64" or "x86_64". */
PMU_METRICS_API const char* pmu_metrics_host_arch(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
