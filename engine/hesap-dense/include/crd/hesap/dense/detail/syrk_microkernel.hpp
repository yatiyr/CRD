#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/gemm_microkernel.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Packed register-tiled SYRK — symmetric rank-k update of the LOWER
// triangle: C := C - A · Aᵀ, where A is m × k and C is m × m
// (only the lower triangle, i ≥ j, is touched). Phase 3.1.6 v0e-perf-attack.
//
// Reuses the GEMM register-tiled microkernel (`gemm_microkernel<T>`) +
// the BLIS pack layout. The A operand is packed ONCE into both:
//   - a_pack: MR-row panels (the "A" role: rows i)
//   - b_pack: NR-col panels of Aᵀ (the "B" role: B[p,c] = A[c,p], rows j)
// Then we iterate only the LOWER-triangle MR×NR tiles of C — half the
// FLOPs of a full gemm. Diagonal tiles are masked to i ≥ j.
//
// Multi-platform: the microkernel is built on crd::math::simd Vec types
// (scalar / AVX2 / NEON backends) — never raw intrinsics (ADR-0082).
//
// This is a reusable primitive: future eigensolvers, sparse direct, and
// optimization (normal equations AᵀA) all want syrk.
// -----------------------------------------------------------------------

template <typename T, Layout L>
inline void syrk_lower_minus(MatrixView<const T, L> a, MatrixView<T, L> c,
                             crd::memory::IAllocator* scratch)
{
    static_assert(L == Layout::RowMajor, "syrk_lower_minus supports RowMajor only");
    const crd::usize m = a.rows();
    const crd::usize k = a.cols();
    if (m == 0 || k == 0)
    {
        return;
    }

    auto* alloc = (scratch != nullptr) ? scratch : crd::memory::default_allocator();

    // Pack the full A panel once (m rows, k cols) into MR-row panels.
    // a_pack layout: ceil(m/MR) panels, each MR × k row-major.
    const crd::usize num_m_panels = (m + kGemmMr - 1) / kGemmMr;
    const crd::usize num_n_panels = (m + kGemmNr - 1) / kGemmNr;
    crd::containers::Array<T> a_pack(alloc);
    a_pack.resize(num_m_panels * kGemmMr * k);
    crd::containers::Array<T> b_pack(alloc);
    b_pack.resize(num_n_panels * k * kGemmNr);

    // a_pack[panel][i_local * k + p] = A[panel*MR + i_local, p]
    for (crd::usize panel = 0; panel < num_m_panels; ++panel)
    {
        T* panel_out = a_pack.data() + panel * kGemmMr * k;
        for (crd::usize il = 0; il < kGemmMr; ++il)
        {
            const crd::usize ig = panel * kGemmMr + il;
            const bool inside = ig < m;
            for (crd::usize p = 0; p < k; ++p)
            {
                panel_out[il * k + p] = inside ? a.at(ig, p) : T{0};
            }
        }
    }
    // b_pack[panel][p * NR + jl] = Aᵀ[p, panel*NR + jl] = A[panel*NR + jl, p]
    for (crd::usize panel = 0; panel < num_n_panels; ++panel)
    {
        T* panel_out = b_pack.data() + panel * k * kGemmNr;
        for (crd::usize p = 0; p < k; ++p)
        {
            for (crd::usize jl = 0; jl < kGemmNr; ++jl)
            {
                const crd::usize jg = panel * kGemmNr + jl;
                const bool inside = jg < m;
                panel_out[p * kGemmNr + jl] = inside ? a.at(jg, p) : T{0};
            }
        }
    }

    // State captured by a single pointer (parallel_for's 41-byte SBO limit;
    // see memory/feedback_jobs_worker_index_aliasing). One block-row of
    // lower-triangle tiles writes a disjoint band of C rows → safe per worker.
    struct State
    {
        const T* a_pack;
        const T* b_pack;
        T* c_data;
        crd::usize ldc;
        crd::usize m;
        crd::usize k;
        crd::usize num_n_panels;
    };
    State st{a_pack.data(), b_pack.data(), c.data(), c.ld(), m, k, num_n_panels};
    State* sp = &st;

    auto block_row = [sp](crd::usize pa) noexcept
    {
        const T* a_panel = sp->a_pack + pa * kGemmMr * sp->k;
        const crd::usize i_base = pa * kGemmMr;
        const crd::usize rows_in = std::min(kGemmMr, sp->m - i_base);
        for (crd::usize pb = 0; pb < sp->num_n_panels; ++pb)
        {
            const crd::usize j_base = pb * kGemmNr;
            if (i_base + rows_in <= j_base)
            {
                break;  // remaining tiles are strict-upper
            }
            const T* b_panel = sp->b_pack + pb * sp->k * kGemmNr;
            const crd::usize cols_in = std::min(kGemmNr, sp->m - j_base);
            T micro[kGemmMr * kGemmNr]{};
            gemm_microkernel<T>(sp->k, a_panel, b_panel, micro, kGemmNr);
            for (crd::usize i = 0; i < rows_in; ++i)
            {
                const crd::usize ig = i_base + i;
                for (crd::usize j = 0; j < cols_in; ++j)
                {
                    const crd::usize jg = j_base + j;
                    if (ig >= jg)
                    {
                        sp->c_data[ig * sp->ldc + jg] -= micro[i * kGemmNr + j];
                    }
                }
            }
        }
    };

    // Parallelize over block-rows when the update is large enough to amortize
    // fork/join (mirrors gemm_parallel_auto's mnk threshold).
    const crd::u32 nw = crd::jobs::num_workers();
    const bool parallel = (nw > 1) && (m * m * k > crd::usize{256} * 1024);
    if (parallel)
    {
        auto* counter = crd::jobs::parallel_for(
            static_cast<crd::u32>(num_m_panels), nw,
            [block_row](crd::u32 begin, crd::u32 end)
            {
                for (crd::u32 pa = begin; pa < end; ++pa)
                {
                    block_row(static_cast<crd::usize>(pa));
                }
            });
        crd::jobs::wait(counter);
    }
    else
    {
        for (crd::usize pa = 0; pa < num_m_panels; ++pa)
        {
            block_row(pa);
        }
    }
}

} // namespace crd::hesap::dense::detail
