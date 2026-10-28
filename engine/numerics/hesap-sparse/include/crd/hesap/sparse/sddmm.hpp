#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_dispatch.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/jobs/jobs.hpp>

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
// The f64 and f32 dots run the level kernels (SddmmRows, crd/hesap/sparse/sparse_kernels.hpp): the Y-row access is a
// random gather, so a single scalar accumulator (a 4-cycle add chain) starves the gathers of in-flight parallelism, and
// the kernels' lane sums break the chain. Two-rounded mul+add per term (NOT a single-rounded FMA; D(sparse)-3 /
// ADR-0063), in one fixed order on every level. A complex dot has no lane type: four scalar partial sums.
template <typename T> struct SddmmRun
{
    const crd::u32* outer;
    const crd::u32* inner;
    const T* x;
    crd::u32 ldx;
    const T* y;
    crd::u32 ldy;
    crd::u32 r;
    T alpha;
    T* out_vals;

    void operator()(crd::u32 i_begin, crd::u32 i_end) const
    {
        if constexpr (std::is_same_v<T, crd::f64> || std::is_same_v<T, crd::f32>)
        {
            const SddmmRows<T> rows{outer, inner, x, y, out_vals, ldx, ldy, r, alpha};
            const SparseKernels& k = *sparse_kernels().table;
            if constexpr (std::is_same_v<T, crd::f64>)
            {
                k.sddmm_rows_f64(rows, i_begin, i_end);
            }
            else
            {
                k.sddmm_rows_f32(rows, i_begin, i_end);
            }
        }
        else
        {
            for (crd::u32 i = i_begin; i < i_end; ++i)
            {
                const T* xi = x + static_cast<crd::usize>(i) * ldx;
                for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
                {
                    const T* yj = y + static_cast<crd::usize>(inner[k]) * ldy;
                    T a0 = T{};
                    T a1 = T{};
                    T a2 = T{};
                    T a3 = T{};
                    crd::u32 col = 0;
                    for (; col + 4 <= r; col += 4)
                    {
                        a0 = a0 + xi[col] * yj[col];
                        a1 = a1 + xi[col + 1] * yj[col + 1];
                        a2 = a2 + xi[col + 2] * yj[col + 2];
                        a3 = a3 + xi[col + 3] * yj[col + 3];
                    }
                    T acc = (a0 + a1) + (a2 + a3);
                    for (; col < r; ++col)
                    {
                        acc = acc + xi[col] * yj[col];
                    }
                    out_vals[k] = alpha * acc;
                }
            }
        }
    }
};
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

    const detail::SddmmRun<T> run{mp.outer_ptr.data(), mp.inner_idx.data(), x, ldx, y, ldy, r, alpha,
                                  vals.values.data()};
    run(0, mp.rows);
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
    constexpr crd::u32 max_jobs = 1024;
    if (jobs > max_jobs)
    {
        jobs = max_jobs;
    }
    crd::u32 bnd[max_jobs + 1];
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
        detail::SddmmRun<T> run;
        const crd::u32* bnd;
    };
    const Ctx ctx{{outer, mp.inner_idx.data(), x, ldx, y, ldy, r, alpha, vals.values.data()}, bnd};

    auto* counter = crd::jobs::parallel_for(jobs, jobs,
                                            [&ctx](crd::u32 jb0, crd::u32 jb1)
                                            {
                                                for (crd::u32 jb = jb0; jb < jb1; ++jb)
                                                {
                                                    ctx.run(ctx.bnd[jb], ctx.bnd[jb + 1]);
                                                }
                                            });
    crd::jobs::wait(counter);

    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

} // namespace crd::hesap::sparse
