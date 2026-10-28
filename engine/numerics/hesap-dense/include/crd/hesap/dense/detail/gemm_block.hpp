// The GEMM block driver (GemmBlock, crd/hesap/dense/dense_kernels.hpp): the panel packers, the walk over the register
// tiles, the strict-upper skip of a lower block and the merges into C. The kernel units
// (../../kernels/dense_kernels.cpp) run it on each level's tile and shape; general code runs it on PortableTile for the
// complex types. Its inline code
// lives in the level namespace (ADR-0134 §5) and calls no standard-library inline function.

#pragma once

#include <crd/hesap/dense/dense_kernels.hpp>
#include <crd/math/kernel_level.hpp>

namespace crd::hesap::dense::detail::inline CRD_MATH_LEVEL_NS
{

// Rows [0, rows) by columns [0, depth) of read(r, c) as row panels of MR rows (row stride depth), the padding rows T{}.
// A source whose rows are contiguous down a column (rows_contiguous) is read four columns at a time across the panel's
// rows: four source streams run at once, where one column at a time is a single DRAM stream (22.7 against 36.9 GB/s
// measured on the dev host, 2026-06-11). Both orders write the same bytes.
template <crd::usize MR, typename T, typename Read>
void pack_a_panels(Read read, crd::usize rows, crd::usize depth, bool rows_contiguous, T* out) noexcept
{
    const crd::usize interleaved_end = depth - depth % 4U;
    for (crd::usize row_base = 0; row_base < rows; row_base += MR)
    {
        T* panel = out + row_base * depth;
        if (rows_contiguous)
        {
            for (crd::usize p = 0; p < interleaved_end; p += 4U)
            {
                for (crd::usize i = 0; i < MR; ++i)
                {
                    const bool inside = row_base + i < rows;
                    T* o = panel + i * depth + p;
                    o[0] = inside ? read(row_base + i, p) : T{};
                    o[1] = inside ? read(row_base + i, p + 1U) : T{};
                    o[2] = inside ? read(row_base + i, p + 2U) : T{};
                    o[3] = inside ? read(row_base + i, p + 3U) : T{};
                }
            }
            for (crd::usize p = interleaved_end; p < depth; ++p)
            {
                for (crd::usize i = 0; i < MR; ++i)
                {
                    panel[i * depth + p] = row_base + i < rows ? read(row_base + i, p) : T{};
                }
            }
            continue;
        }
        for (crd::usize i = 0; i < MR; ++i)
        {
            T* o = panel + i * depth;
            const bool inside = row_base + i < rows;
            for (crd::usize p = 0; p < depth; ++p)
            {
                o[p] = inside ? read(row_base + i, p) : T{};
            }
        }
    }
}

// Rows [0, depth) by columns [0, cols) of read(r, c) as column panels of NR columns (depth rows of NR values), the
// padding columns T{}. Four source rows are read at a time (four streams, as pack_a_panels); the interleaved rows end
// at a closed-form row, so the remainder provably runs fewer than four times (GCC's link-time ranges otherwise lose
// that and report -Waggressive-loop-optimizations on it).
template <crd::usize NR, typename T, typename Read>
void pack_b_panels(Read read, crd::usize depth, crd::usize cols, T* out) noexcept
{
    const crd::usize interleaved_end = depth - depth % 4U;
    for (crd::usize col_base = 0; col_base < cols; col_base += NR)
    {
        T* panel = out + col_base * depth;
        for (crd::usize p = 0; p < interleaved_end; p += 4U)
        {
            for (crd::usize j = 0; j < NR; ++j)
            {
                const bool inside = col_base + j < cols;
                T* o = panel + p * NR + j;
                o[0] = inside ? read(p, col_base + j) : T{};
                o[NR] = inside ? read(p + 1U, col_base + j) : T{};
                o[2U * NR] = inside ? read(p + 2U, col_base + j) : T{};
                o[3U * NR] = inside ? read(p + 3U, col_base + j) : T{};
            }
        }
        for (crd::usize p = interleaved_end; p < depth; ++p)
        {
            for (crd::usize j = 0; j < NR; ++j)
            {
                panel[p * NR + j] = col_base + j < cols ? read(p, col_base + j) : T{};
            }
        }
    }
}

// How a tile reaches C: GemmUpdate with Add split at alpha == 1 and Scale at beta == 0.
enum class GemmMerge : crd::u8
{
    Add,      // c + p
    AddAlpha, // c + alpha * p
    Subtract, // c - p
    Assign,   // p
    Zero,     // +0 + alpha * p (alpha == 1: +0 + p)
    Scale,    // beta * c + alpha * p (alpha == 1: beta * c + p)
};

// p, alpha and beta by reference: MSVC 14.51's LTCG code generation crashes (C1001) on the by-value Complex<f32> copy
// loop of an Assign merge inlined into hesap-direct's supernodal row-block solve.
template <GemmMerge M, typename T> inline void merge_element(T& c, const T& p, const T& alpha, const T& beta) noexcept
{
    if constexpr (M == GemmMerge::Assign)
    {
        c = p;
    }
    else if constexpr (M == GemmMerge::Zero)
    {
        if (alpha == T{1}) // a complex (1, 0) * p is not p where a part of p is infinite
        {
            c = T{} + p;
        }
        else
        {
            c = T{} + alpha * p;
        }
    }
    else if constexpr (M == GemmMerge::Scale)
    {
        if (alpha == T{1})
        {
            c = beta * c + p;
        }
        else
        {
            c = beta * c + alpha * p;
        }
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

// out (MR by NR, row stride NR) = the product of one A row panel and one B column panel, each element summed in depth
// order from +0 with a rounded multiply and a rounded add per step. The accumulators are one register block, so a
// small shape keeps them in registers; with Fused, fused<M> merges a full row-major tile from them into C.
template <typename T, crd::usize MR, crd::usize NR, bool Fused> struct PortableTile
{
    static constexpr bool kFused = Fused;
    static constexpr crd::usize kMr = MR;
    static constexpr crd::usize kNr = NR;

    static void sum(crd::usize depth, crd::usize lda, const T* a, const T* b, T (&acc)[MR][NR]) noexcept
    {
        for (crd::usize p = 0; p < depth; ++p)
        {
            for (crd::usize i = 0; i < MR; ++i)
            {
                const T a_ip = a[i * lda + p];
                for (crd::usize j = 0; j < NR; ++j)
                {
                    acc[i][j] = acc[i][j] + a_ip * b[p * NR + j];
                }
            }
        }
    }

    static void product(crd::usize depth, crd::usize lda, const T* a, const T* b, const T* /*b_next*/, T* out) noexcept
    {
        T acc[MR][NR]{};
        sum(depth, lda, a, b, acc);
        for (crd::usize i = 0; i < MR; ++i)
        {
            for (crd::usize j = 0; j < NR; ++j)
            {
                out[i * NR + j] = acc[i][j];
            }
        }
    }

    template <GemmMerge M>
    static void fused(crd::usize depth, crd::usize lda, const T* a, const T* b, const T* /*b_next*/, T* c,
                      crd::usize ldc, T alpha, T beta) noexcept
    {
        T acc[MR][NR]{};
        sum(depth, lda, a, b, acc);
        for (crd::usize i = 0; i < MR; ++i)
        {
            for (crd::usize j = 0; j < NR; ++j)
            {
                merge_element<M>(c[i * ldc + j], acc[i][j], alpha, beta);
            }
        }
    }
};

// The rows_in by cols_in corner of tile (i0, j0) merged from out (row stride NR); masked keeps a lower block's i >= j
// elements.
template <GemmMerge M, crd::usize NR, typename T>
void merge_tile(const GemmBlock<T>& g, const T* out, crd::usize i0, crd::usize j0, crd::usize rows_in,
                crd::usize cols_in, bool masked) noexcept
{
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
                merge_element<M>(col[i], out[i * NR + j], g.alpha, g.beta);
            }
        }
        return;
    }
    for (crd::usize i = 0; i < rows_in; ++i)
    {
        T* row = g.c + (i0 + i) * g.ldc + j0;
        const T* p = out + i * NR;
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
            merge_element<M>(row[j], p[j], g.alpha, g.beta);
        }
    }
}

