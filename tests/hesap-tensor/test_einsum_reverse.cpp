// v16-c gates: the einsum VJP, executed on the REAL v14 EinsumPlan (reuse > reimplement, SANITY #8).
//   The gradient of einsum(spec, A_0..) wrt operand k is itself an einsum with a permuted spec (output = operand k's
//   subscripts; inputs = the other operands + ȳ, plus a ones operand for a summed-out private index). Gate: central-FD
//   gradcheck of every operand's VJP across matmul / batched matmul / a 3-operand chain / a reduction (the ones-
//   broadcast path) / A·Bᵀ; run-to-run bit-determinism; and equivalence to the dense nn::matmul_vjp.

#include <crd/hesap/autodiff/einsum_reverse.hpp>
#include <crd/hesap/autodiff/nn_reverse.hpp>
#include <crd/hesap/tensor/einsum_exec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace crd::hesap::tensor;
namespace ad = crd::hesap::autodiff::reverse;
namespace nn = crd::hesap::autodiff::reverse::nn;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
crd::u32 parse_ranks(const char* expr, crd::u32* ranks) noexcept
{
    crd::u32 n_ops = 0;
    crd::u32 r = 0;
    for (const char* p = expr; *p != '\0' && *p != '-'; ++p)
    {
        if (*p == ',') { ranks[n_ops++] = r; r = 0; }
        else { ++r; }
    }
    ranks[n_ops++] = r;
    return n_ops;
}

void build_idx_size(const char* names, const crd::u64* sizes, crd::u64* idx_size) noexcept
{
    for (crd::u32 i = 0; i < kEinsumMaxIndices; ++i) { idx_size[i] = 1; }
    crd::u32 n = 0;
    for (const char* p = names; *p != '\0'; ++p, ++n) { idx_size[static_cast<crd::u32>(*p - 'a')] = sizes[n]; }
}

TensorStatus forward_einsum(const EinsumExpr& e, const crd::u64* idx_size, const TensorView<const f64>* views,
                            crd::u32 n_ops, Tensor<f64>& out, crd::memory::IAllocator* alloc) noexcept
{
    EinsumPlan         plan;
    const TensorStatus st = einsum_plan_build(e, idx_size, EinsumOptimize::Optimal, plan);
    if (st != TensorStatus::Ok) { return st; }
    return einsum_execute<f64>(plan, {views, n_ops}, out, alloc);
}

struct VjpCase
{
    const char* expr;
    const char* names;
    crd::u64    sizes[8];
};
} // namespace

TEST_CASE("v16-c: einsum VJP over the real EinsumPlan == central FD, and is deterministic",
          "[hesap][tensor][autodiff][einsum-vjp]")
{
    crd::memory::TlsfAllocator alloc(1U << 24U);
    const VjpCase              cases[] = {
        {"ik,kj->ij", "ijk", {3, 2, 4}},        // matmul
        {"bik,bkj->bij", "bijk", {2, 3, 2, 4}}, // batched matmul
        {"ij,jk,kl->il", "ijkl", {3, 4, 2, 3}}, // 3-operand chain
        {"ij->i", "ij", {3, 4}},                // reduction — exercises the ones-broadcast operand
        {"ij,kj->ik", "ijk", {3, 2, 4}},        // A·Bᵀ (contract the trailing index)
    };
    for (const VjpCase& c : cases)
    {
        INFO(c.expr);
        crd::u32       ranks[kEinsumMaxOperands];
        const crd::u32 n_ops = parse_ranks(c.expr, ranks);
        crd::u64       idx_size[kEinsumMaxIndices];
        build_idx_size(c.names, c.sizes, idx_size);
        EinsumExpr e;
        REQUIRE(einsum_parse(c.expr, {ranks, n_ops}, e) == TensorStatus::Ok);

        Tensor<f64>           ops[kEinsumMaxOperands];
        TensorView<const f64> views[kEinsumMaxOperands];
        for (crd::u32 t = 0; t < n_ops; ++t)
        {
            crd::u64 shape[kMaxRank];
            for (crd::u32 d = 0; d < e.term[t].count; ++d) { shape[d] = idx_size[e.term[t].idx[d]]; }
            ops[t] = Tensor<f64>(&alloc, {shape, e.term[t].count});
            for (crd::u64 i = 0; i < ops[t].size(); ++i)
            {
                ops[t].data()[i] = 0.4 * std::sin(0.7 + 1.1 * static_cast<f64>(i) + 3.0 * static_cast<f64>(t));
            }
            views[t] = ops[t].view();
        }

        // output cotangent ȳ (same shape/order as the forward output)
        crd::u64 out_shape[kMaxRank];
        for (crd::u32 d = 0; d < e.out_count; ++d) { out_shape[d] = idx_size[e.out_idx[d]]; }
        Tensor<f64> cten(&alloc, {out_shape, e.out_count});
        for (crd::u64 i = 0; i < cten.size(); ++i) { cten.data()[i] = 0.3 * std::cos(0.2 + 0.9 * static_cast<f64>(i)); }
        const TensorView<const f64> cview = cten.view();

        // scalar loss = Σ ȳ·Y (recomputes the forward from the current operand buffers)
        auto loss = [&]() -> f64
        {
            Tensor<f64> yl(&alloc);
            REQUIRE(forward_einsum(e, idx_size, views, n_ops, yl, &alloc) == TensorStatus::Ok);
            f64 s = 0.0;
            for (crd::u64 i = 0; i < yl.size(); ++i) { s += cten.data()[i] * yl.data()[i]; }
            return s;
        };

        for (crd::u32 k = 0; k < n_ops; ++k)
        {
            INFO("operand " << k);
            Tensor<f64> g(&alloc);
            Tensor<f64> g2(&alloc);
            REQUIRE(ad::einsum_vjp<f64>(e, idx_size, {views, n_ops}, cview, k, g, &alloc) == TensorStatus::Ok);
            REQUIRE(ad::einsum_vjp<f64>(e, idx_size, {views, n_ops}, cview, k, g2, &alloc) == TensorStatus::Ok);
            REQUIRE(g.size() == ops[k].size());
            const f64 h = 1e-6;
            for (crd::u64 i = 0; i < ops[k].size(); ++i)
            {
                CHECK(g.data()[i] == g2.data()[i]); // deterministic — bit-identical run to run
                const f64 saved  = ops[k].data()[i];
                ops[k].data()[i] = saved + h;
                const f64 fp     = loss();
                ops[k].data()[i] = saved - h;
                const f64 fm     = loss();
                ops[k].data()[i] = saved;
                CHECK_THAT(g.data()[i], WithinAbs((fp - fm) / (2.0 * h), 1e-7));
            }
        }
    }
}

