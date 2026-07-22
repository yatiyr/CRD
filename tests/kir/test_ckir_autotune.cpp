// tests/kir/test_ckir_autotune.cpp — ADR-0098 §4 · AS-1a: the CKIR auto-scheduler SEARCH SPACE (backend-free half).
//
// The enumerator produces the valid WarpTiled `TileSchedule` candidates the CUDA timed search (AS-1b) then measures. These tests
// pin the search-space CONTRACT with no GPU: every emitted schedule is self-consistent + fits the device, the hand-tuned v17-e
// winner is a member (so the autotuner can rediscover it), and the divisibility guard actually prunes.

#include <catch2/catch_test_macros.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_autotune.hpp>
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
    REQUIRE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, 1024, 1024, 1024, tuned));
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
    CHECK(crd::kir::lookup_tuned(crd::kir::KOp::Contract, 512, 512, 512, tuned512));

    // a shape NOT in the DB but heuristic-eligible (768 = 6*128) ⇒ the hand heuristic still fires (WarpTiled).
    crd::kir::TileSchedule none;
    CHECK_FALSE(crd::kir::lookup_tuned(crd::kir::KOp::Contract, 768, 768, 768, none));
    CHECK(sched_for(&alloc, 768, 768, 768).kind == crd::kir::Sched::WarpTiled);

    // a shape neither in the DB nor heuristic-eligible ⇒ Naive (bit-exact reference).
    CHECK(sched_for(&alloc, 96, 96, 96).kind == crd::kir::Sched::Naive);
    std::printf("[AS-2] DB replay: 1024^3+512^3 -> tuned WarpTiled (BM%d BN%d BK%d NT%d DB%d); 768^3 -> heuristic; 96^3 -> Naive\n",
                s1024.bm, s1024.bn, s1024.bk, s1024.nt, s1024.double_buffer ? 1 : 0);
}
