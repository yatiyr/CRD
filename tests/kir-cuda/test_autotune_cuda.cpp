// tests/kir-cuda/test_autotune_cuda.cpp — ADR-0098 §4 · AS-1b: the CKIR auto-scheduler MEASURED SEARCH LOOP (CUDA).
//
// The backend-free enumerator (AS-1a) produces the valid `TileSchedule` space; here the CUDA runtime TIMES the heuristic top-K on
// the real GPU, the sampled GEMM reference REJECTS any schedule that miscomputes, and the fastest correct one is the autotuned
// winner. The gate: the autotuner's winner is oracle-correct AND at least as fast as the hand-tuned `select_schedule` entry —
// i.e. the search reproduces (or beats) the manual v17-e sweep, automatically. This is the loop the whole AS band builds on.

#include <catch2/catch_test_macros.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_autotune.hpp>
#include <crd/kir/ckir_cuda.hpp> // AS-2-for-attention: select_attention_tile (the DB-replay tile selector)
#include <crd/kir/ckir_tile.hpp>
#include <crd/kir/cuda/autotune_cuda.hpp> // AS-4: autotune_contract (the packaged search) for the rectangular generalization test
#include <crd/kir/cuda/backend_cuda.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>

// AS-4: a wall-clock QUALITY margin is meaningless under ASan — instrumentation perturbs CPU-side kernel-submission
// timing, so two near-tied tiles swap past the calibrated 1.30 swing (the assert is stable in every non-instrumented
// config: verified 20/20 in win-debug). Timing asserts run in a clean env only; the EXACT wiring checks still run
// everywhere. Feature-detect must be preprocessor (mirrors engine/memory/src/virtual_memory_allocator.cpp).
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#if defined(__SANITIZE_ADDRESS__)
#define CRD_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CRD_TEST_ASAN 1
#endif
#endif
#ifndef CRD_TEST_ASAN
#define CRD_TEST_ASAN 0
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace kir = crd::kir;
namespace at  = crd::kir::autotune;

namespace
{
// deterministic, NON-constant fills (constant A/B make every output identical — a schedule bug would hide).
float av_at(int i, int k) { return static_cast<float>((i * 7 + k) % 13) * 0.01F - 0.06F; }
float bv_at(int k, int j) { return static_cast<float>((k * 5 + j) % 11) * 0.008F - 0.04F; }

// CHEAP correctness check: recompute a SAMPLE of C[i,j] on the CPU (each a length-K dot product) and compare to the GPU readback.
// Catches a miscomputing schedule without a full M·N·K host GEMM. FMA fast tier ⇒ relative tolerance, not bit-exact.
bool sampled_correct(const float* c, int mm, int nn, int kk)
{
    float maxrel = 0.0F;
    for (int s = 0; s < 512; ++s)
    {
        const int   i   = (s * 977) % mm;
        const int   j   = (s * 1471) % nn;
        double      acc = 0.0;
        for (int k = 0; k < kk; ++k) { acc += static_cast<double>(av_at(i, k)) * static_cast<double>(bv_at(k, j)); }
        const float ref = static_cast<float>(acc);
        const float got = c[static_cast<crd::usize>(i) * nn + j];
        const float rel = (got - ref) / (1.0F + (ref < 0.0F ? -ref : ref));
        const float ar  = rel < 0.0F ? -rel : rel;
        if (ar > maxrel) { maxrel = ar; }
    }
    return maxrel < 3e-3F;
}
} // namespace

