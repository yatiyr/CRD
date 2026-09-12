// v14-f gates: einsum EXECUTION (TTGT over the deterministic GEMM).
//   Correctness: integer-valued operands (every contraction order exact in
//   f64) vs a naive full-index-space reference evaluator, across matmul /
//   chains / batch / outer / trace-ring / diagonals / ellipsis / private-
//   index pre-summing — both Greedy and Optimal plans.
//   Determinism: run-twice bit-identity + the {1,2,4,8,16} moat (the
//   gemm_parallel path is bit-exact across worker counts per ADR-0063).

#include <crd/hesap/tensor/einsum_exec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>

using namespace crd::hesap::tensor;

namespace
{

// Naive reference: iterate the FULL joint index space; out[...] += prod.
void ref_einsum(const EinsumExpr& e, const crd::u64* idx_size, const TensorView<const crd::f64>* ops, crd::f64* out,
                crd::u64 out_count) noexcept
{
    for (crd::u64 o = 0; o < out_count; ++o)
    {
        out[o] = 0.0;
    }
    // enumerate all indices used anywhere
    crd::u64 all_mask = e.out_mask;
    for (crd::u32 t = 0; t < e.n_ops; ++t)
    {
        all_mask |= e.term[t].mask;
    }
    crd::u32 ids[kEinsumMaxIndices];
    crd::u32 nid = 0;
    crd::u64 m = all_mask;
    while (m != 0U)
    {
        ids[nid++] = static_cast<crd::u32>(std::countr_zero(m));
        m &= m - 1U;
    }
    crd::u64 total = 1;
    for (crd::u32 i = 0; i < nid; ++i)
    {
        total *= idx_size[ids[i]];
    }
    crd::u64 assign[kEinsumMaxIndices];
    for (crd::u64 it = 0; it < total; ++it)
    {
        crd::u64 rem = it;
        for (crd::u32 i = 0; i < nid; ++i)
        {
            assign[ids[i]] = rem % idx_size[ids[i]];
            rem /= idx_size[ids[i]];
        }
        crd::f64 prod = 1.0;
        for (crd::u32 t = 0; t < e.n_ops; ++t)
        {
            crd::u64 off = 0;
            for (crd::u32 d = 0; d < e.term[t].count; ++d)
            {
                off = off * ops[t].shape(d) + assign[e.term[t].idx[d]];
            }
            prod *= ops[t].data()[off]; // contiguous canonical operands in this test
        }
        crd::u64 oo = 0;
        for (crd::u32 d = 0; d < e.out_count; ++d)
        {
            oo = oo * idx_size[e.out_idx[d]] + assign[e.out_idx[d]];
        }
        out[oo] += prod;
    }
}

struct ExecCase
{
    const char* expr;
    const char* names; // index letters with sizes below (parallel arrays)
    crd::u64 sizes[8];
};

} // namespace

