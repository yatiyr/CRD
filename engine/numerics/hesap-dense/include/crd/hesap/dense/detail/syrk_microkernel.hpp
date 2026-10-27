#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/gemm_pack.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/construct.hpp> // allocate_array_or_fatal (DIAG.3a)

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
// blocking, the GEMM block — and only changes which C tiles are visited
// (strict-upper tiles skipped, diagonal-crossing tiles masked to i ≥ j).
//
// DETERMINISM / VALUE CONTRACT: the per-element reduction is the SAME
// K-grouping as dense::gemm (kKc chunks accumulated into C memory in
// ascending-pc order, each tile summed from zero per chunk, the same
// p-order within a chunk). Negation symmetry is exact in IEEE
// (−(x+y) = (−x)+(−y); a −= b ≡ a += (−b)), so the lower-triangle values
// this produces are BIT-IDENTICAL to computing the full update with
// dense::gemm and subtracting — which is what the supernodal cmod's
// node-parallel path does. Serial-syrk vs parallel-gemm therefore agree
// bit-exactly for ANY k (the old full-k-per-tile reduction diverged from
// the gemm path once k > kGemmKc).
//
// The block runs on the kernel level the process selects
// (crd/hesap/dense/dense_kernels.hpp, ADR-0134).
//
// This is a reusable primitive: future eigensolvers, sparse direct, and
// optimization (normal equations AᵀA) all want syrk.
// -----------------------------------------------------------------------

// The inner tile sweep for one (ic, jc, pc) macro block: gemm_packed_inner's block subtracted (C -= A Aᵀ) and
// restricted to the lower triangle (GemmBlock::lower: strict-upper tiles are skipped, tiles that cross the diagonal
// keep i >= j). C indexing per `col_indexed_out`: false => the row-major-indexed scratch contract (c_data[i*ldc + j],
// read back the same way by the cmod scatter); true => ColMajor in-place output (c_data[j*ldc + i], the supernodal
// trailing-Schur panel merge). The merged value per (i >= j) pair is the same either way. `pb_begin/pb_end` bound the
// visited col-panels of nr columns (the level's tile width) so the parallel path can split them across workers (each
// pb writes a disjoint C column range => race-free). `nc` bounds the real columns; the zero-padded tail of the final
// panel is never merged.
template <typename T>
inline void syrk_packed_inner_lower(crd::usize ic, crd::usize jc, crd::usize mc, crd::usize nc, crd::usize kc,
                                    crd::usize pb_begin, crd::usize pb_end, crd::usize nr, const T* a_packed,
                                    const T* b_packed, T* c_data, crd::usize ldc, bool col_indexed_out) noexcept
{
    const crd::usize col_first = pb_begin * nr;
    const crd::usize col_end = std::min(nc, pb_end * nr);
    if (col_end <= col_first)
    {
        return;
    }
    const crd::usize col0 = jc + col_first;
    T* corner = col_indexed_out ? c_data + col0 * ldc + ic : c_data + ic * ldc + col0;
    run_gemm_block(GemmBlock<T>{.a = a_packed,
                                .b = b_packed + col_first * kc,
                                .c = corner,
                                .lda = kc,
                                .ldc = ldc,
                                .rows = mc,
                                .cols = col_end - col_first,
                                .depth = kc,
                                .row0 = ic,
                                .col0 = col0,
                                .alpha = T{},
                                .update = GemmUpdate::Subtract,
                                .col_major = col_indexed_out,
                                .lower = true});
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
    const GemmShape shape = gemm_shape_of<T>();
    const crd::usize nr = shape.nr;
    const crd::usize a_pack_capacity = packed_a_size(shape, a_dim_m, a_dim_k);
    const crd::usize b_pack_capacity = packed_b_size(shape, a_dim_k, b_dim_n);
    const crd::usize align = alignof(T) > 32 ? alignof(T) : 32;
    auto* a_pack = crd::memory::allocate_array_or_fatal<T>(*alloc, a_pack_capacity, align, kGemmPackOutOfMemory);
    auto* b_pack = crd::memory::allocate_array_or_fatal<T>(*alloc, b_pack_capacity, align, kGemmPackOutOfMemory);

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
            const crd::usize num_panels_n = (nc + nr - 1) / nr;

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
                const crd::usize pb_end = j_hi > jc ? std::min(num_panels_n, (j_hi - jc + nr - 1) / nr) : crd::usize{0};
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
                        crd::usize ic, jc, mc, nc, kc, nr;
                        bool col_out;
                    };
                    State st{a_pack, b_pack, c_data, ldc, ic, jc, mc, nc, kc, nr, col_indexed_out};
                    State* sp = &st;
                    auto* counter = crd::jobs::parallel_for(
                        static_cast<crd::u32>(pb_end), nw,
                        [sp](crd::u32 begin, crd::u32 end)
                        {
                            syrk_packed_inner_lower<T>(sp->ic, sp->jc, sp->mc, sp->nc, sp->kc, begin, end, sp->nr,
                                                       sp->a_pack, sp->b_pack, sp->c_data, sp->ldc, sp->col_out);
                        });
                    crd::jobs::wait(counter);
                }
                else
                {
                    syrk_packed_inner_lower<T>(ic, jc, mc, nc, kc, 0, pb_end, nr, a_pack, b_pack, c_data, ldc,
                                               col_indexed_out);
                }
            }
        }
    }

    alloc->deallocate(b_pack);
    alloc->deallocate(a_pack);
}

} // namespace crd::hesap::dense::detail
