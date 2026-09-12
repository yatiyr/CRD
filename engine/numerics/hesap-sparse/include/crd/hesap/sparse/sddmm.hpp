#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <type_traits>
#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SDDMM -- Sampled Dense-Dense Matrix Multiply (GNN / attention). Samples a
// dense product at a sparse mask's nonzeros: given mask M (m x n, only its
// PATTERN is used), X (m x r, row-major), Y (n x r, row-major),
//
//     C = alpha * sample(X * Yᵀ, M)  =>  C[i,j] = alpha * dot(X[i,:], Y[j,:])
//                                        for each (i,j) in pattern(M).
//
// C has M's exact pattern; only the masked dots are computed (the whole point
// -- never form the dense X*Yᵀ). Deterministic: each output entry is one
// fixed-order length-r dot; entries are independent -> row-parallel writes
// disjoint output slots -> bit-exact vs serial at any worker count.
//
// No Eigen equivalent (SDDMM is not an Eigen op): correctness + perf
// characterisation, no Eigen gate.
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
void sddmm_rows(const SparseMatrix<T, SparseFormat::Csr>& mask, const T* x, crd::u32 ldx, const T* y, crd::u32 ldy,
                crd::u32 r, T alpha, T* out_vals, crd::u32 i_begin, crd::u32 i_end)
{
    const crd::u32* outer = mask.pattern().outer_ptr.data();
    const crd::u32* inner = mask.pattern().inner_idx.data();
    for (crd::u32 i = i_begin; i < i_end; ++i)
    {
        const T* xi = x + static_cast<crd::usize>(i) * ldx;
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            const T* yj = y + static_cast<crd::usize>(inner[k]) * ldy;
            // The dot is the hot loop and the Y-row access is a random gather, so a
            // single scalar accumulator (4-cycle FP-add latency chain) starves the
            // gathers of in-flight parallelism. Explicit SIMD with multiple lane
            // accumulators breaks the chain (mirrors the SELL kernel that beat
            // Eigen). Two-rounded mul+add per term (NOT a single-rounded FMA;
            // D(sparse)-3 / ADR-0063). Vec4d/Vec8f reduce via a store-then-scalar
            // `horizontal_sum` -> the SAME fixed order on AVX2 and the scalar
            // fallback build, so the result is bit-exact across SIMD widths.
            T acc;
            if constexpr (std::is_same_v<T, crd::f64>)
            {
                using V    = crd::math::simd::Vec4d;
                V        a0 = V::zero();
                V        a1 = V::zero();
                crd::u32 col = 0;
                for (; col + 8 <= r; col += 8)
                {
                    a0 = a0 + V::load(xi + col) * V::load(yj + col);
                    a1 = a1 + V::load(xi + col + 4) * V::load(yj + col + 4);
                }
                acc = crd::math::simd::horizontal_sum(a0 + a1);
                for (; col + 4 <= r; col += 4)
                {
                    acc = acc + crd::math::simd::horizontal_sum(V::load(xi + col) * V::load(yj + col));
                }
                for (; col < r; ++col)
                {
                    acc = acc + xi[col] * yj[col];
                }
            }
            else if constexpr (std::is_same_v<T, crd::f32>)
            {
                using V    = crd::math::simd::Vec8f;
                V        a0 = V::zero();
                crd::u32 col = 0;
                for (; col + 8 <= r; col += 8)
                {
                    a0 = a0 + V::load(xi + col) * V::load(yj + col);
                }
                acc = crd::math::simd::horizontal_sum(a0);
                for (; col < r; ++col)
                {
                    acc = acc + xi[col] * yj[col];
                }
            }
            else  // complex / other: four scalar partial sums (no SIMD lane type)
            {
                T        a0  = T{};
                T        a1  = T{};
                T        a2  = T{};
                T        a3  = T{};
                crd::u32 col = 0;
                for (; col + 4 <= r; col += 4)
                {
                    a0 = a0 + xi[col] * yj[col];
                    a1 = a1 + xi[col + 1] * yj[col + 1];
                    a2 = a2 + xi[col + 2] * yj[col + 2];
                    a3 = a3 + xi[col + 3] * yj[col + 3];
                }
                acc = (a0 + a1) + (a2 + a3);
                for (; col < r; ++col)
                {
                    acc = acc + xi[col] * yj[col];
                }
            }
            out_vals[k] = alpha * acc;
        }
    }
}
} // namespace detail

