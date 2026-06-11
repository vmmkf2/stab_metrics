# pmu_metrics — minimal stub

**Current iteration goal:** integrate into the host project and determine
the right interface.  No real hardware counters yet.

---

## What this is

A single-header C++17 library with one class (`PmuMetricsManager`) and one
result struct (`Sample`).  Zero external dependencies.

Counter values returned by `Sample()` are **monotonic integers** that
increment by a fixed delta each call — enough to verify plumbing end-to-end
without needing real PMU hardware.

---

## API

```cpp
#include "pmu_metrics/pmu_metrics.h"

pmu_metrics::PmuMetricsManager mgr;

mgr.Init({
    .counters = pmu_metrics::Counter::IPC | pmu_metrics::Counter::CPI,
    .tid      = gettid(),   // 0 = calling thread
    .cpu      = -1,         // -1 = follow thread across CPUs
});

// ... do work ...

auto s = mgr.Sample();
// s.timestamp_ns  — CLOCK_MONOTONIC_RAW at sample time
// s.instructions  — optional<uint64_t>, raw stub counter
// s.cycles        — optional<uint64_t>, raw stub counter
// s.ipc           — optional<double>
// s.cpi           — optional<double>
```

`Sample()` resets internal accumulators, so consecutive calls measure
the interval since the previous call (or `Init()`).

---

## GN integration

```gn
# In target_server BUILD.gn:
deps += [ "//third_party/pmu_metrics:pmu_metrics" ]
```

---

## Roadmap

| Iteration | What changes |
|---|---|
| **This one** | Stub — monotonic integers, zero deps, nail the interface |
| Next | Replace stub with real `perf_event_open` calls |
| After that | Proprietary event codes + derived metrics in `.so` |
| Final | Perfetto write path wired via host callback |
