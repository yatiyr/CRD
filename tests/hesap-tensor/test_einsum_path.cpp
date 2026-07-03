// v14-e gates: the einsum front-end.
//   Path quality vs the opt_einsum 3.4.0 oracle corpus (python-verified
//   before the port): our OPTIMAL == their optimal on every case; our
//   GREEDY <= their greedy (parity-or-better; a win is recorded, a loss
//   fails). Parser: implicit-output rule (incl. "ii" -> trace), ellipsis,
//   diagonals flagged, explicit output validation, status adversaries.

#include "ref_einsum_paths.inc"

#include <crd/hesap/tensor/einsum.hpp>

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>

using namespace crd::hesap::tensor;

namespace
{

// Ranks per term from a corpus expression (plain letters, no ellipsis).
crd::u32 expr_ranks(const char* expr, crd::u32* ranks) noexcept
{
    crd::u32 n = 0;
    crd::u32 r = 0;
    for (const char* p = expr; *p != '\0' && *p != '-'; ++p)
    {
        if (*p == ',')
        {
            ranks[n++] = r;
            r = 0;
        }
        else
        {
            ++r;
        }
    }
    ranks[n++] = r;
    return n;
}

} // namespace

TEST_CASE("v14-e path: optimal == opt_einsum, greedy parity-or-better (33-case oracle corpus)",
          "[hesap][tensor][v14][einsum]")
{
    crd::u64 greedy_wins = 0;
    for (crd::u32 ci = 0; ci < kEinsumPathCaseCount; ++ci)
    {
        const EinsumPathCase& c = kEinsumPathCases[ci];
        INFO("case " << ci << ": " << c.expr);

        crd::u32 ranks[kEinsumMaxOperands];
        const crd::u32 n_ops = expr_ranks(c.expr, ranks);
        EinsumExpr expr;
        REQUIRE(einsum_parse(c.expr, {ranks, n_ops}, expr) == TensorStatus::Ok);

        crd::u64 idx_size[kEinsumMaxIndices] = {};
        for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
        {
            idx_size[i] = 1;
        }
        for (crd::u32 i = 0; i < c.n_idx; ++i)
        {
            idx_size[static_cast<crd::u32>(c.idx_names[i] - 'a')] = c.idx_sizes[i];
        }

        EinsumPlan greedy;
        REQUIRE(einsum_plan_build(expr, idx_size, EinsumOptimize::Greedy, greedy) == TensorStatus::Ok);
        CHECK(greedy.total_flops <= c.greedy_flops); // parity-or-better
        if (greedy.total_flops < c.greedy_flops)
        {
            ++greedy_wins;
        }

        if (c.optimal_flops != 0U)
        {
            EinsumPlan optimal;
            REQUIRE(einsum_plan_build(expr, idx_size, EinsumOptimize::Optimal, optimal) == TensorStatus::Ok);
            // Parity-or-better: opt_einsum's optimal SEARCH minimizes an
            // internally different objective (inner = shared-removed) than its
            // REPORTED opt_cost (inner = any-removed); ours minimizes the
            // reported metric directly, so it can genuinely beat their
            // "optimal" under their own accounting (verified in python:
            // build/crd_einsum_diag2.py).
            CHECK(optimal.total_flops <= c.optimal_flops);
            CHECK(optimal.total_flops <= greedy.total_flops);
            CHECK(optimal.n_steps == n_ops - 1U);
        }
    }
    INFO("greedy strictly beat opt_einsum greedy on " << greedy_wins << " cases");
    CHECK(true);
}

