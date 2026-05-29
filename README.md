# pmu_metrics

A C++17 shared library (`libpmu_metrics.so`) that collects hardware PMU
counters via `perf_event_open(2)` and reports derived metrics to the host
project via a **writer callback**.  The host feeds those callbacks into its
existing Perfetto trace buffer.

The library is **shipped as a prebuilt** in two variants: **base** and
**proprietary**.  Both expose the same C ABI.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│  host binary (target_server)                         │
│                                                      │
│  ┌────────────────────────┐  ┌─────────────────────┐ │
│  │  Perfetto SDK          │  │  libpmu_metrics.so  │ │
│  │  (host-owned)          │  │                     │ │
│  │                        │  │  perf_event_open    │ │
│  │  PmuMetricsDataSource  │◄─│  writer_fn callback │ │
│  │  (compiled into host   │  │                     │ │
│  │   via _host_shim.h)    │  │  ZERO Perfetto syms │ │
│  └────────────────────────┘  └─────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

- `libpmu_metrics.so` contains **zero Perfetto symbols**.
  Compiled with `-fvisibility=hidden`; only `PMU_METRICS_API` symbols exported.
- The **host** compiles `pmu_metrics_host_shim.h` (header-only) into its own
  binary.  This provides the Perfetto `DataSource` and the `PmuMetricsShim`
  C++ wrapper.
- The `.so` calls back into the host via a plain C function pointer
  (`pmu_metrics_writer_fn`) — no C++ ABI, no Perfetto linkage.

---

## Repository layout

```
pmu_metrics/
├── BUILD.gn                                 ← shared_library + shim source_set
├── pmu_metrics.gni                          ← declare_args: perfetto target
├── README.md
├── include/pmu_metrics/
│   ├── pmu_metrics_c.h                      ← C ABI shipped with the .so
│   └── pmu_metrics_host_shim.h              ← host-side Perfetto DataSource
└── src/
    ├── perf_group.h / .cc                   ← RAII perf fd group (STUB)
    └── pmu_session.cc                       ← C ABI implementation
```

---

## Design decisions

### Shared library + C ABI

The library ships in two variants (base / proprietary) with a stable C ABI
so the host project can link against either without recompiling.

### Zero Perfetto symbols in the .so

If the `.so` embedded its own Perfetto SDK copy, the host binary would have
**two SDK instances** in the same process — duplicate singleton state, broken
DataSource registry, no packets in the trace.

Solution: compile the `.so` with `-fvisibility=hidden`.  The Perfetto
`DataSource` (`PmuMetricsDataSource`) lives in `pmu_metrics_host_shim.h`,
compiled directly into the host binary alongside the host's own Perfetto SDK.

### Writer callback bridge

The `.so` accepts a `pmu_metrics_writer_fn` function pointer at session
creation time.  On every `pmu_metrics_tick()` it calls this function with
`(track_uuid, timestamp_ns, metric_name, value)`.  The shim header wires this
to `PmuMetricsDataSource::EmitSample()`.

### Counter scope: thread-local (`pid=0, cpu=-1`)

One `pmu_metrics_session_t` per thread.  Sessions are independent.

### Collection model: explicit `pmu_metrics_tick()`

No background thread.  The host drives sampling at its own cadence.

### Architecture: aarch64, IPC + CPI (initial)

Generic `PERF_TYPE_HARDWARE` events.  No root required when
`perf_event_paranoid ≤ 1`.

---

## Host GN integration

### 1. Obtain the Perfetto SDK amalgam

```bash
curl -L https://github.com/google/perfetto/releases/latest/download/\
perfetto-cpp-sdk-src.zip -o /tmp/sdk.zip
unzip /tmp/sdk.zip -d third_party/perfetto/sdk/
```

### 2. Place pmu_metrics in your source tree

```
your_project/third_party/pmu_metrics/   ← this repo
your_project/third_party/perfetto/      ← Perfetto SDK
```

### 3. Wire target_server

```gn
# In target_server's BUILD.gn:
executable("target_server") {
  sources = [ ... ]

  deps = [
    # Pulls in: PmuMetricsDataSource, PmuMetricsShim, C ABI header,
    #           libpmu_metrics.so (linked at build time), Perfetto SDK.
    "//third_party/pmu_metrics:pmu_metrics_shim",
  ]
}
```

### 4. Register the DataSource (once per binary)

In exactly **one** `.cc` file in the host:

```cpp
#define PMU_METRICS_DEFINE_DATA_SOURCE
#include "pmu_metrics/pmu_metrics_host_shim.h"
```

In all other files that use the shim:

```cpp
#include "pmu_metrics/pmu_metrics_host_shim.h"
```

### 5. Use

```cpp
#include "pmu_metrics/pmu_metrics_host_shim.h"

// Check availability first.
if (!pmu_metrics_is_perf_available()) { /* adjust perf_event_paranoid */ }

// C++ wrapper (recommended):
PmuMetricsShim pmu("my_benchmark");
if (!pmu) { /* perf_event_open failed */ }

// ... do work ...
auto snap = pmu.Tick();
printf("IPC=%.3f  CPI=%.3f\n", snap.ipc, snap.cpi);
// CounterTrack packets appear in the host's .perfetto-trace automatically.

// Or raw C ABI:
pmu_metrics_config_t cfg{};
cfg.track_name    = "my_benchmark";
cfg.writer_fn     = my_writer;
cfg.descriptor_fn = my_descriptor;
auto* s = pmu_metrics_create(&cfg);
pmu_metrics_tick(s, nullptr);
pmu_metrics_destroy(s);
```

### 6. Override Perfetto target (if needed)

```gn
# args.gn
pmu_metrics_perfetto_target = "//your/existing:perfetto"
```

---

## What the two GN targets produce

| Target | Output | Deps on Perfetto? |
|---|---|---|
| `//third_party/pmu_metrics:pmu_metrics` | `libpmu_metrics.so` | No |
| `//third_party/pmu_metrics:pmu_metrics_shim` | compiled into host binary | Yes (host's SDK) |

---

## Kernel requirements

| Requirement | Value |
|---|---|
| Linux kernel | ≥ 4.3 (aarch64 PMU) |
| `perf_event_paranoid` | ≤ 1 or `CAP_PERFMON` |
| `CAP_SYS_ADMIN` | Not required (`exclude_kernel=1`) |
