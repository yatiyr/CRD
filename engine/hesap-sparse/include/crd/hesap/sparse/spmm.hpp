#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spmv.hpp>  // detail::spmv_is_zero
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// spmm -- sparse-times-dense: C = alpha * A * B + beta * C, where A is a
// compressed CSR (m x k), and B (k x r) / C (m x r) are DENSE ROW-MAJOR with
// leading dimensions ldb / ldc (>= r) so a caller can spmm into a strided
// block of a larger matrix (block-Krylov / batched solve).
//
// Each A-row is scanned ONCE and its scaled B-row is axpy'd into all r columns
// of C[i,:] (the inner column loop is a contiguous auto-vectorised axpy). The
// C row is initialised once (0 for beta==0 -> NaN-safe; beta*C[i,:] otherwise)
// then accumulated in place -- no per-row temp buffer. Deterministic: fixed
// A-row accumulation order; each C[i,c] is its own ordered sum. Row-parallel
// writes disjoint C rows -> bit-exact with serial at any worker count.
//
// NOTE (honest framing): Eigen's sparse*dense is ALSO one-pass-over-A; the win
// over Eigen is row-PARALLELISM (Eigen-MT is the meaningful gate), not multi-RHS
// reuse (both reuse the A scan).
// -----------------------------------------------------------------------

namespace detail
{
template <typename T>
void spmm_rows(T alpha, const SparseMatrix<T, SparseFormat::Csr>& a, const T* b, crd::u32 ldb, crd::u32 r, T beta,
               T* c, crd::u32 ldc, crd::u32 i_begin, crd::u32 i_end)
{
    const crd::u32* outer = a.pattern().outer_ptr.data();
    const crd::u32* inner = a.pattern().inner_idx.data();
    const T*        av    = a.values().values.data();
    const bool      bzero = detail::spmv_is_zero(beta);

    for (crd::u32 i = i_begin; i < i_end; ++i)
    {
        T* ci = c + static_cast<crd::usize>(i) * ldc;
        if (bzero)
        {
            for (crd::u32 col = 0; col < r; ++col)
            {
                ci[col] = T{};
            }
        }
        else
        {
            for (crd::u32 col = 0; col < r; ++col)
            {
                ci[col] = beta * ci[col];
            }
        }
        for (crd::u32 kk = outer[i]; kk < outer[i + 1]; ++kk)
        {
            const T        aa = alpha * av[kk];
            const T*       bk = b + static_cast<crd::usize>(inner[kk]) * ldb;
            for (crd::u32 col = 0; col < r; ++col)
            {
                ci[col] = ci[col] + aa * bk[col];  // contiguous axpy (auto-vectorised)
            }
        }
    }
}
} // namespace detail

// Serial spmm: C = alpha * A * B + beta * C.
template <typename T>
void spmm(T alpha, const SparseMatrix<T, SparseFormat::Csr>& a, const T* b, crd::u32 ldb, crd::u32 r, T beta, T* c,
          crd::u32 ldc)
{
    CRD_ASSERT_MSG(a.pattern().is_compressed(), "spmm requires a compressed CSR matrix");
    CRD_ASSERT_MSG(ldb >= r && ldc >= r, "spmm: leading dimensions must be >= r");
    detail::spmm_rows<T>(alpha, a, b, ldb, r, beta, c, ldc, 0, a.rows());
}

// Parallel spmm: nnz-balanced A-row partition over crd::jobs; each job writes
// disjoint C rows -> bit-exact with serial spmm at any job count.
template <typename T>
void spmm_parallel(T alpha, const SparseMatrix<T, SparseFormat::Csr>& a, const T* b, crd::u32 ldb, crd::u32 r, T beta,
                   T* c, crd::u32 ldc, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(a.pattern().is_compressed(), "spmm requires a compressed CSR matrix");
    CRD_ASSERT_MSG(ldb >= r && ldc >= r, "spmm: leading dimensions must be >= r");
    const crd::u32 m = a.rows();
    if (m == 0)
    {
        return;
    }
    const crd::u32* outer = a.pattern().outer_ptr.data();
    const crd::u32  nnz   = outer[m];

    crd::u32 jobs = (num_jobs == 0) ? crd::jobs::num_workers() : num_jobs;
    if (jobs == 0)
    {
        jobs = 1;
    }
    jobs = jobs < m ? jobs : m;

    // nnz-balanced row boundaries computed ONCE (not per job).
    crd::u32 bnd_stack[1024 + 1];
    if (jobs > 1024)
    {
        jobs = 1024;
    }
    bnd_stack[0]    = 0;
    bnd_stack[jobs] = m;
    {
        crd::u32 i = 0;
        for (crd::u32 jb = 1; jb < jobs; ++jb)
        {
            const crd::u32 target = static_cast<crd::u32>((static_cast<crd::u64>(nnz) * jb) / jobs);
            while (i < m && outer[i + 1] <= target)
            {
                ++i;
            }
            bnd_stack[jb] = i;
        }
    }

    struct Ctx
    {
        T                                         alpha;
        T                                         beta;
        const SparseMatrix<T, SparseFormat::Csr>* a;
        const T*                                  b;
        T*                                        c;
        crd::u32                                  ldb;
        crd::u32                                  ldc;
        crd::u32                                  r;
        const crd::u32*                           bnd;
    };
    Ctx ctx{alpha, beta, &a, b, c, ldb, ldc, r, bnd_stack};

    auto* counter = crd::jobs::parallel_for(jobs, jobs, [&ctx](crd::u32 j_begin, crd::u32 j_end) {
        for (crd::u32 j = j_begin; j < j_end; ++j)
        {
            const crd::u32 r0 = ctx.bnd[j];
            const crd::u32 r1 = ctx.bnd[j + 1];
            if (r0 < r1)
            {
                detail::spmm_rows<T>(ctx.alpha, *ctx.a, ctx.b, ctx.ldb, ctx.r, ctx.beta, ctx.c, ctx.ldc, r0, r1);
            }
        }
    });
    crd::jobs::wait(counter);
}

} // namespace crd::hesap::sparse
