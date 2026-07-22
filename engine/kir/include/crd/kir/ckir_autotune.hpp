#pragma once

// ckir_autotune.hpp — Phase 3.1.6 v17 · ADR-0098 §4: the CKIR **auto-scheduler** search space (AS-1).
//
// The hand-written `select_schedule` table (ckir_tile.hpp) is one measured winner per op — produced by a MANUAL bench sweep
// (v17-e). The autotuner AUTOMATES that sweep: it enumerates the VALID `TileSchedule` space for a shape, a backend runtime
// times each candidate on the real GPU, the CPU oracle rejects any that are wrong, and the winner is checked in — deterministic
// replay from the DB, never tuned at runtime (ADR-0098). This header is the backend-FREE half: the schedule-space generator +
// its validity/resource model. No GPU dep (it lives in `crd-kir`), so it is unit-testable without a device.
//
// The validity constraints are the EXACT relationships the warp-tiled Contract emitter (`emit_contract_tiled_cuda`, the
// CUTLASS block→warp→thread hierarchy) requires — generate a schedule that violates one and the kernel miscomputes or fails to
// compile. Deriving them here (once) is what lets the search stay a pure combinatorial sweep the oracle then certifies.

#include <crd/kir/ckir_tile.hpp> // TileSchedule / Sched

