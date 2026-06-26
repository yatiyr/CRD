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

#include <crd/math/cmath.hpp>
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
        return tol > static_cast<R>(0) ? tol : crd::math::sqrt(std::numeric_limits<R>::epsilon());
    }
};

// The shared result. Eigenvalues are COMPLEX in general (a real nonsymmetric A yields conjugate pairs); for
// the symmetric problem the imaginary parts are exactly zero. The k-th eigenvector is `vectors[:,k] (REAL part)
// + i·vectors_im[:,k] (IMAGINARY part)`, both column-major n × nconv; `vectors_im` is EMPTY for the symmetric/
// real solvers (the eigenvectors are real). `residuals[k] = ‖A·x_k − λ_k·x_k‖ / ‖x_k‖` (the TRUE residual).
template <typename T> struct EigenResult
{
    using R = crd::hesap::dense::RealType<T>;

    crd::containers::Array<crd::hesap::Complex<R>> values;     // length nconv (ascending by `Which`)
    crd::containers::Array<T>                      vectors;    // n × nconv, column-major (Re; empty if !compute_vectors)
    crd::containers::Array<T>                      vectors_im; // n × nconv (Im; EMPTY for real/symmetric eigenvectors)
    crd::containers::Array<R>                      residuals;  // length nconv
    crd::u32                                       n = 0;
    crd::u32                                       nconv = 0;       // converged eigenpairs
    crd::u32                                       iterations = 0;  // matvecs / restart cycles
    bool                                           converged = false; // nconv >= nev

    explicit EigenResult(crd::memory::IAllocator* alloc) noexcept
        : values(alloc), vectors(alloc), vectors_im(alloc), residuals(alloc)
    {
    }
};

} // namespace crd::hesap::eigen