TEST_CASE("AS-1b: the CUDA autotuner reproduces/beats the hand-tuned GEMM winner, oracle-correct", "[kir][cuda][gpu][autotune]")
{
    crd::memory::TlsfAllocator alloc(512U << 20U);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }

    constexpr int mm = 1024;
    constexpr int nn = 1024;
    constexpr int kk = 1024;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b);

    crd::containers::Array<float> av(&alloc);
    crd::containers::Array<float> bv(&alloc);
    crd::containers::Array<float> out(&alloc);
    av.resize(static_cast<crd::usize>(mm) * kk);
    bv.resize(static_cast<crd::usize>(kk) * nn);
    out.resize(static_cast<crd::usize>(mm) * nn);
    for (int i = 0; i < mm; ++i) { for (int k = 0; k < kk; ++k) { av[static_cast<crd::usize>(i) * kk + k] = av_at(i, k); } }
    for (int k = 0; k < kk; ++k) { for (int j = 0; j < nn; ++j) { bv[static_cast<crd::usize>(k) * nn + j] = bv_at(k, j); } }
    const float* inputs[] = {av.data(), bv.data()};

    // 1. enumerate the valid space, heuristic-rank the top-K to explore (the whole space is 1500+ — AS-3 prunes analytically).
    at::DeviceLimits       lim;
    crd::kir::TileSchedule  cand[4096];
    const int               nc = at::enumerate_contract_schedules(mm, nn, kk, lim, cand, 4096);
    REQUIRE(nc > 0);
    constexpr int top_k = 20;
    int           idx[top_k];
    const int     ntop = at::rank_top_k(cand, nc, idx, top_k);
    REQUIRE(ntop > 0);

    // 2. SEED the search with the hand-seeded schedule (ADR-0098 §4: "hot ops get hand-seeded schedules; the autotuner refines"),
    //    then measure it + the heuristic top-K. Every candidate is oracle-gated: a schedule that miscomputes can NEVER win.
    const kir::TileSchedule seed = kir::select_schedule(g, c);
    REQUIRE(seed.kind == kir::Sched::WarpTiled);
    const auto measure = [&](const kir::TileSchedule& s) -> kir::ContractTiming {
        const kir::ContractTiming r = cu.time_contract_schedule(g, c, s, inputs, 2, out.data(), 3, 12);
        if (r.ok && !sampled_correct(out.data(), mm, nn, kk)) { return kir::ContractTiming{}; } // correctness gate
        return r;
    };
    const kir::ContractTiming sr = measure(seed);
    REQUIRE(sr.ok); // the hand-seed must be correct

    double            best_ms = sr.min_ms;
    kir::TileSchedule best    = seed;
    int               measured = 1;
    int               correct  = 1;
    for (int t = 0; t < ntop; ++t)
    {
        const kir::ContractTiming r = measure(cand[idx[t]]);
        if (!r.ok) { continue; }
        ++measured; ++correct;
        if (r.min_ms < best_ms) { best_ms = r.min_ms; best = cand[idx[t]]; }
    }
    CHECK(correct == measured); // every emittable candidate computed correctly

    // 3. the NAIVE (untiled) baseline — what tiling+search buys over the one-thread-per-output reference.
    const kir::ContractTiming nr = cu.time_contract_schedule(g, c, kir::TileSchedule{}, inputs, 2, out.data(), 3, 6);
    REQUIRE(nr.ok);
    REQUIRE(sampled_correct(out.data(), mm, nn, kk));

    // 4. determinism: the WINNING schedule replays bit-identical run-to-run — measure the SAME schedule twice and compare the
    // readback element-wise. (The fast/ULP tier is not bit-exact ACROSS different tile configs — only same-config replay is.)
    crd::containers::Array<float> out1(&alloc);
    crd::containers::Array<float> out2(&alloc);
    out1.resize(static_cast<crd::usize>(mm) * nn);
    out2.resize(static_cast<crd::usize>(mm) * nn);
    const kir::ContractTiming w1 = cu.time_contract_schedule(g, c, best, inputs, 2, out1.data(), 1, 4);
    const kir::ContractTiming w2 = cu.time_contract_schedule(g, c, best, inputs, 2, out2.data(), 1, 4);
    REQUIRE(w1.ok);
    REQUIRE(w2.ok);
    bool bit_identical = true;
    for (crd::usize i = 0; i < out1.size(); ++i) { if (out1[i] != out2[i]) { bit_identical = false; break; } }

    const double gflops     = 2.0 * static_cast<double>(mm) * nn * kk / (best_ms * 1.0e6);
    const double vs_naive   = nr.min_ms / best_ms;
    const double vs_seed    = sr.min_ms / best_ms;
    std::printf("[AS-1b] GEMM %dx%dx%d: autotuned %.3f ms (%.0f GFLOP/s) from %d correct candidates; naive %.3f ms -> %.1fx; "
                "seed %.3f ms -> autotuner %.2fx; deterministic=%d\n",
                mm, nn, kk, best_ms, gflops, correct, nr.min_ms, vs_naive, sr.min_ms, vs_seed, bit_identical ? 1 : 0);
    std::printf("[AS-1b] winner: BM%d BN%d BK%d WM%d WN%d WNITER%d TM%d TN%d NT%d DB%d\n", best.bm, best.bn, best.bk, best.wm,
                best.wn, best.wniter, best.tm, best.tn, best.nt, best.double_buffer ? 1 : 0);

    // GATES (the loop's guarantees — not the cost model's quality, which is AS-3):
    CHECK(measured >= 2);                  // the loop explored beyond the seed
    CHECK(best_ms <= sr.min_ms * 1.001);   // never regresses below the hand-seed
    CHECK(vs_naive >= 5.0);                // tiling+search delivers a large crush over the reference
    CHECK(bit_identical);                  // the tuned schedule is deterministic (replays bit-exact)
}

