#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/gemm_microkernel.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// Goto/BLIS Ac / Bc packing layers. Phase 3.1.6 v0d-perf.
//
// Packing is part of the INTRINSICS backend (ADR-0082): the per-backend
// packed-layout contract is coupled to the microkernel that consumes it.
// When the future ASM backend lands, BLIS-convention packing comes with it.
// For today's intrinsics microkernel the packed layout is:
//
//   Ac: MR contiguous rows × K cols (row-major) per "row-panel" of MR rows.
//       Full Ac = ceil(Mc / MR) row-panels stacked vertically.
//       Block (mc_block × Kc) → ceil(mc_block / MR) panels.
//
//   Bc: K rows × NR contiguous cols (row-major) per "col-panel" of NR cols.
//       Full Bc = ceil(Nc / NR) col-panels stacked horizontally.
//       Block (Kc × nc_block) → ceil(nc_block / NR) panels.
//
// Each microkernel call consumes ONE row-panel of Ac (MR × Kc) and ONE
// col-panel of Bc (Kc × NR), producing an MR × NR tile of C.
//
// Trans handling: the pack functions take the EFFECTIVE source index
// (i.e. callers pass `Trans` and the pack func reads A[i, p] when None,
// A[p, i] when Transpose, conj(A[p, i]) when ConjTranspose). Complex
// support added when those microkernels need it.
// -----------------------------------------------------------------------

// v5a-4 gemm lift: Mc 120→480 — the Ac block (Mc·Kc·8 B) should fill the 14900K's 2MB-per-P-core
// L2 (120 = 240KB badly underused it; 480 = 960KB ≈ half L2, leaving room for the streamed Bc
// panel + C). Kc kept at 256 (the K-accumulation grouping ⇒ keeping it = ZERO value change vs the
// current factor ⇒ moat/residual bit-identical; only Mc/Nc, which don't touch K-summation, move).
// FLAWN #74 analytical-block-size model. Nc kept (Bc panel in L3).
inline constexpr crd::usize kGemmMc = 480;
inline constexpr crd::usize kGemmKc = 256;
inline constexpr crd::usize kGemmNc = 4080;

// Helper: read effective A element honoring trans flag.
template <typename T, Layout L>
[[nodiscard]] inline T eff_a_read(MatrixView<const T, L> a, crd::usize r, crd::usize c, Trans tr) noexcept
{
    auto base = [&](crd::usize i, crd::usize j) -> T
    {
        if constexpr (L == Layout::RowMajor)
        {
            return a.data()[i * a.ld() + j];
        }
        else
        {
            return a.data()[j * a.ld() + i];
        }
    };
    switch (tr)
    {
        case Trans::None:
            return base(r, c);
        case Trans::Transpose:
            return base(c, r);
        case Trans::ConjTranspose:
            if constexpr (std::is_floating_point_v<T>)
            {
                return base(c, r);
            }
            else
            {
                return crd::hesap::conj(base(c, r));
            }
    }
    return base(r, c);
}

