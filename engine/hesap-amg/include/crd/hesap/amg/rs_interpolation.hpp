#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/amg/cf_splitting.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::amg
{
// ---------------------------------------------------------------------------
// rs_direct_interpolation -- classical Ruge-Stüben DIRECT interpolation. v4k-d.
//
// Builds the prolongator P (n × n_coarse) from the matrix A, the directed strength
// graph S, and the C/F split. A C-point injects (P[i, cmap[i]] = 1). An F-point i
// interpolates from its strong COARSE dependencies C_i = {j ∈ S[i] : j is C}:
//   negative couplings:  α = (Σ_{k≠i} aₙₑ𝓰(a_ik)) / (Σ_{j∈C_i} aₙₑ𝓰(a_ij))
//   positive couplings:  β = (Σ_{k≠i} aₚₒₛ(a_ik)) / (Σ_{j∈C_i} aₚₒₛ(a_ij))
//   w_ij = −(α·a_ij)/a_ii for a_ij<0, −(β·a_ij)/a_ii for a_ij>0   (j ∈ C_i)
// i.e. all off-diagonal mass is lumped proportionally onto the strong-C set, split
// by sign so a positive coupling can't be interpolated by a negative one (Stüben
// 2001, "direct interpolation"). For complex T the +/− split + ratios use Re(·)
// (classical RS targets real M-matrices; this is a sound Hermitian generalisation).
//
// Relies on the rs_cf_split guarantee that every F-point with strong dependencies
// has ≥1 strong C-dependency ⇒ C_i is non-empty (no zero rows). Deterministic.
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>
rs_direct_interpolation(const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
                        const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& s,
                        const crd::containers::Array<CfTag>& cf, crd::u32 n_coarse, crd::memory::IAllocator* alloc)
{
    using R          = crd::hesap::dense::RealType<T>;
    const crd::u32 n = a.rows();
    const auto*    ao = a.pattern().outer_ptr.data();
    const auto*    ai = a.pattern().inner_idx.data();
    const T*       av = a.values().values.data();
    const auto*    so = s.pattern().outer_ptr.data();
    const auto*    si = s.pattern().inner_idx.data();

    // coarse index map (prefix sum over C-points) + a per-row "is strong dep" mark.
    crd::containers::Array<crd::u32> cmap(alloc);
    crd::containers::Array<crd::u8>  strong(alloc);
    cmap.resize(n == 0 ? 1 : n);
    strong.resize(n == 0 ? 1 : n);
    crd::u32 c = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        strong[i] = 0;
        cmap[i]   = (cf[i] == CfTag::Coarse) ? c++ : ~crd::u32{0};
    }

    auto re = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re; }
        else { return v; }
    };

    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n_coarse);
    for (crd::u32 i = 0; i < n; ++i)
    {
        if (cf[i] == CfTag::Coarse) { tb.add(i, cmap[i], T(R(1))); continue; } // injection

        for (crd::u32 q = so[i]; q < so[i + 1]; ++q) { strong[si[q]] = 1; } // mark i's strong deps

        T diag = T{};
        R off_neg = R(0), off_pos = R(0), c_neg = R(0), c_pos = R(0);
        for (crd::u32 q = ao[i]; q < ao[i + 1]; ++q)
        {
            const crd::u32 j = ai[q];
            if (j == i) { diag = av[q]; continue; }
            const R rj = re(av[q]);
            if (rj < R(0)) { off_neg += rj; } else { off_pos += rj; }
            if (strong[j] && cf[j] == CfTag::Coarse) // strong C-dependency
            {
                if (rj < R(0)) { c_neg += rj; } else { c_pos += rj; }
            }
        }
        const R alpha = (c_neg != R(0)) ? off_neg / c_neg : R(0);
        const R beta  = (c_pos != R(0)) ? off_pos / c_pos : R(0);
        for (crd::u32 q = ao[i]; q < ao[i + 1]; ++q)
        {
            const crd::u32 j = ai[q];
            if (j == i || !strong[j] || cf[j] != CfTag::Coarse) { continue; }
            const R scale = (re(av[q]) < R(0)) ? alpha : beta; // sign-matched lumping factor
            const T w     = T(-scale) * av[q] / diag;          // −(α|β)·a_ij / a_ii
            tb.add(i, cmap[j], w);
        }

        for (crd::u32 q = so[i]; q < so[i + 1]; ++q) { strong[si[q]] = 0; } // unmark
    }
    return tb.compress();
}

} // namespace crd::hesap::amg
