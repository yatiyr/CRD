#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>

namespace crd::hesap::ordering
{
// -----------------------------------------------------------------------
// MC64 -- maximum-weight bipartite matching + scaling (Duff-Koster 2001). v4j-1a.
//
// Permutes large entries to the diagonal and scales the matrix toward an "I-matrix"
// (matched |diagonal| = 1, all |off-diagonals| ≤ 1) -- the robustness front-end that
// makes ILUT / multilevel-ILU reliable on hard, badly-scaled, non-diagonally-dominant
// matrices (the same role MC64 plays in ILUPACK / MUMPS / SuperLU).
//
// The max-PRODUCT matching max_σ ∏|a_{i,σ(i)}| is reformulated as the min-SUM linear
// assignment problem on c_ij = log(maxⱼ|a_ij|) − log(|a_ij|) ≥ 0 (0 at each row's
// largest entry), solved by a sparse shortest-augmenting-path (Dijkstra over reduced
// costs). The dual potentials uᵢ, vⱼ give the scaling: D_r[i] = exp(uᵢ)/maxⱼ|a_ij|,
// D_c[j] = exp(vⱼ). Complementary slackness ⇒ the matched entry scales to exactly 1
// and every other to ≤ 1.
//
// Determinism (D(ord) discipline): the Dijkstra argmin breaks ties by ASCENDING column
// index; row adjacency is walked in stored (ascending) order; the dual constant is
// pinned (free columns get v=0). No RNG. → bit-reproducible match + scaling.
// -----------------------------------------------------------------------

struct Mc64Scaling
{
    // colperm[i] = column matched to row i; the entry a[i, colperm[i]] is the (max-weight)
    // matched entry that a column permutation would move onto the diagonal. A valid
    // permutation of [0,n) when the matrix has structural full rank (else unmatched rows
    // fall back to any free column, recorded too).
    crd::containers::Array<crd::u32> colperm;
    crd::containers::Array<crd::f64> dr; // row scaling D_r (length n)
    crd::containers::Array<crd::f64> dc; // column scaling D_c (length n)
    bool                             full_rank = true; // false if no perfect matching exists

    explicit Mc64Scaling(crd::memory::IAllocator* alloc) : colperm(alloc), dr(alloc), dc(alloc) {}
};

// Core (non-templated): `mag` holds |a_ij| for each stored nonzero, parallel to
// pattern.inner_idx. Square CSR pattern. Defined in mc64.cpp.
[[nodiscard]] Mc64Scaling mc64_match_and_scale(const sparse::SparsePattern& pattern,
                                               crd::containers::ConstSpan<crd::f64> mag,
                                               crd::memory::IAllocator* alloc);

namespace detail
{
// |·| as f64, overloaded so the wrapper needs no is_complex_v (the ordering module does not
// depend on crd-hesap-dense). f32 / f64 / Complex<f32> / Complex<f64>.
[[nodiscard]] inline crd::f64 mc64_abs(crd::f32 v) noexcept { return v < 0.0F ? -static_cast<crd::f64>(v) : static_cast<crd::f64>(v); }
[[nodiscard]] inline crd::f64 mc64_abs(crd::f64 v) noexcept { return v < 0.0 ? -v : v; }
template <typename R>
[[nodiscard]] inline crd::f64 mc64_abs(const crd::hesap::Complex<R>& v) noexcept
{
    return crd::math::sqrt(static_cast<crd::f64>(v.re) * static_cast<crd::f64>(v.re)
                     + static_cast<crd::f64>(v.im) * static_cast<crd::f64>(v.im));
}
} // namespace detail

// Templated front-end: builds the |a_ij| magnitude array from a typed CSR matrix and
// runs the core. f32 / f64 / Complex32 / Complex64.
template <typename T>
[[nodiscard]] Mc64Scaling mc64_match_and_scale(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                               crd::memory::IAllocator* alloc)
{
    const T*         vals = a.values().values.data();
    const crd::usize nnz  = a.values().values.size();
    crd::containers::Array<crd::f64> mag(alloc);
    mag.resize(nnz);
    for (crd::usize k = 0; k < nnz; ++k) { mag[k] = detail::mc64_abs(vals[k]); }
    return mc64_match_and_scale(a.pattern(), crd::containers::ConstSpan<crd::f64>{mag.data(), mag.size()}, alloc);
}

} // namespace crd::hesap::ordering
