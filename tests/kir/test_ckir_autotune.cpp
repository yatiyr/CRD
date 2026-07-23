// tests/kir/test_ckir_autotune.cpp — ADR-0098 §4 · AS-1a: the CKIR auto-scheduler SEARCH SPACE (backend-free half).
//
// The enumerator produces the valid WarpTiled `TileSchedule` candidates the CUDA timed search (AS-1b) then measures. These tests
// pin the search-space CONTRACT with no GPU: every emitted schedule is self-consistent + fits the device, the hand-tuned v17-e
// winner is a member (so the autotuner can rediscover it), and the divisibility guard actually prunes.

#include <catch2/catch_test_macros.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_autotune.hpp>
#include <crd/kir/ckir_eval.hpp>    // AS-5: eval_cpu — prove the superoptimizer is bit-exact
#include <crd/kir/ckir_harness.hpp> // AS-5: bit_equal
#include <crd/kir/ckir_tile.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>

namespace at = crd::kir::autotune;

// The v17-e checked-in winner (select_schedule's one entry): BM128 BN128 BK8 · WM64 WN32 WNITER1 · TM8 TN8 · NT256 · DB.
namespace
{
[[nodiscard]] crd::kir::TileSchedule handtuned_winner()
{
    crd::kir::TileSchedule s;
    s.kind          = crd::kir::Sched::WarpTiled;
    s.bm            = 128; s.bn = 128; s.bk = 8;
    s.wm            = 64;  s.wn = 32;  s.wniter = 1;
    s.tm            = 8;   s.tn = 8;
    s.nt            = 256;
    s.double_buffer = true;
    s.fma           = true;
    return s;
}
[[nodiscard]] bool same_schedule(const crd::kir::TileSchedule& a, const crd::kir::TileSchedule& b)
{
    return a.bm == b.bm && a.bn == b.bn && a.bk == b.bk && a.wm == b.wm && a.wn == b.wn && a.wniter == b.wniter
        && a.tm == b.tm && a.tn == b.tn && a.nt == b.nt && a.double_buffer == b.double_buffer;
}
} // namespace

TEST_CASE("AS-1a: the hand-tuned v17-e winner is a VALID member of the search space", "[kir][autotune]")
{
    const at::DeviceLimits lim; // default Ada SM ceilings
    const crd::kir::TileSchedule w = handtuned_winner();
    // it must pass every membership constraint for the 1024-cube it was tuned on...
    CHECK(at::contract_schedule_valid(w, 1024, 1024, 1024, lim));
    at::ScheduleResources r;
    REQUIRE(at::schedule_resources(w, r));
    CHECK(r.threads == 256U);
    CHECK(r.wmiter == 1U);
    CHECK(r.accum == 64U);                 // WMITER·TM·WNITER·TN = 1·8·1·8
    CHECK(r.smem == 16640U);               // (8·132 + 8·128)·4·2
    CHECK(r.smem <= lim.smem_bytes);

    // ...and the enumerator must actually EMIT it (so the autotuner can rediscover it).
    crd::kir::TileSchedule cand[4096];
    const int              nc = at::enumerate_contract_schedules(1024, 1024, 1024, lim, cand, 4096);
    REQUIRE(nc > 0);
    bool found = false;
    for (int i = 0; i < nc; ++i) { if (same_schedule(cand[i], w)) { found = true; break; } }
    CHECK(found);
    std::printf("[AS-1a] search space for 1024^3: %d valid schedules; hand-tuned winner present=%d\n", nc, found ? 1 : 0);
}

TEST_CASE("AS-1a: EVERY enumerated schedule is self-consistent + fits the device", "[kir][autotune]")
{
    const at::DeviceLimits lim;
    crd::kir::TileSchedule  cand[4096];
    const int               nc = at::enumerate_contract_schedules(1024, 1024, 1024, lim, cand, 4096);
    REQUIRE(nc > 0);
    int bad = 0;
    for (int i = 0; i < nc; ++i)
    {
        at::ScheduleResources r;
        const bool res_ok = at::schedule_resources(cand[i], r);
        const bool val_ok = at::contract_schedule_valid(cand[i], 1024, 1024, 1024, lim);
        const bool fits   = res_ok && r.threads <= lim.max_threads && r.smem <= lim.smem_bytes && r.accum <= lim.max_accum
                         && (cand[i].nt == static_cast<int>(r.threads));
        if (!(res_ok && val_ok && fits)) { ++bad; }
    }
    CHECK(bad == 0);
    std::printf("[AS-1a] all %d enumerated schedules valid + fit (smem<=48KB, threads<=1024, accum<=128)\n", nc);
}

