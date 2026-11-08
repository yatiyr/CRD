// The board's FlatBuffers arm: board.fbs through the C++ flatc generates, the library's own way to read untrusted
// bytes (its Verifier, then the generated accessors). FlatBuffers builds a message back to front in its builder's
// buffer, so the arm keeps one builder, cleared per message (its buffer stays after the untimed warm-up), and copies
// the finished bytes into the caller's; a reader needs no copy. A field at its default is left out, as flatc's
// builders do. The arm refuses counts past the plain messages' arrays. Peer code: built without the engine's headers.

#include "board_generated.h"
#include "board_model.hpp"

#include <cstring>

namespace serialize_board
{
namespace
{

flatbuffers::FlatBufferBuilder& builder()
{
    static flatbuffers::FlatBufferBuilder kept(8192U);
    kept.Clear();
    return kept;
}

std::size_t finish(flatbuffers::FlatBufferBuilder& b, std::uint8_t* out, std::size_t capacity)
{
    const std::size_t size = b.GetSize();
    if (size > capacity)
    {
        return 0U;
    }
    std::memcpy(out, b.GetBufferPointer(), size);
    return size;
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    flatbuffers::FlatBufferBuilder& b = builder();
    fb::Entity* entities = nullptr;
    const auto list = b.CreateUninitializedVectorOfStructs(in.count, &entities);
    for (std::uint32_t i = 0U; i < in.count; ++i)
    {
        const PlainEntity& e = in.entities[i];
        entities[i] = fb::Entity(e.x, e.y, e.z, e.turn[0], e.turn[1], e.turn[2], e.turn[3], e.vx, e.vy, e.vz, e.id,
                                 e.target, e.health, static_cast<fb::Stance>(e.stance), e.firing, e.has_target);
    }
    b.Finish(fb::CreateSnapshot(b, in.tick, list));
    return finish(b, out, capacity);
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    flatbuffers::Verifier verifier(bytes, size);
    if (!verifier.VerifyBuffer<fb::Snapshot>(nullptr))
    {
        return false;
    }
    const fb::Snapshot* message = flatbuffers::GetRoot<fb::Snapshot>(bytes);
    const auto* entities = message->entities();
    const std::uint32_t count = entities == nullptr ? 0U : entities->size();
    if (count > kMaxEntities)
    {
        return false;
    }
    out.tick = message->tick();
    out.count = count;
    for (std::uint32_t i = 0U; i < count; ++i)
    {
        const fb::Entity& e = *entities->Get(i);
        PlainEntity& p = out.entities[i];
        p.id = e.id();
        p.x = e.x();
        p.y = e.y();
        p.z = e.z();
        p.turn[0] = e.qx();
        p.turn[1] = e.qy();
        p.turn[2] = e.qz();
        p.turn[3] = e.qw();
        p.vx = e.vx();
        p.vy = e.vy();
        p.vz = e.vz();
        p.health = e.health();
        p.stance = static_cast<std::uint8_t>(e.stance());
        p.firing = e.firing();
        p.has_target = e.has_target();
        p.target = e.target();
    }
    return true;
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    flatbuffers::FlatBufferBuilder& b = builder();
    b.Finish(fb::CreateCommand(b, in.tick, in.buttons, in.yaw, in.pitch, in.move_x, in.move_y));
    return finish(b, out, capacity);
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    flatbuffers::Verifier verifier(bytes, size);
    if (!verifier.VerifyBuffer<fb::Command>(nullptr))
    {
        return false;
    }
    const fb::Command* message = flatbuffers::GetRoot<fb::Command>(bytes);
    out.tick = message->tick();
    out.buttons = message->buttons();
    out.yaw = message->yaw();
    out.pitch = message->pitch();
    out.move_x = message->move_x();
    out.move_y = message->move_y();
    return true;
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    flatbuffers::FlatBufferBuilder& b = builder();
    const auto text = b.CreateString(in.text, in.text_bytes);
    const auto mentions = b.CreateVector(in.mentions, in.mention_count);
    b.Finish(fb::CreateChat(b, in.sender, static_cast<fb::Room>(in.room), text, mentions));
    return finish(b, out, capacity);
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    flatbuffers::Verifier verifier(bytes, size);
    if (!verifier.VerifyBuffer<fb::Chat>(nullptr))
    {
        return false;
    }
    const fb::Chat* message = flatbuffers::GetRoot<fb::Chat>(bytes);
    const flatbuffers::String* text = message->text();
    const auto* mentions = message->mentions();
    const std::uint32_t text_bytes = text == nullptr ? 0U : text->size();
    const std::uint32_t mention_count = mentions == nullptr ? 0U : mentions->size();
    if (text_bytes > kMaxText || mention_count > kMaxMentions)
    {
        return false;
    }
    out.sender = message->sender();
    out.room = static_cast<std::uint8_t>(message->room());
    out.text_bytes = static_cast<std::uint8_t>(text_bytes);
    if (text_bytes != 0U)
    {
        std::memcpy(out.text, text->data(), text_bytes);
    }
    out.text[text_bytes] = '\0';
    out.mention_count = static_cast<std::uint8_t>(mention_count);
    for (std::uint32_t i = 0U; i < mention_count; ++i)
    {
        out.mentions[i] = mentions->Get(i);
    }
    return true;
}

} // namespace

const PeerArm kFlatBuffersArm{"FlatBuffers 25.12.19", "bytes; aligned natural types, offsets and vtables, verified",
                              encode_snapshot,        decode_snapshot,
                              encode_command,         decode_command,
                              encode_chat,            decode_chat};

} // namespace serialize_board