template <typename Tile, GemmMerge M, typename T> void gemm_block_merged(const GemmBlock<T>& block) noexcept
{
    // A local copy: the stores through C cannot alias it, so its fields stay in registers across the tiles.
    const GemmBlock<T> g = block;
    constexpr crd::usize mr = Tile::kMr;
    constexpr crd::usize nr = Tile::kNr;
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
            const T* b_next = j0 + nr < g.cols ? b + nr * g.depth : g.b; // the next tile's panel, for a prefetch
            const crd::usize cols_in = g.cols - j0 < nr ? g.cols - j0 : nr;
            const bool masked = g.lower && g.row0 + i0 < g.col0 + j0 + cols_in - 1;
            if constexpr (Tile::kFused)
            {
                if (!g.col_major && !masked && rows_in == mr && cols_in == nr)
                {
                    Tile::template fused<M>(g.depth, g.lda, a, b, b_next, g.c + i0 * g.ldc + j0, g.ldc, g.alpha,
                                            g.beta);
                    continue;
                }
            }
            T out[mr * nr];
            Tile::product(g.depth, g.lda, a, b, b_next, out);
            merge_tile<M, nr>(g, out, i0, j0, rows_in, cols_in, masked);
        }
    }
}

// The block on Tile (shape kMr by kNr): product(depth, lda, a, b, b_next, out) as PortableTile's (b_next: the next
// tile's B panel, which a tile may prefetch), and, when Tile::kFused, fused<M>(depth, lda, a, b, b_next, c, ldc,
// alpha, beta), which merges a full row-major tile from its registers with merge_element's expression (the same bits,
// no stored tile).
template <typename Tile, typename T> void gemm_block(const GemmBlock<T>& g) noexcept
{
    if (g.update == GemmUpdate::Assign)
    {
        gemm_block_merged<Tile, GemmMerge::Assign>(g);
    }
    else if (g.update == GemmUpdate::Scale)
    {
        if (g.beta == T{0})
        {
            gemm_block_merged<Tile, GemmMerge::Zero>(g);
        }
        else
        {
            gemm_block_merged<Tile, GemmMerge::Scale>(g);
        }
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
