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
// (only the lower triangle, i ≥ j, is touched). Phase 3.1.6 v0e-perf-attack;
// REBUILT as a triangular-output mirror of the Goto gemm driver at the
// lattice-crush dig (2026-06-11): the original packed the FULL m×k operand
// through elementwise MatrixView::at() — for a ColMajor source with a large
// leading dim that transpose-pack is one cache/TLB miss per element and
// MEASURED 3.4 GF/s standalone (20× under OpenBLAS dsyrk) while the gemm
// driver's pack_a/pack_b run stride-friendly. This version reuses the gemm
// driver's EXACT machinery — pack_a / pack_b, the kGemmMc/Kc/Nc cache
// blocking, gemm_microkernel — and only changes which C tiles are visited
// (strict-upper tiles skipped, diagonal-crossing tiles masked to i ≥ j).
//
// DETERMINISM / VALUE CONTRACT: the per-element reduction is the SAME
// K-grouping as dense::gemm (kKc chunks accumulated into C memory in
// ascending-pc order, zero-initialised micro tile per chunk, the same
// microkernel p-order within a chunk). Negation symmetry is exact in IEEE
// (−(x+y) = (−x)+(−y); a −= b ≡ a += (−b)), so the lower-triangle values
// this produces are BIT-IDENTICAL to computing the full update with
// dense::gemm and subtracting — which is what the supernodal cmod's
// node-parallel path does. Serial-syrk vs parallel-gemm therefore agree
// bit-exactly for ANY k (the old full-k-per-tile reduction diverged from
// the gemm path once k > kGemmKc).
//
// Multi-platform: the microkernel is built on crd::math::simd Vec types
// (scalar / AVX2 / NEON backends) — never raw intrinsics (ADR-0082).
//
// This is a reusable primitive: future eigensolvers, sparse direct, and
// optimization (normal equations AᵀA) all want syrk.
// -----------------------------------------------------------------------

// The inner tile sweep for one (ic, jc, pc) macro block: identical loop structure to gemm_packed_inner,
// but strict-upper tiles are skipped (break: j ascending), fully-lower tiles merge unmasked, and
// diagonal-crossing tiles mask to i ≥ j. C indexing per `col_indexed_out`: false ⇒ the original
// row-major-indexed scratch contract (c_data[i*ldc + j], read back the same way by the cmod scatter);
// true ⇒ ColMajor in-place output (c_data[j*ldc + i] = element (row i, col j) of a ColMajor matrix —
// the supernodal trailing-Schur panel merge; i-inner is the contiguous walk there). The merged VALUE per
// (i ≥ j) pair is identical either way — only the destination address changes.
// `pb_begin/pb_end` bound the visited col-panels so the parallel path can split them across workers
// (each pb writes a disjoint C column range ⇒ race-free). `nc` bounds the real (non-padded) columns —
// the zero-padded tail of the final panel is never merged (it would be a `-= 0` out of the block).
template <typename T>
inline void syrk_packed_inner_lower(crd::usize ic, crd::usize jc, crd::usize mc, crd::usize nc, crd::usize kc,
                                    crd::usize pb_begin, crd::usize pb_end, const T* a_packed, const T* b_packed,
                                    T* c_data, crd::usize ldc, bool col_indexed_out) noexcept
{
    const crd::usize num_panels_m = (mc + GemmTraits<T>::MR - 1) / GemmTraits<T>::MR;
    for (crd::usize pa = 0; pa < num_panels_m; ++pa)
    {
        const T* a_panel = a_packed + pa * GemmTraits<T>::MR * kc;
        const crd::usize i_global = ic + pa * GemmTraits<T>::MR;
        const crd::usize rows_in = std::min(GemmTraits<T>::MR, mc - pa * GemmTraits<T>::MR);
        for (crd::usize pb = pb_begin; pb < pb_end; ++pb)
        {
            const crd::usize j_global = jc + pb * GemmTraits<T>::NR;
            if (i_global + rows_in <= j_global)
            {
                break; // this and all later tiles in the row are strict-upper (j ascending)
            }
            const T* b_panel = b_packed + pb * kc * GemmTraits<T>::NR;
            const crd::usize cols_in = std::min(GemmTraits<T>::NR, nc - pb * GemmTraits<T>::NR);
            T micro[GemmTraits<T>::MR * GemmTraits<T>::NR]; // ZeroInit kernel: no zero pass (identical bits)
            gemm_microkernel<T, true>(kc, a_panel, b_panel, micro, GemmTraits<T>::NR);
            const bool fully_lower = i_global >= j_global + cols_in - 1; // min i ≥ max real j
            if (col_indexed_out)
            {
                for (crd::usize j = 0; j < cols_in; ++j)
                {
                    const crd::usize jg = j_global + j;
                    T* ccol = c_data + jg * ldc;
                    const crd::usize i_lo = fully_lower ? 0 : (jg > i_global ? jg - i_global : 0);
                    for (crd::usize i = i_lo; i < rows_in; ++i)
                    {
                        ccol[i_global + i] -= micro[i * GemmTraits<T>::NR + j];
                    }
                }
            }
            else if (fully_lower)
            {
                for (crd::usize i = 0; i < rows_in; ++i)
                {
                    T* crow = c_data + (i_global + i) * ldc + j_global;
                    const T* mrow = micro + i * GemmTraits<T>::NR;
                    for (crd::usize j = 0; j < cols_in; ++j)
                    {
                        crow[j] -= mrow[j];
                    }
                }
            }
            else // diagonal-crossing: mask to i ≥ j
            {
                for (crd::usize i = 0; i < rows_in; ++i)
                {
                    const crd::usize ig = i_global + i;
                    T* crow = c_data + ig * ldc;
                    const T* mrow = micro + i * GemmTraits<T>::NR;
                    for (crd::usize j = 0; j < cols_in; ++j)
                    {
                        const crd::usize jg = j_global + j;
                        if (ig >= jg)
                        {
                            crow[jg] -= mrow[j];
                        }
                    }
                }
            }
        }
    }
}

