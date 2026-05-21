#pragma once

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// SparseValues<T> — the numeric leg of the trinity. A flat array of stored
// values, parallel to a SparsePattern's `inner_idx` (values[k] is the value
// at the structural slot inner_idx[k]). Raw `T` per ADR-0078 §5 (hesap is
// the numerical-kernel layer; the typed Quantity boundary lives where the
// engine consumes hesap, never inside it). T ∈ {f32, f64, Complex32,
// Complex64}.
//
// `frame_stamp` is an OPAQUE monotonic counter. Contract: the *consumer*
// bumps it whenever it mutates the values; the library only READS it (as a
// cache-invalidation hint — e.g. "have these values changed since the
// preconditioner was built?"). The library never writes frame_stamp itself.
//
// Move-only (owns its value Array), matching dense Vector<T> (D15).
// -----------------------------------------------------------------------

template <typename T>
struct SparseValues
{
    crd::containers::Array<T> values;
    crd::u32                  frame_stamp = 0;

    explicit SparseValues(crd::memory::IAllocator* alloc) : values(alloc) {}

    [[nodiscard]] crd::usize size() const noexcept { return values.size(); }
};

} // namespace crd::hesap::sparse
