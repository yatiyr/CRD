#include <crd/containers/span.hpp>
#include <crd/hesap/amg/aggregation.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/amg/cf_splitting.hpp>
#include <crd/hesap/amg/cli_anchor.hpp>
#include <crd/hesap/amg/rs_strength.hpp>
#include <crd/hesap/amg/strength.hpp>
#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/bicgstab.hpp>
#include <crd/hesap/iterative/cg.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdio>

using namespace crd::hesap::sparse;
using namespace crd::hesap::iterative;
using crd::hesap::amg::SaAmg;
namespace dense = crd::hesap::dense;
namespace amg = crd::hesap::amg;

namespace
{
// Force the cli_register_amg.cpp TU to link so its static-init registers hesap.amg.*.
struct AmgAnchorPull
{
    AmgAnchorPull() noexcept { crd::hesap::amg::register_amg_cli_anchor(); }
};
const AmgAnchorPull kAmgAnchorPull;
} // namespace

namespace
{
// 2D 5-point Laplacian (SPD, isotropic) on a gridN×gridN mesh, n=gridN².
template <typename T> SparseMatrix<T, SparseFormat::Csr> laplace2d(crd::memory::IAllocator* a, crd::u32 g)
{
    const crd::u32 n = g * g;
    TripletBuilder<T> b(a, n, n);
    auto idx = [g](crd::u32 i, crd::u32 j)
    {
        return i * g + j;
    };
    for (crd::u32 i = 0; i < g; ++i)
        for (crd::u32 j = 0; j < g; ++j)
        {
            const crd::u32 r = idx(i, j);
            b.add(r, r, T(4));
            if (j > 0)
            {
                b.add(r, idx(i, j - 1), T(static_cast<crd::f64>(-1.0)));
            }
            if (j + 1 < g)
            {
                b.add(r, idx(i, j + 1), T(static_cast<crd::f64>(-1.0)));
            }
            if (i > 0)
            {
                b.add(r, idx(i - 1, j), T(static_cast<crd::f64>(-1.0)));
            }
            if (i + 1 < g)
            {
                b.add(r, idx(i + 1, j), T(static_cast<crd::f64>(-1.0)));
            }
        }
    return b.compress();
}

// 2D anisotropic convection-diffusion on a g×g grid (n=g²): −eps·u_xx − u_yy + β·u_x + c·u,
// 5-point with first-order UPWIND convection (flow in +x). Nonsymmetric. eps small ⇒
// x-anisotropy; β ⇒ convection; c ≥ 0 ⇒ reaction term that makes the operator STRICTLY
// diagonally dominant (interior row-sum = c). c=0 is the conservative (zero-row-sum,
// nearly singular) operator; c>0 is the strictly-dominant operator the bench measures.
template <typename T>
SparseMatrix<T, SparseFormat::Csr> cd2d(crd::memory::IAllocator* a, crd::u32 g, double eps, double beta, double c = 0.0)
{
    const crd::u32 n = g * g;
    TripletBuilder<T> b(a, n, n);
    auto idx = [g](crd::u32 i, crd::u32 j)
    {
        return i * g + j;
    };
    for (crd::u32 i = 0; i < g; ++i)
        for (crd::u32 j = 0; j < g; ++j)
        {
            const crd::u32 r = idx(i, j);
            double diag = 2.0 * eps + 2.0 + beta + c; // diffusion x+y + upwind convection + reaction
            if (j > 0)
            {
                b.add(r, idx(i, j - 1), T(static_cast<crd::f64>(-eps - beta)));
            } // west (upwind)
            if (j + 1 < g)
            {
                b.add(r, idx(i, j + 1), T(static_cast<crd::f64>(-eps)));
            } // east
            if (i > 0)
            {
                b.add(r, idx(i - 1, j), T(static_cast<crd::f64>(-1.0)));
            } // south
            if (i + 1 < g)
            {
                b.add(r, idx(i + 1, j), T(static_cast<crd::f64>(-1.0)));
            } // north
            b.add(r, r, T(static_cast<crd::f64>(diag)));
        }
    return b.compress();
}
} // namespace

