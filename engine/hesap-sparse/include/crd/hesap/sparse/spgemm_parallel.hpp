#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/spgemm_hash.hpp>
#include <crd/jobs/jobs.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// Parallel hash-accumulator spgemm (cols > kMaxSpaCols). Same two-phase
// structure as the dense path but per-JOB hash accumulators (bounded memory,
// O(row distinct nnz)) instead of a per-worker dense SPA sized by B.cols.
// Bit-exact with serial spgemm (identical per-row accumulation order + sorted
// emit). `jobs` is small (~workers), so per-job hashes are built once up front.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> spgemm_parallel_hash(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                                      const SparseMatrix<T, SparseFormat::Csr>& b,
                                                                      crd::memory::IAllocator* alloc, crd::u32 jobs)
{
    const crd::u32  m  = a.rows();
    const crd::u32  n  = b.cols();
    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();

    crd::containers::Array<crd::u64> flop_prefix(alloc);
    flop_prefix.resize(static_cast<crd::usize>(m) + 1);
    flop_prefix[0] = 0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        crd::u64 f = 0;
        for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
        {
            f += bo[ai[ka] + 1] - bo[ai[ka]];
        }
        flop_prefix[i + 1] = flop_prefix[i] + f;
    }
    const crd::u64 total_flop = flop_prefix[m];

    crd::containers::Array<crd::u32> bnd(alloc);
    bnd.resize(static_cast<crd::usize>(jobs) + 1);
    bnd[0]    = 0;
    bnd[jobs] = m;
    {
        crd::u32 i = 0;
        for (crd::u32 jb = 1; jb < jobs; ++jb)
        {
            const crd::u64 target = (total_flop * jb) / jobs;
            while (i < m && flop_prefix[i] < target)
            {
                ++i;
            }
            bnd[jb] = i;
        }
    }

    // Per-job hash accumulators (each touched by exactly one job at a time).
    crd::containers::Array<detail::SpgemmHash<T>> hashes(alloc);
    hashes.reserve(jobs);
    for (crd::u32 jb = 0; jb < jobs; ++jb)
    {
        hashes.push_back(detail::SpgemmHash<T>(alloc));
        // Pre-size to the job's max per-row flop bound (single-threaded) so the
        // parallel phases never touch the (non-thread-safe) allocator.
        crd::u32 max_ub = 0;
        for (crd::u32 i = bnd[jb]; i < bnd[jb + 1]; ++i)
        {
            const crd::u32 ub = detail::spgemm_row_flop_ub<T>(a, b, i);
            max_ub            = ub > max_ub ? ub : max_ub;
        }
        hashes[jb].preinit(max_ub);
    }

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = m;
    pat.cols       = n;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(m) + 1);
    pat.outer_ptr[0] = 0;

    struct Ctx
    {
        const SparseMatrix<T, SparseFormat::Csr>* a;
        const SparseMatrix<T, SparseFormat::Csr>* b;
        detail::SpgemmHash<T>*                    hashes;
        crd::u32*                                 outer;
        crd::u32*                                 inner;
        T*                                        cvals;
        const crd::u32*                           bnd;
    };

    // ---- Phase 1: symbolic distinct-column counts ----
    Ctx c1{&a, &b, hashes.data(), pat.outer_ptr.data(), nullptr, nullptr, bnd.data()};
    auto* cnt1 = crd::jobs::parallel_for(jobs, jobs, [&c1](crd::u32 jb0, crd::u32 jb1) {
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            detail::SpgemmHash<T>& h = c1.hashes[jb];
            for (crd::u32 i = c1.bnd[jb]; i < c1.bnd[jb + 1]; ++i)
            {
                c1.outer[i + 1] = detail::spgemm_row_hash_count<T>(*c1.a, *c1.b, i, h);
            }
        }
    });
    crd::jobs::wait(cnt1);

    for (crd::u32 i = 0; i < m; ++i)
    {
        pat.outer_ptr[i + 1] += pat.outer_ptr[i];
    }
    const crd::u32 nnz = pat.outer_ptr[m];
    pat.inner_idx.resize_uninitialized(nnz);
    vals.values.resize_uninitialized(nnz);

    // ---- Phase 2: numeric accumulate + sorted write into disjoint slices ----
    Ctx c2{&a, &b, hashes.data(), pat.outer_ptr.data(), pat.inner_idx.data(), vals.values.data(), bnd.data()};
    auto* cnt2 = crd::jobs::parallel_for(jobs, jobs, [&c2](crd::u32 jb0, crd::u32 jb1) {
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            detail::SpgemmHash<T>& h = c2.hashes[jb];
            for (crd::u32 i = c2.bnd[jb]; i < c2.bnd[jb + 1]; ++i)
            {
                const crd::u32 w0 = c2.outer[i];
                detail::spgemm_row_hash_numeric<T>(*c2.a, *c2.b, i, h, c2.inner + w0, c2.cvals + w0);
            }
        }
    });
    crd::jobs::wait(cnt2);

    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// Parallel spgemm C = A * B. Two-phase (required for parallel disjoint writes):
