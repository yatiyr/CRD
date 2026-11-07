// The board's bitsery arm: bit packing with value ranges at the schema's bounds and resolutions, the way bitsery's
// documentation packs game state. It has no smallest-three quaternion, so a rotation is four ranged components at the
// ten bits that hold the step of the schema's nine-bit smallest three. It writes into the caller's bytes through a
// buffer trait for a span, bitsery's extension point for user buffers. Peer code: built without the engine's headers.

#include "board_model.hpp"

#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/ext/value_range.h>
#include <bitsery/traits/core/std_defaults.h>
#include <bitsery/traits/string.h>
#include <cstring>
#include <span>
#include <type_traits>

namespace bitsery::traits
{

template <typename T> struct ContainerTraits<std::span<T>> : public StdContainer<std::span<T>, false, true>
{
};

template <typename T> struct BufferAdapterTraits<std::span<T>>
{
    using TIterator = typename std::span<T>::iterator;
    using TConstIterator = typename std::span<T>::iterator;
    using TValue = std::remove_const_t<T>;
};

} // namespace bitsery::traits

namespace serialize_board
{
namespace
{

using Writer = bitsery::OutputBufferAdapter<std::span<std::uint8_t>>;
using Reader = bitsery::InputBufferAdapter<std::span<const std::uint8_t>>;
using bitsery::ext::BitsConstraint;
using bitsery::ext::ValueRange;

const ValueRange<float> kPosition{kPositionMin, kPositionMax, kPositionResolution};
const ValueRange<float> kVelocity{kVelocityMin, kVelocityMax, kVelocityResolution};
const ValueRange<float> kYaw{kYawMin, kYawMax, kAngleResolution};
const ValueRange<float> kPitch{kPitchMin, kPitchMax, kAngleResolution};
const ValueRange<float> kComponent{-1.0F, 1.0F, BitsConstraint{kQuaternionBits + 1}};
const ValueRange<std::uint16_t> kId{0U, kMaxId};

template <typename S> void entity(S& s, PlainEntity& e)
{
    s.ext(e.id, kId);
    s.ext(e.x, kPosition);
    s.ext(e.y, kPosition);
    s.ext(e.z, kPosition);
    for (float& component : e.turn)
    {
        s.ext(component, kComponent);
    }
    s.ext(e.vx, kVelocity);
    s.ext(e.vy, kVelocity);
    s.ext(e.vz, kVelocity);
    s.ext(e.health, ValueRange<std::uint8_t>{0U, kMaxHealth});
    s.ext(e.stance, ValueRange<std::uint8_t>{0U, 3U});
    s.boolValue(e.firing);
    s.boolValue(e.has_target);
    if (e.has_target)
    {
        s.ext(e.target, kId);
    }
    else
    {
        e.target = 0U;
    }
}

template <typename S> void snapshot(S& s, PlainSnapshot& o)
{
    s.enableBitPacking(
        [&o](typename S::BPEnabledType& b)
        {
            b.value4b(o.tick);
            b.ext(o.count, ValueRange<std::uint32_t>{0U, static_cast<std::uint32_t>(kMaxEntities)});
            for (std::uint32_t i = 0U; i < o.count; ++i)
            {
                entity(b, o.entities[i]);
            }
        });
}

template <typename S> void command(S& s, PlainCommand& o)
{
    s.enableBitPacking(
        [&o](typename S::BPEnabledType& b)
        {
            b.value4b(o.tick);
            b.value2b(o.buttons);
            b.ext(o.yaw, kYaw);
            b.ext(o.pitch, kPitch);
            b.ext(o.move_x, ValueRange<std::int8_t>{-127, 127});
            b.ext(o.move_y, ValueRange<std::int8_t>{-127, 127});
        });
}

template <typename S> void chat(S& s, PlainChat& o)
{
    s.enableBitPacking(
        [&o](typename S::BPEnabledType& b)
        {
            b.ext(o.sender, kId);
            b.ext(o.room, ValueRange<std::uint8_t>{0U, 3U});
            b.text1b(o.text);
            b.ext(o.mention_count, ValueRange<std::uint8_t>{0U, static_cast<std::uint8_t>(kMaxMentions)});
            for (std::uint8_t i = 0U; i < o.mention_count; ++i)
            {
                b.ext(o.mentions[i], kId);
            }
        });
}

template <typename T, void (*Body)(bitsery::Serializer<Writer>&, T&)>
std::size_t write(const T& in, std::uint8_t* out, std::size_t capacity)
{
    std::span<std::uint8_t> view(out, capacity);
    bitsery::Serializer<Writer> ser{Writer{view}};
    T copy = in; // one function serves both directions, so it takes the value by reference
    Body(ser, copy);
    ser.adapter().flush();
    return ser.adapter().writtenBytesCount();
}

template <typename T, void (*Body)(bitsery::Deserializer<Reader>&, T&)>
bool read(const std::uint8_t* bytes, std::size_t size, T& out)
{
    const std::span<const std::uint8_t> view(bytes, size);
    bitsery::Deserializer<Reader> des{Reader{view.begin(), size}};
    Body(des, out);
    return des.adapter().error() == bitsery::ReaderError::NoError && des.adapter().isCompletedSuccessfully();
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainSnapshot, snapshot<bitsery::Serializer<Writer>>>(in, out, capacity);
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    return read<PlainSnapshot, snapshot<bitsery::Deserializer<Reader>>>(bytes, size, out);
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainCommand, command<bitsery::Serializer<Writer>>>(in, out, capacity);
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    return read<PlainCommand, command<bitsery::Deserializer<Reader>>>(bytes, size, out);
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainChat, chat<bitsery::Serializer<Writer>>>(in, out, capacity);
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    if (!read<PlainChat, chat<bitsery::Deserializer<Reader>>>(bytes, size, out))
    {
        return false;
    }
    out.text_bytes = static_cast<std::uint8_t>(std::strlen(out.text));
    return true;
}

} // namespace

const PeerArm kBitseryArm{"bitsery 5.2.5", "bits; value ranges, four ranged quaternion components, 32-bit tick",
                          encode_snapshot, decode_snapshot,
                          encode_command,  decode_command,
                          encode_chat,     decode_chat};

} // namespace serialize_board
