#include <crd/resources/resource_id.hpp>
#include "detail/sha1.hpp"

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

#include <atomic>
#include <cstring>
#include <random>

namespace crd::resources
{

namespace
{

// Extract UUID byte i (0-based, 0..15) from a ResourceId.
// Bytes 0..7 are packed big-endian into hi; bytes 8..15 into lo.
crd::u8 uuid_byte(const ResourceId& id, int i) noexcept
{
    if (i < 8)
    {
        return static_cast<crd::u8>(id.hi >> (56U - static_cast<crd::u32>(i) * 8U));
    }
    return static_cast<crd::u8>(id.lo >> (56U - static_cast<crd::u32>(i - 8) * 8U));
}

// Set UUID byte i into a ResourceId.
void set_uuid_byte(ResourceId& id, int i, crd::u8 val) noexcept
{
    if (i < 8)
    {
        const crd::u32 shift = 56U - static_cast<crd::u32>(i) * 8U;
        id.hi = (id.hi & ~(static_cast<crd::u64>(0xFFU) << shift))
              | (static_cast<crd::u64>(val) << shift);
    }
    else
    {
        const crd::u32 shift = 56U - static_cast<crd::u32>(i - 8) * 8U;
        id.lo = (id.lo & ~(static_cast<crd::u64>(0xFFU) << shift))
              | (static_cast<crd::u64>(val) << shift);
    }
}

// Build a ResourceId from 16 raw UUID bytes.
ResourceId from_bytes(const crd::u8 (&raw)[16]) noexcept
{
    ResourceId id;
    id.hi = 0;
    id.lo = 0;
    for (int i = 0; i < 16; ++i)
    {
        set_uuid_byte(id, i, raw[i]);
    }
    return id;
}

// Hex character to nibble value; returns -1 for invalid characters.
int hex_nibble(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

// Cerid content namespace UUID for UUID v5 (ADR-0037).
// Arbitrary fixed namespace used when content-addressing engine assets.
constexpr crd::u8 kCrdContentNamespace[16] = {
    0x1eU, 0x57U, 0xabU, 0x3cU, // time-low
    0xf2U, 0xa4U,                // time-mid
    0x5fU, 0x62U,                // time-high-and-version (5 already set here for documentation)
    0xa0U, 0x12U,                // clock-seq
    0x3bU, 0x4cU, 0x5dU, 0x6eU, 0x7fU, 0x80U // node
};

} // anonymous namespace

ResourceId ResourceId::mint_random() noexcept
{
    // Thread-local RNG seeded from std::random_device. This avoids contention
    // on a shared RNG across worker threads while preserving good randomness.
    thread_local std::mt19937_64 rng{std::random_device{}()};  // NOLINT(cert-msc51-cpp)

    ResourceId id;
    id.hi = rng();
    id.lo = rng();

    // Set UUID v4 version: byte[6] high nibble = 0x4.
    // byte[6] lives at bits 15..8 of hi; its high nibble is bits 15..12.
    id.hi = (id.hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;

    // Set RFC 4122 variant: byte[8] high 2 bits = 0b10.
    // byte[8] is the MSByte of lo; its high 2 bits are lo bits 63..62.
    id.lo = (id.lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    return id;
}

ResourceId ResourceId::from_content(crd::containers::ConstSpan<crd::u8> bytes) noexcept
{
    // UUID v5: SHA-1(namespace || content), first 16 bytes with version/variant stamped.
    const auto digest = detail::sha1_compute(
        kCrdContentNamespace, sizeof(kCrdContentNamespace),
        bytes.data(), bytes.size());

    crd::u8 raw[16];
    std::memcpy(raw, digest.data(), 16);

    // Stamp UUID version 5: byte[6] high nibble = 0x5.
    raw[6] = static_cast<crd::u8>((raw[6] & 0x0FU) | 0x50U);
    // Stamp RFC 4122 variant: byte[8] high 2 bits = 0b10.
    raw[8] = static_cast<crd::u8>((raw[8] & 0x3FU) | 0x80U);

    return from_bytes(raw);
}

ResourceId ResourceId::parse(crd::containers::StringView text) noexcept
{
    if (text.size() != 36U)
    {
        return kNullResourceId;
    }
    if (text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-')
    {
        return kNullResourceId;
    }

    crd::u8 raw[16] = {};
    int     byte_idx = 0;

    for (int i = 0; i < 36; ++i)
    {
        if (text[static_cast<crd::usize>(i)] == '-')
        {
            continue;
        }
        const int hi_nib = hex_nibble(text[static_cast<crd::usize>(i)]);
        const int lo_nib = hex_nibble(text[static_cast<crd::usize>(i) + 1U]);
        if (hi_nib < 0 || lo_nib < 0 || byte_idx >= 16)
        {
            return kNullResourceId;
        }
        raw[byte_idx++] = static_cast<crd::u8>((hi_nib << 4) | lo_nib);
        ++i; // consumed the lo nibble
    }

    if (byte_idx != 16)
    {
        return kNullResourceId;
    }

    return from_bytes(raw);
}

crd::containers::String ResourceId::to_string(crd::memory::IAllocator* a) const
{
    static const char kHex[] = "0123456789abcdef";

    crd::u8 raw[16];
    for (int i = 0; i < 16; ++i)
    {
        raw[i] = uuid_byte(*this, i);
    }

    char buf[37];
    int  pos = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            buf[pos++] = '-';
        }
        buf[pos++] = kHex[raw[i] >> 4U];
        buf[pos++] = kHex[raw[i] & 0x0FU];
    }
    buf[36] = '\0';

    return crd::containers::String(buf, 36U, a);
}

} // namespace crd::resources
