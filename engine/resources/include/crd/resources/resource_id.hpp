#pragma once

#include <crd/containers/hash.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::resources
{

// 128-bit resource identifier using the UUID layout.
//
// Three minting modes (ADR-0037):
//   mint_random()    — UUID v4 (random); for first import and procedural resources.
//   from_content()   — UUID v5 (SHA-1 of namespace + bytes); same content → same id.
//   parse()          — round-trip from the 8-4-4-4-12 textual form.
//
// On-disk layout inside CRDR: hi (u64 LE) then lo (u64 LE).
// In memory: byte[0..7] packed big-endian into hi; byte[8..15] into lo.
// Byte extraction: byte[i] = (i<8) ? (hi >> (56-i*8)) & 0xFF : (lo >> (56-(i-8)*8)) & 0xFF.
struct ResourceId
{
    crd::u64 hi = 0;
    crd::u64 lo = 0;

    [[nodiscard]] static ResourceId mint_random() noexcept;

    [[nodiscard]] static ResourceId from_content(
        crd::containers::ConstSpan<crd::u8> bytes) noexcept;

    [[nodiscard]] static ResourceId parse(crd::containers::StringView text) noexcept;

    [[nodiscard]] crd::containers::String to_string(
        crd::memory::IAllocator* a = crd::memory::default_allocator()) const;

    [[nodiscard]] constexpr bool is_null() const noexcept { return hi == 0 && lo == 0; }
    [[nodiscard]] constexpr bool operator==(const ResourceId&) const noexcept = default;
};

inline constexpr ResourceId kNullResourceId{0, 0};

} // namespace crd::resources

// DefaultHash specialization so ResourceId can be a HashMap key.
namespace crd::containers
{
template <>
struct DefaultHash<crd::resources::ResourceId>
{
    crd::u64 operator()(const crd::resources::ResourceId& id) const noexcept
    {
        return hash_u64(id.hi ^ 0x9E3779B97F4A7C15ULL) ^ hash_u64(id.lo ^ 0x6C62272E07BB0142ULL);
    }
};
} // namespace crd::containers