// ADR-0098 §4 · AS-3: the ANALYTICAL COST MODEL prunes the search. Rank the full valid space by predicted runtime (roofline ×
// occupancy) and measure only the top-K_cost (6) — vs the AS-1 heuristic's ~20 or the exhaustive 1516. Gate: the cost-model
// top-6 measured winner is the optimum (within noise of the known DB/hand-tuned winner), so the model finds the best schedule
// measuring FAR fewer NVRTC compiles. This is what makes full-space search tractable (and generalizes to unmeasured shapes).
TEST_CASE("AS-3: the analytical cost model finds the GEMM optimum measuring only top-6", "[kir][cuda][gpu][autotune]")
{
    crd::memory::TlsfAllocator alloc(512U << 20U);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }

    constexpr int mm = 1024;
    constexpr int nn = 1024;
    constexpr int kk = 1024;
    kir::KGraph   g(&alloc);
    const int     a = g.input(kir::make_shape({mm, kk}), kir::DType::F32);
    const int     b = g.input(kir::make_shape({kk, nn}), kir::DType::F32);
    const int     c = g.contract(a, b);

    crd::containers::Array<float> av(&alloc);
    crd::containers::Array<float> bv(&alloc);
    crd::containers::Array<float> out(&alloc);
    av.resize(static_cast<crd::usize>(mm) * kk);
    bv.resize(static_cast<crd::usize>(kk) * nn);
    out.resize(static_cast<crd::usize>(mm) * nn);
    for (int i = 0; i < mm; ++i) { for (int k = 0; k < kk; ++k) { av[static_cast<crd::usize>(i) * kk + k] = av_at(i, k); } }
    for (int k = 0; k < kk; ++k) { for (int j = 0; j < nn; ++j) { bv[static_cast<crd::usize>(k) * nn + j] = bv_at(k, j); } }
    const float* inputs[] = {av.data(), bv.data()};

    // rank the FULL valid space by the analytical cost model, take only the top-6.
    at::DeviceLimits       lim;
    at::DeviceSpec         spec;
    crd::kir::TileSchedule  cand[4096];
    const int               nc = at::enumerate_contract_schedules(mm, nn, kk, lim, cand, 4096);
    REQUIRE(nc > 0);
    constexpr int cost_k = 6;
    int           idx[cost_k];
    const int     ntop = at::rank_top_k_cost(cand, nc, mm, nn, kk, spec, idx, cost_k);
    REQUIRE(ntop == cost_k);

    const auto measure = [&](const kir::TileSchedule& s) -> kir::ContractTiming {
        const kir::ContractTiming r = cu.time_contract_schedule(g, c, s, inputs, 2, out.data(), 3, 12);
        if (r.ok && !sampled_correct(out.data(), mm, nn, kk)) { return kir::ContractTiming{}; }
        return r;
    };

    double best_ms = 1.0e30;
    for (int t = 0; t < ntop; ++t)
    {
        const kir::ContractTiming r = measure(cand[idx[t]]);
        if (r.ok && r.min_ms < best_ms) { best_ms = r.min_ms; }
    }
    REQUIRE(best_ms < 1.0e29);

    // the KNOWN optimum for this shape (the DB / hand-tuned winner) as the reference.
    const kir::ContractTiming ref = measure(kir::select_schedule(g, c));
    REQUIRE(ref.ok);

    std::printf("[AS-3] cost-model top-%d of %d valid: best %.3f ms vs DB/hand optimum %.3f ms (ratio %.3f); "
                "measured %d instead of %d (%.0fx fewer compiles)\n",
                cost_k, nc, best_ms, ref.min_ms, best_ms / ref.min_ms, cost_k, nc, static_cast<double>(nc) / cost_k);

    // GATE: measuring only the cost-model top-6 finds a schedule close to the known optimum (the model surfaced it), while
    // measuring 250x fewer configs than the full space. 15% margin absorbs GPU boost-clock run-to-run timing variance.
    CHECK(best_ms <= ref.min_ms * 1.15);
    CHECK(nc / cost_k >= 50); // a large pruning factor
}

