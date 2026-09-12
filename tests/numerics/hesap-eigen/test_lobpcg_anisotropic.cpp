// crd-hesap-eigen v6-e-d — ANISOTROPIC / harder-matrix CHARACTERIZATION for LOBPCG (standard + generalized).
//
// This is a CHARACTERIZATION, not an optimization (advisor-gated). Smoothed-aggregation AMG is built for
// near-isotropic operators; on a strongly anisotropic stiffness its isotropic aggregation does NOT capture the
// (axis-aligned) smooth error modes, so its preconditioning power DEGRADES — that is the KNOWN hypothesis behind
// the v6 crush bench's guard ("3D model-Poisson is AMG's BEST case; general/anisotropic FEM is a hypothesis").
// We do NOT try to fix AMG (semi-coarsening / line smoothers are out of scope). We show two honest things:
//   (1) ROBUSTNESS — the eigensolver still CONVERGES on anisotropic operators (eigenvalues match the dense
//       reference), both for the standard A·x = λ·x and the generalized K·x = λ·M·x problem;
//   (2) DEGRADATION — AMG needs materially MORE LOBPCG iterations on the anisotropic operator than on the
//       isotropic one of the same size (its near-isotropic advantage shrinks). Honest, measured, reported.

#include <crd/containers/array.hpp>
#include <crd/hesap/amg/amg.hpp>
#include <crd/hesap/dense/eig_sym.hpp>
#include <crd/hesap/dense/matrix_types.hpp>
#include <crd/hesap/eigen/eigen.hpp>
#include <crd/hesap/preconditioners/ic0.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>

namespace eig = crd::hesap::eigen;
namespace sp = crd::hesap::sparse;
namespace pc = crd::hesap::preconditioners;
namespace dn = crd::hesap::dense;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

// Anisotropic 5-point Dirichlet stiffness on an mx×my grid: −εxx·∂²/∂x² − εyy·∂²/∂y². diag = 2εxx + 2εyy,
// x-edges = −εxx, y-edges = −εyy. SPD (weakly diagonally dominant + irreducible), nonsingular (Dirichlet).
// εxx = εyy = 1 ⇒ the isotropic Laplacian. εxx ≪ εyy ⇒ strong anisotropy (weak x-coupling) — AMG's hard case.
Csr aniso_2d(crd::memory::IAllocator* a, crd::u32 mx, crd::u32 my, double exx, double eyy)
{
    const crd::u32 n = mx * my;
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    auto id = [&](crd::u32 i, crd::u32 j) { return i * my + j; };
    const double dgl = 2.0 * exx + 2.0 * eyy;
    for (crd::u32 i = 0; i < mx; ++i)
    {
        for (crd::u32 j = 0; j < my; ++j)
        {
            const crd::u32 r = id(i, j);
            tb.add(r, r, dgl);
            if (i + 1 < mx)
            {
                tb.add(r, id(i + 1, j), -exx);
                tb.add(id(i + 1, j), r, -exx);
            }
            if (j + 1 < my)
            {
                tb.add(r, id(i, j + 1), -eyy);
                tb.add(id(i, j + 1), r, -eyy);
            }
        }
    }
    return tb.compress();
}

double diag_b(crd::u32 i) // non-uniform SPD lumped mass in [1, 2)
{
    return 1.0 + static_cast<double>((i * 37U + 11U) % 100U) / 100.0;
}

Csr diag_spd(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, diag_b(i));
    }
    return tb.compress();
}

// Dense reference (smallest `cnt`): standard ⇒ eig_sym(K); generalized-vs-diag-mass ⇒ eig_sym(D^{-1/2}·K·D^{-1/2}).
crd::containers::Array<crd::f64> smallest_ref(crd::memory::IAllocator* a, const Csr& k, crd::u32 cnt, bool gen)
{
    const crd::u32 n = k.rows();
    dn::Symmetric<crd::f64> c(a, n);
    const auto* outer = k.pattern().outer_ptr.data();
    const auto* inner = k.pattern().inner_idx.data();
    const auto& kv = k.values().values;
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (auto p = outer[i]; p < outer[i + 1]; ++p)
        {
            const crd::u32 j = inner[p];
            if (j <= i)
            {
                c.at(i, j) = gen ? kv[p] / std::sqrt(diag_b(i) * diag_b(j)) : kv[p];
            }
        }
    }
    dn::EigSym<crd::f64> es = dn::eig_sym<crd::f64>(a, c);
    crd::containers::Array<crd::f64> out(a);
    out.resize(cnt);
    for (crd::u32 s = 0; s < cnt; ++s)
    {
        out[s] = es.values.data()[s];
    }
    return out;
}

