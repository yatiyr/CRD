#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/sparse_values.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// TripletBuilder<T> -- coordinate (COO) assembly front-end. Accumulate
// (row, col, value) triplets in any order, then `compress()` to a canonical
// CSR SparseMatrix. This is the PETSc/Eigen assembly lever: a single counting
// pass preallocates exact per-row storage, so there is no incremental
// reallocation churn.
//
// Determinism (ADR-0063 / D1): compress() is bit-reproducible for a fixed
// triplet sequence. A stable counting-sort groups triplets by row preserving
// insertion order within a row; a stable per-row sort by column keeps equal
// (row,col) duplicates in insertion order; duplicates are summed
// left-to-right in that order. Plain ordered summation -- matching Eigen's
// setFromTriplets -- not KBN; the contract here is reproducibility and the
// order is fixed.
//
// Raw T per ADR-0078 (hesap is the numerical-kernel layer). T in
// {f32, f64, Complex32, Complex64}. CSC compress lands in v1a-3.
// -----------------------------------------------------------------------

template <typename T>
class TripletBuilder
{
public:
    TripletBuilder(crd::memory::IAllocator* alloc, crd::u32 rows, crd::u32 cols)
        : m_alloc(alloc), m_rows(rows), m_cols(cols), m_row(alloc), m_col(alloc), m_val(alloc)
    {
    }

    void reserve(crd::usize n)
    {
        m_row.reserve(n);
        m_col.reserve(n);
        m_val.reserve(n);
    }

    void add(crd::u32 r, crd::u32 c, T v)
    {
        CRD_ASSERT_MSG(r < m_rows && c < m_cols, "TripletBuilder::add out-of-range (r,c)");
        m_row.push_back(r);
        m_col.push_back(c);
        m_val.push_back(v);
    }

    [[nodiscard]] crd::u32 rows() const noexcept { return m_rows; }
    [[nodiscard]] crd::u32 cols() const noexcept { return m_cols; }
    [[nodiscard]] crd::usize triplet_count() const noexcept { return m_row.size(); }

    // Build a canonical compressed CSR matrix. Duplicate (row,col) entries
    // are summed; columns within each row are sorted ascending.
    [[nodiscard]] SparseMatrix<T, SparseFormat::Csr> compress() const
    {
        SparsePattern   pat(m_alloc);
        SparseValues<T> vals(m_alloc);
        pat.rows       = m_rows;
        pat.cols       = m_cols;
        pat.format     = SparseFormat::Csr;
        pat.block_size = 1;
        assemble</*ByRow=*/true>(pat.outer_ptr, pat.inner_idx, vals.values);
        pat.recompute_topology_hash();
        return SparseMatrix<T, SparseFormat::Csr>(std::move(pat), std::move(vals));
    }

    // Build a canonical compressed CSC matrix (column-major compress directly
    // from the triplets -- not a CSR->CSC conversion, which is v1c). Duplicate
    // (row,col) entries are summed; rows within each column are sorted ascending.
    [[nodiscard]] SparseMatrix<T, SparseFormat::Csc> compress_csc() const
    {
        SparsePattern   pat(m_alloc);
        SparseValues<T> vals(m_alloc);
        pat.rows       = m_rows;
        pat.cols       = m_cols;
        pat.format     = SparseFormat::Csc;
        pat.block_size = 1;
        assemble</*ByRow=*/false>(pat.outer_ptr, pat.inner_idx, vals.values);
        pat.recompute_topology_hash();
        return SparseMatrix<T, SparseFormat::Csc>(std::move(pat), std::move(vals));
    }

private:
    // Sortable (inner-index, value) pair. Default-constructible for stable_sort.
    struct Entry
    {
        crd::u32 inner = 0;
        T        val = T{};
    };