// ADR-0098 §4 · AS-4 (generalization): the autotuner is SHAPE-GENERAL — it tunes RECTANGULAR MLP-shaped GEMMs (M≠N≠K), not just
// square, oracle-correct + crushing naive, and the checked-in DB replays a rectangular entry. This is what makes the
// auto-scheduler a general COMPILER PROPERTY, not a square-GEMM special case.
TEST_CASE("AS-4: the autotuner is SHAPE-GENERAL -- rectangular MLP GEMM tunes correct + crushes naive", "[kir][cuda][gpu][autotune]")
{
    crd::memory::TlsfAllocator alloc(512U << 20U);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }

    // a rectangular MLP-shaped GEMM: M=2048, N=512, K=1024 (A[2048,1024] * B[1024,512]).
    const crd::kir::AutotuneResult r = crd::kir::autotune_contract(cu, 2048, 512, 1024, 16, true, &alloc);
    REQUIRE(r.ok);
    CHECK(r.correct == r.measured); // every candidate oracle-correct on the rectangular shape
    REQUIRE(r.naive_ms > 0.0);
    CHECK(r.naive_ms / r.ms >= 5.0); // tiling+search crushes naive on the rectangular shape too

    // the DB (regenerated with rectangular entries) replays a rectangular winner deterministically.
    kir::KGraph g(&alloc);
    const int   a = g.input(kir::make_shape({2048, 1024}), kir::DType::F32);
    const int   b = g.input(kir::make_shape({1024, 512}), kir::DType::F32);
    const int   c = g.contract(a, b);
    const kir::TileSchedule sched = kir::select_schedule(g, c);
    CHECK(sched.kind == kir::Sched::WarpTiled);
    const double gflops = 2.0 * 2048.0 * 512.0 * 1024.0 / (r.ms * 1.0e6);
    std::printf("[AS-4 gen] rectangular 2048x512x1024: autotuned %.3f ms (%.0f GFLOP/s), %d/%d correct, %.1fx naive; DB replays WarpTiled\n",
                r.ms, gflops, r.correct, r.measured, r.naive_ms / r.ms);
}

