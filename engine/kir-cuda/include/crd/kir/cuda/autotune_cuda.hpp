#pragma once

// autotune_cuda.hpp — ADR-0098 §4 · AS-2b: the CKIR auto-scheduler SEARCH, packaged. `autotune_contract` runs the full AS-1 loop
// for one GEMM shape — enumerate the valid schedule space, measure the hand-seed + heuristic top-K on the GPU, reject any that
// miscompute (a sampled GEMM reference), return the fastest CORRECT schedule — and is shared by the AS-1b test and the offline
// `kir_autotune` CLI that regenerates the checked-in tuning DB. Self-contained: it fills its own deterministic inputs.

#include <crd/kir/ckir_tile.hpp>          // TileSchedule
#include <crd/kir/cuda/backend_cuda.hpp>  // KirBackendCuda
#include <crd/memory/allocator.hpp>

namespace crd::kir
{

struct AutotuneResult
{
    bool         ok       = false;
    TileSchedule sched;              // the fastest CORRECT schedule found (== seed if exploration didn't beat it)
    double       ms       = 0.0;     // the winner's min GPU-timed launch
    double       seed_ms  = 0.0;     // the hand-seed's time (select_schedule) — 0 if the shape isn't heuristic-eligible
    double       naive_ms = 0.0;     // the naive (untiled) baseline — 0 unless measure_naive
    int          measured = 0;       // # candidates timed
    int          correct  = 0;       // # that passed the oracle
    int          m        = 0;
    int          n        = 0;
    int          k        = 0;
};

// Autotune GEMM M×K·K×N on CUDA: build the graph, enumerate valid schedules, measure the hand-seed + heuristic `top_k`, reject
// miscomputers (sampled reference), return the fastest correct. `measure_naive` also times the untiled baseline (for the crush
// board). Deterministic inputs ⇒ reproducible. `ok=false` if no CUDA device or nothing valid/correct.
[[nodiscard]] AutotuneResult autotune_contract(KirBackendCuda& cu, int m, int n, int k, int top_k, bool measure_naive,
                                               crd::memory::IAllocator* a);

} // namespace crd::kir