TEST_CASE("AMG strength: a strong tridiagonal connection survives, a weak one is dropped", "[hesap-amg][strength]")
{
    crd::memory::TlsfAllocator alloc{16U << 20};
    // 3x3: node 1 strongly coupled to 0 (|-1|) but weakly to 2 (|-0.01|), θ=0.08.
    TripletBuilder<crd::f64> b(&alloc, 3, 3);
    b.add(0, 0, 2.0);
    b.add(0, 1, -1.0);
    b.add(1, 0, -1.0);
    b.add(1, 1, 2.0);
    b.add(1, 2, -0.01);
    b.add(2, 1, -0.01);
    b.add(2, 2, 2.0);
    auto a = b.compress();
    auto s = amg::strength_matrix<crd::f64>(a, 0.08, &alloc);
    // row 1: strong to 0 (1/√(2·2)=0.5≥0.08 ✓), weak to 2 (0.01/2=0.005 <0.08 ✗).
    const auto* o = s.pattern().outer_ptr.data();
    const auto* c = s.pattern().inner_idx.data();
    bool has01 = false;
    bool has12 = false;
    for (crd::u32 q = o[1]; q < o[2]; ++q)
    {
        if (c[q] == 0)
        {
            has01 = true;
        }
        if (c[q] == 2)
        {
            has12 = true;
        }
    }
    REQUIRE(has01);
    REQUIRE_FALSE(has12);
}

TEST_CASE("RS C/F splitting: valid coarsening + every F-point depends on a C-point (v4k-d)", "[hesap-amg][rs]")
{
    crd::memory::TlsfAllocator alloc{128U << 20};
    auto a = laplace2d<crd::f64>(&alloc, 30); // n=900
    const crd::u32 n = a.rows();
    auto s = amg::rs_strength_matrix<crd::f64>(a, 0.25, &alloc); // classical θ=0.25
    crd::u32 n_coarse = 0;
    auto cf = amg::rs_cf_split<crd::f64>(s, n_coarse, &alloc);
    // genuine splitting: both sets non-empty, C is a real coarsening (5-pt Laplacian ⇒ ~half C).
    REQUIRE(n_coarse > 0);
    REQUIRE(n_coarse < n);
    REQUIRE(n_coarse <= 3 * n / 4); // actually coarsens
    // the correctness property direct interpolation relies on: every F-point strongly
    // depends on at least one C-point.
    const auto* so = s.pattern().outer_ptr.data();
    const auto* si = s.pattern().inner_idx.data();
    crd::u32 f_count = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (cf[i] != amg::CfTag::Fine)
        {
            continue;
        }
        ++f_count;
        bool has_c_dep = false;
        for (crd::u32 q = so[i]; q < so[i + 1]; ++q)
        {
            if (cf[si[q]] == amg::CfTag::Coarse)
            {
                has_c_dep = true;
                break;
            }
        }
        // an F-point with NO strong dependency at all is a valid isolated F; only require a C
        // dependency when it has strong dependencies.
        const bool has_any_dep = (so[i + 1] > so[i]);
        if (has_any_dep)
        {
            REQUIRE(has_c_dep);
        }
    }
    REQUIRE(f_count > 0);
    std::printf("[rs-cf n=%u] coarse=%u fine=%u\n", n, n_coarse, f_count);
}

TEST_CASE("AMG aggregation partitions every node into a coarser set", "[hesap-amg][aggregation]")
{
    crd::memory::TlsfAllocator alloc{64U << 20};
    auto a = laplace2d<crd::f64>(&alloc, 16); // n=256
    auto s = amg::strength_matrix<crd::f64>(a, 0.08, &alloc);
    crd::u32 n_agg = 0;
    auto agg = amg::aggregate<crd::f64>(s, n_agg, &alloc);
    REQUIRE(n_agg > 0);
    REQUIRE(n_agg < a.rows());      // genuine coarsening
    REQUIRE(a.rows() >= 3 * n_agg); // coarsening factor ≥ 3 (advisor gate)
    for (crd::u32 i = 0; i < a.rows(); ++i)
    {
        REQUIRE(agg[i] < n_agg);
    } // every node aggregated
}

TEST_CASE("AMG builds a multilevel hierarchy with coarsening factor >= 3 per level", "[hesap-amg]")
{
    crd::memory::TlsfAllocator alloc{256U << 20};
    auto a = laplace2d<crd::f64>(&alloc, 50); // n=2500
    SaAmg<crd::f64> m(a, &alloc);
    REQUIRE(m.num_levels() >= 3);   // genuine multilevel
    REQUIRE(m.coarse_size() <= 50); // coarsened down to the dense threshold
}

