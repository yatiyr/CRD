#pragma once

#include <crd/containers/hash.hpp>  // hash_u64 — the counter-based RNG core
#include <crd/core/types.hpp>
#include <crd/hesap/dense/eig_sym.hpp>  // EigSym (rsyev_op return)
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/svd.hpp>  // SVD (rsvd_op return)
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::dense
{
// -----------------------------------------------------------------------
// Phase 3.1.6 v5e-1b — randomized range finder generalized to matrix-free
// `LinearOp<T>` SAMPLING (extends v3b-3's `rsvd`/`rsyev`, which take a dense
// `Matrix`). This is the substrate STRUMPACK-style HSS front construction
// (v5e-1c/2) builds on: compress a front by sampling `F·Ω` / `Fᵀ·Ω` without
// ever forming the dense front.
//
// Three entry points:
//   - `randomized_range(op, rank)`  — orthonormal basis Q for an approximate
//     range of `op` (Halko-Martinsson-Tropp 2011 §4.1 + subspace/power
//     iteration §4.5). The HSS off-diagonal-block primitive.
//   - `rsvd_op(op, rank)`           — randomized truncated SVD of `op`
//     (range finder + B = Qᵀ·op via `apply_transpose` + small dense svd).
//   - `rsyev_op(op, rank)`          — randomized symmetric eig of a symmetric
//     `op` (Rayleigh-Ritz B = Qᵀ·op·Q + small dense eig_sym), top-k descending.
//
// DETERMINISM (D(direct)-6, the v5e moat primitive): the Gaussian sketch uses
// a COUNTER-BASED RNG keyed by linear index — `counter_gaussian(seed, idx)` is
// a pure function of (seed, idx), NOT a sequential stream (unlike the dense
// `rsvd`'s LCG). So the sketch — hence the basis — is a pure function of
// (op, seed, params), independent of fill order / thread schedule. v5e-2 keys
// `seed` by front index so fronts compressed on different workers get
// bit-identical bases (the cross-thread moat is proven THERE, on the tree
// slice, not here — v5e-1b is serial). NOTE: `randomized_range` does NOT
// adaptively grow the rank to a tolerance — adaptive rank is a v5e-2 STRUMPACK
// deliverable, where the front-compression consumer fixes the tolerance
// semantics.
//
// Real f32/f64. Lower layer: raw scalars (ADR-0078 §5).
// -----------------------------------------------------------------------

// =======================================================================
// counter_gaussian — sketch Gaussian sample at linear index `idx`, keyed by
// `seed`. PURE function of (seed, idx) ⇒ order-independent (the thread-
// independence primitive). Irwin-Hall sum-of-12 uniforms (NOT a true Gaussian
// — truncated to ±6 — but an adequate random sketch, matching the dense rsvd).
// The seed is avalanche-mixed BEFORE the counter is added so per-front seeds
// (v5e-2: seed = base + front_id) produce independent streams across both
// front index and sample index. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] T counter_gaussian(crd::u64 seed, crd::u64 idx) noexcept
{
    using R = RealType<T>;
    const crd::u64 base = crd::containers::hash_u64(seed) + idx * 12ULL;  // mix seed first
    R acc{0};
    for (crd::u64 t = 0; t < 12; ++t)
    {
        const crd::u64 bits = crd::containers::hash_u64(base + t);
        acc += static_cast<R>((bits >> 11) & 0xFFFFFFFFFFFFFULL) / static_cast<R>(1ULL << 52);
    }
    return static_cast<T>(acc - R{6});
}

template <typename T>
struct RangeBasis
{
    Matrix<T> q;            // op.n_rows() × rank, orthonormal columns
    crd::usize rank = 0;    // number of basis columns (= ell, the oversampled rank)

    explicit RangeBasis(crd::memory::IAllocator* alloc) noexcept : q(alloc) {}
    RangeBasis(RangeBasis&&) noexcept = default;
    RangeBasis& operator=(RangeBasis&&) noexcept = default;
    RangeBasis(const RangeBasis&) = delete;
    RangeBasis& operator=(const RangeBasis&) = delete;
};

// =======================================================================
// randomized_range — orthonormal basis Q (n_rows × ell) for an approximate
// range of `op`, ell = min(rank + oversampling, min(n_rows, n_cols)). Gaussian
// sketch Ω → Y = op·Ω → Householder-QR → `power_iters` subspace iterations
// (Z = opᵀ·Y, Y = op·Z, re-orthonormalized between). `power_iters` is clamped
// to 0 when `op` has no transpose (the basic scheme needs only `apply`).
// Matrix-free: only `apply` / `apply_transpose`. Deterministic given `seed`.
// =======================================================================
template <typename T>
[[nodiscard]] RangeBasis<T> randomized_range(crd::memory::IAllocator* alloc, const LinearOp<T>& op,
                                             crd::usize rank, crd::usize oversampling = 8,
                                             crd::usize power_iters = 2, crd::u64 seed = 0x5EED5A11ULL);

// =======================================================================
// rsvd_op — randomized truncated SVD of a matrix-free `op` (m × n): a rank-k
// approximation op ≈ U diag(S) Vᵀ, k = min(rank, min(m,n)). REQUIRES
// `op.has_transpose()` (B = Qᵀ·op formed via `apply_transpose`); returns an
// empty SVD otherwise. Mirrors the dense `rsvd` pipeline. Real f32/f64.
// =======================================================================
template <typename T>
[[nodiscard]] SVD<T> rsvd_op(crd::memory::IAllocator* alloc, const LinearOp<T>& op, crd::usize rank,
                             crd::usize oversampling = 8, crd::usize power_iters = 2,
                             crd::u64 seed = 0x5EED5D11ULL);

// =======================================================================
// rsyev_op — randomized symmetric eigendecomposition of a SYMMETRIC matrix-free
// `op` (n × n): top-k eigenpairs (values DESCENDING by algebraic value, top of
// spectrum) via Rayleigh-Ritz on B = Qᵀ·op·Q. The caller guarantees `op` is
// symmetric (apply == apply_transpose). Mirrors the dense `rsyev`. Real f32/f64.
// CAVEAT (matches the dense rsyev convention): returns the largest-ALGEBRAIC
// eigenvalues, while the range finder captures the largest-MAGNITUDE subspace.
// For a strongly indefinite op (a dominant large-NEGATIVE eigenvalue) the
// returned positive top-of-spectrum pair may be poorly captured — use a PSD /
// definite op, or shift, when the magnitude-dominant end is what you need.
// =======================================================================
template <typename T>
[[nodiscard]] EigSym<T> rsyev_op(crd::memory::IAllocator* alloc, const LinearOp<T>& op, crd::usize rank,
                                 crd::usize oversampling = 8, crd::usize power_iters = 2,
                                 crd::u64 seed = 0x5EE59E0FULL);

} // namespace crd::hesap::dense
