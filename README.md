# pmu_metrics

A C++17 static library (`libpmu_metrics.a`) that collects hardware PMU
counters via `perf_event_open(2)` and writes derived metrics directly into
the **host project's existing Perfetto trace buffer** as a Perfetto
`DataSource`.

## Status

**Stub / skeleton.**  The public API, CMake wiring, and Perfetto DataSource
plumbing are in place.  The actual `perf_event_open` syscall paths in
`src/perf_group.cc` are stubbed out with detailed `TODO` comments.

---

## Design decisions

### A — Static library only

`libpmu_metrics.a` is always built as `STATIC`.  A shared library would
introduce duplicate Perfetto SDK symbols when linked into a host binary that
already embeds the SDK.

### B — In-process Perfetto DataSource

The library registers `dev.pmu_metrics` as a Perfetto `DataSource` during
**static initialisation** (before `main()`).  When the host calls
`perfetto::Tracing::Initialize(...)`, the SDK finds the data source
automatically.  The library:

* **NEVER** calls `perfetto::Tracing::Initialize()`.
* **NEVER** reads back the trace buffer.
* Writes packets via `ctx.NewTracePacket()` inside `DataSource::Trace(...)`,
  sharing the host's in-process backend and producing one coherent
  `.perfetto-trace` file.

### C — Counter scope: thread-local

`perf_event_open` is called with `pid = 0, cpu = -1`, which measures the
calling thread only.  One `PmuSession` per thread; sessions are independent.

### D — Collection model: explicit `Tick()`

No background thread is created.  The host drives sampling by calling
`PmuSession::Tick()` at its own cadence (e.g. around a benchmark region).
`Tick()` synchronously:

1. Reads the counter group from the kernel.
2. Derives metrics (IPC, CPI).
3. Emits two Perfetto `TYPE_COUNTER` packets (one per metric).
4. Resets the counter group for the next interval.

### E — Architecture: aarch64, IPC + CPI only (initial)

Uses generic `PERF_TYPE_HARDWARE` events (`PERF_COUNT_HW_CPU_CYCLES`,
`PERF_COUNT_HW_INSTRUCTIONS`) which are available on all aarch64 Linux
kernels without special privileges (subject to `perf_event_paranoid ≤ 1`).

Planned expansions:
- MPKI (requires `PERF_COUNT_HW_CACHE_MISSES` + `PERF_COUNT_HW_INSTRUCTIONS`)
- Branch Miss Rate (`PERF_COUNT_HW_BRANCH_MISSES` + `PERF_COUNT_HW_BRANCH_INSTRUCTIONS`)
- TMA Level-1 approximation via ARM SPE or Arm CoreSight

---

## Repository layout

```
pmu_metrics/
├── CMakeLists.txt
├── README.md
├── include/
│   └── pmu_metrics/
│       └── pmu_metrics.h        ← public API only; no raw perf types exposed
└── src/
    ├── perf_group.h             ← PRIVATE: RAII perf fd group
    ├── perf_group.cc            ← STUB: perf_event_open syscall wrappers
    ├── pmu_data_source.h        ← PRIVATE: Perfetto DataSource declaration
    ├── pmu_data_source.cc       ← Perfetto packet emission
    ├── pmu_session.cc           ← PmuSession, IsPerfAvailable, HostArchitecture
    └── static_registration.cc  ← Forces DataSource registration before main()
```

---

## Host integration

### 1. Add as a subdirectory (or use `FetchContent`)

```cmake
# Host CMakeLists.txt

# 1. Build (or import) the Perfetto SDK amalgam and export a "perfetto" target.
add_library(perfetto STATIC third_party/perfetto/sdk/perfetto.cc)
target_include_directories(perfetto PUBLIC third_party/perfetto/sdk)

# 2. Add pmu_metrics — it will find the "perfetto" target automatically.
add_subdirectory(third_party/pmu_metrics)

# 3. Link your binary.
target_link_libraries(my_binary PRIVATE pmu_metrics::pmu_metrics)

# 4. If using LTO, prevent dead-code elimination of the static registrar:
target_link_options(my_binary PRIVATE
    -Wl,--whole-archive $<TARGET_FILE:pmu_metrics::pmu_metrics>
    -Wl,--no-whole-archive)
```

### 2. Host tracing lifecycle (unchanged)

```cpp
// Host code — pmu_metrics never touches these calls.
perfetto::TracingInitArgs args;
args.backends = perfetto::kInProcessBackend;
perfetto::Tracing::Initialize(args);

auto session = perfetto::Tracing::NewTrace();
session->Setup(cfg);
session->StartBlocking();
```

### 3. Use pmu_metrics

```cpp
#include "pmu_metrics/pmu_metrics.h"

if (!pmu_metrics::IsPerfAvailable()) {
    // perf_event_paranoid too restrictive — run as root or adjust sysctl
}

pmu_metrics::PmuConfig cfg;
cfg.track_name = "my_benchmark";

auto pmu = pmu_metrics::PmuSession::Create(cfg);
// pmu is std::nullopt if perf_event_open failed

// ... do work ...

if (pmu) {
    auto snap = pmu->Tick();
    printf("IPC=%.3f  CPI=%.3f\n", snap.ipc, snap.cpi);
}
```

---

## Kernel requirements

| Requirement | Value |
|---|---|
| Linux kernel | ≥ 4.3 (aarch64 PMU support) |
| `perf_event_paranoid` | ≤ 1 (or run as root) |
| `CAP_SYS_ADMIN` | Not required when `exclude_kernel=1` |

Check at runtime: `pmu_metrics::IsPerfAvailable()`.

---

## Building (standalone, for development)

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=<aarch64-cross-toolchain.cmake>
cmake --build build
```

> The build will fail with `FATAL_ERROR` unless a `perfetto` CMake target is
> defined first.  Use the host project's CMake tree or mock the target for
> unit testing.