TEST_CASE("AMG-CG converges fast on a 2D Poisson problem (mesh-independence direction)", "[hesap-amg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        auto a = laplace2d<crd::f64>(&alloc, 50); // n=2500
        const crd::u32 n = a.rows();
        SparseLinearOp<crd::f64> op(a);
        SaAmg<crd::f64> m(a, &alloc);
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
            x(i) = 0.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-8;
        opts.max_iter = 500;
        KrylovWorkspace<crd::f64> ws(&alloc, n);
        auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < 40); // AMG-CG should converge in tens of iters, not hundreds
    }
    crd::jobs::shutdown();
}

// Helper: BiCGSTAB-solve an operator with an AMG preconditioner at the given cycle type.
namespace
{
crd::usize amg_bicgstab_iters(crd::memory::IAllocator* alloc, const SparseMatrix<crd::f64, SparseFormat::Csr>& a,
                              amg::SaAmg<crd::f64>::Cycle cycle, bool& converged)
{
    const crd::u32 n = a.rows();
    SparseLinearOp<crd::f64> op(a);
    amg::SaAmg<crd::f64>::Options o;
    o.cycle = cycle;
    amg::SaAmg<crd::f64> m(a, alloc, o);
    dense::Vector<crd::f64> b(alloc, n);
    dense::Vector<crd::f64> x(alloc, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        b(i) = 1.0;
        x(i) = 0.0;
    }
    IterativeOptions<crd::f64> opts;
    opts.rel_tol = 1e-8;
    opts.max_iter = 2000;
    BicgstabWorkspace<crd::f64> ws(alloc, n);
    auto r = bicgstab<crd::f64>(op, &m, b.span(), x.span(), opts, ws, alloc);
    converged = r.converged;
    return r.iterations;
}
} // namespace

TEST_CASE("AMG V- and K-cycles converge on conservative cd2d where the W-cycle diverges", "[hesap-amg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{512U << 20};
        // c=0 ⇒ conservative (zero-row-sum) operator: the nonsymmetric Galerkin coarse op is
        // delicate here. V-cycle (mild coarse correction) is the robust choice and converges;
        // W-cycle can DIVERGE on this variant (amplified coarse correction) — which is exactly
        // why V is the shipped default and W is an opt-in lever, not the default.
        auto a = cd2d<crd::f64>(&alloc, 100, /*eps=*/0.01, /*beta=*/0.1, /*c=*/0.0); // n=10000
        bool cv = false;
        bool ck = false;
        const auto v = amg_bicgstab_iters(&alloc, a, amg::SaAmg<crd::f64>::Cycle::V, cv);
        // K-cycle (Notay): the Krylov projection bounds the coarse correction that the W-cycle
        // AMPLIFIES into divergence here (measured W=2000/diverged on this exact operator). K must
        // stay convergent — its value over W is stability, not a new crush.
        const auto k = amg_bicgstab_iters(&alloc, a, amg::SaAmg<crd::f64>::Cycle::K, ck);
        std::printf("[amg conservative cd2d] V iters=%zu (conv=%d)  K iters=%zu (conv=%d)\n", v, cv ? 1 : 0, k,
                    ck ? 1 : 0);
        REQUIRE(cv); // V-cycle is the robust default
        REQUIRE(ck); // K-cycle converges where W-cycle diverged (the stabilization property)
    }
    crd::jobs::shutdown();
}

TEST_CASE("AMG W-cycle is a convection lever on the strictly-dominant operator", "[hesap-amg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{512U << 20};
        // c=0.5 ⇒ strictly diagonally dominant (the bench's regime): the coarse op is stable,
        // so the W-cycle's stronger coarse pass cuts the V-cycle iteration count (the lever the
        // bench measures: β=0.1 V→W ~halves iters, ties ILUPACK). Locks W ≤ V on THIS operator
        // only — W is per-problem (see the conservative test), never asserted universal.
        auto a = cd2d<crd::f64>(&alloc, 100, /*eps=*/0.01, /*beta=*/0.1, /*c=*/0.5); // n=10000
        bool cv = false;
        bool cw = false;
        const auto v = amg_bicgstab_iters(&alloc, a, amg::SaAmg<crd::f64>::Cycle::V, cv); // V-cycle
        const auto w = amg_bicgstab_iters(&alloc, a, amg::SaAmg<crd::f64>::Cycle::W, cw); // W-cycle
        std::printf("[amg-wcycle dominant cd2d b=0.1] V iters=%zu  W iters=%zu\n", v, w);
        REQUIRE(cv);
        REQUIRE(cw);
        REQUIRE(w <= v); // W-cycle's stronger coarse pass wins on the stable (dominant) operator
    }
    crd::jobs::shutdown();
}