// `allow_parallel` (default true): self-parallelize over col-panels when large. Pass FALSE when calling from a hot
// per-block loop that already manages its own parallelism (e.g. the supernodal cmod's per-descendant diagonal
// update — one self-fork per call there would be 1000s of fork/joins). `a` may be RowMajor OR ColMajor: pack_a /
// pack_b read it through the layout-aware fast paths; the C output is always indexed row-major (ldc, i≥j) so the
// caller reads it back the same way regardless of a's layout.
template <typename T, Layout L>
inline void syrk_lower_minus(MatrixView<const T, L> a, MatrixView<T, L> c, crd::memory::IAllocator* scratch,
                             bool allow_parallel = true, bool col_indexed_out = false)
{
    const crd::usize m = a.rows();
    const crd::usize k = a.cols();
    if (m == 0 || k == 0)
    {
        return;
    }

    auto* alloc = (scratch != nullptr) ? scratch : crd::memory::default_allocator();

    // Pack buffers sized like dense::gemm (block-bounded, not full-operand).
    const crd::usize a_dim_m = std::min<crd::usize>(m, kGemmMc);
    const crd::usize a_dim_k = std::min<crd::usize>(k, kGemmKc);
    const crd::usize b_dim_n = std::min<crd::usize>(m, kGemmNc);
    const crd::usize a_pack_capacity =
        ((a_dim_m + GemmTraits<T>::MR - 1) / GemmTraits<T>::MR) * GemmTraits<T>::MR * a_dim_k;
    const crd::usize b_pack_capacity =
        ((b_dim_n + GemmTraits<T>::NR - 1) / GemmTraits<T>::NR) * a_dim_k * GemmTraits<T>::NR;
    const crd::usize align = alignof(T) > 32 ? alignof(T) : 32;
    auto* a_pack = static_cast<T*>(alloc->allocate(a_pack_capacity * sizeof(T), align));
    auto* b_pack = static_cast<T*>(alloc->allocate(b_pack_capacity * sizeof(T), align));

    T* c_data = c.data();
    const crd::usize ldc = c.ld();
    const crd::u32 nw = crd::jobs::num_workers();
    const bool parallel = allow_parallel && (nw > 1) && (m * m * k > crd::usize{256} * 1024);

    for (crd::usize jc = 0; jc < m; jc += kGemmNc)
    {
        const crd::usize nc = (jc + kGemmNc < m) ? kGemmNc : (m - jc);
        for (crd::usize pc = 0; pc < k; pc += kGemmKc)
        {
            const crd::usize kc = (pc + kGemmKc < k) ? kGemmKc : (k - pc);
            // The "B" role is Aᵀ: pack_b with Trans::Transpose reads the effective B(p, j) = A(j, p)
            // through the stride-friendly path (for ColMajor A that read is contiguous in j).
            pack_b(a, pc, jc, kc, nc, Trans::Transpose, b_pack);
            const crd::usize num_panels_n = (nc + GemmTraits<T>::NR - 1) / GemmTraits<T>::NR;

            for (crd::usize ic = 0; ic < m; ic += kGemmMc)
            {
                const crd::usize mc = (ic + kGemmMc < m) ? kGemmMc : (m - ic);
                if (ic + mc <= jc)
                {
                    continue; // the whole macro block is strict-upper
                }
                pack_a(a, ic, pc, mc, kc, Trans::None, a_pack);
                // Only col-panels with j ≤ max i of this block can carry lower-triangle work.
                const crd::usize j_hi = ic + mc; // exclusive upper bound on relevant j
                const crd::usize pb_end =
                    j_hi > jc ? std::min(num_panels_n, (j_hi - jc + GemmTraits<T>::NR - 1) / GemmTraits<T>::NR)
                              : crd::usize{0};
                if (pb_end == 0)
                {
                    continue;
                }
                if (parallel)
                {
                    // Split the col-panels across workers — each pb writes a disjoint C column range,
                    // and WHO computes a tile never changes its value (same packed reduction) ⇒ the
                    // cross-thread determinism contract of gemm_parallel holds here too.
                    struct State
                    {
                        const T* a_pack;
                        const T* b_pack;
                        T* c_data;
                        crd::usize ldc;
                        crd::usize ic, jc, mc, nc, kc;
                        bool col_out;
                    };
                    State st{a_pack, b_pack, c_data, ldc, ic, jc, mc, nc, kc, col_indexed_out};
                    State* sp = &st;
                    auto* counter = crd::jobs::parallel_for(
                        static_cast<crd::u32>(pb_end), nw,
                        [sp](crd::u32 begin, crd::u32 end)
                        {
                            syrk_packed_inner_lower<T>(sp->ic, sp->jc, sp->mc, sp->nc, sp->kc, begin, end, sp->a_pack,
                                                       sp->b_pack, sp->c_data, sp->ldc, sp->col_out);
                        });
                    crd::jobs::wait(counter);
                }
                else
                {
                    syrk_packed_inner_lower<T>(ic, jc, mc, nc, kc, 0, pb_end, a_pack, b_pack, c_data, ldc,
                                               col_indexed_out);
                }
            }
        }
    }

    alloc->deallocate(b_pack);
    alloc->deallocate(a_pack);
}

} // namespace crd::hesap::dense::detail