TEST_CASE("v14-f exec: exact vs the naive evaluator across the semantics corpus", "[hesap][tensor][v14][einsumx]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U);
    const ExecCase cases[] = {
        {"ab,bc->ac", "abc", {5, 4, 6}},
        {"ab,bc,cd->ad", "abcd", {5, 4, 6, 3}},
        {"ab,bc,cd,de,ef->af", "abcdef", {4, 5, 3, 6, 4, 5}},
        {"ij,jk,kl,li->", "ijkl", {4, 5, 3, 6}}, // trace ring
        {"ab,cd->abcd", "abcd", {3, 4, 2, 5}},   // outer product
        {"abc,bad->dc", "abcd", {4, 5, 3, 6}},   // permuted operands
        {"ea,fb,abcd,gc,hd->efgh", "abcdefgh", {3, 4, 2, 3, 4, 2, 3, 4}},
        {"ab,ab->", "ab", {6, 7}},                   // full inner
        {"abc->b", "abc", {4, 5, 6}},                // single-op sum-out
        {"ab,cb->", "abc", {4, 5, 3}},               // private-index pre-sum both sides
        {"ea,abcd->ebcd", "abcde", {7, 6, 5, 8, 7}}, // thin small-M kernel shape (N=240)
        {"ab,bcde->acde", "abcde", {6, 7, 5, 4, 6}}, // ATrans small-M variant
    };
    for (const ExecCase& c : cases)
    {
        INFO(c.expr);
        // build sizes + ranks + operands
        crd::u64 idx_size[kEinsumMaxIndices];
        for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
        {
            idx_size[i] = 1;
        }
        crd::u32 nn = 0;
        for (const char* p = c.names; *p != '\0'; ++p, ++nn)
        {
            idx_size[static_cast<crd::u32>(*p - 'a')] = c.sizes[nn];
        }
        crd::u32 ranks[kEinsumMaxOperands];
        crd::u32 n_ops = 0;
        {
            crd::u32 r = 0;
            for (const char* p = c.expr; *p != '\0' && *p != '-'; ++p)
            {
                if (*p == ',')
                {
                    ranks[n_ops++] = r;
                    r = 0;
                }
                else
                {
                    ++r;
                }
            }
            ranks[n_ops++] = r;
        }
        EinsumExpr e;
        REQUIRE(einsum_parse(c.expr, {ranks, n_ops}, e) == TensorStatus::Ok);

        Tensor<crd::f64> ops[kEinsumMaxOperands];
        TensorView<const crd::f64> views[kEinsumMaxOperands];
        crd::u64 fill = 1;
        for (crd::u32 t = 0; t < n_ops; ++t)
        {
            crd::u64 shape[kMaxRank];
            for (crd::u32 d = 0; d < e.term[t].count; ++d)
            {
                shape[d] = idx_size[e.term[t].idx[d]];
            }
            ops[t] = Tensor<crd::f64>(&alloc, {shape, e.term[t].count});
            for (crd::u64 i = 0; i < ops[t].size(); ++i)
            {
                ops[t].data()[i] = static_cast<crd::f64>((fill * 13U + i * 7U) % 19U) - 9.0; // small ints
            }
            ++fill;
            views[t] = ops[t].view();
        }

        for (const EinsumOptimize mode : {EinsumOptimize::Greedy, EinsumOptimize::Optimal})
        {
            EinsumPlan plan;
            REQUIRE(einsum_plan_build(e, idx_size, mode, plan) == TensorStatus::Ok);
            Tensor<crd::f64> out(&alloc);
            REQUIRE(einsum_execute<crd::f64>(plan, {views, n_ops}, out, &alloc) == TensorStatus::Ok);

            crd::u64 out_count = 1;
            for (crd::u32 d = 0; d < e.out_count; ++d)
            {
                out_count *= idx_size[e.out_idx[d]];
            }
            REQUIRE(out.size() == out_count);
            crd::f64 ref[4096];
            REQUIRE(out_count <= 4096U);
            ref_einsum(e, idx_size, views, ref, out_count);
            for (crd::u64 o = 0; o < out_count; ++o)
            {
                INFO("mode " << static_cast<int>(mode) << " out " << o);
                CHECK(out.data()[o] == ref[o]);
            }
        }
    }
}

TEST_CASE("v14-f exec: diagonals + trace + ellipsis batch matmul", "[hesap][tensor][v14][einsumx]")
{
    crd::memory::TlsfAllocator alloc(1U << 22U);
    // "ii->" trace and "ii->i" diagonal (zero-copy stride-sum view)
    {
        const crd::u64 s[] = {5U, 5U};
        Tensor<crd::f64> a(&alloc, s);
        for (crd::u64 i = 0; i < 25U; ++i)
        {
            a.data()[i] = static_cast<crd::f64>(i);
        }
        crd::u64 idx_size[kEinsumMaxIndices];
        for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
        {
            idx_size[i] = 1;
        }
        idx_size['i' - 'a'] = 5U;
        const crd::u32 r1[] = {2U};
        EinsumExpr e;
        REQUIRE(einsum_parse("ii", {r1, 1U}, e) == TensorStatus::Ok);
        EinsumPlan p;
        REQUIRE(einsum_plan_build(e, idx_size, EinsumOptimize::Greedy, p) == TensorStatus::Ok);
        TensorView<const crd::f64> v[] = {a.view()};
        Tensor<crd::f64> out(&alloc);
        REQUIRE(einsum_execute<crd::f64>(p, {v, 1U}, out, &alloc) == TensorStatus::Ok);
        CHECK(out.data()[0] == 0.0 + 6.0 + 12.0 + 18.0 + 24.0); // trace = 60

        REQUIRE(einsum_parse("ii->i", {r1, 1U}, e) == TensorStatus::Ok);
        REQUIRE(einsum_plan_build(e, idx_size, EinsumOptimize::Greedy, p) == TensorStatus::Ok);
        REQUIRE(einsum_execute<crd::f64>(p, {v, 1U}, out, &alloc) == TensorStatus::Ok);
        for (crd::u64 i = 0; i < 5U; ++i)
        {
            CHECK(out.data()[i] == static_cast<crd::f64>(i * 6U));
        }
    }
    // "...ij,...jk->...ik" rank-3 batch matmul vs per-batch reference
    {
        const crd::u64 sa[] = {3U, 4U, 5U};
        const crd::u64 sb[] = {3U, 5U, 2U};
        Tensor<crd::f64> a(&alloc, sa);
        Tensor<crd::f64> b(&alloc, sb);
        for (crd::u64 i = 0; i < a.size(); ++i)
        {
            a.data()[i] = static_cast<crd::f64>((i * 5U) % 11U) - 5.0;
        }
        for (crd::u64 i = 0; i < b.size(); ++i)
        {
            b.data()[i] = static_cast<crd::f64>((i * 3U) % 13U) - 6.0;
        }
        const crd::u32 r[] = {3U, 3U};
        EinsumExpr e;
        REQUIRE(einsum_parse("...ij,...jk->...ik", {r, 2U}, e) == TensorStatus::Ok);
        crd::u64 idx_size[kEinsumMaxIndices];
        for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
        {
            idx_size[i] = 1;
        }
        idx_size['i' - 'a'] = 4U;
        idx_size['j' - 'a'] = 5U;
        idx_size['k' - 'a'] = 2U;
        idx_size[kEinsumEllipsisBase + kMaxRank - 1U] = 3U; // the single batch dim
        EinsumPlan p;
        REQUIRE(einsum_plan_build(e, idx_size, EinsumOptimize::Greedy, p) == TensorStatus::Ok);
        TensorView<const crd::f64> v[] = {a.view(), b.view()};
        Tensor<crd::f64> out(&alloc);
        REQUIRE(einsum_execute<crd::f64>(p, {v, 2U}, out, &alloc) == TensorStatus::Ok);
        REQUIRE(out.size() == 3U * 4U * 2U);
        for (crd::u64 bt = 0; bt < 3U; ++bt)
        {
            for (crd::u64 i = 0; i < 4U; ++i)
            {
                for (crd::u64 k = 0; k < 2U; ++k)
                {
                    crd::f64 acc = 0.0;
                    for (crd::u64 j = 0; j < 5U; ++j)
                    {
                        acc += a.data()[bt * 20U + i * 5U + j] * b.data()[bt * 10U + j * 2U + k];
                    }
                    CHECK(out.data()[bt * 8U + i * 2U + k] == acc);
                }
            }
        }
    }
}