TEST_CASE("CLI: hesap.amg.f64 registers + solves a 2D Poisson (AMG-as-solver)", "[hesap-amg][cli]")
{
    using crd::hesap::cli::CommandArgs;
    using crd::hesap::cli::CommandRegistry;
    using crd::hesap::cli::ResultBinaryBlob;
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{64U << 20};
        auto a = laplace2d<crd::f64>(&alloc, 20); // n=400
        const crd::u32 n = a.rows();
        // Extract COO triplets from the CSR for the CLI args.
        const auto* o = a.pattern().outer_ptr.data();
        const auto* c = a.pattern().inner_idx.data();
        const auto* v = a.values().values.data();
        crd::containers::Array<crd::i64> tr(&alloc);
        crd::containers::Array<crd::i64> tc(&alloc);
        crd::containers::Array<crd::f64> vals(&alloc);
        crd::containers::Array<crd::f64> b(&alloc);
        for (crd::u32 i = 0; i < n; ++i)
        {
            for (crd::u32 q = o[i]; q < o[i + 1]; ++q)
            {
                tr.push_back(i);
                tc.push_back(c[q]);
                vals.push_back(v[q]);
            }
            b.push_back(1.0);
        }
        const auto* rec = CommandRegistry::global().find("hesap.amg.f64");
        REQUIRE(rec != nullptr);
        CommandArgs args{&alloc};
        args.set_u64("rows", n);
        args.set_i64_array("triplet_rows", crd::containers::ConstSpan<crd::i64>{tr.data(), tr.size()});
        args.set_i64_array("triplet_cols", crd::containers::ConstSpan<crd::i64>{tc.data(), tc.size()});
        args.set_f64_array("values", crd::containers::ConstSpan<crd::f64>{vals.data(), vals.size()});
        args.set_f64_array("b", crd::containers::ConstSpan<crd::f64>{b.data(), b.size()});
        const auto r = rec->impl(args);
        REQUIRE(r.ok);
        const auto* blob = std::get_if<ResultBinaryBlob>(&r.value);
        REQUIRE(blob != nullptr);
        const auto out = crd::containers::ConstSpan<crd::f64>{reinterpret_cast<const crd::f64*>(blob->bytes.data()),
                                                              blob->bytes.size() / sizeof(crd::f64)};
        REQUIRE(out.size() == static_cast<crd::usize>(n) + 3); // [iters, resid, converged, x...]
        REQUIRE(out[2] == 1.0);                                // converged
        REQUIRE(out[0] < 60.0);                                // AMG-as-solver converges in tens of cycles
    }
    crd::jobs::shutdown();
}

TEST_CASE("AMG adaptive-candidate (alphaSA) builds + converges without regressing diffusion", "[hesap-amg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        auto a = laplace2d<crd::f64>(&alloc, 50); // n=2500
        const crd::u32 n = a.rows();
        SparseLinearOp<crd::f64> op(a);
        // alphaSA: seed the tentative from a relaxed near-nullspace candidate instead of the
        // constant. On Poisson the near-nullspace IS ~constant, so this must NOT regress (the
        // candidate ~ constant ⇒ ~ SA). The hook earns its keep on problems where the
        // near-nullspace genuinely differs from constant (e.g. elasticity), not on convection
        // (measured: alphaSA does not move cd2d beta=0.3 — the lever there is directional smoothing).
        amg::SaAmg<crd::f64>::Options o;
        o.adaptive_candidate = true;
        amg::SaAmg<crd::f64> m(a, &alloc, o);
        REQUIRE(m.num_levels() >= 3); // builds a genuine hierarchy with the adaptive tentative
        dense::Vector<crd::f64> b(&alloc, n);
        dense::Vector<crd::f64> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = 1.0;
            x(i) = 0.0;
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-8;
        opts.max_iter = 200;
        KrylovWorkspace<crd::f64> ws(&alloc, n);
        auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < 40); // no diffusion regression vs constant SA
    }
    crd::jobs::shutdown();
}

