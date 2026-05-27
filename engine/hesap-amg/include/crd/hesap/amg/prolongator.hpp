#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::amg
{
using SaCsr = crd::hesap::sparse::SparseFormat;

// Tentative (piecewise-constant) prolongator T: n × n_agg, one 1.0 per row at
// its aggregate column. The unsmoothed inter-grid transfer. Deterministic.
template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>
tentative_prolongator(const crd::containers::Array<crd::u32>& agg, crd::u32 n_agg, crd::memory::IAllocator* alloc)
{
    const crd::u32 n = static_cast<crd::u32>(agg.size());
    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n_agg);
    for (crd::u32 i = 0; i < n; ++i) { tb.add(i, agg[i], T(crd::hesap::dense::RealType<T>(1))); }
    return tb.compress();
}

// Adaptive (αSA) tentative prolongator: instead of the constant 1, each aggregate's
// column carries the near-nullspace CANDIDATE restricted to that aggregate, normalized so
// the column has unit 2-norm (⇒ orthonormal columns, like SA). For convection/anisotropy
// the slow modes are NOT constant; seeding T with a relaxed candidate lets the coarse space
// represent them (Brezina et al. αSA 2004). Deterministic (candidate seed pinned upstream).
template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>
tentative_prolongator_adaptive(const crd::containers::Array<crd::u32>& agg, crd::u32 n_agg,
                               const crd::containers::Array<T>& candidate, crd::memory::IAllocator* alloc)
{
    using R          = crd::hesap::dense::RealType<T>;
    const crd::u32 n = static_cast<crd::u32>(agg.size());
    // Per-aggregate 2-norm of the candidate (for column normalization).
    crd::containers::Array<R> nrm(alloc);
    nrm.resize(n_agg == 0 ? 1 : n_agg);
    for (crd::u32 a = 0; a < n_agg; ++a) { nrm[a] = R(0); }
    auto mag2 = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re * v.re + v.im * v.im; }
        else { return v * v; }
    };
    for (crd::u32 i = 0; i < n; ++i) { nrm[agg[i]] += mag2(candidate[i]); }
    for (crd::u32 a = 0; a < n_agg; ++a) { nrm[a] = std::sqrt(nrm[a]); }
    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n_agg);
    for (crd::u32 i = 0; i < n; ++i)
    {
        const R s = nrm[agg[i]];
        // Empty/zero aggregate ⇒ fall back to constant 1 (keeps the column nonzero).
        tb.add(i, agg[i], (s > R(0)) ? candidate[i] / T(s) : T(R(1)));
    }
    return tb.compress();
}

// Spectral radius of D⁻¹A by power iteration (deterministic seed, fixed iters):
// used to set the Jacobi-smoothing weight ω = 4/(3·λmax) for the prolongator.
template <typename T>
[[nodiscard]] crd::hesap::dense::RealType<T>
estimate_drinv_a_radius(const crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>& a,
                        const crd::containers::Array<T>& dinv,
                        crd::memory::IAllocator* alloc, crd::u32 iters = 12)
{
    using R = crd::hesap::dense::RealType<T>;
    // |v|² (real square or complex squared-modulus) — power-iteration norms work for complex A.
    auto mag2 = [](T v) -> R {
        if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re * v.re + v.im * v.im; }
        else { return v * v; }
    };
    const crd::u32 n     = a.rows();
    const auto*    outer = a.pattern().outer_ptr.data();
    const auto*    inner = a.pattern().inner_idx.data();
    const T*       vals  = a.values().values.data();
    crd::containers::Array<T> x(alloc), y(alloc);
    x.resize(n);
    y.resize(n);
    for (crd::u32 i = 0; i < n; ++i) { x[i] = T(R(1) + R(static_cast<R>(i % 7)) / R(7)); } // D(amg)-4 seed
    R lambda = R(0);
    for (crd::u32 it = 0; it < iters; ++it)
    {
        R nx = R(0);
        for (crd::u32 i = 0; i < n; ++i) { nx += mag2(x[i]); }
        nx = std::sqrt(nx);
        if (nx == R(0)) { break; }
        for (crd::u32 i = 0; i < n; ++i) { x[i] = x[i] / T(nx); }
        // y = D⁻¹ A x
        for (crd::u32 i = 0; i < n; ++i)
        {
            T s = T{};
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q) { s = s + vals[q] * x[inner[q]]; }
            y[i] = s * dinv[i];
        }
        R ny = R(0);
        for (crd::u32 i = 0; i < n; ++i) { ny += mag2(y[i]); }
        lambda = std::sqrt(ny);
        for (crd::u32 i = 0; i < n; ++i) { x[i] = y[i]; }
    }
    return lambda;
}

// Smoothed prolongator  P = (I − ω D⁻¹ A) T = T − ω D⁻¹ (A T). This single
// Jacobi smoothing of the tentative T is what gives SA-AMG its mesh-independent
// convergence (Vaněk 1996). ω = 4/(3·λmax(D⁻¹A)).
template <typename T>
[[nodiscard]] crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>
smoothed_prolongator(const crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>& a,
                     const crd::hesap::sparse::SparseMatrix<T, SaCsr::Csr>& tent,
                     const crd::containers::Array<T>& dinv,
                     crd::hesap::dense::RealType<T> omega, crd::memory::IAllocator* alloc)
{
    const crd::u32 n  = a.rows();
    const crd::u32 nc = tent.cols();
    // A T (n × n_agg).
    auto at = crd::hesap::sparse::spgemm<T>(a, tent, alloc);
    const auto* to = tent.pattern().outer_ptr.data();
    const auto* ti = tent.pattern().inner_idx.data();
    const T*    tv = tent.values().values.data();
    const auto* ao = at.pattern().outer_ptr.data();
    const auto* ai = at.pattern().inner_idx.data();
    const T*    av = at.values().values.data();

    crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, nc);
    for (crd::u32 i = 0; i < n; ++i)
    {
        for (crd::u32 q = to[i]; q < to[i + 1]; ++q) { tb.add(i, ti[q], tv[q]); }      // + T
        const T coef = T(-omega) * dinv[i]; // −ω·(1/a_ii) (complex for complex A)
        for (crd::u32 q = ao[i]; q < ao[i + 1]; ++q) { tb.add(i, ai[q], coef * av[q]); } // − ω D⁻¹ A T
    }
    return tb.compress();
}

} // namespace crd::hesap::amg