namespace crd::kir::autotune
{

// The device resource ceilings the search prunes against (defaults = a safe Ada/Ampere SM: 48 KB static smem, 1024 threads/block,
// 255 regs/thread). A real run fills these from the device query so the space matches the target GPU.
struct DeviceLimits
{
    crd::u32 smem_bytes  = 48U * 1024U; // static shared memory per block (opt-in dynamic smem can raise this to 96–228 KB)
    crd::u32 max_threads = 1024U;       // threads per block
    crd::u32 max_regs    = 255U;        // registers per thread
    crd::u32 max_accum   = 128U;        // accumulator registers we let a thread hold (WMITER·TM·WNITER·TN) — the occupancy knob
};

// Derived resource footprint of a WarpTiled Contract schedule (also the raw inputs the AS-3 cost model will score).
struct ScheduleResources
{
    crd::u32 threads   = 0U; // NT = 32 · (BM/WM) · (BN/WN)
    crd::u32 smem      = 0U; // bytes: (BK·(BM+4) + BK·BN)·4·(DB?2:1)
    crd::u32 accum     = 0U; // WMITER·TM·WNITER·TN accumulator registers
    crd::u32 warps     = 0U; // NT/32
    crd::u32 wmiter    = 0U; // (WM·WN)/(32·TM·TN·WNITER)
};

// Compute a WarpTiled schedule's derived footprint. `ok=false` (via the return) iff a division is non-integral or a field is 0.
[[nodiscard]] inline bool schedule_resources(const TileSchedule& s, ScheduleResources& r) noexcept
{
    if (s.kind != Sched::WarpTiled) { return false; }
    if (s.bm <= 0 || s.bn <= 0 || s.bk <= 0 || s.wm <= 0 || s.wn <= 0 || s.wniter <= 0 || s.tm <= 0 || s.tn <= 0) { return false; }
    if ((s.bm % s.wm) != 0 || (s.bn % s.wn) != 0) { return false; }              // warps tile the block
    const int warps = (s.bm / s.wm) * (s.bn / s.wn);
    const int nt    = 32 * warps;
    const int denom = 32 * s.tm * s.tn * s.wniter;
    if (denom == 0 || (s.wm * s.wn) % denom != 0) { return false; }               // WMITER integral
    const int wmiter = (s.wm * s.wn) / denom;
    if (wmiter <= 0 || (s.wm % wmiter) != 0 || (s.wn % s.wniter) != 0) { return false; }
    const int wsubm = s.wm / wmiter;
    const int wsubn = s.wn / s.wniter;
    if ((wsubm % s.tm) != 0 || (wsubn % s.tn) != 0) { return false; }
    if ((wsubm / s.tm) * (wsubn / s.tn) != 32) { return false; }                  // the 32 lanes tile the warp sub-tile
    if (nt != s.nt) { return false; }                                            // NT must equal the derived thread count
    r.threads = static_cast<crd::u32>(nt);
    r.warps   = static_cast<crd::u32>(warps);
    r.wmiter  = static_cast<crd::u32>(wmiter);
    r.accum   = static_cast<crd::u32>(wmiter * s.tm * s.wniter * s.tn);
    r.smem    = static_cast<crd::u32>((s.bk * (s.bm + 4) + s.bk * s.bn) * 4 * (s.double_buffer ? 2 : 1));
    return true;
}

// Is `s` a VALID + shape-compatible + resource-fitting WarpTiled schedule for an M×K · K×N GEMM on `lim`? Enforces every
// relationship the emitter assumes (float4 vectorized loads, the warp/thread hierarchy, the smem staging) plus the shape
// divisibility (`select_schedule`'s guard) and the device ceilings. This is the search-space membership test.
[[nodiscard]] inline bool contract_schedule_valid(const TileSchedule& s, int m, int n, int k, const DeviceLimits& lim) noexcept
{
    ScheduleResources r;
    if (!schedule_resources(s, r)) { return false; }
    // shape divisibility (else the tiled kernel reads/writes out of bounds — select_schedule's constraint)
    if (m < s.bm || n < s.bn || (m % s.bm) != 0 || (n % s.bn) != 0 || (k % s.bk) != 0) { return false; }
    // float4 vectorized global loads: A along K (BK%4), B along N (BN%4)
    if ((s.bk % 4) != 0 || (s.bn % 4) != 0) { return false; }
    // the smem-load stride pattern must tile evenly (STRIDEA = NT·4/BK divides BM; STRIDEB = NT/(BN/4) divides BK)
    const int strideA = (r.threads * 4) / static_cast<crd::u32>(s.bk);
    if (strideA == 0 || ((r.threads * 4) % static_cast<crd::u32>(s.bk)) != 0 || (s.bm % strideA) != 0) { return false; }
    if ((r.threads % (static_cast<crd::u32>(s.bn) / 4U)) != 0) { return false; }
    const int strideB = static_cast<int>(r.threads / (static_cast<crd::u32>(s.bn) / 4U));
    if (strideB == 0 || (s.bk % strideB) != 0) { return false; }
    // device ceilings
    if (r.threads < 32U || r.threads > lim.max_threads) { return false; }
    if (r.smem > lim.smem_bytes) { return false; }
    if (r.accum > lim.max_accum) { return false; }
    return true;
}

// Enumerate every valid WarpTiled schedule for an M×K·K×N GEMM into `out` (≤ `cap`), returning the count. Sweeps the standard
// CUTLASS axes (block/warp/thread tiles + double-buffer); each survivor is guaranteed emittable + oracle-checkable. Order is
// deterministic (nested loops) so a capped sweep drops the SAME tail every run (log it if it matters). The Naive schedule is
// NOT included — it is always the correctness fallback; this space is the PERF candidates.
[[nodiscard]] inline int enumerate_contract_schedules(int m, int n, int k, const DeviceLimits& lim, TileSchedule* out, int cap)
{
    static constexpr int kBM[]     = {64, 128, 256};
    static constexpr int kBN[]     = {64, 128, 256};
    static constexpr int kBK[]     = {8, 16, 32};
    static constexpr int kWM[]     = {32, 64, 128};
    static constexpr int kWN[]     = {32, 64, 128};
    static constexpr int kWNITER[] = {1, 2, 4};
    static constexpr int kT[]      = {4, 8};
    int count = 0;
    for (int bm : kBM) {
    for (int bn : kBN) {
    for (int bk : kBK) {
    for (int wm : kWM) {
    for (int wn : kWN) {
    for (int wniter : kWNITER) {
    for (int tm : kT) {
    for (int tn : kT) {
    for (int db = 0; db < 2; ++db) {
        if ((bm % wm) != 0 || (bn % wn) != 0) { continue; }
        TileSchedule s;
        s.kind          = Sched::WarpTiled;
        s.bm            = bm; s.bn = bn; s.bk = bk;
        s.wm            = wm; s.wn = wn; s.wniter = wniter;
        s.tm            = tm; s.tn = tn;
        s.nt            = 32 * (bm / wm) * (bn / wn); // NT is DERIVED, never searched
        s.double_buffer = db != 0;
        s.fma           = true; // the perf tier; the exact (no-FMA) tier is validated ULP-tolerant against the oracle
        if (!contract_schedule_valid(s, m, n, k, lim)) { continue; }
        if (count < cap) { out[count] = s; }
        ++count;
    }}}}}}}}}
    return count < cap ? count : cap;
}

// A CHEAP static throughput proxy for RANKING WarpTiled schedules before measurement — so the search measures only the top-K of
// a large space instead of all 1500+. Deliberately rough (the AS-3 analytical cost model replaces it), but built on the RIGHT
// GEMM levers so the winner-class floats up:
//   • ARITHMETIC INTENSITY = BM·BN/(BM+BN) — the global-memory reuse (loads per tile ≈ (BM+BN)·K, MACs ≈ BM·BN·K). Bigger tiles
//     are less memory-bound; this is the dominant term. (The earlier compute/smem proxy over-rewarded oversized low-occupancy
//     tiles → mis-ranked the winner 7×; AS-1b caught it.)
//   • OCCUPANCY = resident warps/SM, saturating once there are enough to hide memory latency.
//   • REGISTER PRESSURE penalty on the accumulator tile (WMITER·TM·WNITER·TN) — a fat register tile caps occupancy.
// SM constants target Ada (4070 Ti: ~100 KB usable smem/SM, 1536 threads/SM, 24 blocks/SM) — the AS-3 model reads them per device.
[[nodiscard]] inline double heuristic_score(const TileSchedule& s, const ScheduleResources& r) noexcept
{
    const double smem    = r.smem == 0U ? 1.0 : static_cast<double>(r.smem);
    const double nt      = r.threads == 0U ? 1.0 : static_cast<double>(r.threads);
    const double ai      = static_cast<double>(s.bm) * static_cast<double>(s.bn) / (static_cast<double>(s.bm) + static_cast<double>(s.bn));
    // resident blocks/SM = min over the three occupancy limiters (smem, threads, the hardware block cap)
    double blocks_smem = (100.0 * 1024.0) / smem;
    double blocks_thr  = 1536.0 / nt;
    double blocks      = blocks_smem < blocks_thr ? blocks_smem : blocks_thr;
    if (blocks > 24.0) { blocks = 24.0; }
    const double warps = blocks * (nt / 32.0);
    double       occ   = warps / 16.0;         // saturate at ~16 resident warps/SM (enough to hide DRAM latency)
    if (occ > 1.0) { occ = 1.0; }
    const double reg_penalty = r.accum <= 64U ? 1.0 : 64.0 / static_cast<double>(r.accum); // fat accumulator tile → low occupancy
    const double db_bonus    = s.double_buffer ? 1.15 : 1.0;                                // double-buffering hides global-load latency
    const int    tt          = s.tm * s.tn;
    const double ilp         = 0.6 + 0.4 * (static_cast<double>(tt > 64 ? 64 : tt) / 64.0); // per-thread register-tile work (ILP)
    return ai * occ * reg_penalty * db_bonus * ilp;
}

// ── AS-3: the ANALYTICAL cost model — rank the schedule space by a PREDICTED runtime (roofline × occupancy), so the search
// measures only the top-K of 1500+ (no crude heuristic, no exhaustive sweep). The model captures the real GEMM tradeoff: bigger
// block tiles cut global-memory traffic but shrink occupancy; the roofline max(compute, memory) picks the binding resource, and
// the occupancy efficiency scales the compute side (few resident warps ⇒ FMA latency stalls). Absolute numbers are unimportant —
// only the RANK — but the constants target the tuning device (RTX 4070 Ti SUPER: ~44 TFLOP f32, 672 GB/s, 66 SMs, 100 KB smem/SM).
struct DeviceSpec
{
    double peak_gflops    = 11000.0;       // EFFECTIVE f32 GEMM ceiling (CUDA-core FMA, non-tensor-core) — the roofline knee,
                                           // well below the 44 TFLOP theoretical peak (register/smem-bandwidth bound in practice)
    double bw_gbps        = 672.0;         // DRAM bandwidth
    double smem_per_sm    = 100.0 * 1024;  // usable shared memory per SM
    double threads_per_sm = 1536.0;        // Ada resident-thread cap
    double regs_per_sm    = 65536.0;       // register file per SM — the limiter fat register tiles hit FIRST
    double max_blocks_sm  = 24.0;          // resident-block cap
    double hide_warps     = 12.0;          // resident warps/SM needed to hide FMA+load latency (the occupancy target)
};

// Predicted kernel time (ms) for a WarpTiled Contract schedule on `spec`. Roofline: max(compute-bound, memory-bound), the compute
// side scaled by occupancy efficiency. Occupancy is INTEGER resident-blocks/SM = floor(min over smem / threads / REGISTERS /
// block-cap) — registers are the limiter big tiles hit first (a 256² tile wants ~190 regs/thread × 512 = 98K > the 64K file, so
// zero blocks fit — the AS-3 failure that motivated this term). A fractional block can't run ⇒ floor.
[[nodiscard]] inline double predict_contract_ms(const TileSchedule& s, const ScheduleResources& r, crd::i64 m, crd::i64 n,
                                                crd::i64 k, const DeviceSpec& spec)
{
    const double flops = 2.0 * static_cast<double>(m) * static_cast<double>(n) * static_cast<double>(k);
    // global traffic WITH tile reuse: A re-read once per BN-column band, B once per BM-row band, C stored once.
    const double gbytes = 4.0 * (static_cast<double>(m) * static_cast<double>(n) * static_cast<double>(k)
                                     * (1.0 / static_cast<double>(s.bm) + 1.0 / static_cast<double>(s.bn))
                                 + static_cast<double>(m) * static_cast<double>(n));
    const double smem = r.smem == 0U ? 1.0 : static_cast<double>(r.smem);
    const double nt   = r.threads == 0U ? 1.0 : static_cast<double>(r.threads);
    // registers/thread ≈ accumulators + regM(WMITER·TM) + regN(WNITER·TN) + the float4 stage buffers + overhead
    const double regs = static_cast<double>(r.accum) + static_cast<double>(r.wmiter) * s.tm + static_cast<double>(s.wniter) * s.tn + 28.0;
    double       blocks = spec.smem_per_sm / smem;
    const double b_thr  = spec.threads_per_sm / nt;
    const double b_reg  = spec.regs_per_sm / (regs * nt);
    if (b_thr < blocks) { blocks = b_thr; }
    if (b_reg < blocks) { blocks = b_reg; }
    if (blocks > spec.max_blocks_sm) { blocks = spec.max_blocks_sm; }
    const double resident = blocks >= 1.0 ? static_cast<double>(static_cast<int>(blocks)) : 0.0; // floor; <1 ⇒ can't fit a block
    const double warps    = resident * (nt / 32.0);
    double       occ_eff  = warps / spec.hide_warps; // <1 ⇒ too few resident warps to hide latency ⇒ compute stalls
    if (occ_eff > 1.0) { occ_eff = 1.0; }
    if (occ_eff < 0.02) { occ_eff = 0.02; }          // an infeasible/near-empty config is heavily penalized (ranked last)
    const double db         = s.double_buffer ? 1.10 : 1.0;
    const double compute_ms = flops / (spec.peak_gflops * 1.0e6) / (occ_eff * db);
    const double mem_ms     = gbytes / (spec.bw_gbps * 1.0e6);
    return compute_ms > mem_ms ? compute_ms : mem_ms;
}

// Select the top-`k` schedules of `cand[0..n)` by PREDICTED time (ascending) into `out_idx`. The AS-3 replacement for the AS-1
// heuristic rank — same interface, but the search now measures ~K=6 instead of ~20 to find the optimum. Deterministic on ties.
[[nodiscard]] inline int rank_top_k_cost(const TileSchedule* cand, int n, crd::i64 m, crd::i64 nn, crd::i64 k,
                                         const DeviceSpec& spec, int* out_idx, int kk)
{
    const int mcount     = kk < n ? kk : n;
    bool      used[4096] = {};
    const int lim        = n < 4096 ? n : 4096;
    for (int t = 0; t < mcount; ++t)
    {
        int    best_i = -1;
        double best_t = 1.0e300;
        for (int i = 0; i < lim; ++i)
        {
            if (used[i]) { continue; }
            ScheduleResources r;
            if (!schedule_resources(cand[i], r)) { continue; }
            const double pt = predict_contract_ms(cand[i], r, m, nn, k, spec);
            if (pt < best_t) { best_t = pt; best_i = i; }
        }
        if (best_i < 0) { return t; }
        used[best_i] = true;
        out_idx[t]   = best_i;
    }
    return mcount;
}

// Select the top-`k` schedules of `cand[0..n)` by `heuristic_score` into `out_idx` (best first), returning how many were written
// (min(k, n)). A partial selection sort — k is small (measurement is the cost, not the ranking). Deterministic on ties (lower
// index wins), so a replayed search picks the same candidates.
[[nodiscard]] inline int rank_top_k(const TileSchedule* cand, int n, int* out_idx, int k)
{
    const int m = k < n ? k : n;
    bool      used[4096] = {};
    const int lim        = n < 4096 ? n : 4096;
    for (int t = 0; t < m; ++t)
    {
        int    best_i = -1;
        double best_s = -1.0;
        for (int i = 0; i < lim; ++i)
        {
            if (used[i]) { continue; }
            ScheduleResources r;
            if (!schedule_resources(cand[i], r)) { continue; }
            const double sc = heuristic_score(cand[i], r);
            if (sc > best_s) { best_s = sc; best_i = i; }
        }
        if (best_i < 0) { return t; }
        used[best_i]   = true;
        out_idx[t]     = best_i;
    }
    return m;
}

} // namespace crd::kir::autotune
