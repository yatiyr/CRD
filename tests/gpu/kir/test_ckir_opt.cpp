// test_ckir_opt.cpp — Phase 3.1.6 v17-a: the CKIR optimization passes (const-fold -> DCE -> CSE hash-cons) + the
// oracle-harness primitives. Gates: passes are SEMANTICS-PRESERVING (eval bit-identical before/after) AND shrink the
// node count — including on a real reverse-AD gradient graph (which the passes clean substantially). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_grad.hpp>
#include <crd/kir/ckir_harness.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;
using crd::f64;

TEST_CASE("v17-a: CKIR optimize preserves semantics + shrinks (CSE + const-fold + DCE)", "[kir][opt]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({4});
    const int                  x  = g.input(sh, kir::DType::F64);
    const int a  = g.binary(kir::KOp::Mul, x, x);           // X*X
    const int b  = g.binary(kir::KOp::Mul, x, x);           // X*X again  -> CSE should merge a,b
    const int s  = g.binary(kir::KOp::Add, a, b);
    const int c2 = g.constant(2.0, sh, kir::DType::F64);
    const int c3 = g.constant(3.0, sh, kir::DType::F64);
    const int cc = g.binary(kir::KOp::Add, c2, c3);         // -> const-fold to 5
    const int sc = g.binary(kir::KOp::Mul, s, cc);
    const int dead = g.unary(kir::KOp::Exp, x);             // unused -> DCE removes
    (void)dead;
    const int loss = g.reduce(kir::KOp::ReduceSum, sc, 0x1U);

    const f64        xin[4]   = {1.0, 2.0, -0.5, 3.0};
    const f64* const inputs[] = {xin};
    f64              before[1];
    kir::eval_cpu(g, inputs, &alloc, loss, before);
    const int size_before = g.size();

    int roots[1] = {loss};
    g.optimize(roots, 1);
    const int size_after = g.size();
    f64       after[1];
    kir::eval_cpu(g, inputs, &alloc, roots[0], after);

    CHECK(kir::bit_equal(before, after, 1)); // exact — the passes never change the result
    CHECK(size_after < size_before);          // CSE merged X*X, folded 2+3, dropped the dead exp

    // idempotent: a second optimize changes nothing
    int roots2[1] = {roots[0]};
    g.optimize(roots2, 1);
    f64 after2[1];
    kir::eval_cpu(g, inputs, &alloc, roots2[0], after2);
    CHECK(kir::bit_equal(after, after2, 1));
    CHECK(g.size() == size_after);
}

TEST_CASE("v17-a: CKIR optimize on a reverse-AD graph -- gradient BIT-IDENTICAL, graph shrinks", "[kir][opt]")
{
    crd::memory::TlsfAllocator alloc(64 << 20);
    kir::KGraph                g(&alloc);
    const int x  = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int w  = g.input(kir::make_shape({3, 2}), kir::DType::F64);
    const int mm = g.contract(x, w);
    const int th = g.unary(kir::KOp::Tanh, mm);
    const int loss = g.reduce(kir::KOp::ReduceSum, th, 0x3U);
    const int seed = g.constant(1.0, g.node(loss).shape, kir::DType::F64);
    int       gin[2];
    kir::reverse_ad(g, loss, seed, &alloc, gin, 2);

    f64              xv[6]     = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1};
    f64              wv[6]     = {0.4, -0.6, 0.9, 0.1, -0.5, 0.7};
    const f64* const inputs[] = {xv, wv};
    f64              gx_before[6];
    f64              gw_before[6];
    kir::eval_cpu(g, inputs, &alloc, gin[0], gx_before);
    kir::eval_cpu(g, inputs, &alloc, gin[1], gw_before);
    const int size_before = g.size();

    int roots[2] = {gin[0], gin[1]};
    g.optimize(roots, 2);
    const int size_after = g.size();
    f64       gx_after[6];
    f64       gw_after[6];
    kir::eval_cpu(g, inputs, &alloc, roots[0], gx_after);
    kir::eval_cpu(g, inputs, &alloc, roots[1], gw_after);

    CHECK(kir::bit_equal(gx_before, gx_after, 6)); // the optimized gradient is EXACTLY the same
    CHECK(kir::bit_equal(gw_before, gw_after, 6));
    CHECK(size_after < size_before);               // the reverse-AD graph had redundant consts + subexprs
}

// A mat4's 4th column lives in KNode::d — the operand the DCE walk honours and the CSE key hashes. This drives the
// RENUMBERING path: dead nodes ahead of the columns force every later id to shift, so an operand left un-remapped
// becomes a forward/out-of-range reference. A graph without dead nodes never renumbers and therefore cannot catch a
// missing remap, which is why the whole vec/mat suite sails over it (SANITY #3 — boundary adversaries, not volume).
TEST_CASE("v17-a: CKIR optimize renumbers the 4th operand (mat4 column) under a DCE id-shift", "[kir][opt]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({2});
    const int                  x  = g.input(sh, kir::DType::F64);

    (void)g.unary(kir::KOp::Exp, x); // dead nodes AHEAD of the columns: DCE drops them, every later id shifts down
    (void)g.unary(kir::KOp::Sin, x);
    (void)g.unary(kir::KOp::Cos, x);

    const int z   = g.constant(0.0, sh, kir::DType::F64);
    const int c0  = g.vec4(x, z, z, z); // four DISTINCT columns, so CSE cannot merge them
    const int c1  = g.vec4(z, x, z, z);
    const int c2  = g.vec4(z, z, x, z);
    const int c3  = g.vec4(z, z, z, x); // -> the mat4's KNode::d
    const int m   = g.mat4(c0, c1, c2, c3);
    const int det = g.determinant(m);    // diag(x,x,x,x) -> x^4

    const f64        xin[2]   = {2.0, 3.0};
    const f64* const inputs[] = {xin};
    f64              before[2];
    kir::eval_cpu(g, inputs, &alloc, det, before);
    CHECK(before[0] == 16.0); // 2^4 — the graph really is the diagonal mat4 we think it is
    CHECK(before[1] == 81.0); // 3^4

    int roots[1] = {det};
    g.optimize(roots, 1);

    // Structural gate FIRST, and REQUIRE not CHECK: a stale operand indexes past the compacted offset table, so
    // evaluating the graph would read out of bounds rather than fail an assertion.
    REQUIRE(g.operands_valid());

    f64 after[2];
    kir::eval_cpu(g, inputs, &alloc, roots[0], after);
    CHECK(kir::bit_equal(before, after, 2)); // the passes never change the result
}

TEST_CASE("v17-a: oracle-harness bit/ulp primitives", "[kir][harness]")
{
    const f64 a[3] = {1.0, 2.0, 3.0};
    f64       b[3] = {1.0, 2.0, 3.0};
    CHECK(kir::bit_equal(a, b, 3));
    CHECK(kir::max_ulp(a, b, 3) == 0);

    b[1] = std::nextafter(2.0, 3.0); // 1 ULP up
    CHECK_FALSE(kir::bit_equal(a, b, 3));
    CHECK(kir::ulp_distance(2.0, b[1]) == 1);
    CHECK(kir::max_ulp(a, b, 3) == 1);
    CHECK(kir::within_ulp(a, b, 3, 1));
    CHECK_FALSE(kir::within_ulp(a, b, 3, 0));
    CHECK(kir::max_abs_diff(a, b, 3) > 0.0);
    CHECK(kir::max_abs_diff(a, b, 3) < 1e-15);
}
