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
#include <cstring>
#include <limits>

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
// A real or complex value from f64 parts, each rounded to the type's real.
template <typename T> T amg_value(crd::f64 re, crd::f64 im)
{
    using R = dense::RealType<T>;
    if constexpr (dense::is_complex_v<T>)
    {
        return T{static_cast<R>(re), static_cast<R>(im)};
    }
    else
    {
        (void)im;
        return static_cast<T>(re);
    }
}

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
    {
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
    {
        for (crd::u32 j = 0; j < g; ++j)
        {
            const crd::u32 r = idx(i, j);
            double diag = 2.0 * eps + 2.0 + beta + c; // diffusion x+y + upwind convection + reaction
            if (j > 0)
            {
                b.add(r, idx(i, j - 1), amg_value<T>(-eps - beta, 0.0));
            } // west (upwind)
            if (j + 1 < g)
            {
                b.add(r, idx(i, j + 1), amg_value<T>(-eps, 0.0));
            } // east
            if (i > 0)
            {
                b.add(r, idx(i - 1, j), T(static_cast<crd::f64>(-1.0)));
            } // south
            if (i + 1 < g)
            {
                b.add(r, idx(i + 1, j), T(static_cast<crd::f64>(-1.0)));
            } // north
            b.add(r, r, amg_value<T>(diag, 0.0));
        }
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

// ---- sa_amg_arm: every products pick gives the one-job hierarchy and cycle bits ----

namespace
{
template <typename U> bool amg_same_bytes(const U* x, const U* y, crd::usize n)
{
    return n == 0 || std::memcmp(x, y, n * sizeof(U)) == 0;
}

template <typename T>
bool amg_same_csr(const SparseMatrix<T, SparseFormat::Csr>& x, const SparseMatrix<T, SparseFormat::Csr>& y)
{
    return x.rows() == y.rows() && x.cols() == y.cols() && x.nnz() == y.nnz() &&
           x.pattern().topology_hash == y.pattern().topology_hash &&
           amg_same_bytes(x.pattern().outer_ptr.data(), y.pattern().outer_ptr.data(), x.pattern().outer_ptr.size()) &&
           amg_same_bytes(x.pattern().inner_idx.data(), y.pattern().inner_idx.data(), x.nnz()) &&
           amg_same_bytes(x.values().values.data(), y.values().values.data(), x.nnz());
}

// Every level's A, P, R and 1/diag(A), and the coarsest dense LU.
template <typename T> bool same_hierarchy(const SaAmg<T>& x, const SaAmg<T>& y)
{
    if (x.num_levels() != y.num_levels() || x.coarse_size() != y.coarse_size())
    {
        return false;
    }
    for (crd::u32 k = 0; k + 1 < x.num_levels(); ++k)
    {
        const auto& lx = x.level(k);
        const auto& ly = y.level(k);
        if (lx.n != ly.n || !amg_same_csr(lx.a, ly.a) || !amg_same_csr(lx.p, ly.p) || !amg_same_csr(lx.r, ly.r) ||
            lx.dinv.size() != ly.dinv.size() || !amg_same_bytes(lx.dinv.data(), ly.dinv.data(), lx.dinv.size()))
        {
            return false;
        }
    }
    const crd::usize nc = x.coarse_size();
    return x.coarse_lu().info() == y.coarse_lu().info() &&
           amg_same_bytes(x.coarse_lu().packed().data(), y.coarse_lu().packed().data(), nc * nc);
}

constexpr crd::usize kAmgGuard = 4;

// z = M^-1 r into a span followed by guard words; false when apply wrote past n.
template <typename T> bool amg_apply(const SaAmg<T>& m, crd::containers::Array<T>& z, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = static_cast<crd::u32>(m.n_rows());
    crd::containers::Array<T> r(alloc);
    r.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        r[i] = amg_value<T>(1.0 + 0.25 * static_cast<crd::f64>(i % 5), 0.125 * static_cast<crd::f64>(i % 3));
    }
    const T guard = amg_value<T>(-7777.0, 3.0);
    z.resize(n + kAmgGuard);
    for (crd::usize i = 0; i < z.size(); ++i)
    {
        z[i] = guard;
    }
    (void)m.apply(crd::containers::ConstSpan<T>{r.data(), n}, crd::containers::Span<T>{z.data(), n});
    bool intact = true;
    for (crd::usize i = n; i < z.size(); ++i)
    {
        intact = intact && amg_same_bytes(&z[i], &guard, 1);
    }
    return intact;
}

struct AmgArm
{
    const char* name;
    amg::detail::AmgPick pick;
};

const AmgArm kAmgArms[] = {
    {"spgemm forced to one job", {.spgemm = {.parallel_min_flop = 0, .workers = 1}}},
    {"spgemm on the pool", {.spgemm = {.parallel_min_flop = 0}}},
    {"spgemm on 3 jobs", {.spgemm = {.parallel_min_flop = 0, .workers = 3}}},
    {"spgemm on 8 jobs", {.spgemm = {.parallel_min_flop = 0, .workers = 8}}},
    {"spgemm on 32 jobs", {.spgemm = {.parallel_min_flop = 0, .workers = 32}}},
    {"spmv on the pool", {.spmv_min_nnz = 0}},
    {"spmv on 7 jobs", {.spmv_min_nnz = 0, .spmv_workers = 7}},
    {"spmv on 16 jobs", {.spmv_min_nnz = 0, .spmv_workers = 16}},
    {"spmv on 32 jobs", {.spmv_min_nnz = 0, .spmv_workers = 32}},
    {"both on 16 jobs", {.spgemm = {.parallel_min_flop = 0, .workers = 16}, .spmv_min_nnz = 0, .spmv_workers = 16}},
    {"finest levels only", {.spgemm = {.parallel_min_flop = 20000}, .spmv_min_nnz = 2000}},
};

template <typename T>
void check_amg_arms(const SparseMatrix<T, SparseFormat::Csr>& a, const typename SaAmg<T>::Options& opts,
                    crd::memory::IAllocator* alloc)
{
    const SaAmg<T> ref(a, alloc, opts);
    REQUIRE(ref.num_levels() >= 3); // a K-cycle level above the coarsest one
    crd::containers::Array<T> zref(alloc);
    REQUIRE(amg_apply(ref, zref, alloc));
    for (const AmgArm& arm : kAmgArms)
    {
        INFO(arm.name);
        const SaAmg<T> m(a, alloc, opts, arm.pick);
        CHECK(same_hierarchy(ref, m));
        crd::containers::Array<T> z(alloc);
        CHECK(amg_apply(m, z, alloc));
        CHECK(amg_same_bytes(z.data(), zref.data(), zref.size()));
    }
}

// `a` with its middle row's first off-diagonal coupling NaN (strength drops it as weak).
template <typename T>
SparseMatrix<T, SparseFormat::Csr> with_nan_coupling(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                     crd::memory::IAllocator* alloc)
{
    TripletBuilder<T> b(alloc, a.rows(), a.cols());
    const auto* o = a.pattern().outer_ptr.data();
    const auto* c = a.pattern().inner_idx.data();
    const T* v = a.values().values.data();
    bool placed = false;
    for (crd::u32 i = 0; i < a.rows(); ++i)
    {
        for (crd::u32 q = o[i]; q < o[i + 1]; ++q)
        {
            const bool nan = !placed && i == a.rows() / 2 && c[q] != i;
            placed = placed || nan;
            b.add(i, c[q], nan ? amg_value<T>(std::numeric_limits<crd::f64>::quiet_NaN(), 0.0) : v[q]);
        }
    }
    return b.compress();
}

template <typename T> void check_amg_arm_type(crd::memory::IAllocator* alloc)
{
    using Amg = SaAmg<T>;
    const auto poisson = laplace2d<T>(alloc, 40); // n 1600: three levels
    for (const auto cycle : {Amg::Cycle::V, Amg::Cycle::W, Amg::Cycle::F, Amg::Cycle::K})
    {
        INFO("Poisson, cycle " << static_cast<int>(cycle));
        typename Amg::Options o;
        o.cycle = cycle;
        check_amg_arms<T>(poisson, o, alloc);
    }
    {
        INFO("Poisson, Ruge-Stuben");
        typename Amg::Options o;
        o.coarsening = Amg::Coarsening::RugeStuben;
        check_amg_arms<T>(poisson, o, alloc);
    }
    {
        INFO("Poisson, adaptive candidate, ILU(0) smoother");
        typename Amg::Options o;
        o.adaptive_candidate = true;
        o.smoother = Amg::Smoother::Ilu0;
        check_amg_arms<T>(poisson, o, alloc);
    }
    {
        INFO("Poisson, one NaN coupling");
        check_amg_arms<T>(with_nan_coupling<T>(poisson, alloc), typename Amg::Options{}, alloc);
    }
    {
        INFO("convection-diffusion, Petrov-Galerkin restriction, K-cycle");
        typename Amg::Options o;
        o.cycle = Amg::Cycle::K;
        check_amg_arms<T>(cd2d<T>(alloc, 40, 0.1, 0.3, 0.01), o, alloc);
    }
    {
        INFO("convection-diffusion, plain aggregation");
        typename Amg::Options o;
        o.smooth_prolongator = false;
        check_amg_arms<T>(cd2d<T>(alloc, 40, 0.1, 0.3, 0.01), o, alloc);
    }
}
} // namespace

TEST_CASE("sa_amg_arm: every products pick keeps the one-job hierarchy and cycle bits",
          "[hesap-amg][jobs][determinism]")
{
    crd::jobs::Config cfg;
    cfg.num_threads = 4; // a hosted runner's pool on every host: the 8 to 32 forced jobs outnumber its workers
    crd::jobs::init(cfg);
    {
        crd::memory::TlsfAllocator alloc{256U << 20};
        {
            INFO("f64");
            check_amg_arm_type<crd::f64>(&alloc);
        }
        {
            INFO("f32");
            check_amg_arm_type<crd::f32>(&alloc);
        }
        {
            INFO("c64");
            check_amg_arm_type<crd::hesap::Complex<crd::f64>>(&alloc);
        }
        {
            INFO("c32");
            check_amg_arm_type<crd::hesap::Complex<crd::f32>>(&alloc);
        }
    }
    crd::jobs::shutdown();
}

TEST_CASE("sa_amg_arm: a forced parallel pick on a pool that is down runs serially", "[hesap-amg][jobs][determinism]")
{
    REQUIRE(crd::jobs::num_workers() == 0);
    crd::memory::TlsfAllocator alloc{64U << 20};
    const auto a = laplace2d<crd::f64>(&alloc, 40);
    check_amg_arms<crd::f64>(a, SaAmg<crd::f64>::Options{}, &alloc);
}
