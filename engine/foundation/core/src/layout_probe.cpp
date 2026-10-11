#include <crd/core/layout_probe.hpp>

#include <cstring>
#include <limits>

// Integer arithmetic only, every product and sum checked before it is formed (see the header).

namespace crd
{

namespace
{
constexpr crd::i64 kI64Max  = std::numeric_limits<crd::i64>::max();
constexpr crd::i64 kI64Min  = std::numeric_limits<crd::i64>::min();
constexpr crd::u64 kU64Max  = std::numeric_limits<crd::u64>::max();
constexpr crd::u64 kTwoTo63 = 1ULL << 63U;

constexpr crd::u32 bit(LayoutViolation v) noexcept
{
    return static_cast<crd::u32>(v);
}

// a * b for a signed a and an unsigned b; false when the product leaves the i64 range.
bool mul_i64_u64(crd::i64 a, crd::u64 b, crd::i64& out) noexcept
{
    if (a == 0 || b == 0U)
    {
        out = 0;
        return true;
    }
    if (b > static_cast<crd::u64>(kI64Max))
    {
        if (a == -1 && b == kTwoTo63) // the one product past i64's positive range that still fits: -2^63
        {
            out = kI64Min;
            return true;
        }
        return false;
    }
    const auto sb = static_cast<crd::i64>(b);
    if (a > 0 ? a > kI64Max / sb : a < kI64Min / sb)
    {
        return false;
    }
    out = a * sb;
    return true;
}

bool add_i64(crd::i64 a, crd::i64 b, crd::i64& out) noexcept
{
    if (b > 0 ? a > kI64Max - b : a < kI64Min - b)
    {
        return false;
    }
    out = a + b;
    return true;
}

bool mul_u64(crd::u64 a, crd::u64 b, crd::u64& out) noexcept
{
    if (a != 0U && b > kU64Max / a)
    {
        return false;
    }
    out = a * b;
    return true;
}

crd::u64 magnitude(crd::i64 v) noexcept
{
    return v < 0 ? (~static_cast<crd::u64>(v)) + 1U : static_cast<crd::u64>(v);
}

// Whether two distinct indices meet, for the dims with an extent above 1. Proven overlaps first, then a nesting proof.
LayoutOverlap prove_overlap(const crd::u64* shape, const crd::i64* strides, crd::u32 rank) noexcept
{
    crd::u64 extent[kLayoutMaxRank] = {};
    crd::u64 step[kLayoutMaxRank]   = {};
    crd::u32 n                      = 0U;
    for (crd::u32 d = 0U; d < rank; ++d)
    {
        if (shape[d] > 1U)
        {
            extent[n] = shape[d];
            step[n]   = magnitude(strides[d]);
            ++n;
        }
    }
    for (crd::u32 i = 0U; i < n; ++i)
    {
        if (step[i] == 0U)
        {
            return LayoutOverlap::Overlapping; // every index along i addresses the same element
        }
    }
    // |s_b| divides |s_a| with k = |s_a| / |s_b| <= extent_b - 1: index (1 along a) meets index (k along b) when the
    // signs agree, and index (1 along a, k along b) meets the origin when they differ.
    for (crd::u32 a = 0U; a < n; ++a)
    {
        for (crd::u32 b = 0U; b < n; ++b)
        {
            if (a != b && step[b] <= step[a] && step[a] % step[b] == 0U && step[a] / step[b] <= extent[b] - 1U)
            {
                return LayoutOverlap::Overlapping;
            }
        }
    }
    // Nesting: sorted by stride, each stride must exceed the largest offset the smaller ones reach.
    for (crd::u32 i = 1U; i < n; ++i)
    {
        for (crd::u32 j = i; j > 0U && step[j - 1U] > step[j]; --j)
        {
            const crd::u64 s = step[j];
            const crd::u64 e = extent[j];
            step[j]          = step[j - 1U];
            extent[j]        = extent[j - 1U];
            step[j - 1U]     = s;
            extent[j - 1U]   = e;
        }
    }
    crd::u64 covered = 0U;
    for (crd::u32 i = 0U; i < n; ++i)
    {
        crd::u64 reach = 0U;
        if (step[i] <= covered || !mul_u64(step[i], extent[i] - 1U, reach) || reach > kU64Max - covered)
        {
            return LayoutOverlap::NotProven;
        }
        covered += reach;
    }
    return LayoutOverlap::Disjoint;
}

template <typename Scalar> crd::u64 scalar_bits(Scalar v) noexcept
{
    if constexpr (sizeof(Scalar) == 4U)
    {
        crd::u32 b = 0U;
        std::memcpy(&b, &v, sizeof b);
        return b;
    }
    else
    {
        crd::u64 b = 0U;
        std::memcpy(&b, &v, sizeof b);
        return b;
    }
}

template <typename Scalar>
StridedValueReport walk(const FloatDomain& d, const Scalar* base, const crd::u64* shape, const crd::i64* strides,
                        crd::u32 rank, crd::u32 lanes) noexcept
{
    StridedValueReport r{};
    r.values.domain_valid = domain_valid(d);
    LayoutExpectation expect{};
    expect.element_size = static_cast<crd::u64>(lanes) * sizeof(Scalar);
    r.layout            = probe_layout(shape, strides, rank, expect);
    if (!r.values.domain_valid || lanes == 0U || !r.layout.ok() || r.layout.element_count == 0U || base == nullptr)
    {
        return r;
    }
    r.walked = true;
    crd::u64 coords[kLayoutMaxRank] = {};
    crd::i64 offset                 = 0;
    for (crd::u64 index = 0U; index < r.layout.element_count; ++index)
    {
        const Scalar* element = base + offset * static_cast<crd::i64>(lanes);
        for (crd::u32 lane = 0U; lane < lanes; ++lane)
        {
            const crd::u32 v = value_violations(d, element[lane]);
            ++r.values.checked;
            if (v == 0U)
            {
                continue;
            }
            ++r.values.violating;
            r.values.violations |= v;
            if (r.values.first_index == kNoValueIndex)
            {
                r.values.first_index      = index;
                r.values.first_violations = v;
                r.values.first_bits       = scalar_bits(element[lane]);
                r.first_lane              = lane;
                for (crd::u32 k = 0U; k < rank; ++k)
                {
                    r.first_coords[k] = coords[k];
                }
            }
        }
        // Advance the row-major odometer; every partial offset formed here is an addressed element's offset.
        for (crd::u32 k = rank; k-- > 0U;)
        {
            if (coords[k] + 1U < shape[k])
            {
                ++coords[k];
                offset += strides[k];
                break;
            }
            offset -= strides[k] * static_cast<crd::i64>(coords[k]);
            coords[k] = 0U;
        }
    }
    return r;
}
} // namespace

const char* layout_violation_name(LayoutViolation v) noexcept
{
    switch (v)
    {
    case LayoutViolation::RankOverflow:
        return "rank-overflow";
    case LayoutViolation::ElementCountOverflow:
        return "element-count-overflow";
    case LayoutViolation::OffsetOverflow:
        return "offset-overflow";
    case LayoutViolation::OutsideStorage:
        return "outside-storage";
    case LayoutViolation::Overlap:
        return "overlap";
    }
    return "unknown";
}

const char* layout_overlap_name(LayoutOverlap o) noexcept
{
    switch (o)
    {
    case LayoutOverlap::Disjoint:
        return "disjoint";
    case LayoutOverlap::Overlapping:
        return "overlapping";
    case LayoutOverlap::NotProven:
        return "not-proven";
    }
    return "unknown";
}

LayoutReport probe_layout(const crd::u64* shape, const crd::i64* strides, crd::u32 rank,
                          const LayoutExpectation& expect) noexcept
{
    LayoutReport r{};
    r.rank = rank;
    if (rank > expect.max_rank || rank > kLayoutMaxRank)
    {
        r.violations = bit(LayoutViolation::RankOverflow);
        return r;
    }

    // The element count, and the product of the nonzero extents an empty view must still be able to form.
    crd::u64 count   = 1U;
    crd::u64 nonzero = 1U;
    bool     counted = true;
    for (crd::u32 d = 0U; d < rank; ++d)
    {
        if (shape[d] == 0U)
        {
            count = 0U;
            continue;
        }
        if (!mul_u64(nonzero, shape[d], nonzero))
        {
            counted = false;
            break;
        }
        if (count != 0U)
        {
            count = nonzero;
        }
    }
    crd::u64 bytes = 0U;
    if (!counted || !mul_u64(nonzero, expect.element_size, bytes))
    {
        r.violations |= bit(LayoutViolation::ElementCountOverflow);
        return r;
    }
    r.element_count = count;
    if (count == 0U)
    {
        r.overlap         = LayoutOverlap::Disjoint; // no element, no two indices to meet
        r.storage_checked = expect.has_storage;      // nothing is addressed, so nothing lies outside
        return r;
    }

    // The first and last addressed offsets, one dimension at a time, then in bytes.
    crd::i64 lo       = 0;
    crd::i64 hi       = 0;
    bool     in_range = true;
    for (crd::u32 d = 0U; d < rank && in_range; ++d)
    {
        crd::i64 reach = 0;
        in_range       = mul_i64_u64(strides[d], shape[d] - 1U, reach);
        if (in_range)
        {
            in_range = reach < 0 ? add_i64(lo, reach, lo) : add_i64(hi, reach, hi);
        }
    }
    crd::i64 lo_bytes = 0;
    crd::i64 hi_bytes = 0;
    if (!in_range || !mul_i64_u64(lo, expect.element_size, lo_bytes) || !mul_i64_u64(hi, expect.element_size, hi_bytes))
    {
        r.violations |= bit(LayoutViolation::OffsetOverflow);
    }
    else
    {
        r.min_offset = lo;
        r.max_offset = hi;
        if (expect.has_storage)
        {
            r.storage_checked = true;
            if (lo < expect.storage_first || hi >= expect.storage_end)
            {
                r.violations |= bit(LayoutViolation::OutsideStorage);
            }
        }
    }

    r.overlap = prove_overlap(shape, strides, rank);
    if (expect.writable && r.overlap == LayoutOverlap::Overlapping)
    {
        r.violations |= bit(LayoutViolation::Overlap);
    }
    return r;
}

StridedValueReport probe_strided_values(const FloatDomain& d, const crd::f32* base, const crd::u64* shape,
                                        const crd::i64* strides, crd::u32 rank, crd::u32 lanes) noexcept
{
    return walk(d, base, shape, strides, rank, lanes);
}

StridedValueReport probe_strided_values(const FloatDomain& d, const crd::f64* base, const crd::u64* shape,
                                        const crd::i64* strides, crd::u32 rank, crd::u32 lanes) noexcept
{
    return walk(d, base, shape, strides, rank, lanes);
}

} // namespace crd
