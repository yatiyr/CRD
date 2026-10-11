#pragma once

// Layout probes: does a strided view address what its boundary says it does? DIAG.10c.
//
// A shape and a set of strides arriving at a boundary (a DLPack capsule from another framework, a file header, a
// caller's view) are claims. A wrong claim rarely crashes where it arrives: an element count that wraps reads as a
// small tensor, an offset past the i64 range is undefined pointer arithmetic in the first kernel that walks it, and a
// view whose strides make two indices meet writes one element twice. The layout probe checks the claim, class by class,
// without touching any element (docs/design/runtime-diagnostics.md, DIAG.10c):
// - the rank against the boundary's bound;
// - the element count (the product of the extents, and its byte size) against the u64 range. An empty view's other
//   extents must still multiply out, because a compact layout of the same dims is computed from them;
// - the offsets of the first and last addressed element, in elements and in bytes, against the i64 range, computed
//   exactly per dimension, never by multiplying along;
// - those offsets against the storage the caller says the view lives in, when it says. Storage it does not name is
//   reported as unchecked, never as inside;
// - whether two distinct indices address one element. Only what can be proven is reported: a zero stride on an extent
//   above 1, or two dimensions whose strides are multiples with the smaller one's extent reaching the larger stride,
//   overlap for certain; strides that nest (each above the span the smaller ones cover) are disjoint for certain;
//   anything else is "not proven" and is never reported as an overlap. Overlap is a violation only where the boundary
//   writes through the view: a broadcast read is a valid stride-0 view.
//
// The strided value probe walks such a view and judges every value against a FloatDomain (crd/core/value_domain.hpp)
// from its bit pattern only, reporting the first offender by its logical coordinates, not by a flat index that means
// nothing for a strided view.

#include <crd/core/types.hpp>
#include <crd/core/value_domain.hpp>

namespace crd
{

// The rank bound a probe report can hold coordinates for (the engine's tensor bound).
inline constexpr crd::u32 kLayoutMaxRank = 8U;

// What a layout claims that cannot hold, one bit each.
// NOLINTNEXTLINE(performance-enum-size)
enum class LayoutViolation : crd::u32
{
    RankOverflow         = 1U << 0U, // more dimensions than the boundary's bound (nothing else is checked)
    ElementCountOverflow = 1U << 1U, // the extents' product, or its byte size, does not fit a u64
    OffsetOverflow       = 1U << 2U, // an addressed element's offset, in elements or bytes, does not fit an i64
    OutsideStorage       = 1U << 3U, // an addressed element lies outside the storage the caller named
    Overlap              = 1U << 4U, // a written view addresses one element from two distinct indices (proven)
};
inline constexpr crd::u32 kLayoutViolationCount = 5U;
[[nodiscard]] const char* layout_violation_name(LayoutViolation v) noexcept;

// Whether distinct indices of a view address distinct elements.
// NOLINTNEXTLINE(performance-enum-size)
enum class LayoutOverlap : crd::u8
{
    Disjoint,    // proven: every index has its own element
    Overlapping, // proven: two distinct indices meet
    NotProven,   // neither could be shown (or the layout was not examined)
};
[[nodiscard]] const char* layout_overlap_name(LayoutOverlap o) noexcept;

// What the boundary expects of a layout.
struct LayoutExpectation
{
    crd::u32 max_rank     = kLayoutMaxRank;
    crd::u64 element_size = 1U;    // bytes per element, for the byte-size and byte-offset checks
    bool     writable     = false; // the boundary writes through the view, so a proven overlap is a violation
    bool     has_storage  = false; // [storage_first, storage_end) in elements, relative to the view's data pointer
    crd::i64 storage_first = 0;
    crd::i64 storage_end   = 0;
};

struct LayoutReport
{
    crd::u32      rank            = 0U;
    crd::u32      violations      = 0U;     // LayoutViolation bits
    crd::u64      element_count   = 0U;     // the extents' product (valid without ElementCountOverflow)
    crd::i64      min_offset      = 0;      // first and last addressed element, in elements (valid when the view is
    crd::i64      max_offset      = 0;      // non-empty and without OffsetOverflow; 0 otherwise)
    LayoutOverlap overlap         = LayoutOverlap::NotProven;
    bool          storage_checked = false;  // the offsets were compared with a named storage range
    [[nodiscard]] bool ok() const noexcept { return violations == 0U; }
};

// Probe `rank` extents and element strides. Reads shape[0..rank) and strides[0..rank) only when rank is within the
// bound. Never touches an element.
[[nodiscard]] LayoutReport probe_layout(const crd::u64* shape, const crd::i64* strides, crd::u32 rank,
                                        const LayoutExpectation& expect) noexcept;

// What a walk over a strided view found. `values.first_index` is the first offender's row-major logical element index;
// `first_coords` are its coordinates and `first_lane` which scalar of a multi-lane element (1 for a complex value's
// imaginary part). `walked` is false when the layout did not pass the probe (an overflowing or over-rank layout is not
// walked) or the view is empty; `values.checked` counts scalars.
struct StridedValueReport
{
    bool              walked = false;
    LayoutReport      layout{};
    ValueDomainReport values{};
    crd::u64          first_coords[kLayoutMaxRank] = {};
    crd::u32          first_lane                    = 0U;
};

// Walk every element of a strided view whose elements are `lanes` consecutive scalars (1 for real, 2 for an
// interleaved complex value) and whose strides count elements. Reads bits only; changes nothing.
[[nodiscard]] StridedValueReport probe_strided_values(const FloatDomain& d, const crd::f32* base,
                                                      const crd::u64* shape, const crd::i64* strides, crd::u32 rank,
                                                      crd::u32 lanes) noexcept;
[[nodiscard]] StridedValueReport probe_strided_values(const FloatDomain& d, const crd::f64* base,
                                                      const crd::u64* shape, const crd::i64* strides, crd::u32 rank,
                                                      crd::u32 lanes) noexcept;

} // namespace crd
