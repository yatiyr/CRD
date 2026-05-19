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

inline constexpr crd::usize kGemmMc = 120;
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
// `ceil(mc / kGemmMr) * kGemmMr * kc`. Row-panels are stacked vertically;
// rows beyond `mc` within the final panel are zero-padded.
template <typename T, Layout L>
inline void pack_a(MatrixView<const T, L> a, crd::usize ic, crd::usize pc, crd::usize mc, crd::usize kc,
                   Trans trans_a, T* out) noexcept
{
    const crd::usize num_panels = (mc + kGemmMr - 1) / kGemmMr;
    for (crd::usize panel = 0; panel < num_panels; ++panel)
    {
        const crd::usize row_base = panel * kGemmMr;
        T* panel_out = out + panel * kGemmMr * kc;
        for (crd::usize i_local = 0; i_local < kGemmMr; ++i_local)
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

// pack_b: copy a (kc × nc) block of the EFFECTIVE B starting at col `jc`
// into a packed buffer of size `ceil(nc / kGemmNr) * kc * kGemmNr`.
// Col-panels are stacked horizontally; cols beyond `nc` within the final
// panel are zero-padded.
template <typename T, Layout L>
inline void pack_b(MatrixView<const T, L> b, crd::usize pc, crd::usize jc, crd::usize kc, crd::usize nc,
                   Trans trans_b, T* out) noexcept
{
    const crd::usize num_panels = (nc + kGemmNr - 1) / kGemmNr;
    for (crd::usize panel = 0; panel < num_panels; ++panel)
    {
        const crd::usize col_base = panel * kGemmNr;
        T* panel_out = out + panel * kc * kGemmNr;
        for (crd::usize p = 0; p < kc; ++p)
        {
            for (crd::usize j_local = 0; j_local < kGemmNr; ++j_local)
            {
                const crd::usize j_global = jc + col_base + j_local;
                const bool inside = (col_base + j_local) < nc;
                panel_out[p * kGemmNr + j_local] = inside ? eff_a_read<T, L>(b, pc + p, j_global, trans_b) : T{};
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
inline void gemm_packed_inner(
    T alpha,
    crd::usize ic, crd::usize jc, crd::usize mc, crd::usize nc, crd::usize kc,
    const T* a_packed, const T* b_packed,
    MatrixView<T, L> c) noexcept
{
    const crd::usize num_panels_m = (mc + kGemmMr - 1) / kGemmMr;
    const crd::usize num_panels_n = (nc + kGemmNr - 1) / kGemmNr;

    for (crd::usize pa = 0; pa < num_panels_m; ++pa)
    {
        const T* a_panel = a_packed + pa * kGemmMr * kc;
        const crd::usize i_global = ic + pa * kGemmMr;
        const crd::usize rows_in_panel = std::min(kGemmMr, mc - pa * kGemmMr);

        for (crd::usize pb = 0; pb < num_panels_n; ++pb)
        {
            const T* b_panel = b_packed + pb * kc * kGemmNr;
            const crd::usize j_global = jc + pb * kGemmNr;
            const crd::usize cols_in_panel = std::min(kGemmNr, nc - pb * kGemmNr);

            // Zero-initialised micro-tile of size MR × NR; microkernel
            // accumulates into it. ldc = kGemmNr for this contiguous slab.
            T micro[kGemmMr * kGemmNr]{};
            gemm_microkernel<T>(kc, a_panel, b_panel, micro, kGemmNr);

            // Merge alpha * micro into C; only the rows_in_panel × cols_in_panel
            // sub-tile (the rest of the micro tile came from zero-padded A/B
            // and is itself zero, but we skip it to avoid touching C outside
            // the [ic, ic+mc) × [jc, jc+nc) macro tile).
            for (crd::usize i = 0; i < rows_in_panel; ++i)
            {
                for (crd::usize j = 0; j < cols_in_panel; ++j)
                {
                    if constexpr (L == Layout::RowMajor)
                    {
                        c.data()[(i_global + i) * c.ld() + (j_global + j)] += alpha * micro[i * kGemmNr + j];
                    }
                    else
                    {
                        c.data()[(j_global + j) * c.ld() + (i_global + i)] += alpha * micro[i * kGemmNr + j];
                    }
                }
            }
        }
    }
}

} // namespace crd::hesap::dense::detail
