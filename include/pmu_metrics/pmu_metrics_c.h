/* pmu_metrics/include/pmu_metrics/pmu_metrics_c.h
 *
 * Stable C ABI for libpmu_metrics.so
 *
 * ── Purpose ──────────────────────────────────────────────────────────────────
 *  The library extends the host project's existing PMU counter collection by
 *  gathering *proprietary* counters and emitting *derived metrics* (whose
 *  formulas are hidden from the host's source tree) into the host's existing
 *  Perfetto trace buffer.
 *
 *  The host project already owns:
 *    • perf_event_open fd groups for standard counters
 *    • a running Perfetto tracing session
 *    • logic to write CounterTrack packets
 *
 *  This library adds:
 *    • secret PMU event codes (proprietary variant) or extended HW events
 *    • formulas that derive higher-level metrics from those counters
 *    • per-thread fd management (keyed by Linux tid)
 *
 * ── Host interaction model ────────────────────────────────────────────────────
 *
 *   1. pmu_metrics_init()       — called once at program startup
 *                                 host provides a write_fn that bridges into
 *                                 its existing Perfetto write path
 *
 *   2. pmu_metrics_attach(tid)  — called when a thread of interest starts
 *                                 library opens its fd group for that tid
 *
 *   3. pmu_metrics_sample(tid)  — called periodically by the host
 *                                 library reads counters, computes metrics,
 *                                 calls write_fn for each derived metric
 *                                 host never sees counter values or formulas
 *
 *   4. pmu_metrics_detach(tid)  — called when a thread finishes
 *
 *   5. pmu_metrics_shutdown()   — called once at program exit
 *
 * ── What the host sees ───────────────────────────────────────────────────────
 *  Only the write_fn call:  (track_uuid, timestamp_ns, metric_name, value)
 *  No raw counter values. No event codes. No formulas.
 *
 * ── Symbol visibility ────────────────────────────────────────────────────────
 *  Compiled with -fvisibility=hidden.  Only PMU_METRICS_API symbols exported.
 *  Zero Perfetto symbols in the .so.
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
#define PMU_METRICS_VERSION_MINOR 2
#define PMU_METRICS_VERSION_PATCH 0

PMU_METRICS_API const char* pmu_metrics_version(void);

/* ── Write callback ──────────────────────────────────────────────────────── */
/*
 * Provided by the host at pmu_metrics_init() time.
 * Called synchronously from pmu_metrics_sample() — once per derived metric.
 *
 * The host implements this by writing a CounterTrack packet into its existing
 * Perfetto trace buffer.  See pmu_metrics_host_shim.h for a ready-made
 * implementation.
 *
 * Parameters:
 *   userdata     — opaque value from pmu_metrics_init_args_t, passed through
 *   track_uuid   — stable 64-bit track identifier, unique per (tid, metric)
 *                  use directly as Perfetto CounterTrack uuid
 *   timestamp_ns — CLOCK_MONOTONIC_RAW nanoseconds at sample time
 *   metric_name  — NUL-terminated ASCII label, e.g. "ipc", "cpi"
 *                  stable across calls; the host may use it for track naming
 *   value        — the computed metric value
 */
typedef void (*pmu_metrics_write_fn)(void*       userdata,
                                     uint64_t    track_uuid,
                                     uint64_t    timestamp_ns,
                                     const char* metric_name,
                                     double      value);

/*
 * Optional: called once per (tid, metric) at pmu_metrics_attach() time so
 * the host can emit a CounterTrack descriptor packet (name, unit) before the
 * first sample arrives.  May be NULL.
 *
 * Parameters:
 *   userdata     — same opaque value
 *   track_uuid   — same UUID as subsequent write_fn calls for this metric
 *   metric_name  — same label as subsequent write_fn calls
 *   unit         — human-readable unit string, e.g. "instr/cycle"
 */
typedef void (*pmu_metrics_describe_fn)(void*       userdata,
                                        uint64_t    track_uuid,
                                        const char* metric_name,
                                        const char* unit);

/* ── Init args ───────────────────────────────────────────────────────────── */
typedef struct pmu_metrics_init_args {
    /* Required: bridge into the host's Perfetto write path. */
    pmu_metrics_write_fn    write_fn;

    /* Optional: emit CounterTrack descriptors at attach time. */
    pmu_metrics_describe_fn describe_fn;

    /* Opaque value forwarded unchanged to write_fn and describe_fn. */
    void* userdata;

    /* Name prefix for CounterTrack descriptors: "<prefix>/<tid>/<metric>".
     * If NULL, defaults to "pmu_metrics". */
    const char* track_name_prefix;

    /* Reserved for future flags.  Set to 0. */
    uint32_t flags;
} pmu_metrics_init_args_t;

/* ── Error codes ─────────────────────────────────────────────────────────── */
#define PMU_METRICS_OK            0
#define PMU_METRICS_ERR_ARGS     -1   /* NULL or invalid args               */
#define PMU_METRICS_ERR_PERF     -2   /* perf_event_open failed (see errno) */
#define PMU_METRICS_ERR_ALREADY  -3   /* tid already attached               */
#define PMU_METRICS_ERR_NOTFOUND -4   /* tid not attached                   */
#define PMU_METRICS_ERR_STATE    -5   /* init not called / already shutdown */

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/*
 * Initialise the library.  Call once before any other function.
 *
 * The library does NOT open any perf fds here — fd groups are opened lazily
 * per tid in pmu_metrics_attach().
 *
 * Returns PMU_METRICS_OK or PMU_METRICS_ERR_ARGS.
 */
PMU_METRICS_API
int pmu_metrics_init(const pmu_metrics_init_args_t* args);

/*
 * Release all resources.  Safe to call multiple times.
 * Implicitly detaches all attached tids.
 */
PMU_METRICS_API
void pmu_metrics_shutdown(void);

/* ── Per-thread management ───────────────────────────────────────────────── */

/*
 * Open the proprietary perf fd group for the given Linux thread id.
 * Emits CounterTrack descriptor packets via describe_fn (if non-NULL).
 *
 * tid — Linux thread id (gettid() / syscall(SYS_gettid)), NOT pthread_t.
 *
 * Returns PMU_METRICS_OK, PMU_METRICS_ERR_PERF, or PMU_METRICS_ERR_ALREADY.
 */
PMU_METRICS_API
int pmu_metrics_attach(uint32_t tid);

/*
 * Close the perf fd group for the given tid.
 * It is safe to call this after the thread has exited.
 *
 * Returns PMU_METRICS_OK or PMU_METRICS_ERR_NOTFOUND.
 */
PMU_METRICS_API
int pmu_metrics_detach(uint32_t tid);

/*
 * Read counters for the given tid, compute all derived metrics, and call
 * write_fn once per metric.
 *
 * Must be called from ANY thread — the fd was opened with pid=tid so the
 * kernel measures that specific thread regardless of which thread reads it.
 *
 * After reading, resets the counter group so the next sample measures
 * only the interval since this call.
 *
 * Returns PMU_METRICS_OK, PMU_METRICS_ERR_NOTFOUND, or PMU_METRICS_ERR_PERF.
 */
PMU_METRICS_API
int pmu_metrics_sample(uint32_t tid);

/* ── Utility ─────────────────────────────────────────────────────────────── */

/* 1 if /proc/sys/kernel/perf_event_paranoid <= 1. */
PMU_METRICS_API int         pmu_metrics_is_perf_available(void);

/* "aarch64", "x86_64", etc. */
PMU_METRICS_API const char* pmu_metrics_host_arch(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