    // Counting-sort grouping by outer vector + per-outer stable sort by inner
    // + in-place dedup-compaction. ByRow => outer = row, inner = col (CSR);
    // ByRow=false => outer = col, inner = row (CSC).
    //
    // Scatters DIRECTLY into the final inner_idx/values (no intermediate
    // Entry AoS) and compacts duplicates in place, so the hot path touches
    // the data twice (scatter + compact) rather than three times. Small inner
    // vectors (the sparse common case) sort with an in-place parallel-array
    // insertion sort -- no allocation. Large inner vectors fall back to a
    // single reused merge-sort scratch sized to the longest vector, preserving
    // O(n log n) robustness for pathological dense rows.
    template <bool ByRow>
    void assemble(crd::containers::Array<crd::u32>& outer_ptr,
                  crd::containers::Array<crd::u32>& inner_idx,
                  crd::containers::Array<T>&        values) const
    {
        constexpr crd::u32 kInsertionMax = 64U;  // below this, in-place insertion sort wins

        const crd::u32   n_outer = ByRow ? m_rows : m_cols;
        const crd::usize nt      = m_row.size();

        // 1. count entries per outer vector + longest vector (for scratch sizing).
        crd::containers::Array<crd::u32> start(m_alloc);
        start.resize(static_cast<crd::usize>(n_outer) + 1);  // value-initialised to 0
        for (crd::usize k = 0; k < nt; ++k)
        {
            ++start[(ByRow ? m_row[k] : m_col[k]) + 1];
        }
        crd::u32 max_len = 0;
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            max_len = start[o + 1] > max_len ? start[o + 1] : max_len;
            start[o + 1] += start[o];  // prefix sum in place -> bucket starts
        }

        // 2. scatter directly into the final arrays grouped by outer (insertion
        //    order preserved within an outer via a sequential write pointer).
        //    Uninitialised: every slot is written by the scatter below before
        //    any read, so the zero-init pass would be pure waste.
        inner_idx.resize_uninitialized(nt);
        values.resize_uninitialized(nt);
        crd::containers::Array<crd::u32> wp(m_alloc);
        wp.resize(n_outer);
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            wp[o] = start[o];
        }
        for (crd::usize k = 0; k < nt; ++k)
        {
            const crd::u32 o = ByRow ? m_row[k] : m_col[k];
            const crd::u32 p = wp[o]++;
            inner_idx[p] = ByRow ? m_col[k] : m_row[k];
            values[p]    = m_val[k];
        }

        // 3. per-outer sort by inner index + in-place forward dedup-compaction.
        crd::containers::Array<Entry> scratch(m_alloc);
        if (max_len > kInsertionMax)
        {
            scratch.resize(max_len);
        }
        outer_ptr.resize(static_cast<crd::usize>(n_outer) + 1);
        outer_ptr[0] = 0;
        crd::u32 w = 0;  // running compacted write position (always <= start[o])
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            const crd::u32 b   = start[o];
            const crd::u32 e   = start[o + 1];
            const crd::u32 len = e - b;
            if (len > 1)
            {
                if (len <= kInsertionMax)
                {
                    // Stable in-place insertion sort on the parallel arrays.
                    for (crd::u32 i = b + 1; i < e; ++i)
                    {
                        const crd::u32 ki = inner_idx[i];
                        T              vi = values[i];
                        crd::u32       j  = i;
                        while (j > b && inner_idx[j - 1] > ki)
                        {
                            inner_idx[j] = inner_idx[j - 1];
                            values[j]    = values[j - 1];
                            --j;
                        }
                        inner_idx[j] = ki;
                        values[j]    = vi;
                    }
                }
                else
                {
                    for (crd::u32 t = 0; t < len; ++t)
                    {
                        scratch[t].inner = inner_idx[b + t];
                        scratch[t].val   = values[b + t];
                    }
                    crd::containers::stable_sort(
                        scratch.data(), scratch.data() + len,
                        [](const Entry& x, const Entry& y) { return x.inner < y.inner; }, m_alloc);
                    for (crd::u32 t = 0; t < len; ++t)
                    {
                        inner_idx[b + t] = scratch[t].inner;
                        values[b + t]    = scratch[t].val;
                    }
                }
            }
            // Forward dedup-compact [b,e) into [w,...). w <= b always, so the
            // read cursor is never behind the write cursor: safe in place.
            crd::u32 i = b;
            while (i < e)
            {
                const crd::u32 idx = inner_idx[i];
                T              acc = values[i];
                crd::u32       j   = i + 1;
                while (j < e && inner_idx[j] == idx)
                {
                    acc = acc + values[j];  // insertion-order sum (deterministic)
                    ++j;
                }
                inner_idx[w] = idx;
                values[w]    = acc;
                ++w;
                i = j;
            }
            outer_ptr[o + 1] = w;
        }
        inner_idx.resize(w);  // shrink to the compacted size
        values.resize(w);
    }

    crd::memory::IAllocator*         m_alloc;
    crd::u32                         m_rows;
    crd::u32                         m_cols;
    crd::containers::Array<crd::u32> m_row;
    crd::containers::Array<crd::u32> m_col;
    crd::containers::Array<T>        m_val;
};

} // namespace crd::hesap::sparse