void check_smallest4(const eig::EigenResult<crd::f64>& r, const crd::containers::Array<crd::f64>& ref, double tol)
{
    double got[4];
    for (crd::u32 s = 0; s < 4; ++s)
    {
        got[s] = r.values[s].re;
    }
    std::sort(got, got + 4);
    for (crd::u32 s = 0; s < 4; ++s)
    {
        CHECK(std::fabs(got[s] - ref[s]) < tol);
    }
}
} // namespace

TEST_CASE("v6-e-d anisotropic standard LOBPCG: robust convergence + honest AMG degradation",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 27);
    const crd::u32 mx = 24;
    const crd::u32 my = 24;
    const double exx = 0.001; // STRONG anisotropy (weak x-coupling, 1000:1) — SA-AMG's genuinely hard case
    const double eyy = 1.0;

    Csr iso = aniso_2d(&alloc, mx, my, 1.0, 1.0);  // isotropic Laplacian (AMG's best case)
    Csr ani = aniso_2d(&alloc, mx, my, exx, eyy);  // anisotropic stiffness

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-6;
    opts.max_restarts = 4000; // generous: anisotropy slows convergence — we are characterizing, not racing

    sp::SparseLinearOp<crd::f64> ani_op(ani);

    // (1) ROBUSTNESS — IC0-preconditioned LOBPCG still converges on the anisotropic operator, correct eigenvalues.
    pc::Ic0Preconditioner<crd::f64> ic0(ani, &alloc);
    auto r_ic0 = eig::eigs_sym_lobpcg<crd::f64>(ani_op, opts, &alloc, &ic0);
    REQUIRE(r_ic0.converged);
    crd::containers::Array<crd::f64> ref = smallest_ref(&alloc, ani, 4, /*gen=*/false);
    check_smallest4(r_ic0, ref, 1e-5);

    // (2) DEGRADATION — AMG on isotropic vs anisotropic, SAME grid: anisotropy costs materially more iterations.
    crd::hesap::amg::SaAmg<crd::f64> amg_iso(iso, &alloc);
    sp::SparseLinearOp<crd::f64> iso_op(iso);
    auto r_iso = eig::eigs_sym_lobpcg<crd::f64>(iso_op, opts, &alloc, &amg_iso);

    crd::hesap::amg::SaAmg<crd::f64> amg_ani(ani, &alloc);
    auto r_ani = eig::eigs_sym_lobpcg<crd::f64>(ani_op, opts, &alloc, &amg_ani);

    INFO("AMG iters: isotropic=" << r_iso.iterations << " anisotropic(exx=" << exx << ")=" << r_ani.iterations
                                 << " IC0(aniso)=" << r_ic0.iterations);
    REQUIRE(r_iso.converged);
    REQUIRE(r_ani.converged);
    // The honest, measured characterization: AMG's near-isotropic advantage shrinks under anisotropy — it needs
    // strictly more LOBPCG iterations on the anisotropic operator than on the isotropic one of the same size.
    CHECK(r_ani.iterations > r_iso.iterations);
}

TEST_CASE("v6-e-d anisotropic generalized LOBPCG: robust convergence (K anisotropic, diagonal mass M)",
          "[hesap][eigen][v6]")
{
    crd::memory::TlsfAllocator alloc(1U << 27);
    const crd::u32 mx = 20;
    const crd::u32 my = 24;
    const crd::u32 n = mx * my;
    Csr k = aniso_2d(&alloc, mx, my, 0.001, 1.0); // strong anisotropic stiffness (1000:1)
    Csr m = diag_spd(&alloc, n);                  // lumped mass

    eig::EigenOptions<crd::f64> opts;
    opts.nev = 4;
    opts.which = eig::Which::SmallestAlgebraic;
    opts.tol = 1e-6;
    opts.max_restarts = 4000;

    sp::SparseLinearOp<crd::f64> kop(k);
    sp::SparseLinearOp<crd::f64> mop(m);
    pc::Ic0Preconditioner<crd::f64> ic0(k, &alloc); // T ≈ K⁻¹

    auto r = eig::eigs_sym_gen_lobpcg<crd::f64>(kop, mop, opts, &alloc, &ic0);
    INFO("generalized anisotropic IC0 iters=" << r.iterations << " converged=" << r.converged);
    REQUIRE(r.converged);

    crd::containers::Array<crd::f64> ref = smallest_ref(&alloc, k, 4, /*gen=*/true);
    check_smallest4(r, ref, 1e-5); // correct generalized eigenvalues on the anisotropic operator
}
