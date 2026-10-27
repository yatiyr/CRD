// The GEMM block driver (GemmBlock, crd/hesap/dense/dense_kernels.hpp): the walk over the register tiles, the
// strict-upper skip of a lower block and the merges into C. The kernel units (../../kernels/dense_kernels.cpp) run it
// on each level's tile; general code runs it on PortableTile for the complex types. Its inline code lives in the
// level namespace (ADR-0134 §5) and calls no standard-library inline function.

#pragma once

#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{

// How a tile reaches C: GemmUpdate with Add split at alpha == 1.
enum class GemmMerge : crd::u8
{
    Add,      // c + p
    AddAlpha, // c + alpha * p
    Subtract, // c - p
    Assign,   // p
};

// out (MR by NR, row stride NR) = the product of one A row panel and one B column panel, each element summed in depth
// order from +0 with a rounded multiply and a rounded add per step. No fused tile.
template <typename T> struct PortableTile
{
    static constexpr bool kFused = false;

    static void product(crd::usize depth, crd::usize lda, const T* a, const T* b, T* out) noexcept
    {
        constexpr crd::usize mr = GemmTraits<T>::MR;
        constexpr crd::usize nr = GemmTraits<T>::NR;
        T acc[mr][nr]{};
        for (crd::usize p = 0; p < depth; ++p)
        {
            for (crd::usize i = 0; i < mr; ++i)
            {
                const T a_ip = a[i * lda + p];
                for (crd::usize j = 0; j < nr; ++j)
                {
                    acc[i][j] = acc[i][j] + a_ip * b[p * nr + j];
                }
            }
        }
        for (crd::usize i = 0; i < mr; ++i)
        {
            for (crd::usize j = 0; j < nr; ++j)
            {
                out[i * nr + j] = acc[i][j];
            }
        }
    }
};

template <GemmMerge M, typename T> inline void merge_element(T& c, T p, T alpha) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        c = p;
    }
    else if constexpr (M == GemmMerge::Add)
    {
        c += p;
    }
    else if constexpr (M == GemmMerge::Subtract)
    {
        c -= p;
    }
    else
    {
        c += alpha * p;
    }
}

// The rows_in by cols_in corner of tile (i0, j0) merged from out; masked keeps a lower block's i >= j elements.
template <GemmMerge M, typename T>
void merge_tile(const GemmBlock<T>& g, const T* out, crd::usize i0, crd::usize j0, crd::usize rows_in,
                crd::usize cols_in, bool masked) noexcept
{
    constexpr crd::usize nr = GemmTraits<T>::NR;
    if (g.col_major) // the contiguous walk of C is down a column
    {
        for (crd::usize j = 0; j < cols_in; ++j)
        {
            T* col = g.c + (j0 + j) * g.ldc + i0;
            const crd::usize row_index = g.row0 + i0;
            const crd::usize col_index = g.col0 + j0 + j;
            const crd::usize first = masked && col_index > row_index ? col_index - row_index : 0;
            for (crd::usize i = first; i < rows_in; ++i)
            {
                merge_element<M>(col[i], out[i * nr + j], g.alpha);
            }
        }
        return;
    }
    for (crd::usize i = 0; i < rows_in; ++i)
    {
        T* row = g.c + (i0 + i) * g.ldc + j0;
        const T* p = out + i * nr;
        crd::usize end = cols_in;
        if (masked)
        {
            const crd::usize row_index = g.row0 + i0 + i;
            const crd::usize col_index = g.col0 + j0;
            const crd::usize on_or_below = row_index < col_index ? 0 : row_index - col_index + 1;
            end = on_or_below < cols_in ? on_or_below : cols_in;
        }
        for (crd::usize j = 0; j < end; ++j)
        {
            merge_element<M>(row[j], p[j], g.alpha);
        }
    }
}

template <typename Tile, GemmMerge M, typename T> void gemm_block_merged(const GemmBlock<T>& block) noexcept
{
    // A local copy: the stores through C cannot alias it, so its fields stay in registers across the tiles.
    const GemmBlock<T> g = block;
    constexpr crd::usize mr = GemmTraits<T>::MR;
    constexpr crd::usize nr = GemmTraits<T>::NR;
    for (crd::usize i0 = 0; i0 < g.rows; i0 += mr)
    {
        const T* a = g.a + i0 * g.lda;
        const crd::usize rows_in = g.rows - i0 < mr ? g.rows - i0 : mr;
        for (crd::usize j0 = 0; j0 < g.cols; j0 += nr)
        {
            if (g.lower && g.row0 + i0 + rows_in <= g.col0 + j0)
            {
                break; // this tile and the rest of the row lie above the diagonal
            }
            const T* b = g.b + j0 * g.depth;
            const crd::usize cols_in = g.cols - j0 < nr ? g.cols - j0 : nr;
            const bool masked = g.lower && g.row0 + i0 < g.col0 + j0 + cols_in - 1;
            if constexpr (Tile::kFused)
            {
                if (!g.col_major && !masked && rows_in == mr && cols_in == nr)
                {
                    Tile::template fused<M>(g.depth, g.lda, a, b, g.c + i0 * g.ldc + j0, g.ldc, g.alpha);
                    continue;
                }
            }
            T out[mr * nr];
            Tile::product(g.depth, g.lda, a, b, out);
            merge_tile<M>(g, out, i0, j0, rows_in, cols_in, masked);
        }
    }
}

// The block on Tile: product(depth, lda, a, b, out) as PortableTile's, and, when Tile::kFused, fused<M>(depth, lda, a,
// b, c, ldc, alpha), which merges a full row-major tile from its registers with merge_element's expression (the same
// bits, no stored tile).
template <typename Tile, typename T> void gemm_block(const GemmBlock<T>& g) noexcept
{
    if (g.update == GemmUpdate::Assign)
    {
        gemm_block_merged<Tile, GemmMerge::Assign>(g);
    }
    else if (g.update == GemmUpdate::Subtract)
    {
        gemm_block_merged<Tile, GemmMerge::Subtract>(g);
    }
    else if (g.alpha == T{1})
    {
        gemm_block_merged<Tile, GemmMerge::Add>(g);
    }
    else
    {
        gemm_block_merged<Tile, GemmMerge::AddAlpha>(g);
    }
}

} // namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
