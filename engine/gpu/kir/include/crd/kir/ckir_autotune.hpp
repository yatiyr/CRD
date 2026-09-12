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
    // the smem-load stride pattern must tile evenly (stride_a = NT·4/BK divides BM; stride_b = NT/(BN/4) divides BK)
    const int stride_a = static_cast<int>((r.threads * 4U) / static_cast<crd::u32>(s.bk));
    if (stride_a == 0 || ((r.threads * 4U) % static_cast<crd::u32>(s.bk)) != 0 || (s.bm % stride_a) != 0) { return false; }
    if ((r.threads % (static_cast<crd::u32>(s.bn) / 4U)) != 0) { return false; }
    const int stride_b = static_cast<int>(r.threads / (static_cast<crd::u32>(s.bn) / 4U));
    if (stride_b == 0 || (s.bk % stride_b) != 0) { return false; }
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
    double num_sms        = 66.0;          // SM count (RTX 4070 Ti SUPER) — the reduce cost model's block-saturation target
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

// ═══ AS-4 OP-GENERALITY: the REDUCE schedule space ══════════════════════════════════════════════════════════════════════════
// The auto-scheduler — proven on Contract (a rich compute-bound tile hierarchy) — GENERALIZES to a STRUCTURALLY DIFFERENT op: a
// device-wide REDUCTION (memory-bound, a fan-in tree, no reuse). `build_reduce` is a 2-pass tree reduce parameterized by
// (threads/block, per_thread serial unroll); nblocks = n / (threads·per_thread). The space is small vs the Contract's, but the
// winning (threads, per_thread) still varies by N (L2-resident vs DRAM-bound) and by device, so it is a REAL tuning problem the
// autotuner solves the SAME way: enumerate valid schedules → measure on-device → oracle-validate (a fixed-order reduction is
// bit-exact on the CPU oracle) → cache. This proves the framework is a general kernel auto-scheduler, not a GEMM special case.
struct ReduceSchedule
{
    int threads    = 0; // threads per workgroup (power of two) — the tree width
    int per_thread = 0; // serial elements each thread pre-reduces in pass 0 (the block-strided coalesced unroll)
};

// Blocks in pass 0 for an N-element reduction: nblocks = N / (threads·per_thread). 0 if the per-block span does not divide N.
[[nodiscard]] inline int reduce_nblocks(int n, const ReduceSchedule& s) noexcept
{
    const int span = s.threads * s.per_thread;
    if (span <= 0 || (n % span) != 0) { return 0; }
    return n / span;
}

// Valid iff: threads is a power of two in [32, max_threads]; per_thread ≥ 1; the per-block span divides N; and nblocks is a
// positive MULTIPLE of threads — so the SINGLE final pass reduces the nblocks partials with per_thread_final = nblocks/threads ≥ 1
// (build_reduce's constraint). A schedule that violates one either miscomputes or fails to launch, so the search never emits it.
[[nodiscard]] inline bool reduce_schedule_valid(int n, const ReduceSchedule& s, const DeviceLimits& lim) noexcept
{
    if (s.threads < 32 || s.threads > static_cast<int>(lim.max_threads) || s.per_thread < 1) { return false; }
    if ((s.threads & (s.threads - 1)) != 0) { return false; } // power of two
    const int nb = reduce_nblocks(n, s);
    if (nb <= 0) { return false; }
    return (nb % s.threads) == 0; // final pass: reduce nb partials with `threads` threads, per_thread_final ≥ 1
}

// Enumerate every valid reduce schedule for an N-element reduction into `out` (≤ `cap`), returning the count. Deterministic nested
// sweep (threads × per_thread), so a capped run drops the same tail every time. The Naive (single-block) reduce is the fallback,
// not a candidate.
[[nodiscard]] inline int enumerate_reduce_schedules(int n, const DeviceLimits& lim, ReduceSchedule* out, int cap)
{
    static constexpr int kThreads[]   = {64, 128, 256, 512, 1024};
    static constexpr int kPerThread[] = {1, 2, 4, 8, 16, 32, 64};
    int count = 0;
    for (int threads : kThreads)
    {
        for (int pt : kPerThread)
        {
            const ReduceSchedule s{threads, pt};
            if (!reduce_schedule_valid(n, s, lim)) { continue; }
            if (count < cap) { out[count] = s; }
            ++count;
        }
    }
    return count < cap ? count : cap;
}