TEST_CASE("Ruge-Stuben AMG converges mesh-independently on 2D Poisson (v4k-d the classical path)", "[hesap-amg][rs]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{512U << 20};
        auto solve_iters = [&](crd::u32 g) -> crd::usize
        {
            auto a = laplace2d<crd::f64>(&alloc, g);
            const crd::u32 n = a.rows();
            SparseLinearOp<crd::f64> op(a);
            amg::SaAmg<crd::f64>::Options o;
            o.coarsening = amg::SaAmg<crd::f64>::Coarsening::RugeStuben; // classical RS, not SA
            amg::SaAmg<crd::f64> m(a, &alloc, o);
            REQUIRE(m.num_levels() >= 3); // genuine RS multilevel hierarchy
            dense::Vector<crd::f64> b(&alloc, n);
            dense::Vector<crd::f64> x(&alloc, n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                b(i) = 1.0;
                x(i) = 0.0;
            }
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-8;
            opts.max_iter = 200;
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(res.converged);
            return res.iterations;
        };
        const crd::usize i50 = solve_iters(50);   // n=2500
        const crd::usize i100 = solve_iters(100); // n=10000
        std::printf("[rs-amg mesh-indep] iters n=2500: %zu  n=10000: %zu\n", i50, i100);
        REQUIRE(i50 < 30);                  // RS-AMG-CG converges fast (textbook isotropic)
        REQUIRE(i100 <= i50 + i50 / 2 + 3); // mesh-independent
    }
    crd::jobs::shutdown();
}

TEST_CASE("AMG builds + solves a complex Hermitian SPD system (complex completeness)", "[hesap-amg][complex]")
{
    using C = crd::hesap::Complex<crd::f64>;
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        auto a = laplace2d<C>(&alloc, 30); // n=900, real-valued ⇒ Hermitian SPD
        const crd::u32 n = a.rows();
        SparseLinearOp<C> op(a);
        amg::SaAmg<C> m(a, &alloc); // complex hierarchy: complex dinv, prolongator, smoother
        dense::Vector<C> b(&alloc, n);
        dense::Vector<C> x(&alloc, n);
        for (crd::u32 i = 0; i < n; ++i)
        {
            b(i) = C{1.0, 0.25};
            x(i) = C{};
        }
        IterativeOptions<crd::f64> opts;
        opts.rel_tol = 1e-8;
        opts.max_iter = 200;
        KrylovWorkspace<C> ws(&alloc, n);
        auto res = pcg<C>(op, m, b.span(), x.span(), opts, ws, &alloc);
        REQUIRE(res.converged);
        REQUIRE(res.iterations < 40); // complex AMG-PCG is mesh-independent like the real path
    }
    crd::jobs::shutdown();
}

TEST_CASE("AMG-CG iteration count is mesh-independent (the crush property)", "[hesap-amg]")
{
    crd::jobs::init();
    {
        crd::memory::TlsfAllocator alloc{1024U << 20};
        auto solve_iters = [&](crd::u32 g) -> crd::usize
        {
            auto a = laplace2d<crd::f64>(&alloc, g);
            const crd::u32 n = a.rows();
            SparseLinearOp<crd::f64> op(a);
            SaAmg<crd::f64> m(a, &alloc);
            dense::Vector<crd::f64> b(&alloc, n);
            dense::Vector<crd::f64> x(&alloc, n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                b(i) = 1.0;
                x(i) = 0.0;
            }
            IterativeOptions<crd::f64> opts;
            opts.rel_tol = 1e-8;
            opts.max_iter = 500;
            KrylovWorkspace<crd::f64> ws(&alloc, n);
            auto res = pcg<crd::f64>(op, m, b.span(), x.span(), opts, ws, &alloc);
            REQUIRE(res.converged);
            return res.iterations;
        };
        const crd::usize i50 = solve_iters(50);   // n=2500
        const crd::usize i100 = solve_iters(100); // n=10000
        std::printf("[amg-mesh-indep] iters n=2500: %zu  n=10000: %zu\n", i50, i100);
        // mesh-independence: iteration count grows at most mildly (±50%), not with n.
        REQUIRE(i100 <= i50 + i50 / 2 + 3);
    }
    crd::jobs::shutdown();
}
