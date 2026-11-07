// The board's protobuf arm: protobuf's wire format for board.proto, written and read by protozero, the fastest
// codec of that format without generated code. As proto3 does, a field at its default is left out, so the bytes are
// what libprotobuf writes for the same message. protozero writes into the caller's bytes (a fixed-size buffer) and
// throws on malformed input, which the arm turns into a refusal; it checks nothing beyond the format, so the arm also
// refuses counts past the plain messages' arrays. Peer code: built without the engine's headers.

#include "board_model.hpp"

#include <cstring>
#include <protozero/buffer_fixed.hpp>
#include <protozero/pbf_reader.hpp>
#include <protozero/pbf_writer.hpp>

namespace serialize_board
{
namespace
{

using Writer = protozero::basic_pbf_writer<protozero::fixed_size_buffer_adaptor>;

void put_uint(Writer& w, protozero::pbf_tag_type tag, std::uint32_t value)
{
    if (value != 0U)
    {
        w.add_uint32(tag, value);
    }
}

void put_float(Writer& w, protozero::pbf_tag_type tag, float value)
{
    if (value != 0.0F)
    {
        w.add_float(tag, value);
    }
}

void put_entity(Writer& parent, const PlainEntity& e)
{
    Writer w(parent, 2U);
    put_uint(w, 1U, e.id);
    put_float(w, 2U, e.x);
    put_float(w, 3U, e.y);
    put_float(w, 4U, e.z);
    for (std::uint32_t i = 0U; i < 4U; ++i)
    {
        put_float(w, 5U + i, e.turn[i]);
    }
    put_float(w, 9U, e.vx);
    put_float(w, 10U, e.vy);
    put_float(w, 11U, e.vz);
    put_uint(w, 12U, e.health);
    if (e.stance != 0U)
    {
        w.add_enum(13U, e.stance);
    }
    if (e.firing)
    {
        w.add_bool(14U, true);
    }
    if (e.has_target)
    {
        w.add_uint32(15U, e.target); // proto3 `optional`: present even at zero
    }
}

template <typename Body> std::size_t write(std::uint8_t* out, std::size_t capacity, const Body& body)
{
    protozero::fixed_size_buffer_adaptor buffer(reinterpret_cast<char*>(out), capacity);
    try
    {
        {
            Writer w(buffer);
            body(w);
        }
        return buffer.size();
    }
    catch (const std::length_error&)
    {
        return 0U;
    }
}

template <typename Body> bool read(const std::uint8_t* bytes, std::size_t size, const Body& body)
{
    try
    {
        protozero::pbf_reader message(reinterpret_cast<const char*>(bytes), size);
        return body(message);
    }
    catch (const protozero::exception&)
    {
        return false;
    }
}

void read_entity(protozero::pbf_reader message, PlainEntity& e)
{
    e = PlainEntity{};
    while (message.next())
    {
        const protozero::pbf_tag_type tag = message.tag();
        switch (tag)
        {
            case 1U:
                e.id = static_cast<std::uint16_t>(message.get_uint32());
                break;
            case 2U:
                e.x = message.get_float();
                break;
            case 3U:
                e.y = message.get_float();
                break;
            case 4U:
                e.z = message.get_float();
                break;
            case 5U:
            case 6U:
            case 7U:
            case 8U:
                e.turn[tag - 5U] = message.get_float();
                break;
            case 9U:
                e.vx = message.get_float();
                break;
            case 10U:
                e.vy = message.get_float();
                break;
            case 11U:
                e.vz = message.get_float();
                break;
            case 12U:
                e.health = static_cast<std::uint8_t>(message.get_uint32());
                break;
            case 13U:
                e.stance = static_cast<std::uint8_t>(message.get_enum());
                break;
            case 14U:
                e.firing = message.get_bool();
                break;
            case 15U:
                e.has_target = true;
                e.target = static_cast<std::uint16_t>(message.get_uint32());
                break;
            default:
                message.skip();
                break;
        }
    }
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    return write(out, capacity,
                 [&in](Writer& w)
                 {
                     put_uint(w, 1U, in.tick);
                     for (std::uint32_t i = 0U; i < in.count; ++i)
                     {
                         put_entity(w, in.entities[i]);
                     }
                 });
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    return read(bytes, size,
                [&out](protozero::pbf_reader& message)
                {
                    out.tick = 0U;
                    out.count = 0U;
                    while (message.next())
                    {
                        if (message.tag() == 1U)
                        {
                            out.tick = message.get_uint32();
                        }
                        else if (message.tag() == 2U)
                        {
                            if (out.count == kMaxEntities)
                            {
                                return false;
                            }
                            read_entity(message.get_message(), out.entities[out.count++]);
                        }
                        else
                        {
                            message.skip();
                        }
                    }
                    return true;
                });
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    return write(out, capacity,
                 [&in](Writer& w)
                 {
                     put_uint(w, 1U, in.tick);
                     put_uint(w, 2U, in.buttons);
                     put_float(w, 3U, in.yaw);
                     put_float(w, 4U, in.pitch);
                     if (in.move_x != 0)
                     {
                         w.add_sint32(5U, in.move_x);
                     }
                     if (in.move_y != 0)
                     {
                         w.add_sint32(6U, in.move_y);
                     }
                 });
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    return read(bytes, size,
                [&out](protozero::pbf_reader& message)
                {
                    out = PlainCommand{};
                    while (message.next())
                    {
                        switch (message.tag())
                        {
                            case 1U:
                                out.tick = message.get_uint32();
                                break;
                            case 2U:
                                out.buttons = static_cast<std::uint16_t>(message.get_uint32());
                                break;
                            case 3U:
                                out.yaw = message.get_float();
                                break;
                            case 4U:
                                out.pitch = message.get_float();
                                break;
                            case 5U:
                                out.move_x = static_cast<std::int8_t>(message.get_sint32());
                                break;
                            case 6U:
                                out.move_y = static_cast<std::int8_t>(message.get_sint32());
                                break;
                            default:
                                message.skip();
                                break;
                        }
                    }
                    return true;
                });
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    return write(out, capacity,
                 [&in](Writer& w)
                 {
                     put_uint(w, 1U, in.sender);
                     if (in.room != 0U)
                     {
                         w.add_enum(2U, in.room);
                     }
                     if (in.text_bytes != 0U)
                     {
                         w.add_string(3U, in.text, in.text_bytes);
                     }
                     if (in.mention_count != 0U)
                     {
                         w.add_packed_uint32(4U, in.mentions, in.mentions + in.mention_count);
                     }
                 });
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    return read(bytes, size,
                [&out](protozero::pbf_reader& message)
                {
                    out.sender = 0U;
                    out.room = 0U;
                    out.text_bytes = 0U;
                    out.mention_count = 0U;
                    while (message.next())
                    {
                        switch (message.tag())
                        {
                            case 1U:
                                out.sender = static_cast<std::uint16_t>(message.get_uint32());
                                break;
                            case 2U:
                                out.room = static_cast<std::uint8_t>(message.get_enum());
                                break;
                            case 3U:
                            {
                                const protozero::data_view text = message.get_view();
                                if (text.size() > kMaxText)
                                {
                                    return false;
                                }
                                std::memcpy(out.text, text.data(), text.size());
                                out.text_bytes = static_cast<std::uint8_t>(text.size());
                                break;
                            }
                            case 4U:
                                for (const std::uint32_t mention : message.get_packed_uint32())
                                {
                                    if (out.mention_count == kMaxMentions)
                                    {
                                        return false;
                                    }
                                    out.mentions[out.mention_count++] = static_cast<std::uint16_t>(mention);
                                }
                                break;
                            default:
                                message.skip();
                                break;
                        }
                    }
                    out.text[out.text_bytes] = '\0';
                    return true;
                });
}

} // namespace

const PeerArm kProtozeroArm{"protobuf wire, protozero 1.8.2",
                            "bytes; tagged varints and floats, defaults left out",
                            encode_snapshot,
                            decode_snapshot,
                            encode_command,
                            decode_command,
                            encode_chat,
                            decode_chat};

} // namespace serialize_board