TEST_CASE("v14-f exec: run-twice + the {1,2,4,8,16} moat", "[hesap][tensor][v14][einsumx][moat]")
{
    crd::memory::TlsfAllocator alloc(1ULL << 28U);
    // big enough that gemm_parallel engages: ab,bc,cd->ad at 192^4-ish
    const crd::u64 sza = 192U;
    crd::u64 idx_size[kEinsumMaxIndices];
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i)
    {
        idx_size[i] = 1;
    }
    idx_size[0] = sza;
    idx_size[1] = sza;
    idx_size[2] = sza;
    idx_size[3] = sza;
    const crd::u32 r[] = {2U, 2U, 2U};
    EinsumExpr e;
    REQUIRE(einsum_parse("ab,bc,cd->ad", {r, 3U}, e) == TensorStatus::Ok);
    EinsumPlan p;
    REQUIRE(einsum_plan_build(e, idx_size, EinsumOptimize::Optimal, p) == TensorStatus::Ok);

    const crd::u64 ms[] = {sza, sza};
    Tensor<crd::f64> a(&alloc, ms);
    Tensor<crd::f64> b(&alloc, ms);
    Tensor<crd::f64> c(&alloc, ms);
    for (crd::u64 i = 0; i < a.size(); ++i)
    {
        a.data()[i] = static_cast<crd::f64>((i * 2654435761ULL) % 1000ULL) * 1e-3 - 0.5;
        b.data()[i] = static_cast<crd::f64>((i * 40503ULL) % 1000ULL) * 1e-3 - 0.5;
        c.data()[i] = static_cast<crd::f64>((i * 7919ULL) % 1000ULL) * 1e-3 - 0.5;
    }
    TensorView<const crd::f64> v[] = {a.view(), b.view(), c.view()};

    Tensor<crd::f64> serial(&alloc);
    REQUIRE(einsum_execute<crd::f64>(p, {v, 3U}, serial, &alloc) == TensorStatus::Ok);
    Tensor<crd::f64> again(&alloc);
    REQUIRE(einsum_execute<crd::f64>(p, {v, 3U}, again, &alloc) == TensorStatus::Ok);
    for (crd::u64 i = 0; i < serial.size(); i += 97U)
    {
        REQUIRE(std::bit_cast<crd::u64>(serial.data()[i]) == std::bit_cast<crd::u64>(again.data()[i]));
    }

    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        Tensor<crd::f64> par(&alloc);
        const TensorStatus st = einsum_execute<crd::f64>(p, {v, 3U}, par, &alloc);
        crd::jobs::shutdown();
        REQUIRE(st == TensorStatus::Ok);
        INFO("workers " << nw);
        bool same = true;
        for (crd::u64 i = 0; i < serial.size(); ++i)
        {
            same = same && std::bit_cast<crd::u64>(par.data()[i]) == std::bit_cast<crd::u64>(serial.data()[i]);
        }
        CHECK(same);
    }
}
