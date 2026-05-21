// crd-hesap-sparse v1a-1 -- SparsePattern topology hash.
//
// FNV-1a-64 over the structural fields. See sparse_pattern.hpp for the
// pinned algorithm (D1): little-endian explicit byte feed, used-bytes only,
// format + block_size mixed in.

#include <crd/hesap/sparse/sparse_pattern.hpp>

namespace crd::hesap::sparse
{
namespace
{

constexpr crd::u64 kFnvOffsetBasis = 0xCBF29CE484222325ULL;
constexpr crd::u64 kFnvPrime       = 0x00000100000001B3ULL;

inline void fnv1a_byte(crd::u64& h, crd::u8 b) noexcept
{
    h ^= static_cast<crd::u64>(b);
    h *= kFnvPrime;
}

// Feed an integer little-endian via explicit shifts (endian-independent).
template <typename UInt>
inline void fnv1a_int(crd::u64& h, UInt v) noexcept
{
    for (crd::usize i = 0; i < sizeof(UInt); ++i)
    {
        fnv1a_byte(h, static_cast<crd::u8>((static_cast<crd::u64>(v) >> (8U * i)) & 0xFFU));
    }
}

} // namespace

crd::u64 topology_hash(const SparsePattern& pattern) noexcept
{
    crd::u64 h = kFnvOffsetBasis;

    fnv1a_int<crd::u32>(h, pattern.rows);
    fnv1a_int<crd::u32>(h, pattern.cols);
    fnv1a_int<crd::u8>(h, static_cast<crd::u8>(pattern.format));
    fnv1a_int<crd::u16>(h, pattern.block_size);

    // Canonical LOGICAL walk: per inner vector, feed its used count then its
    // used, sorted indices. Slack-invariant => compressed and uncompressed
    // representations of the same matrix hash identically.
    const crd::u32 n = pattern.n_outer();
    fnv1a_int<crd::u32>(h, n);
    for (crd::u32 k = 0; k < n; ++k)
    {
        const crd::u32 start = pattern.outer_ptr[k];
        const crd::u32 used  = pattern.inner_count(k);
        fnv1a_int<crd::u32>(h, used);
        for (crd::u32 t = 0; t < used; ++t)
        {
            fnv1a_int<crd::u32>(h, pattern.inner_idx[start + t]);
        }
    }

    return h;
}

void SparsePattern::recompute_topology_hash() noexcept
{
    topology_hash = sparse::topology_hash(*this);
}

} // namespace crd::hesap::sparse
