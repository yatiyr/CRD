#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/dense_dispatch.hpp>
#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/hesap/dense/detail/gemm_block.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>

#include <type_traits>

namespace crd::hesap::dense::detail
{
// DIAG.3a: a GEMM-family product cannot run without its packing buffers, so a scratch allocator that refuses one (an
// exhausted sub-budget allocator) is out of memory, fatal in every build, never a null panel packed into
// (crd-hesap-dense-fatal-* modes).
inline constexpr const char* kGemmPackOutOfMemory = "hesap::dense: out of memory for a GEMM packing buffer";

// Goto/BLIS Ac / Bc packing. Each kernel level packs its own panels for its own register tile (GemmShape,
// crd/hesap/dense/dense_kernels.hpp); the panels are per-call scratch, so the shape may differ per level:
//   Ac: ceil(mc / mr) row panels of mr rows by kc values (row-major), the padding rows +0;
//   Bc: ceil(nc / nr) column panels of kc rows by nr values (row-major), the padding columns +0.
// Each register tile consumes one Ac row panel and one Bc column panel and produces an mr by nr tile of C. The pack
// functions take the effective operand: op(A) reads A[i, p] (None), A[p, i] (Transpose) or conj(A[p, i])
// (ConjTranspose). The complex types pack in general code for their portable tile (kComplexGemmShape).

// The cache blocks (FLAWN #74's analytical model): an Mc by Kc Ac block (960 KB of f64) fills about half of the dev
// host's 2 MB per-core L2 beside the streamed Bc panel and C (v5a-4); Nc keeps the Bc panel in L3. Kc is also the
// depth grouping every GEMM path sums by (dense_kernels.hpp), so it is part of the declared arithmetic.
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

// The complex types' tile: two roundings in general code, 8 by 8 (no fused merge; see merge_element).
inline constexpr GemmShape kComplexGemmShape{.mr = 8, .nr = 8};
template <typename T> using ComplexGemmTile = PortableTile<T, kComplexGemmShape.mr, kComplexGemmShape.nr, false>;

// The tile shape of the level the process runs (f64, f32), or the complex types'.
template <typename T> [[nodiscard]] inline GemmShape gemm_shape_of() noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        return dense_kernels_of<T>().gemm_shape;
    }
    else
    {
        return kComplexGemmShape;
    }
}

// The values of the packed Ac (rows by depth) and Bc (depth by cols) panels of a shape.
[[nodiscard]] inline crd::usize packed_a_size(GemmShape s, crd::usize rows, crd::usize depth) noexcept
{
    return (rows + s.mr - 1U) / s.mr * s.mr * depth;
}

[[nodiscard]] inline crd::usize packed_b_size(GemmShape s, crd::usize depth, crd::usize cols) noexcept
{
    return (cols + s.nr - 1U) / s.nr * s.nr * depth;
}

// op(x) from element (r0, c0) on as a strided source (real types: ConjTranspose is Transpose).
template <typename T, Layout L>
[[nodiscard]] inline GemmSource<T> gemm_source(MatrixView<const T, L> x, crd::usize r0, crd::usize c0,
                                               Trans tr) noexcept
{
    crd::usize rs = L == Layout::RowMajor ? x.ld() : 1U;
    crd::usize cs = L == Layout::RowMajor ? 1U : x.ld();
    if (tr != Trans::None)
    {
        const crd::usize t = rs;
        rs = cs;
        cs = t;
    }
    return GemmSource<T>{.data = x.data() + r0 * rs + c0 * cs, .row_stride = rs, .col_stride = cs};
}

// pack_a: the (mc by kc) block of op(A) at (ic, pc) as Ac panels into out (packed_a_size values).
template <typename T, Layout L>
inline void pack_a(MatrixView<const T, L> a, crd::usize ic, crd::usize pc, crd::usize mc, crd::usize kc, Trans trans_a,
                   T* out) noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        dense_kernels_of<T>().pack_a(gemm_source(a, ic, pc, trans_a), mc, kc, out);
    }
    else
    {
        // op(A) is column-contiguous when the layout and the transpose disagree on row-major-ness.
        const bool rows_contiguous = (L == Layout::ColMajor) != (trans_a != Trans::None);
        pack_a_panels<kComplexGemmShape.mr>([a, ic, pc, trans_a](crd::usize r, crd::usize c)
                                            { return eff_a_read<T, L>(a, ic + r, pc + c, trans_a); }, mc, kc,
                                            rows_contiguous, out);
    }
}

// pack_b: the (kc by nc) block of op(B) at (pc, jc) as Bc panels into out (packed_b_size values).
template <typename T, Layout L>
inline void pack_b(MatrixView<const T, L> b, crd::usize pc, crd::usize jc, crd::usize kc, crd::usize nc, Trans trans_b,
                   T* out) noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        dense_kernels_of<T>().pack_b(gemm_source(b, pc, jc, trans_b), kc, nc, out);
    }
    else
    {
        pack_b_panels<kComplexGemmShape.nr>([b, pc, jc, trans_b](crd::usize r, crd::usize c)
                                            { return eff_a_read<T, L>(b, pc + r, jc + c, trans_b); }, kc, nc, out);
    }
}

// One GEMM block on the level the process runs (f64, f32) or on the complex types' tile.
template <typename T> inline void run_gemm_block(const GemmBlock<T>& block) noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        dense_kernels_of<T>().gemm_block(block);
    }
    else
    {
        gemm_block<ComplexGemmTile<T>>(block);
    }
}

// C[ic .. ic + mc, jc .. jc + nc) += alpha * the product of the Ac rows (mc by kc, row stride lda: packed panels or
// resident whole panels) and the packed Bc (kc by nc) block; C already holds beta * C from the driver's scaling pass.
template <typename T, Layout L>
inline void gemm_packed_inner(T alpha, crd::usize ic, crd::usize jc, crd::usize mc, crd::usize nc, crd::usize kc,
                              const T* a_rows, crd::usize lda, const T* b_packed, MatrixView<T, L> c) noexcept
{
    constexpr bool col_major = L == Layout::ColMajor;
    T* corner = col_major ? c.data() + jc * c.ld() + ic : c.data() + ic * c.ld() + jc;
    run_gemm_block(GemmBlock<T>{.a = a_rows,
                                .b = b_packed,
                                .c = corner,
                                .lda = lda,
                                .ldc = c.ld(),
                                .rows = mc,
                                .cols = nc,
                                .depth = kc,
                                .row0 = ic,
                                .col0 = jc,
                                .alpha = alpha,
                                .update = GemmUpdate::Add,
                                .col_major = col_major,
                                .lower = false});
}

} // namespace crd::hesap::dense::detail
