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
// rs_strength_matrix -- classical Ruge-Stüben strength of connection. v4k-d.
//
// Point i strongly DEPENDS on point j iff  −a_ij ≥ θ·max_{k≠i}(−a_ik)  (θ ≈ 0.25),
// i.e. the coupling to j is a large fraction of i's strongest NEGATIVE coupling.
// This is the classical M-matrix measure (Ruge-Stüben 1987): for a discretized
// elliptic PDE the large negative off-diagonals are the directions error is smooth
// along, so they drive both the C/F splitting and the interpolation.
//
// The returned CSR is the DIRECTED strength graph S: a nonzero (i,j) means "i
// strongly depends on j" (row i = i's strong dependencies). NOT symmetrized —
// classical RS uses the directed graph (and its transpose) explicitly. Diagonal
// excluded. Deterministic: row-major, ascending column order (D(amg)-1).
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
rs_strength_matrix(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                   crd::hesap::dense::RealType<T> theta, crd::memory::IAllocator* alloc)
{
    using R = crd::hesap::dense::RealType<T>;
    CRD_ASSERT_MSG(a.rows() == a.cols(), "rs_strength_matrix: matrix must be square");
    CRD_ASSERT_MSG(a.pattern().is_compressed(), "rs_strength_matrix: requires compressed CSR");
    const crd::u32 n     = a.rows();
    const auto*    outer = a.pattern().outer_ptr.data();
    const auto*    inner = a.pattern().inner_idx.data();
    const T*       vals  = a.values().values.data();

    // −Re(a_ij): classical RS keys on the negative coupling (smooth error aligns with the
    // large negative off-diagonals of an M-matrix). For complex T use the real part.
    auto neg = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return -v.re; }
        else { return -v; }
    };

    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        // strongest negative off-diagonal coupling of row i.
        R maxneg = R(0);
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
        {
            const crd::u32 j = inner[q];
            if (j == i) { continue; }
            const R c = neg(vals[q]);
            if (c > maxneg) { maxneg = c; }
        }
        if (maxneg <= R(0)) { continue; } // no negative couplings ⇒ no strong dependencies
        const R thr = theta * maxneg;
        for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
        {
            const crd::u32 j = inner[q];
            if (j == i) { continue; }
            if (neg(vals[q]) >= thr) { tb.add(i, j, T(R(1))); } // i strongly depends on j
        }
    }
    return tb.compress();
}

} // namespace crd::hesap::amg