TEST_CASE("v16-c: einsum matmul VJP == the dense nn::matmul_vjp", "[hesap][tensor][autodiff][einsum-vjp]")
{
    crd::memory::TlsfAllocator alloc(1U << 20U);
    constexpr int              m = 3;
    constexpr int              kk = 4;
    constexpr int              p = 2;
    // A[m×kk] "ik", B[kk×p] "kj", Y[m×p] "ij"
    const crd::u64 as[] = {static_cast<crd::u64>(m), static_cast<crd::u64>(kk)};
    const crd::u64 bs[] = {static_cast<crd::u64>(kk), static_cast<crd::u64>(p)};
    Tensor<f64>    a(&alloc, as);
    Tensor<f64>    b(&alloc, bs);
    for (int i = 0; i < m * kk; ++i) { a.data()[i] = 0.2 + 0.1 * i; }
    for (int i = 0; i < kk * p; ++i) { b.data()[i] = -0.3 + 0.15 * i; }

    crd::u64       idx_size[kEinsumMaxIndices];
    const crd::u64 sizes_ijk[] = {static_cast<crd::u64>(m), static_cast<crd::u64>(p), static_cast<crd::u64>(kk)};
    build_idx_size("ijk", sizes_ijk, idx_size);
    crd::u32       ranks[kEinsumMaxOperands];
    const crd::u32 n_ops = parse_ranks("ik,kj->ij", ranks);
    EinsumExpr     e;
    REQUIRE(einsum_parse("ik,kj->ij", {ranks, n_ops}, e) == TensorStatus::Ok);

    const TensorView<const f64> views[2] = {a.view(), b.view()};
    const crd::u64              cs[]     = {static_cast<crd::u64>(m), static_cast<crd::u64>(p)};
    Tensor<f64>                 gc(&alloc, cs);
    for (int i = 0; i < m * p; ++i) { gc.data()[i] = 0.5 - 0.2 * i; }

    Tensor<f64> g_a(&alloc);
    Tensor<f64> g_b(&alloc);
    REQUIRE(ad::einsum_vjp<f64>(e, idx_size, {views, 2}, gc.view(), 0, g_a, &alloc) == TensorStatus::Ok);
    REQUIRE(ad::einsum_vjp<f64>(e, idx_size, {views, 2}, gc.view(), 1, g_b, &alloc) == TensorStatus::Ok);

    f64 g_a_dense[m * kk];
    f64 g_b_dense[kk * p];
    nn::matmul_vjp(a.data(), b.data(), gc.data(), g_a_dense, g_b_dense, m, kk, p);

    REQUIRE(g_a.size() == static_cast<crd::usize>(m * kk));
    REQUIRE(g_b.size() == static_cast<crd::usize>(kk * p));
    for (int i = 0; i < m * kk; ++i) { CHECK_THAT(g_a.data()[i], WithinAbs(g_a_dense[i], 1e-11)); }
    for (int i = 0; i < kk * p; ++i) { CHECK_THAT(g_b.data()[i], WithinAbs(g_b_dense[i], 1e-11)); }
}
