// test_ckir_grad.cpp — Phase 3.1.6 v17-a: symbolic reverse-AD on CKIR-Graph, gated against central finite differences
// (the v16 methodology). The gradient graph is itself CKIR, evaluated on the CPU reference — proving that a kernel
// authored once in CKIR is differentiable (and later runs that gradient on every backend). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_grad.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace kir = crd::kir;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
// evaluate a scalar-output graph L at the given inputs.
f64 eval_scalar(const kir::KGraph& g, const f64* const* inputs, crd::memory::IAllocator* a, int out)
{
    f64 v = 0.0;
    kir::eval_cpu(g, inputs, a, out, &v);
    return v;
}
} // namespace

TEST_CASE("v17-a: CKIR reverse-AD of L=sum(tanh(X@W)) matches central FD", "[kir][grad]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KGraph                g(&alloc);
    const int x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
    const int mm  = g.contract(x, w);                        // [2,2]
    const int th  = g.unary(kir::KOp::Tanh, mm);
    const int loss = g.reduce(kir::KOp::ReduceSum, th, 0x3U); // [1,1]

    const int seed = g.constant(1.0, g.node(loss).shape, kir::DType::F64);
    int       grad_in[2];
    kir::reverse_ad(g, loss, seed, &alloc, grad_in, 2);
    REQUIRE(grad_in[0] >= 0);
    REQUIRE(grad_in[1] >= 0);

    f64              xv[6]     = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1};
    f64              wv[6]     = {0.4, -0.6, 0.9, 0.1, -0.5, 0.7};
    const f64* const inputs[] = {xv, wv};

    f64 gx[6];
    f64 gw[6];
    kir::eval_cpu(g, inputs, &alloc, grad_in[0], gx);
    kir::eval_cpu(g, inputs, &alloc, grad_in[1], gw);

    // central FD on X and W
    const f64 h = 1e-6;
    for (int m = 0; m < 6; ++m)
    {
        const f64 sav = xv[m];
        xv[m]         = sav + h;
        const f64 lp  = eval_scalar(g, inputs, &alloc, loss);
        xv[m]         = sav - h;
        const f64 lm  = eval_scalar(g, inputs, &alloc, loss);
        xv[m]         = sav;
        CHECK_THAT(gx[m], WithinAbs((lp - lm) / (2.0 * h), 1e-6));
    }
    for (int m = 0; m < 6; ++m)
    {
        const f64 sav = wv[m];
        wv[m]         = sav + h;
        const f64 lp  = eval_scalar(g, inputs, &alloc, loss);
        wv[m]         = sav - h;
        const f64 lm  = eval_scalar(g, inputs, &alloc, loss);
        wv[m]         = sav;
        CHECK_THAT(gw[m], WithinAbs((lp - lm) / (2.0 * h), 1e-6));
    }
}

TEST_CASE("v17-a: CKIR reverse-AD through broadcast + elementwise (bias/relu-free smooth)", "[kir][grad]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);
    kir::KGraph                g(&alloc);
    // L = sum( exp(X + bias_broadcast) ) ; X[2,3], bias[1,3] broadcast to [2,3]
    const int x  = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int bb = g.input(kir::make_shape({1, 3}), kir::DType::F64);
    const int bc = g.broadcast(bb, kir::make_shape({2, 3}));
    const int s  = g.binary(kir::KOp::Add, x, bc);
    const int e  = g.unary(kir::KOp::Exp, s);
    const int loss = g.reduce(kir::KOp::ReduceSum, e, 0x3U);

    const int seed = g.constant(1.0, g.node(loss).shape, kir::DType::F64);
    int       grad_in[2];
    kir::reverse_ad(g, loss, seed, &alloc, grad_in, 2);

    f64              xv[6]     = {0.1, 0.2, -0.3, 0.4, -0.5, 0.15};
    f64              bv[3]     = {0.2, -0.1, 0.3};
    const f64* const inputs[] = {xv, bv};
    f64              gx[6];
    f64              gb[3];
    kir::eval_cpu(g, inputs, &alloc, grad_in[0], gx);
    kir::eval_cpu(g, inputs, &alloc, grad_in[1], gb); // the broadcast VJP = reduce back to [1,3]

    const f64 h = 1e-6;
    for (int m = 0; m < 6; ++m)
    {
        const f64 sav = xv[m];
        xv[m] = sav + h; const f64 lp = eval_scalar(g, inputs, &alloc, loss);
        xv[m] = sav - h; const f64 lm = eval_scalar(g, inputs, &alloc, loss);
        xv[m] = sav;
        CHECK_THAT(gx[m], WithinAbs((lp - lm) / (2.0 * h), 1e-6));
    }
    for (int m = 0; m < 3; ++m) // bias gradient = column sums (the broadcast reduce)
    {
        const f64 sav = bv[m];
        bv[m] = sav + h; const f64 lp = eval_scalar(g, inputs, &alloc, loss);
        bv[m] = sav - h; const f64 lm = eval_scalar(g, inputs, &alloc, loss);
        bv[m] = sav;
        CHECK_THAT(gb[m], WithinAbs((lp - lm) / (2.0 * h), 1e-6));
    }
}
