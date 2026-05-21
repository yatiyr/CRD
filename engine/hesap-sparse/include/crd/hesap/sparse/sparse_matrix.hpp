#pragma once

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/analysis_handle.hpp>
#include <crd/hesap/sparse/sparse_format.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/sparse_values.hpp>
#include <crd/memory/allocator.hpp>

#include <utility>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparseMatrix<T, Format> — an owning bundle of the trinity for one matrix:
// the structural SparsePattern + the numeric SparseValues<T> + a cached
// AnalysisHandle. Format is a compile-time NTTP (D21 dense-Layout
// precedent); the runtime `SparsePattern::format` tag mirrors it so the
// analysis cache and the v1c conversion graph can dispatch dynamically.
//
// Ownership (pinned v1a decision): the matrix OWNS its pattern (move-only,
// matching dense Vector<T>/Matrix<T> D15). A shared-pattern surface — many
// value frames over one symbolic structure — lands when an actual consumer
// (v4 iterative / v5 direct) needs it, not speculatively.
//
// v1a-1 ships the storage shell + accessors. The builder that POPULATES it
// (TripletBuilder -> compress) arrives in v1a-2 (CSR) and v1a-3 (CSC).
// `values.size()` must equal `pattern.nnz()` for a well-formed matrix.
// -----------------------------------------------------------------------

template <typename T, SparseFormat Format = SparseFormat::Csr>
class SparseMatrix
{
public:
    using value_type = T;
    static constexpr SparseFormat format = Format;

    explicit SparseMatrix(crd::memory::IAllocator* alloc)
        : m_pattern(alloc), m_values(alloc)
    {
        m_pattern.format = Format;
    }

    // Adopt a prebuilt pattern + values (the builder hands these over).
    // The value array is parallel to inner_idx (physical alignment): for a
    // compressed matrix that equals nnz(); for an uncompressed matrix it
    // equals the slack-padded capacity (nnz() <= inner_idx.size()).
    SparseMatrix(SparsePattern&& pattern, SparseValues<T>&& values) noexcept
        : m_pattern(std::move(pattern)), m_values(std::move(values))
    {
        CRD_ASSERT_MSG(m_pattern.format == Format, "SparseMatrix Format NTTP must match pattern.format");
        CRD_ASSERT_MSG(m_values.size() == m_pattern.inner_idx.size(),
                       "SparseMatrix values must be parallel to pattern.inner_idx");
    }

    SparseMatrix(SparseMatrix&&) noexcept = default;
    SparseMatrix& operator=(SparseMatrix&&) noexcept = default;
    SparseMatrix(const SparseMatrix&) = delete;
    SparseMatrix& operator=(const SparseMatrix&) = delete;
    ~SparseMatrix() = default;

    [[nodiscard]] crd::u32 rows() const noexcept { return m_pattern.rows; }
    [[nodiscard]] crd::u32 cols() const noexcept { return m_pattern.cols; }
    [[nodiscard]] crd::usize nnz() const noexcept { return m_pattern.nnz(); }

    [[nodiscard]] const SparsePattern& pattern() const noexcept { return m_pattern; }
    [[nodiscard]] SparsePattern& pattern() noexcept { return m_pattern; }
    [[nodiscard]] const SparseValues<T>& values() const noexcept { return m_values; }
    [[nodiscard]] SparseValues<T>& values() noexcept { return m_values; }

    // Fraction of dense cells that are stored. 0 for an empty matrix.
    [[nodiscard]] crd::f64 density() const noexcept
    {
        const crd::u64 dense_cells = static_cast<crd::u64>(m_pattern.rows) * static_cast<crd::u64>(m_pattern.cols);
        if (dense_cells == 0)
        {
            return 0.0;
        }
        return static_cast<crd::f64>(nnz()) / static_cast<crd::f64>(dense_cells);
    }

    [[nodiscard]] bool is_compressed() const noexcept { return m_pattern.is_compressed(); }