TEST_CASE("AS-1a: the divisibility guard prunes shape-incompatible schedules", "[kir][autotune]")
{
    const at::DeviceLimits lim;
    // A prime-ish shape (M not a multiple of any block tile ≥64) ⇒ NO WarpTiled candidate is valid (all fall back to Naive).
    crd::kir::TileSchedule cand[4096];
    const int              nc_bad = at::enumerate_contract_schedules(129, 1024, 1024, lim, cand, 4096);
    CHECK(nc_bad == 0); // 129 % {64,128,256} != 0 ⇒ nothing tiles it
    // A smaller smem budget must prune the double-buffered / large-tile schedules.
    at::DeviceLimits tight = lim;
    tight.smem_bytes       = 8U * 1024U; // 8 KB
    const int nc_tight     = at::enumerate_contract_schedules(1024, 1024, 1024, tight, cand, 4096);
    const int nc_full      = at::enumerate_contract_schedules(1024, 1024, 1024, lim, cand, 4096);
    CHECK(nc_tight < nc_full);
    std::printf("[AS-1a] pruning: 129x1024 -> %d schedules; 8KB smem -> %d vs 48KB -> %d\n", nc_bad, nc_tight, nc_full);
}

// AS-2: the checked-in tuning DB is REPLAYED by select_schedule — deterministically, overriding the hand heuristic on an exact
// shape match, and falling back cleanly (heuristic, then Naive) for shapes the DB doesn't list. Pure (no GPU): replay is a
// compile-time table lookup, the whole point of "tune offline, replay from the DB, never at runtime" (ADR-0098 §4).
namespace
{
[[nodiscard]] crd::kir::TileSchedule sched_for(crd::memory::IAllocator* a, int m, int n, int k)
{
    crd::kir::KGraph g(a);
    const int        x = g.input(crd::kir::make_shape({m, k}), crd::kir::DType::F32);
    const int        y = g.input(crd::kir::make_shape({k, n}), crd::kir::DType::F32);
    const int        c = g.contract(x, y);
    return crd::kir::select_schedule(g, c);
}
} // namespace

TEST_CASE("AS-2: select_schedule REPLAYS the auto-tuned DB deterministically, with clean fallback", "[kir][autotune]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // the DB carries a measured winner for 1024^3 — lookup_tuned finds it, and select_schedule returns exactly it.
    crd::kir::TileSchedule tuned;
    REQUIRE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, nullptr, 1024, 1024, 1024, tuned));
    CHECK(tuned.kind == crd::kir::Sched::WarpTiled);
    const crd::kir::TileSchedule s1024 = sched_for(&alloc, 1024, 1024, 1024);
    CHECK(s1024.kind == crd::kir::Sched::WarpTiled);
    CHECK(s1024.bm == tuned.bm);
    CHECK(s1024.bn == tuned.bn);
    CHECK(s1024.bk == tuned.bk);
    CHECK(s1024.nt == tuned.nt);
    CHECK(s1024.double_buffer == tuned.double_buffer);

    // deterministic replay: same query → byte-identical schedule, every call.
    const crd::kir::TileSchedule s1024b = sched_for(&alloc, 1024, 1024, 1024);
    CHECK(s1024b.bm == s1024.bm);
    CHECK(s1024b.nt == s1024.nt);
    CHECK(s1024b.double_buffer == s1024.double_buffer);

    // 512^3 is ALSO a generated DB entry now (the kir_autotune CLI measured it) ⇒ a DB hit.
    crd::kir::TileSchedule tuned512;
    CHECK(crd::kir::lookup_tuned(crd::kir::KOp::Contract, nullptr, 512, 512, 512, tuned512));

    // a shape NOT in the DB but heuristic-eligible (768 = 6*128) ⇒ the hand heuristic still fires (WarpTiled).
    crd::kir::TileSchedule none;
    CHECK_FALSE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, nullptr, 768, 768, 768, none));
    CHECK(sched_for(&alloc, 768, 768, 768).kind == crd::kir::Sched::WarpTiled);

    // a shape neither in the DB nor heuristic-eligible ⇒ Naive (bit-exact reference).
    CHECK(sched_for(&alloc, 96, 96, 96).kind == crd::kir::Sched::Naive);
    std::printf("[AS-2] DB replay: 1024^3+512^3 -> tuned WarpTiled (BM%d BN%d BK%d NT%d DB%d); 768^3 -> heuristic; 96^3 -> Naive\n",
                s1024.bm, s1024.bn, s1024.bk, s1024.nt, s1024.double_buffer ? 1 : 0);
}