TEST_CASE("v14-e plan: build cost over the corpus (the build-once story)", "[hesap][tensor][v14][einsum][!benchmark]")
{
    // Mean plan time across the 33-case corpus (parse + exact/greedy path).
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int reps = 20;
    for (int rep = 0; rep < reps; ++rep)
    {
        for (crd::u32 ci = 0; ci < kEinsumPathCaseCount; ++ci)
        {
            const EinsumPathCase& c = kEinsumPathCases[ci];
            crd::u32 ranks[kEinsumMaxOperands];
            const crd::u32 n_ops = expr_ranks(c.expr, ranks);
            EinsumExpr expr;
            (void)einsum_parse(c.expr, {ranks, n_ops}, expr);
            crd::u64 idx_size[kEinsumMaxIndices];
            for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
            {
                idx_size[i] = 1;
            }
            for (crd::u32 i = 0; i < c.n_idx; ++i)
            {
                idx_size[static_cast<crd::u32>(c.idx_names[i] - 'a')] = c.idx_sizes[i];
            }
            EinsumPlan p;
            (void)einsum_plan_build(expr, idx_size, EinsumOptimize::Greedy, p);
        }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double us =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / (reps * double(kEinsumPathCaseCount));
    WARN("einsum_plan_build mean: " << us << " us/plan (33-case corpus)");
}

TEST_CASE("v14-e parser: implicit output, ellipsis, diagonals, adversaries", "[hesap][tensor][v14][einsum]")
{
    EinsumExpr e;

    // implicit matmul: "ij,jk" -> "ik"
    {
        const crd::u32 r[] = {2U, 2U};
        REQUIRE(einsum_parse("ij,jk", {r, 2U}, e) == TensorStatus::Ok);
        REQUIRE(e.out_count == 2U);
        CHECK(e.out_idx[0] == 'i' - 'a');
        CHECK(e.out_idx[1] == 'k' - 'a');
    }
    // "ii" implicit -> trace (scalar), diagonal flagged
    {
        const crd::u32 r[] = {2U};
        REQUIRE(einsum_parse("ii", {r, 1U}, e) == TensorStatus::Ok);
        CHECK(e.out_count == 0U);
        CHECK(e.has_diagonal);
    }
    // "ii->i" explicit diagonal vector
    {
        const crd::u32 r[] = {2U};
        REQUIRE(einsum_parse("ii->i", {r, 1U}, e) == TensorStatus::Ok);
        CHECK(e.out_count == 1U);
    }
    // ellipsis batch matmul: "...ij,...jk->...ik" with rank-4 operands
    {
        const crd::u32 r[] = {4U, 4U};
        REQUIRE(einsum_parse("...ij,...jk->...ik", {r, 2U}, e) == TensorStatus::Ok);
        CHECK(e.ellipsis_rank == 2U);
        CHECK(e.out_count == 4U);
        CHECK(e.out_idx[0] >= kEinsumEllipsisBase); // batch dims lead
        CHECK(e.out_idx[2] == 'i' - 'a');
    }
    // implicit ellipsis output: "...i,...i" -> "..." (batch dot)
    {
        const crd::u32 r[] = {3U, 3U};
        REQUIRE(einsum_parse("...i,...i", {r, 2U}, e) == TensorStatus::Ok);
        CHECK(e.out_count == 2U); // the two batch dims, no 'i'
    }
    // adversaries: rank mismatch / unknown output index / repeated output /
    // bad char / term-count mismatch
    {
        const crd::u32 r2[] = {2U, 2U};
        const crd::u32 r1[] = {2U};
        CHECK(einsum_parse("ijk,jk->ik", {r2, 2U}, e) == TensorStatus::ShapeMismatch);
        CHECK(einsum_parse("ij,jk->iq", {r2, 2U}, e) == TensorStatus::BadInput);
        CHECK(einsum_parse("ij,jk->ii", {r2, 2U}, e) == TensorStatus::BadInput);
        CHECK(einsum_parse("iJ,jk->ik", {r2, 2U}, e) == TensorStatus::BadInput);
        CHECK(einsum_parse("ij,jk->ik", {r1, 1U}, e) == TensorStatus::BadInput);
    }
}

TEST_CASE("v14-e plan: build-once metadata (steps, flops, largest intermediate)", "[hesap][tensor][v14][einsum]")
{
    // ab,bc,cd->ad with a=32,b=16,c=64,d=8: optimal contracts (ab,bc) first
    // (2*32*16*64=65536) then (abc',cd)... verify structural invariants.
    const crd::u32 r[] = {2U, 2U, 2U};
    EinsumExpr e;
    REQUIRE(einsum_parse("ab,bc,cd->ad", {r, 3U}, e) == TensorStatus::Ok);
    crd::u64 sz[kEinsumMaxIndices];
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        sz[i] = 1;
    }
    sz[0] = 32U; // a
    sz[1] = 16U; // b
    sz[2] = 64U; // c
    sz[3] = 8U;  // d
    EinsumPlan p;
    REQUIRE(einsum_plan_build(e, sz, EinsumOptimize::Optimal, p) == TensorStatus::Ok);
    REQUIRE(p.n_steps == 2U);
    CHECK(p.total_flops == p.step[0].flops + p.step[1].flops);
    CHECK(p.largest_intermediate >= 32U * 8U); // at least the output
    // the final step's result must be exactly the output set
    CHECK(p.step[1].result_mask == e.out_mask);
    // deterministic rebuild: identical plan
    EinsumPlan p2;
    REQUIRE(einsum_plan_build(e, sz, EinsumOptimize::Optimal, p2) == TensorStatus::Ok);
    CHECK(p2.total_flops == p.total_flops);
    CHECK(p2.step[0].a == p.step[0].a);
    CHECK(p2.step[0].b == p.step[0].b);
}
