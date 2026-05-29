# pmu_metrics

A C++17 shared library (`libpmu_metrics.so`) that **extends** the host
project's existing PMU counter collection by gathering proprietary counters
and emitting derived metrics into the host's existing Perfetto trace buffer.

The library hides from the host's source tree:
- which raw PMU event codes are opened (especially the proprietary variant)
- the formulas that derive higher-level metrics from those counters

The host project already owns the Perfetto session and its own counter
collection.  This library adds tracks alongside them.

---

## Host interaction model

```
host                                  libpmu_metrics.so
────────────────────────────────────  ──────────────────────────────────────
pmu_metrics_init(&args)           →   stores write_fn, describe_fn, userdata
                                      does NOT open any perf fds yet

[worker thread A starts]
pmu_metrics_attach(tid_A)         →   opens secret perf fd group for tid_A
                                      calls describe_fn once per metric track

[worker thread B starts]
pmu_metrics_attach(tid_B)         →   opens secret perf fd group for tid_B

[host sampling loop, any thread]
pmu_metrics_sample(tid_A)         →   reads fds, computes formulas (hidden)
                                      calls write_fn(track_uuid, ts, "ipc", v)
                                      calls write_fn(track_uuid, ts, "cpi", v)
                                      ...
pmu_metrics_sample(tid_B)         →   same for tid_B

[worker thread A finishes]
pmu_metrics_detach(tid_A)         →   closes fd group for tid_A

pmu_metrics_shutdown()            →   closes all remaining fds
```

**The host sees only:** `(track_uuid, timestamp_ns, metric_name, value)` via
`write_fn`.  No raw counter values.  No event codes.  No formulas.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  host binary (target_server)                         │
│                                                      │
│  ┌──────────────────────┐  ┌──────────────────────┐  │
│  │  Perfetto SDK        │  │  libpmu_metrics.so   │  │
│  │  (host-owned)        │  │                      │  │
│  │                      │  │  perf_event_open()   │  │
│  │  host's DataSource   │  │  per-tid fd map      │  │
│  │  ────────────────    │  │  secret formulas     │  │
│  │  write_fn impl  ◄────┼──┼─ write_fn(uuid,ts,v) │  │
│  └──────────────────────┘  └──────────────────────┘  │
└──────────────────────────────────────────────────────┘
```

- `libpmu_metrics.so` — **zero Perfetto symbols**, `-fvisibility=hidden`
- `pmu_metrics_host_shim.h` — header compiled into the host; provides
  `PmuMetricsManager` RAII wrapper and template helpers for `write_fn`

---

## Repository layout

```
pmu_metrics/
├── BUILD.gn
├── pmu_metrics.gni
├── include/pmu_metrics/
│   ├── pmu_metrics_c.h             ← C ABI (shipped with .so)
│   └── pmu_metrics_host_shim.h     ← host-side bridge helpers (host-compiled)
└── src/
    ├── perf_group.h / .cc          ← RAII perf fd group (STUB)
    └── pmu_session.cc              ← C ABI impl, per-tid fd map, formulas
```

---

## GN integration

```gn
# target_server BUILD.gn
executable("target_server") {
  deps = [
    "//third_party/pmu_metrics:pmu_metrics_shim",  # headers + links .so
    # ... existing deps
  ]
}
```

---

## Host wiring (C++ example)

```cpp
// In the file where your existing DataSource lives:
#include "pmu_metrics/pmu_metrics_host_shim.h"

// Implement write_fn in terms of your existing Perfetto DataSource.
// Template sketch (adapt MyDataSource to your actual type):
static void PmuWriteFn(void*       /*userdata*/,
                       uint64_t    track_uuid,
                       uint64_t    timestamp_ns,
                       const char* /*metric_name*/,
                       double      value) {
    MyDataSource::Trace([&](MyDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(timestamp_ns);
        auto* te = packet->set_track_event();
        te->set_track_uuid(track_uuid);
        te->set_type(perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
        te->set_counter_value_double(value);
    });
}

static void PmuDescribeFn(void*       /*userdata*/,
                          uint64_t    track_uuid,
                          const char* metric_name,
                          const char* unit) {
    MyDataSource::Trace([&](MyDataSource::TraceContext ctx) {
        auto packet = ctx.NewTracePacket();
        packet->set_timestamp(0);
        auto* td = packet->set_track_descriptor();
        td->set_uuid(track_uuid);
        td->set_name(metric_name);
        td->set_counter()->set_unit_name(unit);
    });
}

// ── At program startup (after Perfetto::Initialize) ──────────────────────
pmu_metrics::host::PmuMetricsManager pmu_manager(
    PmuWriteFn,
    PmuDescribeFn,
    nullptr,          // userdata — not needed for in-process DataSource
    "pmu_ext"         // track name prefix in the trace viewer
);

// ── When a worker thread starts ──────────────────────────────────────────
pmu_manager.Attach(gettid());   // gettid() or syscall(SYS_gettid)

// ── In the host's sampling loop ──────────────────────────────────────────
pmu_manager.Sample(worker_tid); // can be called from any thread

// ── When a worker thread finishes ────────────────────────────────────────
pmu_manager.Detach(worker_tid);

// ── At program exit ──────────────────────────────────────────────────────
// PmuMetricsManager destructor calls pmu_metrics_shutdown() automatically.
```

---

## Error codes

| Code | Value | Meaning |
|---|---|---|
| `PMU_METRICS_OK` | 0 | Success |
| `PMU_METRICS_ERR_ARGS` | -1 | NULL or invalid arguments |
| `PMU_METRICS_ERR_PERF` | -2 | `perf_event_open` failed (check `errno`) |
| `PMU_METRICS_ERR_ALREADY` | -3 | `tid` already attached |
| `PMU_METRICS_ERR_NOTFOUND` | -4 | `tid` not attached |
| `PMU_METRICS_ERR_STATE` | -5 | `pmu_metrics_init` not called |

## Kernel requirements

| Requirement | Value |
|---|---|
| Linux kernel | ≥ 4.3 (aarch64 PMU) |
| `perf_event_paranoid` | ≤ 1 or `CAP_PERFMON` |
| `CAP_SYS_ADMIN` | Not required (`exclude_kernel=1`) |

Check: `pmu_metrics_is_perf_available()`.