//   symbolic -- per C-row distinct-column COUNT (SPA marker, no values) ->
//               prefix-sum -> C.outer_ptr (each row's output slice known
//               before writing);
//   numeric  -- per-row SPA accumulate (same fixed A-row/B-row order as serial)
//               + column-sorted write into the row's preallocated slice.
// Row-parallel over flop-balanced A-row ranges; each C row is produced by one
// worker -> BIT-EXACT with serial spgemm at any worker count. Per-WORKER SPA
// scratch (sized by num_workers, indexed by worker_index) reused across rows.
// Phase-1 stamps are [1,m]; phase-2 stamps are [m+1,2m] so the shared marker
// scratch never mis-detects a first touch across the two passes.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> spgemm_parallel(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                                 const SparseMatrix<T, SparseFormat::Csr>& b,
                                                                 crd::memory::IAllocator* alloc, crd::u32 num_jobs = 0)
{
    CRD_ASSERT_MSG(a.pattern().is_compressed() && b.pattern().is_compressed(), "spgemm requires compressed CSR");
    CRD_ASSERT_MSG(a.cols() == b.rows(), "spgemm: inner dimension mismatch");
    const crd::u32 m = a.rows();
    const crd::u32 n = b.cols();
    if (m == 0)
    {
        return SparseMatrix<T, SparseFormat::Csr>(alloc);
    }

    const crd::u32 workers = crd::jobs::num_workers() == 0 ? 1U : crd::jobs::num_workers();
    crd::u32       jobs    = (num_jobs == 0) ? workers : num_jobs;
    jobs = jobs < m ? jobs : m;

    if (n > kMaxSpaCols)
    {
        // Dense SPA (sized by B.cols) is memory-infeasible -> hash accumulator.
        return spgemm_parallel_hash<T>(a, b, alloc, jobs);
    }

    const crd::u32* ao = a.pattern().outer_ptr.data();
    const crd::u32* ai = a.pattern().inner_idx.data();
    const crd::u32* bo = b.pattern().outer_ptr.data();

    // flop[i] = sum_{k in A[i]} nnz(B[k]); prefix for flop-balanced partition.
    crd::containers::Array<crd::u64> flop_prefix(alloc);
    flop_prefix.resize(static_cast<crd::usize>(m) + 1);
    flop_prefix[0] = 0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        crd::u64 f = 0;
        for (crd::u32 ka = ao[i]; ka < ao[i + 1]; ++ka)
        {
            f += bo[ai[ka] + 1] - bo[ai[ka]];
        }
        flop_prefix[i + 1] = flop_prefix[i] + f;
    }
    const crd::u64 total_flop = flop_prefix[m];

    crd::containers::Array<crd::u32> bnd(alloc);
    bnd.resize(static_cast<crd::usize>(jobs) + 1);
    bnd[0]    = 0;
    bnd[jobs] = m;
    {
        crd::u32 i = 0;
        for (crd::u32 jb = 1; jb < jobs; ++jb)
        {
            const crd::u64 target = (total_flop * jb) / jobs;
            while (i < m && flop_prefix[i] < target)
            {
                ++i;
            }
            bnd[jb] = i;
        }
    }

    // Per-worker SPA scratch.
    crd::containers::Array<T>        spa_val(alloc);
    crd::containers::Array<crd::u32> marker(alloc);
    spa_val.resize(static_cast<crd::usize>(workers) * n);
    marker.resize(static_cast<crd::usize>(workers) * n);  // 0 == untouched

    SparsePattern   pat(alloc);
    SparseValues<T> vals(alloc);
    pat.rows       = m;
    pat.cols       = n;
    pat.format     = SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(m) + 1);

    // ---- Phase 1: symbolic counts into outer_ptr[i+1] ----
    struct Ctx1
    {
        const SparseMatrix<T, SparseFormat::Csr>* a;
        const SparseMatrix<T, SparseFormat::Csr>* b;
        crd::u32                                  n;
        crd::u32*                                 marker;
        crd::u32*                                 outer;
        const crd::u32*                           bnd;
    };
    Ctx1 c1{&a, &b, n, marker.data(), pat.outer_ptr.data(), bnd.data()};
    auto* cnt1 = crd::jobs::parallel_for(jobs, jobs, [&c1](crd::u32 jb0, crd::u32 jb1) {
        const crd::u32  w   = crd::jobs::worker_index();
        crd::u32*       mk  = c1.marker + static_cast<crd::usize>(w) * c1.n;
        const crd::u32* ao2 = c1.a->pattern().outer_ptr.data();
        const crd::u32* ai2 = c1.a->pattern().inner_idx.data();
        const crd::u32* bo2 = c1.b->pattern().outer_ptr.data();
        const crd::u32* bi2 = c1.b->pattern().inner_idx.data();
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            for (crd::u32 i = c1.bnd[jb]; i < c1.bnd[jb + 1]; ++i)
            {
                const crd::u32 stamp = i + 1U;
                crd::u32       count = 0;
                for (crd::u32 ka = ao2[i]; ka < ao2[i + 1]; ++ka)
                {
                    const crd::u32 k = ai2[ka];
                    for (crd::u32 kb = bo2[k]; kb < bo2[k + 1]; ++kb)
                    {
                        const crd::u32 j = bi2[kb];
                        if (mk[j] != stamp)
                        {
                            mk[j] = stamp;
                            ++count;
                        }
                    }
                }
                c1.outer[i + 1] = count;
            }
        }
    });
    crd::jobs::wait(cnt1);

    pat.outer_ptr[0] = 0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        pat.outer_ptr[i + 1] += pat.outer_ptr[i];
    }
    const crd::u32 nnz = pat.outer_ptr[m];
    pat.inner_idx.resize_uninitialized(nnz);
    vals.values.resize_uninitialized(nnz);

    // ---- Phase 2: numeric accumulate + sorted write into disjoint slices ----
    struct Ctx2
    {
        const SparseMatrix<T, SparseFormat::Csr>* a;
        const SparseMatrix<T, SparseFormat::Csr>* b;
        crd::u32                                  n;
        crd::u32                                  m;
        T*                                        spa;
        crd::u32*                                 marker;
        const crd::u32*                           outer;
        crd::u32*                                 inner;
        T*                                        cvals;
        const crd::u32*                           bnd;
    };
    Ctx2 c2{&a, &b, n, m, spa_val.data(), marker.data(), pat.outer_ptr.data(),
            pat.inner_idx.data(), vals.values.data(), bnd.data()};
    auto* cnt2 = crd::jobs::parallel_for(jobs, jobs, [&c2](crd::u32 jb0, crd::u32 jb1) {
        const crd::u32  w   = crd::jobs::worker_index();
        T*              sv  = c2.spa + static_cast<crd::usize>(w) * c2.n;
        crd::u32*       mk  = c2.marker + static_cast<crd::usize>(w) * c2.n;
        const crd::u32* ao2 = c2.a->pattern().outer_ptr.data();
        const crd::u32* ai2 = c2.a->pattern().inner_idx.data();
        const T*        av2 = c2.a->values().values.data();
        const crd::u32* bo2 = c2.b->pattern().outer_ptr.data();
        const crd::u32* bi2 = c2.b->pattern().inner_idx.data();
        const T*        bv2 = c2.b->values().values.data();
        for (crd::u32 jb = jb0; jb < jb1; ++jb)
        {
            for (crd::u32 i = c2.bnd[jb]; i < c2.bnd[jb + 1]; ++i)
            {
                const crd::u32 stamp = c2.m + 1U + i;  // distinct from phase-1 stamps
                const crd::u32 w0    = c2.outer[i];
                const crd::u32 w1    = c2.outer[i + 1];
                crd::u32       wp    = w0;
                for (crd::u32 ka = ao2[i]; ka < ao2[i + 1]; ++ka)
                {
                    const crd::u32 k    = ai2[ka];
                    const T        aval = av2[ka];
                    for (crd::u32 kb = bo2[k]; kb < bo2[k + 1]; ++kb)
                    {
                        const crd::u32 j    = bi2[kb];
                        const T        prod = aval * bv2[kb];
                        if (mk[j] != stamp)
                        {
                            mk[j]       = stamp;
                            sv[j]       = prod;
                            c2.inner[wp] = j;
                            ++wp;
                        }
                        else
                        {
                            sv[j] = sv[j] + prod;
                        }
                    }
                }
                crd::containers::sort(c2.inner + w0, c2.inner + w1);
                for (crd::u32 t = w0; t < w1; ++t)
                {
                    c2.cvals[t] = sv[c2.inner[t]];
                }
            }
        }
    });
    crd::jobs::wait(cnt2);

    pat.recompute_topology_hash();
    return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// C = A * Aᵀ in parallel (transpose + parallel spgemm).
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> spgemm_ata_parallel(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                                     crd::memory::IAllocator* alloc,
                                                                     crd::u32 num_jobs = 0)
{
    return spgemm_parallel(a, transpose(a, alloc), alloc, num_jobs);
}

} // namespace crd::hesap::sparse
