// The board's Cap'n Proto arm: board.capnp through the C++ capnp generates, in the packed form Cap'n Proto sends where
// bandwidth matters (its words with zero bytes elided), written to and read from the caller's bytes through kj's array
// streams. The builder's first segment and the reader's scratch are this unit's own zeroed words (the builder zeroes
// what it used when it is destroyed), so a message allocates nothing. Cap'n Proto throws on malformed or short input
// and on a full output, which the arm turns into a refusal; it also refuses bytes left after the message and counts
// past the plain messages' arrays. Peer code: built without the engine's headers.

#include "board.capnp.h"
#include "board_model.hpp"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <cstring>
#include <kj/io.h>

namespace serialize_board
{
namespace
{

constexpr std::size_t kSegmentWords = 2048U;

capnp::word g_build_segment[kSegmentWords];
capnp::word g_read_scratch[kSegmentWords];

// Builds one message in the unit's first segment and writes it packed into `out`; 0 when it does not fit.
template <typename Fill>
std::size_t encode(std::uint8_t* out, std::size_t capacity, Fill&& fill)
{
    try
    {
        capnp::MallocMessageBuilder message(kj::arrayPtr(g_build_segment, kSegmentWords));
        fill(message);
        kj::ArrayOutputStream stream(kj::arrayPtr(out, capacity));
        capnp::writePackedMessage(stream, message);
        return stream.getArray().size();
    }
    catch (const kj::Exception&)
    {
        return 0U;
    }
}

// Reads one packed message of root type `Root` from exactly `size` bytes into the plain message.
template <typename Root, typename Read>
bool decode(const std::uint8_t* bytes, std::size_t size, Read&& read)
{
    try
    {
        kj::ArrayInputStream stream(kj::arrayPtr(bytes, size));
        bool accepted = false;
        {
            capnp::PackedMessageReader message(stream, capnp::ReaderOptions(),
                                               kj::arrayPtr(g_read_scratch, kSegmentWords));
            accepted = read(message.getRoot<Root>());
        }
        return accepted && stream.tryGetReadBuffer().size() == 0U;
    }
    catch (const kj::Exception&)
    {
        return false;
    }
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    return encode(out, capacity, [&in](capnp::MessageBuilder& message)
    {
        cp::Snapshot::Builder root = message.initRoot<cp::Snapshot>();
        root.setTick(in.tick);
        auto entities = root.initEntities(in.count);
        for (std::uint32_t i = 0U; i < in.count; ++i)
        {
            const PlainEntity& p = in.entities[i];
            cp::Entity::Builder e = entities[i];
            e.setId(p.id);
            e.setX(p.x);
            e.setY(p.y);
            e.setZ(p.z);
            e.setQx(p.turn[0]);
            e.setQy(p.turn[1]);
            e.setQz(p.turn[2]);
            e.setQw(p.turn[3]);
            e.setVx(p.vx);
            e.setVy(p.vy);
            e.setVz(p.vz);
            e.setHealth(p.health);
            e.setStance(static_cast<cp::Stance>(p.stance));
            e.setFiring(p.firing);
            if (p.has_target)
            {
                e.getTarget().setId(p.target);
            }
        }
    });
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    return decode<cp::Snapshot>(bytes, size, [&out](cp::Snapshot::Reader root)
    {
        const auto entities = root.getEntities();
        if (entities.size() > kMaxEntities)
        {
            return false;
        }
        out.tick = root.getTick();
        out.count = entities.size();
        for (std::uint32_t i = 0U; i < out.count; ++i)
        {
            const cp::Entity::Reader e = entities[i];
            PlainEntity& p = out.entities[i];
            p.id = e.getId();
            p.x = e.getX();
            p.y = e.getY();
            p.z = e.getZ();
            p.turn[0] = e.getQx();
            p.turn[1] = e.getQy();
            p.turn[2] = e.getQz();
            p.turn[3] = e.getQw();
            p.vx = e.getVx();
            p.vy = e.getVy();
            p.vz = e.getVz();
            p.health = e.getHealth();
            p.stance = static_cast<std::uint8_t>(e.getStance());
            p.firing = e.getFiring();
            p.has_target = e.getTarget().isId();
            p.target = p.has_target ? e.getTarget().getId() : std::uint16_t{0U};
        }
        return true;
    });
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    return encode(out, capacity, [&in](capnp::MessageBuilder& message)
    {
        cp::Command::Builder root = message.initRoot<cp::Command>();
        root.setTick(in.tick);
        root.setButtons(in.buttons);
        root.setYaw(in.yaw);
        root.setPitch(in.pitch);
        root.setMoveX(in.move_x);
        root.setMoveY(in.move_y);
    });
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    return decode<cp::Command>(bytes, size, [&out](cp::Command::Reader root)
    {
        out.tick = root.getTick();
        out.buttons = root.getButtons();
        out.yaw = root.getYaw();
        out.pitch = root.getPitch();
        out.move_x = root.getMoveX();
        out.move_y = root.getMoveY();
        return true;
    });
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    return encode(out, capacity, [&in](capnp::MessageBuilder& message)
    {
        cp::Chat::Builder root = message.initRoot<cp::Chat>();
        root.setSender(in.sender);
        root.setRoom(static_cast<cp::Room>(in.room));
        root.setText(capnp::Text::Reader(in.text, in.text_bytes));
        auto mentions = root.initMentions(in.mention_count);
        for (std::uint32_t i = 0U; i < in.mention_count; ++i)
        {
            mentions.set(i, in.mentions[i]);
        }
    });
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    return decode<cp::Chat>(bytes, size, [&out](cp::Chat::Reader root)
    {
        const capnp::Text::Reader text = root.getText();
        const auto mentions = root.getMentions();
        if (text.size() > kMaxText || mentions.size() > kMaxMentions)
        {
            return false;
        }
        out.sender = root.getSender();
        out.room = static_cast<std::uint8_t>(root.getRoom());
        out.text_bytes = static_cast<std::uint8_t>(text.size());
        if (out.text_bytes != 0U)
        {
            std::memcpy(out.text, text.begin(), out.text_bytes);
        }
        out.text[out.text_bytes] = '\0';
        out.mention_count = static_cast<std::uint8_t>(mentions.size());
        for (std::uint32_t i = 0U; i < out.mention_count; ++i)
        {
            out.mentions[i] = mentions[i];
        }
        return true;
    });
}

} // namespace

const PeerArm kCapnProtoArm{"Cap'n Proto 1.5.0, packed", "bytes; 64-bit words and pointers, zero bytes packed away",
                            encode_snapshot,             decode_snapshot,
                            encode_command,              decode_command,
                            encode_chat,                 decode_chat};

} // namespace serialize_board