    // Random read access. Returns the stored value at (r, c), or T{} when the
    // entry is structurally absent. Works in both storage modes. O(log nnz_row)
    // via binary search over the canonical-sorted inner indices.
    [[nodiscard]] T coeff(crd::u32 r, crd::u32 c) const noexcept
    {
        const crd::u32 o      = is_row_major ? r : c;
        const crd::u32 target = is_row_major ? c : r;
        const crd::u32 start  = m_pattern.outer_ptr[o];
        const crd::u32 used   = m_pattern.inner_count(o);
        const crd::u32 pos    = lower_bound_inner(start, used, target);
        if (pos < start + used && m_pattern.inner_idx[pos] == target)
        {
            return m_values.values[pos];
        }
        return T{};
    }

    // -------- Uncompressed (incremental insert) path --------------------

    // Create an empty uncompressed matrix with `slack_per_outer` insert slots
    // reserved per inner vector (Eigen 4-array uncompressed layout).
    [[nodiscard]] static SparseMatrix make_uncompressed(crd::memory::IAllocator* alloc, crd::u32 rows, crd::u32 cols,
                                                        crd::u32 slack_per_outer = 4)
    {
        const crd::u32 n_outer = is_row_major ? rows : cols;
        const crd::u32 slack   = slack_per_outer == 0 ? 1U : slack_per_outer;

        SparsePattern pat(alloc);
        pat.rows       = rows;
        pat.cols       = cols;
        pat.format     = Format;
        pat.block_size = 1;
        pat.outer_ptr.resize(static_cast<crd::usize>(n_outer) + 1);
        for (crd::u32 o = 0; o <= n_outer; ++o)
        {
            pat.outer_ptr[o] = o * slack;
        }
        pat.inner_idx.resize(static_cast<crd::usize>(n_outer) * slack);
        pat.inner_nnz.resize(n_outer);  // all zero => empty matrix, uncompressed
        SparseValues<T> vals(alloc);
        vals.values.resize(static_cast<crd::usize>(n_outer) * slack);
        pat.recompute_topology_hash();
        return SparseMatrix(std::move(pat), std::move(vals));
    }

    // Find-or-insert (r, c); returns a mutable reference to its value (T{} on
    // fresh insert). Uncompressed only. Maintains the canonical column-sorted
    // invariant. Structural inserts leave topology_hash STALE -- after a batch
    // of inserts call pattern().recompute_topology_hash() (or make_compressed,
    // which recomputes). O(nnz_row) amortised; a full inner vector triggers a
    // storage grow.
    [[nodiscard]] T& coeff_ref(crd::u32 r, crd::u32 c)
    {
        CRD_ASSERT_MSG(!is_compressed(), "coeff_ref requires an uncompressed matrix (make_uncompressed)");
        const crd::u32 o      = is_row_major ? r : c;
        const crd::u32 target = is_row_major ? c : r;

        crd::u32 start = m_pattern.outer_ptr[o];
        crd::u32 used  = m_pattern.inner_nnz[o];
        crd::u32 pos   = lower_bound_inner(start, used, target);
        if (pos < start + used && m_pattern.inner_idx[pos] == target)
        {
            return m_values.values[pos];  // already present
        }

        const crd::u32 capacity = m_pattern.outer_ptr[o + 1] - m_pattern.outer_ptr[o];
        if (used == capacity)
        {
            grow_uncompressed();
            // Offsets changed; recompute for this outer.
            start = m_pattern.outer_ptr[o];
            used  = m_pattern.inner_nnz[o];
            pos   = lower_bound_inner(start, used, target);
        }

        // Shift [pos, start+used) right by one to open a slot at pos.
        for (crd::u32 i = start + used; i > pos; --i)
        {
            m_pattern.inner_idx[i] = m_pattern.inner_idx[i - 1];
            m_values.values[i]     = m_values.values[i - 1];
        }
        m_pattern.inner_idx[pos] = target;
        m_values.values[pos]     = T{};
        ++m_pattern.inner_nnz[o];
        return m_values.values[pos];
    }