// ADR-0098 §4 · AS-6a: the tuning DB is DEVICE-KEYED. Each row carries the GPU arch it was tuned on, so `lookup_tuned` matches
// ONLY that device — a different GPU misses and falls back to the heuristic (correct: the best tile schedule genuinely differs
// per device, so replaying sm_89's winner on another GPU would mis-tune). A nullptr device is a wildcard (single-device query).
TEST_CASE("AS-6a: the tuning DB is DEVICE-KEYED -- right GPU hits, others miss, wildcard matches any", "[kir][autotune]")
{
    crd::kir::TileSchedule s;
    // exact device match: the checked-in DB was tuned on sm_89.
    CHECK(crd::kir::lookup_tuned(crd::kir::KOp::Contract, "sm_89", 1024, 1024, 1024, s));
    // a DIFFERENT device is NOT a match ⇒ that GPU falls back to the heuristic (a sm_89-tuned schedule may be wrong for it).
    CHECK_FALSE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, "sm_75", 1024, 1024, 1024, s));
    CHECK_FALSE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, "sm_120", 2048, 512, 1024, s));
    // nullptr = wildcard (backward-compatible single-device query) matches any.
    CHECK(crd::kir::lookup_tuned(crd::kir::KOp::Contract, nullptr, 1024, 1024, 1024, s));

    // tuning_device_eq semantics: wildcard, exact, mismatch, and a PREFIX must NOT count as a match.
    CHECK(crd::kir::tuning_device_eq(nullptr, "sm_89"));
    CHECK(crd::kir::tuning_device_eq("sm_89", "sm_89"));
    CHECK_FALSE(crd::kir::tuning_device_eq("sm_89", "sm_8")); // prefix is not a match
    CHECK_FALSE(crd::kir::tuning_device_eq("sm_8", "sm_89"));
    CHECK_FALSE(crd::kir::tuning_device_eq("sm_89", nullptr));
    std::printf("[AS-6a] device-keyed DB: sm_89 hits, sm_75/sm_120 miss (-> heuristic), wildcard hits; per-GPU replay works\n");
}

// ADR-0098 §4 · AS-5: a BIT-EXACT equality-saturation SUPEROPTIMIZER that beats GREEDY optimize(). optimize()'s CSE matches
// operands POSITIONALLY, so a*b and b*a never collapse (phase-ordering blind spot). superoptimize() canonicalizes commutative
// ops then folds/CSEs to a fixpoint, so commutatively-equal subexpressions coincide. Only bit-exact rules (commutativity + CSE
// + const-fold) — reassociation/distributivity change float rounding, so for a bit-exact compiler this IS the whole safe superopt
// space. Gate: on (a*b+c)*(b*a+c), superoptimize yields FEWER nodes than optimize, and the result is BIT-IDENTICAL to the input.
TEST_CASE("AS-5: bit-exact equality-saturation superoptimizer beats greedy optimize (commutative CSE)", "[kir][autotune]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    const auto                 build = [](crd::kir::KGraph& g) -> int {
        const auto sh = crd::kir::make_shape({4});
        const int  a  = g.input(sh, crd::kir::DType::F64);
        const int  b  = g.input(sh, crd::kir::DType::F64);
        const int  cc = g.input(sh, crd::kir::DType::F64);
        const int  ab = g.binary(crd::kir::KOp::Mul, a, b);
        const int  ba = g.binary(crd::kir::KOp::Mul, b, a); // b*a — greedy CSE won't merge with a*b (positional operand match)
        const int  p1 = g.binary(crd::kir::KOp::Add, ab, cc);
        const int  p2 = g.binary(crd::kir::KOp::Add, ba, cc);
        const int  pr = g.binary(crd::kir::KOp::Mul, p1, p2);
        return g.reduce(crd::kir::KOp::ReduceSum, pr, 0x1U);
    };
    const crd::f64        av[4]     = {1.0, 2.0, -0.5, 3.0};
    const crd::f64        bv[4]     = {0.4, -0.6, 0.9, 0.1};
    const crd::f64        cv[4]     = {0.2, -0.3, 0.5, -0.1};
    const crd::f64* const inputs[]  = {av, bv, cv};

    // greedy optimize (the baseline)
    crd::kir::KGraph g1(&alloc);
    const int        r1 = build(g1);
    crd::f64         ref[1];
    crd::kir::eval_cpu(g1, inputs, &alloc, r1, ref);
    int roots1[1] = {r1};
    g1.optimize(roots1, 1);
    const int size_opt = g1.size();
    crd::f64  opt_out[1];
    crd::kir::eval_cpu(g1, inputs, &alloc, roots1[0], opt_out);
    CHECK(crd::kir::bit_equal(ref, opt_out, 1)); // optimize is semantics-preserving

    // equality-saturation superoptimize
    crd::kir::KGraph g2(&alloc);
    const int        r2 = build(g2);
    int              roots2[1] = {r2};
    g2.superoptimize(roots2, 1);
    const int size_super = g2.size();
    crd::f64  super_out[1];
    crd::kir::eval_cpu(g2, inputs, &alloc, roots2[0], super_out);
    CHECK(crd::kir::bit_equal(ref, super_out, 1)); // BIT-IDENTICAL to the input — no rounding change from the rewrites

    CHECK(size_super < size_opt); // the superoptimizer found the commutative CSE greedy optimize() missed
    std::printf("[AS-5] superopt (a*b+c)*(b*a+c): greedy optimize -> %d nodes; equality-saturation -> %d nodes, bit-exact\n",
                size_opt, size_super);
}

