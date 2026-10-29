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
// (ConjTranspose). Every helper runs the GemmKernels it is given: the process level's (gemm_kernels_of), a board's
// arm (gemm_arms.hpp) or the complex types' portable tile, packed in general code (kComplexGemmKernels).

// The cache blocks (FLAWN #74's analytical model): an Mc by Kc Ac block (960 KB of f64) fills about half of the dev
// host's 2 MB per-core L2 beside the streamed Bc panel and C (v5a-4); Nc keeps the Bc panel in L3. Kc is also the
// depth grouping every GEMM path sums by (dense_kernels.hpp), so it is part of the declared arithmetic.
inline constexpr crd::usize kGemmMc = 480;
inline constexpr crd::usize kGemmKc = 256;
inline constexpr crd::usize kGemmNc = 4080;

// The complex types' tile: two roundings in general code, 8 by 8 (no fused merge; see merge_element), and its kernels.
inline constexpr GemmShape kComplexGemmShape{.mr = 8, .nr = 8};
template <typename T> using ComplexGemmTile = PortableTile<T, kComplexGemmShape.mr, kComplexGemmShape.nr, false>;

template <typename T> void pack_a_complex(const GemmSource<T>& s, crd::usize rows, crd::usize depth, T* out) noexcept
{
    if (s.conj)
    {
        pack_a_source<kComplexGemmShape.mr>(s, rows, depth, out, [](const T& x) { return crd::hesap::conj(x); });
        return;
    }
    pack_a_source<kComplexGemmShape.mr>(s, rows, depth, out, [](const T& x) { return x; });
}

template <typename T> void pack_b_complex(const GemmSource<T>& s, crd::usize depth, crd::usize cols, T* out) noexcept
{
    if (s.conj)
    {
        pack_b_source<kComplexGemmShape.nr>(s, depth, cols, out, [](const T& x) { return crd::hesap::conj(x); });
        return;
    }
    pack_b_source<kComplexGemmShape.nr>(s, depth, cols, out, [](const T& x) { return x; });
}

template <typename T>
inline constexpr GemmKernels<T> kComplexGemmKernels{.shape = kComplexGemmShape,
                                                    .block = &gemm_block<ComplexGemmTile<T>, T>,
                                                    .pack_a = &pack_a_complex<T>,
                                                    .pack_b = &pack_b_complex<T>};

// The GEMM kernels of the level the process runs (f64, f32), or the complex types'.
template <typename T> [[nodiscard]] inline const GemmKernels<T>& gemm_kernels_of() noexcept
{
    if constexpr (kHasDenseKernels<T>)
    {
        return dense_kernels_of<T>().gemm;
    }
    else
    {
        return kComplexGemmKernels<T>;
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

// The Mc and Nc blocks of a shape: kGemmMc and kGemmNc rounded down to whole tiles, so only a matrix's last row or
// column block holds a partial panel (480 rows would leave a 4-row panel of 14-row tiles in every block). The blocks
// change no element's bits: only the depth groups (kGemmKc) shape a sum.
[[nodiscard]] inline crd::usize gemm_mc(GemmShape s) noexcept
{
    return kGemmMc - kGemmMc % s.mr;
}

[[nodiscard]] inline crd::usize gemm_nc(GemmShape s) noexcept
{
    return kGemmNc - kGemmNc % s.nr;
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
    return GemmSource<T>{.data = x.data() + r0 * rs + c0 * cs,
                         .row_stride = rs,
                         .col_stride = cs,
                         .conj = tr == Trans::ConjTranspose && !std::is_floating_point_v<T>};
}

// Whether a GEMM may read op(A) in place as whole row panels on kernels k (blas3's resident-A path; the size
// thresholds are blas3's): a real type, op(A) row-major, and the whole packed B within the packed path's Bc budget (Kc
// by Nc plus a panel), so a skinny product (m = 1, n = k = 10^4: 800 MB of f64) never packs a large B whole.
template <typename T, Layout L>
[[nodiscard]] inline bool gemm_resident_admits(const GemmKernels<T>& k, Trans trans_a, crd::usize n,
                                               crd::usize depth) noexcept
{
    return kHasDenseKernels<T> && (L == Layout::RowMajor) == (trans_a == Trans::None) &&
           packed_b_size(k.shape, depth, n) <= kGemmKc * (kGemmNc + k.shape.nr);
}

// pack_a: the (mc by kc) block of op(A) at (ic, pc) as Ac panels of kernels k into out (packed_a_size values).
template <typename T, Layout L>
inline void pack_a(const GemmKernels<T>& k, MatrixView<const T, L> a, crd::usize ic, crd::usize pc, crd::usize mc,
                   crd::usize kc, Trans trans_a, T* out) noexcept
{
    k.pack_a(gemm_source(a, ic, pc, trans_a), mc, kc, out);
}

// pack_b: the (kc by nc) block of op(B) at (pc, jc) as Bc panels of kernels k into out (packed_b_size values).
template <typename T, Layout L>
inline void pack_b(const GemmKernels<T>& k, MatrixView<const T, L> b, crd::usize pc, crd::usize jc, crd::usize kc,
                   crd::usize nc, Trans trans_b, T* out) noexcept
{
    k.pack_b(gemm_source(b, pc, jc, trans_b), kc, nc, out);
}

// C[ic .. ic + mc, jc .. jc + nc) merged with alpha * the product of the Ac rows (mc by kc, row stride lda: packed
// panels or resident whole panels) and the packed Bc (kc by nc) block on kernels k: GemmUpdate::Scale for a GEMM's
// first depth group (beta C folded in), Add for the later ones.
template <typename T, Layout L>
inline void gemm_packed_inner(const GemmKernels<T>& k, T alpha, T beta, GemmUpdate update, crd::usize ic, crd::usize jc,
                              crd::usize mc, crd::usize nc, crd::usize kc, const T* a_rows, crd::usize lda,
                              const T* b_packed, MatrixView<T, L> c) noexcept
{
    constexpr bool col_major = L == Layout::ColMajor;
    T* corner = col_major ? c.data() + jc * c.ld() + ic : c.data() + ic * c.ld() + jc;
    k.block(GemmBlock<T>{.a = a_rows,
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
                         .beta = beta,
                         .update = update,
                         .col_major = col_major,
                         .lower = false});
}

} // namespace crd::hesap::dense::detail
