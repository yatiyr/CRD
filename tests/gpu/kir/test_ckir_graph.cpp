// test_ckir_graph.cpp — Phase 3.1.6 v17-a: the CKIR-Graph IR + CPU reference interpreter gate. Builds tensor graphs
// (elementwise / reduce / contract / movement), evaluates them on the CPU reference, and checks against hand-computed
// answers + crd::math oracles. Also gates the two invariants v17 rests on: the reference is DETERMINISTIC (bit-
// identical run-to-run) and dtype-faithful (an F32 node rounds to f32). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>

#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace kir = crd::kir;
using crd::f64;
using Catch::Matchers::WithinRel;

TEST_CASE("v17-a: CKIR elementwise + reduce matches crd::math oracle", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({4});
    const int                  x  = g.input(sh, kir::DType::F64);
    // y = sum( exp(x) + x*x )  over axis 0
    const int ex  = g.unary(kir::KOp::Exp, x);
    const int xx  = g.binary(kir::KOp::Mul, x, x);
    const int sum = g.binary(kir::KOp::Add, ex, xx);
    const int red = g.reduce(kir::KOp::ReduceSum, sum, 0x1U); // shape [1]

    const f64        xin[4]   = {0.5, -1.0, 2.0, 0.25};
    const f64* const inputs[] = {xin};
    f64              out[1]    = {};
    kir::eval_cpu(g, inputs, &alloc, red, out);

    f64 expect = 0.0;
    for (const f64 v : xin) { expect += crd::math::exp(v) + v * v; }
    CHECK_THAT(out[0], WithinRel(expect, 1e-14));
}

TEST_CASE("v17-a: CKIR Contract is matmul (2x3 @ 3x2)", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    kir::KGraph                g(&alloc);
    const int a = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int b = g.input(kir::make_shape({3, 2}), kir::DType::F64);
    const int c = g.contract(a, b); // [2,2]

    const f64        av[6]    = {1, 2, 3, 4, 5, 6};       // [[1,2,3],[4,5,6]]
    const f64        bv[6]    = {1, 0, 0, 1, 1, 1};       // [[1,0],[0,1],[1,1]]
    const f64* const inputs[] = {av, bv};
    f64              out[4]    = {};
    kir::eval_cpu(g, inputs, &alloc, c, out);
    // A@B = [[4,5],[10,11]]
    CHECK(out[0] == 4.0);
    CHECK(out[1] == 5.0);
    CHECK(out[2] == 10.0);
    CHECK(out[3] == 11.0);
}

TEST_CASE("v17-a: CKIR movement -- permute + broadcast + reshape", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    kir::KGraph                g(&alloc);
    const int     a  = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const crd::u8 pr[2] = {1, 0};
    const int     tp = g.permute(a, pr); // [3,2]

    const f64        av[6]    = {1, 2, 3, 4, 5, 6};
    const f64* const inputs[] = {av};
    f64              out[6]    = {};
    kir::eval_cpu(g, inputs, &alloc, tp, out);
    // transpose of [[1,2,3],[4,5,6]] = [[1,4],[2,5],[3,6]]
    const f64 expect[6] = {1, 4, 2, 5, 3, 6};
    for (int i = 0; i < 6; ++i) { CHECK(out[i] == expect[i]); }

    // broadcast a [1,3] row to [2,3]
    kir::KGraph      g2(&alloc);
    const int        r  = g2.input(kir::make_shape({1, 3}), kir::DType::F64);
    const int        bc = g2.broadcast(r, kir::make_shape({2, 3}));
    const f64        rv[3]     = {7, 8, 9};
    const f64* const in2[]     = {rv};
    f64              out2[6]    = {};
    kir::eval_cpu(g2, in2, &alloc, bc, out2);
    const f64 expect2[6] = {7, 8, 9, 7, 8, 9};
    for (int i = 0; i < 6; ++i) { CHECK(out2[i] == expect2[i]); }
}

TEST_CASE("v17-a: CKIR composed graph -- matmul then bias then max(0,.) then sum", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const int x = g.input(kir::make_shape({2, 2}), kir::DType::F64);
    const int w = g.input(kir::make_shape({2, 2}), kir::DType::F64);
    const int mm  = g.contract(x, w);                          // [2,2]
    const int z   = g.constant(0.0, kir::make_shape({2, 2}), kir::DType::F64);
    const int rl  = g.binary(kir::KOp::Max, mm, z);            // relu
    const int red = g.reduce(kir::KOp::ReduceSum, rl, 0x3U);   // sum all -> [1,1]

    const f64        xv[4]    = {1, -2, 3, 4};
    const f64        wv[4]    = {1, 1, -1, 1};
    const f64* const inputs[] = {xv, wv};
    f64              out[1]    = {};
    kir::eval_cpu(g, inputs, &alloc, red, out);
    // xw = [[1*1+-2*-1, 1*1+-2*1],[3*1+4*-1, 3*1+4*1]] = [[3,-1],[-1,7]]; relu -> [[3,0],[0,7]]; sum = 10
    CHECK(out[0] == 10.0);
}

TEST_CASE("v17-a: CKIR CPU reference is DETERMINISTIC (bit-identical run-to-run)", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({6});
    const int                  x  = g.input(sh, kir::DType::F64);
    const int t1  = g.unary(kir::KOp::Tanh, x);
    const int t2  = g.binary(kir::KOp::Mul, t1, x);
    const int red = g.reduce(kir::KOp::ReduceSum, t2, 0x1U);

    const f64        xin[6]   = {0.1, 0.2, -0.3, 0.4, -0.5, 0.6};
    const f64* const inputs[] = {xin};
    f64              o1[1]     = {};
    f64              o2[1]     = {};
    kir::eval_cpu(g, inputs, &alloc, red, o1);
    kir::eval_cpu(g, inputs, &alloc, red, o2);
    CHECK(o1[0] == o2[0]); // EXACT — the determinism ground truth
}

TEST_CASE("v17-a: CKIR is dtype-faithful -- an F32 node rounds to f32", "[kir][ckir]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    kir::KGraph                g(&alloc);
    const int c = g.constant(0.1, kir::make_shape({1}), kir::DType::F32); // 0.1 not representable in f32
    f64       out[1] = {};
    kir::eval_cpu(g, nullptr, &alloc, c, out);
    CHECK(out[0] == static_cast<f64>(static_cast<float>(0.1)));
    CHECK(out[0] != 0.1); // proves the f32 rounding actually happened
}
