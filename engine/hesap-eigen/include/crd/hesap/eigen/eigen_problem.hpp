#pragma once

// eigen_problem.hpp — Phase 3.1.6 v6-a: the SPARSE EIGENVALUE substrate spec (the shared contract every v6
// method fills). Matrix-free over `crd::hesap::LinearOp<T>` — the solver sees only A·x (and optionally B·x for
// the generalized problem A·x = λ·B·x). Rayleigh-Ritz on the small projected problem reuses the dense
// eigensolvers (`crd::hesap::dense::eig_sym` / `eig_nonsym`); shift-invert reuses the v5 direct factors.
//
// DETERMINISM MOAT (the v6 differentiator — no ARPACK/PRIMME/SLEPc carries it): the eigenpairs are bit-
// identical across {1,2,4,8} workers. The substrate pins the four things that make that true AND the three
// hazards the linear-solver moat doesn't have (advisor 2026-06-05):
//   • deterministic counter-RNG start (`EigenOptions::seed`) — NOT std::rand / a thread-timing seed,
//   • fixed-order (modified Gram-Schmidt, twice) reorthogonalization,
//   • a pinned eigenvector SIGN/ORDER convention (largest-magnitude component forced positive),
//   • B-orthonormalization fixed-order for the generalized problem.
//   HAZARDS: clustered/multiple eigenvalues ⇒ non-unique eigenvectors (moat tests use WELL-SEPARATED spectra;
//   block methods assert SUBSPACE identity, not vector-by-vector); the dense Rayleigh-Ritz is the inner kernel
//   ⇒ it must itself be bit-deterministic given identical input.

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::eigen
{
// Which end of the spectrum to compute. Interior eigenvalues are reached via shift-invert (v6-d) or FEAST
// (v6-g), not a `Which` value.
enum class Which : crd::u8
{
    LargestMagnitude,  // |λ| largest  (LM) — symmetric + nonsymmetric
    SmallestMagnitude, // |λ| smallest (SM)
    LargestAlgebraic,  // λ largest    (LA) — symmetric (real spectrum)
    SmallestAlgebraic, // λ smallest   (SA) — symmetric
    LargestReal,       // Re(λ) largest  (LR) — nonsymmetric
    SmallestReal,      // Re(λ) smallest (SR) — nonsymmetric
};

template <typename T> struct EigenOptions
{
    using R = crd::hesap::dense::RealType<T>;

    crd::u32 nev = 1;                  // number of eigenpairs wanted
    crd::u32 ncv = 0;                  // Krylov/Ritz subspace dim; 0 ⇒ auto = min(max(2·nev+1, 20), n)
    Which    which = Which::LargestAlgebraic;
    R        tol = static_cast<R>(0);  // 0 ⇒ default ≈ sqrt(eps)·‖A‖-relative on the Ritz residual
    crd::u32 max_restarts = 300;       // restart cycles (v6-b+) / outer iterations
    crd::u64 seed = 0x9E3779B97F4A7C15ULL; // deterministic counter-RNG start (moat) — fixed default
    bool     compute_vectors = true;   // false ⇒ eigenvalues only (cheaper)

    // Default ≈ √eps — the achievable target for the v6-a NO-RESTART Lanczos (machine-precision convergence
    // wants the thick-restart of v6-b). Relative residual ‖A·x − θ·x‖ ≤ tol·max(|θ|, 1).
    [[nodiscard]] R effective_tol() const noexcept
    {
        return tol > static_cast<R>(0) ? tol : std::sqrt(std::numeric_limits<R>::epsilon());
    }
};

// The shared result. Eigenvalues are COMPLEX in general (a real nonsymmetric A yields conjugate pairs); for
// the symmetric problem the imaginary parts are exactly zero. Vectors are column-major n × nconv in the
// OPERATOR scalar T (a real nonsymmetric eigenvector of a complex eigenvalue is stored as its real/imag
// columns per the LAPACK convention — pinned per method). `residuals[k] = ‖A·x_k − λ_k·x_k‖ / ‖x_k‖`.
template <typename T> struct EigenResult
{
    using R = crd::hesap::dense::RealType<T>;

    crd::containers::Array<crd::hesap::Complex<R>> values;    // length nconv (ascending by `Which`)
    crd::containers::Array<T>                      vectors;   // n × nconv, column-major (empty if !compute_vectors)
    crd::containers::Array<R>                      residuals; // length nconv
    crd::u32                                       n = 0;
    crd::u32                                       nconv = 0;       // converged eigenpairs
    crd::u32                                       iterations = 0;  // matvecs / restart cycles
    bool                                           converged = false; // nconv >= nev

    explicit EigenResult(crd::memory::IAllocator* alloc) noexcept
        : values(alloc), vectors(alloc), residuals(alloc)
    {
    }
};

} // namespace crd::hesap::eigen