    // Compact away all insert slack -> compressed storage. Recomputes
    // topology_hash. No-op if already compressed.
    void make_compressed()
    {
        if (is_compressed())
        {
            return;
        }
        const crd::u32 n_outer = m_pattern.n_outer();

        // Build tight arrays into a fresh pattern (same allocator), then swap.
        SparsePattern   packed(allocator());
        SparseValues<T> packed_vals(allocator());
        packed.rows       = m_pattern.rows;
        packed.cols       = m_pattern.cols;
        packed.format     = Format;
        packed.block_size = m_pattern.block_size;
        packed.outer_ptr.resize(static_cast<crd::usize>(n_outer) + 1);
        packed.outer_ptr[0] = 0;
        const crd::usize total = m_pattern.nnz();
        packed.inner_idx.reserve(total);
        packed_vals.values.reserve(total);
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            const crd::u32 start = m_pattern.outer_ptr[o];
            const crd::u32 used  = m_pattern.inner_nnz[o];
            for (crd::u32 t = 0; t < used; ++t)
            {
                packed.inner_idx.push_back(m_pattern.inner_idx[start + t]);
                packed_vals.values.push_back(m_values.values[start + t]);
            }
            packed.outer_ptr[o + 1] = static_cast<crd::u32>(packed.inner_idx.size());
        }
        packed.recompute_topology_hash();
        m_pattern = std::move(packed);
        m_values  = std::move(packed_vals);
    }

private:
    static constexpr bool is_row_major = (Format == SparseFormat::Csr);

    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_pattern.outer_ptr.allocator(); }

    // First index in [start, start+used) whose inner_idx >= target.
    [[nodiscard]] crd::u32 lower_bound_inner(crd::u32 start, crd::u32 used, crd::u32 target) const noexcept
    {
        crd::u32 lo = start;
        crd::u32 hi = start + used;
        while (lo < hi)
        {
            const crd::u32 mid = lo + (hi - lo) / 2;
            if (m_pattern.inner_idx[mid] < target)
            {
                lo = mid + 1;
            }
            else
            {
                hi = mid;
            }
        }
        return lo;
    }

    // Rebuild storage giving every inner vector used*2 + base slack. Preserves
    // used regions + inner_nnz; offsets change.
    void grow_uncompressed()
    {
        const crd::u32 n_outer = m_pattern.n_outer();
        crd::containers::Array<crd::u32> new_outer(allocator());
        new_outer.resize(static_cast<crd::usize>(n_outer) + 1);
        new_outer[0] = 0;
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            const crd::u32 used = m_pattern.inner_nnz[o];
            const crd::u32 cap  = used * 2U + 4U;
            new_outer[o + 1]    = new_outer[o] + cap;
        }
        const crd::u32 new_total = new_outer[n_outer];

        crd::containers::Array<crd::u32> new_idx(allocator());
        new_idx.resize(new_total);
        crd::containers::Array<T> new_vals(allocator());
        new_vals.resize(new_total);
        for (crd::u32 o = 0; o < n_outer; ++o)
        {
            const crd::u32 old_start = m_pattern.outer_ptr[o];
            const crd::u32 new_start = new_outer[o];
            const crd::u32 used      = m_pattern.inner_nnz[o];
            for (crd::u32 t = 0; t < used; ++t)
            {
                new_idx[new_start + t]  = m_pattern.inner_idx[old_start + t];
                new_vals[new_start + t] = m_values.values[old_start + t];
            }
        }
        m_pattern.outer_ptr = std::move(new_outer);
        m_pattern.inner_idx = std::move(new_idx);
        m_values.values     = std::move(new_vals);
    }

    SparsePattern   m_pattern;
    SparseValues<T> m_values;
};

} // namespace crd::hesap::sparse