// pack_a: copy an (mc × kc) block of the EFFECTIVE A (op-A) starting at
// row `ic` of the effective shape into a packed buffer of size
// `ceil(mc / GemmTraits<T>::MR) * GemmTraits<T>::MR * kc`. Row-panels are stacked vertically;
// rows beyond `mc` within the final panel are zero-padded.
template <typename T, Layout L>
inline void pack_a(MatrixView<const T, L> a, crd::usize ic, crd::usize pc, crd::usize mc, crd::usize kc, Trans trans_a,
                   T* out) noexcept
{
    const crd::usize num_panels = (mc + GemmTraits<T>::MR - 1) / GemmTraits<T>::MR;
    // op(A) is effective-COLUMN-major (source columns contiguous in memory) when the
    // storage layout and the transpose flag disagree on row-major-ness. In that case the
    // cold source read is contiguous in the ROW index for a fixed column, so we pack
    // column-slices (p-outer, i-inner). Packing row-by-row there (the old order) reads with
    // stride = leading dim — a measured chunk of the skinny ColMajor cmod/cdiv gemm cost.
    // BIT-IDENTICAL: the same panel_out[i_local*kc+p] entries, just filled in a different
    // order. The effective row-major case keeps the original row-by-row pack.
    const bool eff_colmajor = (L == Layout::ColMajor) != (trans_a != Trans::None);
    for (crd::usize panel = 0; panel < num_panels; ++panel)
    {
        const crd::usize row_base = panel * GemmTraits<T>::MR;
        T* panel_out = out + panel * GemmTraits<T>::MR * kc;
        if (eff_colmajor)
        {
            // 4-way p-INTERLEAVE (2026-06-11 multi-stream dig): a cold source served one column at a
            // time is a SINGLE DRAM stream (~22.7 GB/s on the dev host); four interleaved column
            // streams reach ~36.9 GB/s (bank/page-level parallelism — the same measured mechanism that
            // flipped the solve). Pure copy reordering ⇒ bit-identical packed bytes.
            crd::usize p = 0;
            for (; p + 4 <= kc; p += 4)
            {
                for (crd::usize i_local = 0; i_local < GemmTraits<T>::MR; ++i_local)
                {
                    const bool inside = (row_base + i_local) < mc;
                    const crd::usize ig = ic + row_base + i_local;
                    panel_out[i_local * kc + p + 0] = inside ? eff_a_read<T, L>(a, ig, pc + p + 0, trans_a) : T{};
                    panel_out[i_local * kc + p + 1] = inside ? eff_a_read<T, L>(a, ig, pc + p + 1, trans_a) : T{};
                    panel_out[i_local * kc + p + 2] = inside ? eff_a_read<T, L>(a, ig, pc + p + 2, trans_a) : T{};
                    panel_out[i_local * kc + p + 3] = inside ? eff_a_read<T, L>(a, ig, pc + p + 3, trans_a) : T{};
                }
            }
            for (; p < kc; ++p) // remainder columns
            {
                for (crd::usize i_local = 0; i_local < GemmTraits<T>::MR; ++i_local)
                {
                    const bool inside = (row_base + i_local) < mc;
                    panel_out[i_local * kc + p] =
                        inside ? eff_a_read<T, L>(a, ic + row_base + i_local, pc + p, trans_a) : T{};
                }
            }
        }
        else
        {
            for (crd::usize i_local = 0; i_local < GemmTraits<T>::MR; ++i_local)
            {
                const crd::usize i_global = ic + row_base + i_local;
                const bool inside = (row_base + i_local) < mc;
                for (crd::usize p = 0; p < kc; ++p)
                {
                    panel_out[i_local * kc + p] = inside ? eff_a_read<T, L>(a, i_global, pc + p, trans_a) : T{};
                }
            }
        }
    }
}

// pack_b: copy a (kc × nc) block of the EFFECTIVE B starting at col `jc`
// into a packed buffer of size `ceil(nc / GemmTraits<T>::NR) * kc * GemmTraits<T>::NR`.
// Col-panels are stacked horizontally; cols beyond `nc` within the final
// panel are zero-padded.
template <typename T, Layout L>
inline void pack_b(MatrixView<const T, L> b, crd::usize pc, crd::usize jc, crd::usize kc, crd::usize nc, Trans trans_b,
                   T* out) noexcept
{
    const crd::usize num_panels = (nc + GemmTraits<T>::NR - 1) / GemmTraits<T>::NR;
    for (crd::usize panel = 0; panel < num_panels; ++panel)
    {
        const crd::usize col_base = panel * GemmTraits<T>::NR;
        T* panel_out = out + panel * kc * GemmTraits<T>::NR;
        // 4-way p-INTERLEAVE — for the hot effective-row-major cases (RowMajor/None, ColMajor/adjoint:
        // the supernodal panels) each effective row p is a contiguous source run; reading rows one at a
        // time is a single DRAM stream, four interleaved rows are four (the measured 22.7 → 36.9 GB/s
        // mechanism). Pure copy reordering ⇒ bit-identical packed bytes.
        crd::usize p = 0;
        for (; p + 4 <= kc; p += 4)
        {
            for (crd::usize j_local = 0; j_local < GemmTraits<T>::NR; ++j_local)
            {
                const crd::usize j_global = jc + col_base + j_local;
                const bool inside = (col_base + j_local) < nc;
                panel_out[(p + 0) * GemmTraits<T>::NR + j_local] =
                    inside ? eff_a_read<T, L>(b, pc + p + 0, j_global, trans_b) : T{};
                panel_out[(p + 1) * GemmTraits<T>::NR + j_local] =
                    inside ? eff_a_read<T, L>(b, pc + p + 1, j_global, trans_b) : T{};
                panel_out[(p + 2) * GemmTraits<T>::NR + j_local] =
                    inside ? eff_a_read<T, L>(b, pc + p + 2, j_global, trans_b) : T{};
                panel_out[(p + 3) * GemmTraits<T>::NR + j_local] =
                    inside ? eff_a_read<T, L>(b, pc + p + 3, j_global, trans_b) : T{};
            }
        }
        for (; p < kc; ++p) // remainder rows
        {
            for (crd::usize j_local = 0; j_local < GemmTraits<T>::NR; ++j_local)
            {
                const crd::usize j_global = jc + col_base + j_local;
                const bool inside = (col_base + j_local) < nc;
                panel_out[p * GemmTraits<T>::NR + j_local] =
                    inside ? eff_a_read<T, L>(b, pc + p, j_global, trans_b) : T{};
            }
        }
    }
}

