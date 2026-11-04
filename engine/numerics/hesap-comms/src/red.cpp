// RFC 2198's redundant audio data (crd/hesap/comms/red.hpp): headers through the wire-bytes load and writer.

#include <crd/containers/wire_bytes.hpp>
#include <crd/hesap/comms/red.hpp>

namespace crd::hesap::comms
{
namespace
{

using crd::u16;
using crd::u32;
using crd::u8;
using crd::usize;
using crd::containers::wire_load;
using crd::containers::WireWriter;

constexpr u32 kFollows = 0x80U;
constexpr usize kRedundantHeader = 4U;

// The 24 bits after header i's first byte, the offset and the length; the bytes hold the header.
u32 header_fields(std::span<const u8> bytes, usize i) noexcept
{
    return wire_load<u32>(bytes, kRedundantHeader * i) & 0x00FFFFFFU;
}

} // namespace

// Header i sits at 4·i, so a first pass checks the chain, the room and the lengths, and a second fills the blocks:
// a refusal leaves them as they were.
RedStatus red_parse(std::span<const u8> bytes, u32 timestamp, std::span<RedBlock> blocks, u32& count) noexcept
{
    usize n = 0U;
    usize redundant_bytes = 0U;
    for (bool follows = true; follows; ++n)
    {
        const usize at = kRedundantHeader * n;
        if (bytes.size() <= at)
        {
            return RedStatus::Truncated;
        }
        follows = (bytes[at] & kFollows) != 0U;
        if (follows && bytes.size() - at < kRedundantHeader)
        {
            return RedStatus::Truncated;
        }
        if (n == blocks.size())
        {
            return RedStatus::NoSpace;
        }
        redundant_bytes += follows ? header_fields(bytes, n) & RedBlock::kMaxLength : 0U;
    }
    const std::span<const u8> data = bytes.subspan((kRedundantHeader * (n - 1U)) + 1U);
    if (data.size() < redundant_bytes)
    {
        return RedStatus::Truncated;
    }
    usize at = 0U;
    for (usize i = 0U; i < n; ++i)
    {
        RedBlock& b = blocks[i];
        b.payload_type = static_cast<u8>(bytes[kRedundantHeader * i] & RedBlock::kMaxPayloadType);
        if (i + 1U == n)
        {
            b.offset = 0U;
            b.timestamp = timestamp;
            b.data = data.subspan(at);
            break;
        }
        const u32 fields = header_fields(bytes, i);
        const usize length = fields & RedBlock::kMaxLength;
        b.offset = static_cast<u16>(fields >> 10U);
        b.timestamp = timestamp - b.offset;
        b.data = data.subspan(at, length);
        at += length;
    }
    count = static_cast<u32>(n);
    return RedStatus::Ok;
}

usize red_bytes(std::span<const RedBlock> redundant, usize primary_bytes) noexcept
{
    usize total = 1U + primary_bytes;
    for (const RedBlock& b : redundant)
    {
        total += kRedundantHeader + b.data.size();
    }
    return total;
}

RedStatus red_encode(std::span<const RedBlock> redundant, u8 primary_type, std::span<const u8> primary,
                     std::span<u8> out, usize& written) noexcept
{
    for (usize i = 0U; i < redundant.size(); ++i)
    {
        const RedBlock& b = redundant[i];
        if (b.payload_type > RedBlock::kMaxPayloadType || b.offset > RedBlock::kMaxOffset ||
            b.data.size() > RedBlock::kMaxLength)
        {
            return RedStatus::OutOfRange;
        }
        if (b.offset == 0U)
        {
            return RedStatus::Repeated;
        }
        for (usize j = 0U; j < i; ++j)
        {
            if (redundant[j].offset == b.offset)
            {
                return RedStatus::Repeated;
            }
        }
    }
    if (primary_type > RedBlock::kMaxPayloadType)
    {
        return RedStatus::OutOfRange;
    }
    const usize total = red_bytes(redundant, primary.size());
    if (out.size() < total)
    {
        return RedStatus::NoSpace;
    }
    WireWriter w(out.first(total));
    for (const RedBlock& b : redundant)
    {
        w.put_u8(static_cast<u8>(kFollows | b.payload_type));
        const u32 fields = (u32{b.offset} << 10U) | static_cast<u32>(b.data.size());
        w.put_u16(static_cast<u16>(fields >> 8U));
        w.put_u8(static_cast<u8>(fields));
    }
    w.put_u8(primary_type);
    for (const RedBlock& b : redundant)
    {
        w.put_bytes(b.data);
    }
    w.put_bytes(primary);
    written = w.written();
    return RedStatus::Ok;
}

} // namespace crd::hesap::comms