// AS-4 op-generality: the auto-scheduler's schedule-space machinery, proven on Contract, applies to a REDUCTION. This pins the
// backend-free half (unit-testable without a GPU): the enumerated reduce space is non-empty + every member is valid + build_reduce-
// compatible, the hand-tuned (256,8) winner is a member, and the cost model ranks a DRAM-saturating schedule above a starved one.
TEST_CASE("AS-4: the auto-scheduler generalizes to REDUCE -- valid schedule space + cost-model rank", "[kir][autotune]")
{
    const at::DeviceLimits lim;
    const int              n = 1 << 24; // 16.7M elements (64 MB, DRAM-bound)

    at::ReduceSchedule space[64];
    const int          cnt = at::enumerate_reduce_schedules(n, lim, space, 64);
    REQUIRE(cnt > 0);

    // EVERY enumerated schedule is valid + emittable: power-of-two threads, per-block span divides N, nblocks a multiple of threads.
    for (int i = 0; i < cnt; ++i)
    {
        CHECK(at::reduce_schedule_valid(n, space[i], lim));
        const int nb = at::reduce_nblocks(n, space[i]);
        CHECK(nb > 0);
        CHECK((nb % space[i].threads) == 0);
        CHECK(static_cast<long long>(space[i].threads) * space[i].per_thread * nb == n); // the space tiles N exactly
    }

    // the hand-tuned v17-e / B-cmp winner (256 threads, per_thread=8) is a MEMBER of the space the autotuner searches.
    bool has_handtuned = false;
    for (int i = 0; i < cnt; ++i) { if (space[i].threads == 256 && space[i].per_thread == 8) { has_handtuned = true; } }
    CHECK(has_handtuned);

    // an out-of-range / non-power-of-two / non-dividing schedule is REJECTED (never emitted).
    CHECK(!at::reduce_schedule_valid(n, at::ReduceSchedule{192, 8}, lim));  // 192 not a power of two
    CHECK(!at::reduce_schedule_valid(n, at::ReduceSchedule{2048, 8}, lim)); // exceeds max_threads
    CHECK(!at::reduce_schedule_valid(7, at::ReduceSchedule{256, 8}, lim));  // 256·8 does not divide 7

    // COST MODEL: a schedule that launches enough blocks to saturate DRAM must rank AHEAD of one starved of blocks (huge unroll ⇒
    // too few blocks). Compare a saturating config vs a block-starved one directly.
    const at::DeviceSpec     spec;
    const at::ReduceSchedule saturating{256, 8};   // nblocks = 2^24/2048 = 8192 ≫ 2·SMs ⇒ full bandwidth
    const at::ReduceSchedule starved{64, 4096};    // nblocks = 2^24/262144 = 64 < 2·66 SMs ⇒ under-occupied (valid: 64%64==0)
    REQUIRE(at::reduce_schedule_valid(n, saturating, lim));
    REQUIRE(at::reduce_schedule_valid(n, starved, lim));
    CHECK(at::predict_reduce_ms(n, saturating, spec) < at::predict_reduce_ms(n, starved, spec));

    // the cost-model top-K selection returns valid, distinct indices.
    int       topk[6];
    const int got = at::rank_reduce_top_k_cost(space, cnt, n, spec, topk, 6);
    REQUIRE(got > 0);
    for (int t = 0; t < got; ++t) { CHECK(topk[t] >= 0); CHECK(topk[t] < cnt); }

    std::printf("[AS-4] reduce schedule space: %d valid schedules for N=%d (hand-tuned 256x8 a member); cost model ranks saturating > starved; top-%d selected\n",
                cnt, n, got);
}

