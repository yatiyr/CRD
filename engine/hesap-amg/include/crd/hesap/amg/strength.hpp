#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::amg
{
// ---------------------------------------------------------------------------
// strength_matrix -- Smoothed-Aggregation strength of connection (Vaněk 1996).
// Phase 3.1.6 v4k-a.
//
// Node j is STRONGLY connected to node i iff  |a_ij| ≥ θ·√(|a_ii·a_jj|)  (the
// symmetric SA measure; θ ≈ 0.08). The returned CSR holds the SYMMETRIZED
// strong off-diagonal graph (a connection survives if strong in A OR Aᵀ —
// required for the aggregation graph on nonsymmetric A, e.g. convection-
// diffusion). Values are the strong |a_ij| magnitudes (the aggregation only
// uses the pattern; the prolongator uses A directly). Diagonal excluded.
//
// Deterministic: row-major build, ascending column order (D(amg)-1).
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
strength_matrix(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                crd::hesap::dense::RealType<T> theta, crd::memory::IAllocator* alloc)
{
    using R = crd::hesap::dense::RealType<T>;
    CRD_ASSERT_MSG(a.rows() == a.cols(), "strength_matrix: matrix must be square");
    CRD_ASSERT_MSG(a.pattern().is_compressed(), "strength_matrix: requires compressed CSR");
    const crd::u32 n     = a.rows();
    const auto*    outer = a.pattern().outer_ptr.data();
    const auto*    inner = a.pattern().inner_idx.data();
    const T*       vals  = a.values().values.data();

    auto mag = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
        else { return v < R(0) ? -v : v; }
    };

    // |a_ii| per node.
    crd::containers::Array<R> diag(alloc);
    diag.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        R d = R(0);
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { if (inner[q] == i) { d = mag(vals[q]); break; } }
        diag[i] = d;
    }

    // Directed strong connections: i -> j strong iff |a_ij| ≥ θ·√(|a_ii a_jj|).
    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
        {
            const crd::u32 j = inner[q];
            if (j == i) { continue; }
            const R thr = theta * std::sqrt(diag[i] * diag[j]);
            if (mag(vals[q]) >= thr && thr > R(0))
            {
                tb.add(i, j, T(R(1)));
                tb.add(j, i, T(R(1))); // symmetrize: strong if strong either direction
            }
        }
    }
    return tb.compress(); // TripletBuilder coalesces duplicates; row-major, col-sorted
}

} // namespace crd::hesap::amg