// gemm_packed_inner: the inner mr × nr loops that call the microkernel
// for each (panel_a, panel_b) pair within the current (Mc × Nc) macro
// tile of C. Writes alpha-scaled microkernel output into a local
// register-resident C accumulator, then merges into the destination C
// tile (which already holds beta * C from the driver's initial scaling
// pass).
//
// The microkernel itself ACCUMULATES into C — so we use a fresh zeroed
// micro-tile as a temp, run the microkernel into it, then add alpha *
// micro_tile to C.
template <typename T, Layout L>
inline void gemm_packed_inner(T alpha, crd::usize ic, crd::usize jc, crd::usize mc, crd::usize nc, crd::usize kc,
                              const T* a_packed, const T* b_packed, MatrixView<T, L> c) noexcept
{
    const crd::usize num_panels_m = (mc + GemmTraits<T>::MR - 1) / GemmTraits<T>::MR;
    const crd::usize num_panels_n = (nc + GemmTraits<T>::NR - 1) / GemmTraits<T>::NR;

    for (crd::usize pa = 0; pa < num_panels_m; ++pa)
    {
        const T* a_panel = a_packed + pa * GemmTraits<T>::MR * kc;
        const crd::usize i_global = ic + pa * GemmTraits<T>::MR;
        const crd::usize rows_in_panel = std::min(GemmTraits<T>::MR, mc - pa * GemmTraits<T>::MR);

        for (crd::usize pb = 0; pb < num_panels_n; ++pb)
        {
            const T* b_panel = b_packed + pb * kc * GemmTraits<T>::NR;
            const crd::usize j_global = jc + pb * GemmTraits<T>::NR;
            const crd::usize cols_in_panel = std::min(GemmTraits<T>::NR, nc - pb * GemmTraits<T>::NR);

            // Zero-initialised micro-tile of size MR × NR; microkernel
            // accumulates into it. ldc = GemmTraits<T>::NR for this contiguous slab.
            T micro[GemmTraits<T>::MR * GemmTraits<T>::NR]{};
            gemm_microkernel<T>(kc, a_panel, b_panel, micro, GemmTraits<T>::NR);

            // Merge alpha * micro into C; only the rows_in_panel × cols_in_panel
            // sub-tile (the rest of the micro tile came from zero-padded A/B
            // and is itself zero, but we skip it to avoid touching C outside
            // the [ic, ic+mc) × [jc, jc+nc) macro tile). The inner loop walks
            // C's CONTIGUOUS index per layout (j for RowMajor, i for ColMajor —
            // the ColMajor i-inner order is the lattice-crush merge fix 2026-06-11:
            // the old j-inner walk strided EVERY ColMajor write by ld). Pure
            // reordering of independent element writes ⇒ bit-identical.
            if constexpr (L == Layout::RowMajor)
            {
                for (crd::usize i = 0; i < rows_in_panel; ++i)
                {
                    T* crow = c.data() + (i_global + i) * c.ld() + j_global;
                    const T* mrow = micro + i * GemmTraits<T>::NR;
                    for (crd::usize j = 0; j < cols_in_panel; ++j)
                    {
                        crow[j] += alpha * mrow[j];
                    }
                }
            }
            else
            {
                for (crd::usize j = 0; j < cols_in_panel; ++j)
                {
                    T* ccol = c.data() + (j_global + j) * c.ld() + i_global;
                    for (crd::usize i = 0; i < rows_in_panel; ++i)
                    {
                        ccol[i] += alpha * micro[i * GemmTraits<T>::NR + j];
                    }
                }
            }
        }
    }
}

} // namespace crd::hesap::dense::detail