// AS-4: the FLASH-ATTENTION autotuner over (BR,BC). Same enumerate → MEASURE-on-device → oracle-validate → pick-best loop as the
// GEMM, now tuning the flash kernel's query/key tile. Each candidate is time_attention'd (GPU-event-timed) and gated against the
// NAIVE CPU attention (`cpu.run` → eval_cpu, the ground truth — attention is 2·S²·D, feasible on CPU); a FAST tier ⇒ ULP-tolerant.
// The autotuner's pick is at least as fast as the fixed 64×32 default (here it finds BR=128×BC=32 — the wider query block beats the
// hard-coded BR=64 at both S) — so flash attention is now a SCHEDULED kernel, not a hard-coded tile.
TEST_CASE("AS-4: the flash-attention autotuner sweeps (BR,BC), oracle-validates, picks the fastest", "[kir][cuda][gpu][autotune][attention]")
{
    crd::memory::TlsfAllocator alloc(512U << 20U);
    kir::KirBackendCuda        cu(&alloc);
    if (!cu.valid()) { WARN("no CUDA device available; skipping"); return; }
    kir::KirBackendCpu cpu(&alloc);

    constexpr int dim      = 64;
    const double  scale    = 1.0 / 8.0;
    const int     slens[4] = {512, 1024, 2048, 4096};
    for (int si = 0; si < 4; ++si)
    {
        const int   slen = slens[si];
        kir::KGraph g(&alloc);
        const int   q = g.input(kir::make_shape({slen, dim}), kir::DType::F32);
        const int   k = g.input(kir::make_shape({slen, dim}), kir::DType::F32);
        const int   v = g.input(kir::make_shape({slen, dim}), kir::DType::F32);
        const int   y = g.attention(q, k, v, scale);

        const crd::usize              nelem = static_cast<crd::usize>(slen) * static_cast<crd::usize>(dim);
        crd::containers::Array<float> qv(&alloc);
        crd::containers::Array<float> kv(&alloc);
        crd::containers::Array<float> vv(&alloc);
        crd::containers::Array<float> oracle(&alloc);
        crd::containers::Array<float> out(&alloc);
        qv.resize(nelem);
        kv.resize(nelem);
        vv.resize(nelem);
        oracle.resize(nelem);
        out.resize(nelem);
        for (crd::usize i = 0; i < nelem; ++i)
        {
            qv[i] = 0.02F * static_cast<float>((i * 7U) % 37U) - 0.35F;
            kv[i] = 0.02F * static_cast<float>((i * 11U) % 41U) - 0.40F;
            vv[i] = 0.02F * static_cast<float>((i * 5U) % 31U) - 0.30F;
        }
        const float* inputs[] = {qv.data(), kv.data(), vv.data()};
        REQUIRE(cpu.run(g, y, inputs, 3, oracle.data())); // the naive ground-truth reference

        at::DeviceLimits      lim;
        at::AttentionSchedule space[64];
        const int             cnt = at::enumerate_attention_schedules(dim, lim, space, 64);
        REQUIRE(cnt > 0);
        const auto max_err = [](const float* aa, const float* bb, crd::usize n) {
            float e = 0.0F;
            for (crd::usize i = 0; i < n; ++i) { const float d = aa[i] > bb[i] ? aa[i] - bb[i] : bb[i] - aa[i]; if (d > e) { e = d; } }
            return e;
        };

        // What run() will replay for this (device,S,D): the checked-in DB row when tuned, else the 64×32 heuristic.
        // Looked up BEFORE the sweep so its tile's time is recorded from THE SAME measurement pass as every other tile —
        // a separate later timing call is not comparable (GPU boost-clock drift between two calls inverts a 5% margin;
        // the per-slice sweep proved it: a same-run 64×32 re-time came in 16% off its sweep-pass number).
        int db_br = 0;
        int db_bc = 0;
        kir::select_attention_tile(dim, slen, cu.device(), db_br, db_bc);
        int tuned_br = 0;
        int tuned_bc = 0;
        const bool is_tuned = crd::kir::lookup_attention_tuned(cu.device(), slen, dim, tuned_br, tuned_bc);

        double best_ms   = 1.0e30;
        int    best_br   = 0;
        int    best_bc   = 0;
        int    measured  = 0;
        double def_ms    = -1.0; // the 64×32 heuristic default's time, from this sweep pass
        double db_ms     = -1.0; // the DB row's time, from this sweep pass
        for (int ci = 0; ci < cnt; ++ci)
        {
            const at::AttentionSchedule& s = space[ci];
            const kir::ContractTiming    r = cu.time_attention(g, y, s.br, s.bc, inputs, 3, out.data(), 3, 12);
            if (!r.ok) { continue; }
            CHECK(max_err(out.data(), oracle.data(), nelem) < 2.0e-3F); // ORACLE gate (fast tier): a wrong tile can never win
            ++measured;
            if (s.br == 64 && s.bc == 32) { def_ms = r.min_ms; }
            if (s.br == db_br && s.bc == db_bc) { db_ms = r.min_ms; }
            if (r.min_ms < best_ms) { best_ms = r.min_ms; best_br = s.br; best_bc = s.bc; }
        }
        REQUIRE(measured > 0);

        // the 64×32 HEURISTIC default (what select_attention_tile emits with no DB row) is IN the search space, so the
        // tuned pick is never worse — asserted on same-pass numbers (exact, no noise tolerance needed).
        REQUIRE(def_ms > 0.0);
        CHECK(best_ms <= def_ms);

        // AS-2-for-attention: select_attention_tile (what run() calls) REPLAYS the checked-in tuned tile. Two claims, kept
        // separate: (1) the WIRING is exact — the replay equals the DB row verbatim (deterministic). (2) the QUALITY holds —
        // the checked-in row is still NEAR-OPTIMAL on this device, never "equal to today's argmin": two near-tied tiles swap
        // order run to run under load (measurement noise, not a stale DB), while a genuinely wrong tile is 1.5–8× off (the
        // register-occupancy scar). Margin CALIBRATED from data: near-tied tiles measured 1.177 apart SAME-PASS under
        // full-suite GPU load (2026-07-23) — 1.30 clears that swing while still catching the ≥1.5 wrong-tile class.
        // Untuned shapes fall back to the heuristic (no assertion — the DB simply lacks a row).
        if (is_tuned)
        {
            CHECK(db_br == tuned_br);
            CHECK(db_bc == tuned_bc);
            REQUIRE(db_ms > 0.0); // the DB row must be an enumerable (valid) schedule on this device
#if !CRD_TEST_ASAN
            CHECK(db_ms <= best_ms * 1.30); // QUALITY margin — clean-env timing only (see the CRD_TEST_ASAN note up top)
#endif
        }
        std::printf("[attn-autotune] S=%d D=%d: measured %d/%d tiles, WINNER BR=%d BC=%d %.4f ms (heuristic 64x32 %.4f ms); run() -> %dx%d (%s, %.4f ms)\n",
                    slen, dim, measured, cnt, best_br, best_bc, best_ms, def_ms, db_br, db_bc, is_tuned ? "DB" : "heuristic",
                    is_tuned ? db_ms : def_ms);
    }
}
