# pmu_metrics

A C++17 static library (`libpmu_metrics.a`) that collects hardware PMU
counters via `perf_event_open(2)` and writes derived metrics directly into
the **host project's existing Perfetto trace buffer** as an in-process
Perfetto `DataSource`.

## Status

**Stub / skeleton.**  The public API, GN build wiring, and Perfetto DataSource
plumbing are complete.  The actual `perf_event_open` syscall paths in
`src/perf_group.cc` are stubbed out with detailed `TODO` comments.

---

## Repository layout

```
pmu_metrics/
├── BUILD.gn                         ← static_library target for the host
├── pmu_metrics.gni                  ← declare_args: pmu_metrics_perfetto_target
├── README.md
├── include/
│   └── pmu_metrics/
│       └── pmu_metrics.h            ← public API; no raw perf types exposed
├── src/
│   ├── perf_group.h / .cc           ← PRIVATE: RAII perf fd group (STUB)
│   ├── pmu_data_source.h / .cc      ← PRIVATE: Perfetto DataSource
│   ├── pmu_session.cc               ← PmuSession, IsPerfAvailable, HostArchitecture
│   └── static_registration.cc      ← Forces DataSource::Register() before main()
└── third_party/
    └── perfetto/
        ├── BUILD.gn                 ← source_set wrapping the SDK amalgam
        └── sdk/                     ← NOT committed; obtain separately (see below)
            ├── perfetto.h
            └── perfetto.cc
```

---

## Design decisions

### A — Static library only

`libpmu_metrics.a` is always `static_library`.  A `shared_library` or
`loadable_module` would cause duplicate Perfetto SDK symbols when linked into
the host binary that already embeds the SDK.

### B — In-process Perfetto DataSource

The library registers `dev.pmu_metrics` as a Perfetto `DataSource` during
**static initialisation** (before `main()`).  When the host calls
`perfetto::Tracing::Initialize(...)`, the SDK finds the data source
automatically.  The library:

- **NEVER** calls `perfetto::Tracing::Initialize()`.
- **NEVER** reads back the trace buffer.
- Writes packets via `ctx.NewTracePacket()` inside `DataSource::Trace(...)`,
  sharing the host's in-process backend and producing one coherent
  `.perfetto-trace` file.

### C — Counter scope: thread-local

`perf_event_open` is called with `pid = 0, cpu = -1`, which measures the
calling thread only.  One `PmuSession` per thread; sessions are independent.

### D — Collection model: explicit `Tick()`

No background thread is created.  The host drives sampling by calling
`PmuSession::Tick()` at its own cadence (e.g. around a benchmark region).
`Tick()` synchronously:

1. Reads the perf counter group from the kernel.
2. Derives metrics (IPC, CPI).
3. Emits two Perfetto `TYPE_COUNTER` packets (one per metric).
4. Resets the counter group for the next interval.

### E — Architecture: aarch64, IPC + CPI only (initial)

Uses generic `PERF_TYPE_HARDWARE` events (`PERF_COUNT_HW_CPU_CYCLES`,
`PERF_COUNT_HW_INSTRUCTIONS`) available on all aarch64 Linux kernels without
special privileges (requires `perf_event_paranoid ≤ 1`).

Planned expansions:
- MPKI (`PERF_COUNT_HW_CACHE_MISSES` / `INSTRUCTIONS`)
- Branch Miss Rate (`PERF_COUNT_HW_BRANCH_MISSES` / `BRANCH_INSTRUCTIONS`)
- TMA Level-1 approximation via ARM SPE or CoreSight

---

## Host GN integration

### Step 1 — Obtain the Perfetto SDK amalgam

```bash
# Download from the latest Perfetto release:
curl -L https://github.com/google/perfetto/releases/latest/download/perfetto-cpp-sdk-src.zip \
     -o /tmp/sdk.zip
unzip /tmp/sdk.zip -d third_party/perfetto/sdk/
# Produces: third_party/perfetto/sdk/perfetto.h
#           third_party/perfetto/sdk/perfetto.cc
```

Or generate from an existing Perfetto checkout:

```bash
cd /path/to/perfetto
tools/gen_amalgamated --output sdk/perfetto
cp sdk/perfetto.{h,cc} <your_project>/third_party/perfetto/sdk/
```

### Step 2 — Place pmu_metrics in your source tree

```
your_project/
└── third_party/
    ├── perfetto/          ← from Step 1 (sdk/ dir with amalgam)
    │   └── BUILD.gn       ← from this repo
    └── pmu_metrics/       ← this repo
        ├── BUILD.gn
        ├── pmu_metrics.gni
        └── ...
```

### Step 3 — Depend on pmu_metrics from your target

In your host `BUILD.gn`:

```gn
executable("my_benchmark") {
  sources = [ "main.cc" ]

  deps = [
    "//third_party/pmu_metrics:pmu_metrics",
    # ... other deps
  ]
}
```

`pmu_metrics` pulls in `//third_party/perfetto:perfetto_sdk` as a
`public_dep` automatically via `pmu_metrics_perfetto_target`.

### Step 4 — Override the Perfetto target (if needed)

If your host project already defines a Perfetto GN target elsewhere, set
the override in your `args.gn` or a `.gni` file **before** the
`pmu_metrics` BUILD.gn is evaluated:

```gn
# args.gn or your root .gni
pmu_metrics_perfetto_target = "//third_party/perfetto:perfetto"
```

### Step 5 — LTO / dead-code elimination guard

`static_registration.cc` uses a file-scope volatile object to prevent
DCE stripping the DataSource registration.  If you use LTO and the
registration is still stripped, add to your linker flags:

```gn
executable("my_benchmark") {
  ldflags = [
    "-Wl,--whole-archive",
    # Use rebase_path() to get the absolute .a path at gen time.
    "-Wl,--no-whole-archive",
  ]
}
```

---

## Host tracing lifecycle (unchanged)

```cpp
// Host code — pmu_metrics never touches these.
perfetto::TracingInitArgs args;
args.backends = perfetto::kInProcessBackend;
perfetto::Tracing::Initialize(args);   // DataSource auto-registered here

auto session = perfetto::Tracing::NewTrace();
session->Setup(cfg);
session->StartBlocking();
```

## Using pmu_metrics

```cpp
#include "pmu_metrics/pmu_metrics.h"

if (!pmu_metrics::IsPerfAvailable()) {
    // /proc/sys/kernel/perf_event_paranoid > 1 — run as root or sysctl
}

pmu_metrics::PmuConfig cfg;
cfg.track_name = "my_benchmark";   // appears as CounterTrack name in UI

auto pmu = pmu_metrics::PmuSession::Create(cfg);
// Returns std::nullopt if perf_event_open fails

// ... do work ...

if (pmu) {
    auto snap = pmu->Tick();
    printf("IPC=%.3f  CPI=%.3f\n", snap.ipc, snap.cpi);
    // Packet also emitted into the Perfetto trace automatically.
}
```

---

## Kernel requirements

| Requirement | Value |
|---|---|
| Linux kernel | ≥ 4.3 (aarch64 PMU support) |
| `perf_event_paranoid` | ≤ 1 (or run as root / `CAP_PERFMON`) |
| `CAP_SYS_ADMIN` | Not required when `exclude_kernel = 1` |

Check at runtime: `pmu_metrics::IsPerfAvailable()`.
