#pragma once

#include <crd/core/assert.hpp>
#include <crd/hesap/sparse/sell.hpp>
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::sparse
{
// Max parallel partitions. num_workers*N is well under this in practice; the
// cap lets us nnz-balance the boundaries on the stack with no allocation.
inline constexpr crd::u32 kMaxSpmvJobs = 1024U;

// Parallel SELL spmv: y = alpha * A * x + beta * y. The slice range is split
// into `num_jobs` **nnz-balanced** sub-ranges (each ~equal stored elements, via
// the cumulative slice_ptr), so the wide-slice cluster a σ sort leaves at the
// tail does not overload one worker. Disjoint slice ranges write disjoint
// original rows (perm is a bijection) -> no cross-thread writes -> deterministic
// and BIT-EXACT with serial spmv_sell at any job count.
//
// `num_jobs` 0 => num_workers (one balanced range per worker minimises dispatch
// overhead). Requires crd::jobs to be initialised.
template <typename T>
void spmv_sell_parallel(T alpha, const SellMatrix<T>& a, crd::containers::ConstSpan<T> x, T beta,
                        crd::containers::Span<T> y, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(x.size() == a.cols && y.size() == a.rows, "spmv_sell_parallel: x must be cols(), y must be rows()");
    if (a.num_slices == 0)
    {
        return;
    }

    crd::u32 jobs = (num_jobs == 0) ? crd::jobs::num_workers() : num_jobs;
    if (jobs == 0)
    {
        jobs = 1;
    }
    jobs = jobs < a.num_slices ? jobs : a.num_slices;
    jobs = jobs < kMaxSpmvJobs ? jobs : kMaxSpmvJobs;

    // nnz-balanced slice boundaries: bnd[j]..bnd[j+1] holds ~total/jobs elements.
    const crd::u32* slice_ptr = a.slice_ptr.data();
    const crd::u32  total     = slice_ptr[a.num_slices];
    crd::u32        bnd[kMaxSpmvJobs + 1];
    bnd[0]    = 0;
    bnd[jobs] = a.num_slices;
    crd::u32 s = 0;
    for (crd::u32 j = 1; j < jobs; ++j)
    {
        const crd::u32 target = static_cast<crd::u32>((static_cast<crd::u64>(total) * j) / jobs);
        while (s < a.num_slices && slice_ptr[s] < target)
        {
            ++s;
        }
        bnd[j] = s;
    }

    // Pack args behind one pointer so the job closure stays under the JobDecl SBO.
    struct Ctx
    {
        const SellMatrix<T>* a;
        T                    alpha;
        T                    beta;
        const T*             xp;
        T*                   yp;
        const crd::u32*      bnd;
    };
    Ctx ctx{&a, alpha, beta, x.data(), y.data(), bnd};

    // count == jobs == num_jobs: each invocation owns one job index -> its
    // nnz-balanced [bnd[j], bnd[j+1]) slice range.
    auto* counter = crd::jobs::parallel_for(jobs, jobs, [&ctx](crd::u32 j_begin, crd::u32 j_end) {
        for (crd::u32 j = j_begin; j < j_end; ++j)
        {
            detail::sell_spmv_range<T>(*ctx.a, ctx.alpha, ctx.beta, ctx.xp, ctx.yp, ctx.bnd[j], ctx.bnd[j + 1]);
        }
    });
    crd::jobs::wait(counter);
}

} // namespace crd::hesap::sparse