// Predicted reduce time (ms) on `spec` — a roofline for a MEMORY-BOUND op. Traffic ≈ pass-0 reads N + writes nblocks, pass-1 reads
// nblocks (the +1 scalar is nil) ⇒ 4·(N + 2·nblocks) bytes. The lever is OCCUPANCY: DRAM saturates only once pass-0 launches
// enough blocks to fill every SM (≈ 2 resident blocks/SM); too few blocks (large per_thread) leaves bandwidth on the table, so the
// effective BW scales by min(1, nblocks / (2·num_sms)). Ranks the space so the search measures only the top few. Rank matters, not
// the absolute ms.
[[nodiscard]] inline double predict_reduce_ms(int n, const ReduceSchedule& s, const DeviceSpec& spec)
{
    const int nb = reduce_nblocks(n, s);
    if (nb <= 0) { return 1.0e30; }
    const double bytes   = 4.0 * (static_cast<double>(n) + 2.0 * static_cast<double>(nb));
    const double target  = 2.0 * spec.num_sms; // resident-block target to saturate DRAM
    double       util    = static_cast<double>(nb) / target;
    if (util > 1.0) { util = 1.0; }
    if (util < 0.05) { util = 0.05; } // a badly under-utilized launch is ranked last, not infinite
    return bytes / (spec.bw_gbps * 1.0e6 * util);
}

// Select the top-`kk` reduce schedules of `cand[0..n)` by predicted time (ascending) into `out_idx`. Same shape as the Contract's
// rank_top_k_cost — so a caller measures only the best few candidates. Deterministic on ties (lower index wins).
[[nodiscard]] inline int rank_reduce_top_k_cost(const ReduceSchedule* cand, int n, int nelem, const DeviceSpec& spec, int* out_idx, int kk)
{
    const int m = kk < n ? kk : n;
    bool      used[256] = {};
    const int lim       = n < 256 ? n : 256;
    for (int t = 0; t < m; ++t)
    {
        int    best_i = -1;
        double best_ms = 1.0e300;
        for (int i = 0; i < lim; ++i)
        {
            if (used[i]) { continue; }
            const double ms = predict_reduce_ms(nelem, cand[i], spec);
            if (ms < best_ms) { best_ms = ms; best_i = i; }
        }
        if (best_i < 0) { return t; }
        used[best_i] = true;
        out_idx[t]   = best_i;
    }
    return m;
}

// ═══ AS-4 FLASH-ATTENTION schedule space (BR × BC) ══════════════════════════════════════════════════════════════════════════
// The flash kernel tiles queries into BR-row blocks (grid = ceil(S/BR) × BR threads) and streams keys/values in BC-column tiles
// through shared. (BR,BC) trade OCCUPANCY (smaller BR ⇒ more blocks to fill the SMs) against per-block K/V reuse + resource
// pressure (a per-thread s[BC] register array; the tiles cost 2·BC·D shared floats). The best (BR,BC) varies by S and head dim, so
// `time_attention` MEASURES the space on-device + oracle-validates each (a FAST tier, ULP-tolerant). The space is small ⇒ measure
// all valid (no cost-model pruning needed).
struct AttentionSchedule
{
    int br = 0; // query-tile height = block threads (power of two)
    int bc = 0; // key/value-tile width streamed through shared per iteration
};

// Valid iff BR is a power of two in [32, max_threads], BC ≥ 8, and the K/V tiles fit static shared (2·BC·D·4 ≤ 48 KB). Register
// pressure (q[D]+acc[D]+s[BC]) is NOT a validity gate — nvcc spills to local, so a fat config still RUNS; the measurement (not the
// enumerator) rejects it by being slow.
[[nodiscard]] inline bool attention_schedule_valid(int dim, const AttentionSchedule& s, const DeviceLimits& lim) noexcept
{
    if (dim <= 0 || s.br < 32 || s.br > static_cast<int>(lim.max_threads) || s.bc < 8) { return false; }
    if ((s.br & (s.br - 1)) != 0) { return false; }                             // BR power of two
    return static_cast<crd::i64>(s.bc) * dim * 2 * 4 <= 48 * 1024;              // static shared cap
}

// Enumerate every valid flash-attention (BR,BC) schedule for a head dim into `out` (≤ `cap`), returning the count. Deterministic
// nested sweep. The fixed default (BR=64, BC=32) is a member for any D whose tiles fit shared.
[[nodiscard]] inline int enumerate_attention_schedules(int dim, const DeviceLimits& lim, AttentionSchedule* out, int cap)
{
    static constexpr int kBR[] = {32, 64, 128, 256};
    static constexpr int kBC[] = {8, 16, 32, 64, 128};
    int count = 0;
    for (int br : kBR)
    {
        for (int bc : kBC)
        {
            const AttentionSchedule s{br, bc};
            if (!attention_schedule_valid(dim, s, lim)) { continue; }
            if (count < cap) { out[count] = s; }
            ++count;
        }
    }
    return count < cap ? count : cap;
}

} // namespace crd::kir::autotune
