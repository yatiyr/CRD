#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::direct
{
// -----------------------------------------------------------------------
// Frontal<T> — a dense work matrix tagged with the GLOBAL row/column ids
// its local rows/cols represent. The unit the multifrontal factorizations
// (QR — v5c, LDLᵀ — v5d, HSS — v5e) assemble, factor, and pass up the
// assembly tree. Row-major dense storage; `row_index`/`col_index` are
// ASCENDING global ids (the determinism + extend-add merge depend on it).
//
// Phase 3.1.6 v5a-1: defined + unit-tested standalone here even though the
// flagship left-looking supernodal Cholesky (v5a-1..3) does not consume it
// — three later slices do, and proving the assembly kernel correct in
// isolation is the right place to pin its determinism contract.
// -----------------------------------------------------------------------
template <typename T>
struct Frontal
{
    crd::u32 nrows = 0;
    crd::u32 ncols = 0;
    crd::containers::Array<T> data;             // nrows * ncols, row-major
    crd::containers::Array<crd::u32> row_index; // length nrows; ascending global row ids
    crd::containers::Array<crd::u32> col_index; // length ncols; ascending global col ids

    explicit Frontal(crd::memory::IAllocator* alloc) : data(alloc), row_index(alloc), col_index(alloc) {}

    void resize(crd::u32 r, crd::u32 c)
    {
        nrows = r;
        ncols = c;
        data.resize(static_cast<crd::usize>(r) * static_cast<crd::usize>(c));
        row_index.resize(r);
        col_index.resize(c);
    }

    void zero_fill()
    {
        for (crd::usize i = 0; i < data.size(); ++i)
        {
            data[i] = T{0};
        }
    }

    [[nodiscard]] T& at(crd::u32 i, crd::u32 j) noexcept
    {
        return data[static_cast<crd::usize>(i) * static_cast<crd::usize>(ncols) + j];
    }
    [[nodiscard]] const T& at(crd::u32 i, crd::u32 j) const noexcept
    {
        return data[static_cast<crd::usize>(i) * static_cast<crd::usize>(ncols) + j];
    }
};

// -----------------------------------------------------------------------
// extend_add — scatter-add the child contribution block into the parent
// front: for each child entry (a, b),
//     parent.at(R[a], C[b]) += child.at(a, b)
// where R maps child-local rows → parent-local rows (by matching global
// ids), C likewise for columns.
//
// PRECONDITION: child.row_index ⊆ parent.row_index AND child.col_index ⊆
// parent.col_index, both ascending — so the maps are built by an ascending
// two-pointer merge (O(nrows + ncols), no search). `scratch` provides the
// column-map workspace (per feedback_no_hidden_default_allocator_malloc —
// a function needing scratch takes the caller's allocator, never conjures
// one). The central multifrontal assembly kernel; D(direct)-5 pins that
// children are extend-added in a FIXED postorder, making the parent front
// thread-order-independent → the cross-thread determinism moat.
// -----------------------------------------------------------------------
template <typename T>
void extend_add(Frontal<T>& parent, const Frontal<T>& child, crd::memory::IAllocator* scratch)
{
    // Column map (child-local col → parent-local col), ascending two-pointer.
    crd::containers::Array<crd::u32> cmap(scratch);
    cmap.resize(child.ncols);
    {
        crd::u32 p = 0;
        for (crd::u32 b = 0; b < child.ncols; ++b)
        {
            const crd::u32 g = child.col_index[b];
            while (p < parent.ncols && parent.col_index[p] < g)
            {
                ++p;
            }
            CRD_ASSERT_MSG(p < parent.ncols && parent.col_index[p] == g,
                           "extend_add: child column id absent from parent front (precondition violated)");
            cmap[b] = p;
        }
    }
    // Row pointer is monotonic across child rows (both ascending) → advance once.
    crd::u32 pr = 0;
    for (crd::u32 a = 0; a < child.nrows; ++a)
    {
        const crd::u32 g = child.row_index[a];
        while (pr < parent.nrows && parent.row_index[pr] < g)
        {
            ++pr;
        }
        CRD_ASSERT_MSG(pr < parent.nrows && parent.row_index[pr] == g,
                       "extend_add: child row id absent from parent front (precondition violated)");
        for (crd::u32 b = 0; b < child.ncols; ++b)
        {
            parent.at(pr, cmap[b]) += child.at(a, b);
        }
    }
}

} // namespace crd::hesap::direct