// AS-4 FLASH-ATTENTION: the backend-free (BR,BC) schedule space — every enumerated tile is valid + shared-fitting, the fixed
// default (64,32) is a member, and illegal tiles (non-power-of-two BR, over-cap BR, shared overflow) are rejected. The on-device
// measured search (time_attention) lives in the kir-cuda test.
TEST_CASE("AS-4: the flash-attention (BR,BC) schedule space is valid + the default is a member", "[kir][autotune]")
{
    const at::DeviceLimits lim;
    const int              dim = 64;

    at::AttentionSchedule space[64];
    const int             cnt = at::enumerate_attention_schedules(dim, lim, space, 64);
    REQUIRE(cnt > 0);
    for (int i = 0; i < cnt; ++i)
    {
        CHECK(at::attention_schedule_valid(dim, space[i], lim));
        CHECK((space[i].br & (space[i].br - 1)) == 0);                                    // BR power of two
        CHECK(static_cast<long long>(space[i].bc) * dim * 2 * 4 <= 48 * 1024);            // K/V tiles fit static shared
    }

    bool has_default = false;
    for (int i = 0; i < cnt; ++i) { if (space[i].br == 64 && space[i].bc == 32) { has_default = true; } }
    CHECK(has_default); // the fixed select_attention_tile default is in the searched space

    CHECK(!at::attention_schedule_valid(dim, at::AttentionSchedule{48, 32}, lim));   // BR 48 not a power of two
    CHECK(!at::attention_schedule_valid(dim, at::AttentionSchedule{2048, 32}, lim)); // BR exceeds max_threads
    CHECK(!at::attention_schedule_valid(256, at::AttentionSchedule{64, 128}, lim));  // 2·128·256·4 = 256 KB > 48 KB shared

    std::printf("[AS-4] flash-attention schedule space: %d valid (BR,BC) tiles for D=%d (fixed 64x32 a member); illegal tiles rejected\n", cnt, dim);
}

// AS-2-for-attention: the checked-in flash-attention tuning DB replays the tuned (BR,BC) per (device,S,D), so run() emits the tuned
// kernel with no runtime search — a wildcard device matches any, and an untuned device/shape MISSES (⇒ the heuristic fallback).
TEST_CASE("AS-4: the attention tuning DB replays the tuned (BR,BC) per (device,S,D)", "[kir][autotune]")
{
    int br = 0;
    int bc = 0;
    // HITS: the on-device measured winners for sm_89, D=64 (BR=128 wide query block across the S sweep).
    CHECK(crd::kir::lookup_attention_tuned("sm_89", 1024, 64, br, bc));
    CHECK(br == 128);
    CHECK(bc == 32);
    CHECK(crd::kir::lookup_attention_tuned("sm_89", 4096, 64, br, bc));
    CHECK(br == 128);
    CHECK(bc == 32);
    CHECK(crd::kir::lookup_attention_tuned(nullptr, 512, 64, br, bc)); // nullptr device = wildcard
    CHECK(br == 128);
    CHECK(crd::kir::lookup_attention_tuned("sm_89", 2048, 32, br, bc)); // D=32 also tuned
    CHECK(br == 128);
    CHECK(bc == 32);

    // MISSES ⇒ false ⇒ the caller (select_attention_tile) falls back to the shared-fitting heuristic.
    CHECK(!crd::kir::lookup_attention_tuned("sm_75", 1024, 64, br, bc)); // untuned device
    CHECK(!crd::kir::lookup_attention_tuned("sm_89", 777, 64, br, bc));  // untuned S
    CHECK(!crd::kir::lookup_attention_tuned("sm_89", 1024, 128, br, bc)); // untuned D (128 spills — needs the warp-collab kernel)

    std::printf("[AS-4] attention DB: (sm_89,1024/4096,64) -> 128x32 replayed, wildcard hits, untuned device/S/D miss -> heuristic fallback\n");
}