// Serial SDDMM: C = alpha * sample(X * Yᵀ, mask). C carries mask's pattern.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> sddmm(const SparseMatrix<T, SparseFormat::Csr>& mask, const T* x,
                                                       crd::u32 ldx, const T* y, crd::u32 ldy, crd::u32 r, T alpha,
                                                       crd::memory::IAllocator* alloc)
{
    const SparsePattern& mp = mask.pattern();
    CRD_ASSERT_MSG(mp.is_compressed(), "sddmm requires a compressed CSR mask");
    CRD_ASSERT_MSG(ldx >= r && ldy >= r, "sddmm: leading dimensions must be >= r");

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = mp.rows;
    pat.cols       = mp.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    const crd::u32 nnz = static_cast<crd::u32>(mp.nnz());
    pat.outer_ptr.resize(mp.outer_ptr.size());
    for (crd::usize i = 0; i < mp.outer_ptr.size(); ++i)
    {
        pat.outer_ptr[i] = mp.outer_ptr[i];
    }
    pat.inner_idx.resize_uninitialized(nnz);
    for (crd::u32 k = 0; k < nnz; ++k)
    {
        pat.inner_idx[k] = mp.inner_idx[k];
    }
    vals.values.resize_uninitialized(nnz);

    detail::sddmm_rows<T>(mask, x, ldx, y, ldy, r, alpha, vals.values.data(), 0, mp.rows);
    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// Parallel SDDMM: nnz-balanced mask-row partition; disjoint output entries ->
// bit-exact with serial at any job count.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> sddmm_parallel(const SparseMatrix<T, SparseFormat::Csr>& mask,
                                                                const T* x, crd::u32 ldx, const T* y, crd::u32 ldy,
                                                                crd::u32 r, T alpha, crd::memory::IAllocator* alloc,
                                                                crd::u32 num_jobs = 0)
{
    const SparsePattern& mp = mask.pattern();
    CRD_ASSERT_MSG(mp.is_compressed(), "sddmm requires a compressed CSR mask");
    CRD_ASSERT_MSG(ldx >= r && ldy >= r, "sddmm: leading dimensions must be >= r");
    const crd::u32 m   = mp.rows;
    const crd::u32 nnz = static_cast<crd::u32>(mp.nnz());

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = mp.rows;
    pat.cols       = mp.cols;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(mp.outer_ptr.size());
    for (crd::usize i = 0; i < mp.outer_ptr.size(); ++i)
    {
        pat.outer_ptr[i] = mp.outer_ptr[i];
    }
    pat.inner_idx.resize_uninitialized(nnz);
    for (crd::u32 k = 0; k < nnz; ++k)
    {
        pat.inner_idx[k] = mp.inner_idx[k];
    }
    vals.values.resize_uninitialized(nnz);

    if (m == 0)
    {
        pat.recompute_topology_hash();
        return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
    }

    const crd::u32* outer = mp.outer_ptr.data();
    crd::u32        jobs   = (num_jobs == 0) ? crd::jobs::num_workers() : num_jobs;
    if (jobs == 0)
    {
        jobs = 1;
    }
    jobs = jobs < m ? jobs : m;
    constexpr crd::u32 kMaxJobs = 1024;
    if (jobs > kMaxJobs)
    {
        jobs = kMaxJobs;
    }
    crd::u32 bnd[kMaxJobs + 1];
    bnd[0]    = 0;
    bnd[jobs] = m;
    {
        crd::u32 i = 0;
        for (crd::u32 jb = 1; jb < jobs; ++jb)
        {
            const crd::u32 target = static_cast<crd::u32>((static_cast<crd::u64>(nnz) * jb) / jobs);
            while (i < m && outer[i + 1] <= target)
            {
                ++i;
            }
            bnd[jb] = i;
        }
    }

    struct Ctx
    {
        const SparseMatrix<T, SparseFormat::Csr>* mask;
        const T*                                  x;
        const T*                                  y;
        crd::u32                                  ldx, ldy, r;
        T                                         alpha;
        T*                                        out;
        const crd::u32*                           bnd;
    };
    Ctx ctx{&mask, x, y, ldx, ldy, r, alpha, vals.values.data(), bnd};

    auto* counter = crd::jobs::parallel_for(jobs, jobs, [&ctx](crd::u32 jb0, crd::u32 jb1) {
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            detail::sddmm_rows<T>(*ctx.mask, ctx.x, ctx.ldx, ctx.y, ctx.ldy, ctx.r, ctx.alpha, ctx.out, ctx.bnd[jb],
                                  ctx.bnd[jb + 1]);
        }
    });
    crd::jobs::wait(counter);

    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
