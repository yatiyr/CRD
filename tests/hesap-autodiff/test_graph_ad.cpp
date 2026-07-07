// test_graph_ad.cpp — Phase 3.1.6 v16-h: structural graph AD + codegen. The graph (traced from a scalar-generic
// functor, symbolically reverse-differentiated, then const-fold/CSE/DCE'd) must: (1) evaluate the forward value
// BIT-IDENTICALLY to a direct f64 evaluation; (2) produce a gradient matching the reverse tape (to fp tolerance) and
// central FD; (3) have optimize() strictly REDUCE the node count (CSE/DCE); (4) emit well-formed C++ codegen.

#include <crd/hesap/autodiff/graph_ad.hpp>
#include <crd/hesap/autodiff/tape.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>

namespace ga  = crd::hesap::autodiff::graph;
namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;
using Catch::Matchers::WithinAbs;

namespace
{
// scalar-generic functor with a REPEATED subexpression (x0+x1) — exercises CSE. Works on GExpr / Var / f64.
struct Fn
{
    template <class T>
    T operator()(const T* x, int) const
    {
        using crd::math::exp;
        using crd::math::sin;
        const T s = x[0] + x[1];
        return exp(x[0]) + sin(x[0] * x[1]) + x[1] * x[1] + s * s + (x[0] + x[1]);
    }
};
} // namespace

TEST_CASE("v16-h: graph AD forward == direct f64, grad == tape == FD; optimize reduces; codegen emits",
          "[autodiff][graph]")
{
    constexpr int              n = 2;
    crd::memory::TlsfAllocator alloc(16 << 20);
    f64                        x[n] = {0.7, -0.4};

    // ---- trace the functor into a graph, symbolically reverse-AD, then optimise ----
    ga::Graph g(&alloc);
    ga::GExpr gx[n];
    for (int i = 0; i < n; ++i) { gx[i] = ga::gexpr_input(g); }
    const ga::GExpr gy  = Fn{}(gx, n);
    int             out = gy.id;
    int             input_nodes[n];
    int             grad_nodes[n];
    for (int i = 0; i < n; ++i) { input_nodes[i] = gx[i].id; }
    g.reverse_ad(out, input_nodes, n, grad_nodes);

    int roots[n + 1];
    roots[0] = out;
    for (int i = 0; i < n; ++i) { roots[i + 1] = grad_nodes[i]; }
    const int before = g.size();
    g.optimize(roots, n + 1);
    const int after = g.size();
    out = roots[0];
    for (int i = 0; i < n; ++i) { grad_nodes[i] = roots[i + 1]; }
    CHECK(after < before); // CSE + DCE strictly shrank the graph

    // ---- interpret the optimised graph ----
    crd::containers::Array<f64> vals(&alloc);
    vals.resize(static_cast<crd::usize>(g.size()));
    g.eval(x, vals.data());
    const f64 gval = vals[static_cast<crd::usize>(out)];
    f64       ggrad[n];
    for (int i = 0; i < n; ++i) { ggrad[i] = vals[static_cast<crd::usize>(grad_nodes[i])]; }

    // (1) forward BIT-IDENTICAL to a direct f64 evaluation
    CHECK(gval == Fn{}(x, n));

    // (2) gradient == the reverse tape
    rev::Tape tape(&alloc);
    rev::Var  vx[n];
    for (int i = 0; i < n; ++i) { vx[i] = rev::make_leaf(tape, x[i]); }
    const rev::Var vy = Fn{}(vx, n);
    tape.seed(vy.node, 1.0);
    tape.backward();
    for (int i = 0; i < n; ++i) { CHECK_THAT(ggrad[i], WithinAbs(tape.grad(vx[i].node), 1e-11)); }

    // (2b) gradient == central FD
    const f64 hh = 1e-6;
    for (int i = 0; i < n; ++i)
    {
        f64 xp[n] = {x[0], x[1]};
        xp[i]     = x[i] + hh;
        const f64 fp = Fn{}(xp, n);
        xp[i]        = x[i] - hh;
        CHECK_THAT(ggrad[i], WithinAbs((fp - Fn{}(xp, n)) / (2.0 * hh), 1e-6));
    }

    // (3) codegen emits a well-formed kernel
    char      buf[8192];
    const int len = ga::emit_cpp(g, &out, 1, grad_nodes, n, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(len < static_cast<int>(sizeof(buf)));
    CHECK(std::strstr(buf, "crd_codegen_kernel") != nullptr);
    CHECK(std::strstr(buf, "grad[1] =") != nullptr);
}

TEST_CASE("v16-h: optimize is semantics-preserving + const-folds a constant subgraph", "[autodiff][graph]")
{
    crd::memory::TlsfAllocator alloc(8 << 20);
    ga::Graph                  g(&alloc);
    // f(x) = (2.0*3.0) + sin(x)  — the 2*3 must const-fold to 6.0
    const ga::GExpr x   = ga::gexpr_input(g);
    const ga::GExpr two = {&g, g.constant(2.0)};
    const ga::GExpr thr = {&g, g.constant(3.0)};
    const ga::GExpr y   = (two * thr) + ga::sin(x);
    int             out = y.id;
    g.optimize(&out, 1);

    // the folded graph has NO Mul node left (2*3 became a Const)
    bool has_mul = false;
    for (int i = 0; i < g.size(); ++i) { if (g.node(i).op == ga::GOp::Mul) { has_mul = true; } }
    CHECK(!has_mul);

    crd::containers::Array<f64> vals(&alloc);
    vals.resize(static_cast<crd::usize>(g.size()));
    const f64 xv = 0.5;
    g.eval(&xv, vals.data());
    CHECK_THAT(vals[static_cast<crd::usize>(out)], WithinAbs(6.0 + std::sin(0.5), 1e-12));
}
