#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/amg/detail/amg_arms.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/jobs/jobs.hpp>

namespace crd::hesap::amg::detail
{
template <typename T> struct CsrApply
{
    const crd::u32* outer;
    const crd::u32* inner;
    const T* vals;
    const T* x;
    T* y;
    const T* scale;
    crd::u32 n;
    crd::u32 jobs;

    void rows(crd::u32 i0, crd::u32 i1) const
    {
        for (crd::u32 i = i0; i < i1; ++i)
        {
            T s = T{};
            for (crd::u32 q = outer[i]; q < outer[i + 1]; ++q)
            {
                s = s + vals[q] * x[inner[q]];
            }
            y[i] = scale != nullptr ? s * scale[i] : s;
        }
    }

    // Job jb's first row: the first whose entries start at or past jb / jobs of them (the last job ends at n, so
    // trailing empty rows are written too).
    [[nodiscard]] crd::u32 job_begin(crd::u32 jb) const
    {
        if (jb == jobs)
        {
            return n;
        }
        const crd::u64 target = static_cast<crd::u64>(outer[n]) * jb / jobs;
        crd::u32 lo = 0;
        crd::u32 hi = n;
        while (lo < hi)
        {
            const crd::u32 mid = lo + (hi - lo) / 2;
            if (outer[mid] < target)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }
};

// y = A x, or y_i = (A x)_i scale_i when `scale` is set (the power iteration's D^-1 A x); each row a left-to-right sum
// in stored order. From pick.spmv_min_nnz stored entries, on a pool that is up and more than one job, the rows split
// into ranges of balanced entries, one per job, each row on one job, so every setting gives the serial bits.
template <typename T>
void csr_apply(const AmgPick& pick, const crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>& a,
               const T* x, T* y, const T* scale = nullptr)
{
    const crd::u32 n = a.rows();
    const crd::u32 workers = crd::jobs::num_workers();
    crd::u32 jobs = pick.spmv_workers == 0 ? workers : pick.spmv_workers;
    jobs = jobs < n ? jobs : n;
    const CsrApply<T> c{
        a.pattern().outer_ptr.data(), a.pattern().inner_idx.data(), a.values().values.data(), x, y, scale, n, jobs};
    if (a.nnz() < pick.spmv_min_nnz || workers == 0 || jobs <= 1)
    {
        c.rows(0, n);
        return;
    }
    auto* counter = crd::jobs::parallel_for(jobs, jobs,
                                            [&c](crd::u32 jb0, crd::u32 jb1)
                                            {
                                                for (crd::u32 jb = jb0; jb < jb1; ++jb)
                                                {
                                                    c.rows(c.job_begin(jb), c.job_begin(jb + 1));
                                                }
                                            });
    crd::jobs::wait(counter);
}
} // namespace crd::hesap::amg::detail
