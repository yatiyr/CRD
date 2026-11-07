// The board's zpp_bits arm: the messages' natural types, byte aligned, through its archives on the caller's bytes. A
// variable-size member goes as its count and then that many elements (zpp_bits' unsized views), and the reader checks
// each count against the plain message's array before reading the elements. Peer code: built without the engine's
// headers.

#include "board_model.hpp"

#include <span>
#include <zpp_bits.h>

namespace serialize_board
{

// zpp_bits counts an aggregate's members by brace initialisation, which an array member defeats, so the members are
// named: free functions it finds by argument-dependent lookup, the form its documentation gives for a type it cannot
// change. Only this unit serializes them with zpp_bits.
constexpr auto serialize(auto& archive, PlainEntity& e)
{
    return archive(e.id, e.x, e.y, e.z, e.turn, e.vx, e.vy, e.vz, e.health, e.stance, e.firing, e.has_target, e.target);
}

constexpr auto serialize(auto& archive, const PlainEntity& e)
{
    return archive(e.id, e.x, e.y, e.z, e.turn, e.vx, e.vy, e.vz, e.health, e.stance, e.firing, e.has_target, e.target);
}

namespace
{

using zpp::bits::failure;
using zpp::bits::unsized;

// Each step's archive call, stopping at the first refusal.
bool ok(zpp::bits::errc result)
{
    return !failure(result);
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    zpp::bits::out archive(std::span<std::uint8_t>(out, capacity));
    return ok(archive(in.tick, in.count, unsized(std::span(in.entities, in.count)))) ? archive.position() : 0U;
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    zpp::bits::in archive(std::span<const std::uint8_t>(bytes, size));
    return ok(archive(out.tick, out.count)) && out.count <= kMaxEntities &&
           ok(archive(unsized(std::span(out.entities, out.count)))) && archive.position() == size;
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    zpp::bits::out archive(std::span<std::uint8_t>(out, capacity));
    return ok(archive(in)) ? archive.position() : 0U;
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    zpp::bits::in archive(std::span<const std::uint8_t>(bytes, size));
    return ok(archive(out)) && archive.position() == size;
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    zpp::bits::out archive(std::span<std::uint8_t>(out, capacity));
    return ok(archive(in.sender, in.room, in.text_bytes, unsized(std::span(in.text, in.text_bytes)), in.mention_count,
                      unsized(std::span(in.mentions, in.mention_count))))
               ? archive.position()
               : 0U;
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    zpp::bits::in archive(std::span<const std::uint8_t>(bytes, size));
    if (!ok(archive(out.sender, out.room, out.text_bytes)) || out.text_bytes > kMaxText ||
        !ok(archive(unsized(std::span(out.text, out.text_bytes)), out.mention_count)) ||
        out.mention_count > kMaxMentions || !ok(archive(unsized(std::span(out.mentions, out.mention_count)))) ||
        archive.position() != size)
    {
        return false;
    }
    out.text[out.text_bytes] = '\0';
    return true;
}

} // namespace

const PeerArm kZppBitsArm{"zpp_bits 4.7.6", "bytes; natural types, counts before lists and text",
                          encode_snapshot,  decode_snapshot,
                          encode_command,   decode_command,
                          encode_chat,      decode_chat};

} // namespace serialize_board
