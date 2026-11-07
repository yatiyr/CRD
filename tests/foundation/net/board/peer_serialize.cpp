// The board's serialize arm: mas-bandwidth/serialize's streams with its precomputed compressed floats (the path its
// documentation gives generated code) and networked-physics' compressed_quaternion<9>, through their own Serialize
// members. Peer code: built without the engine's headers.

#include "board_model.hpp"
#include "networked_physics.hpp"

namespace serialize_board
{
namespace
{

struct FloatLaw
{
    float min;
    float max;
    std::uint32_t max_code = 0U;
    int bits = 0;
    float delta = 0.0F;

    FloatLaw(float low, float high, float resolution) : min(low), max(high)
    {
        serialize::serialize_compressed_float_params(low, high, resolution, max_code, bits, delta);
    }
};

const FloatLaw kPosition(kPositionMin, kPositionMax, kPositionResolution);
const FloatLaw kVelocity(kVelocityMin, kVelocityMax, kVelocityResolution);
const FloatLaw kYaw(kYawMin, kYawMax, kAngleResolution);
const FloatLaw kPitch(kPitchMin, kPitchMax, kAngleResolution);

// NOLINTBEGIN: serialize's macros return from the function they expand in.
template <typename Stream> bool serialize_law(Stream& stream, float& value, const FloatLaw& law)
{
    serialize_compressed_float_precomputed(stream, value, law.max_code, law.bits, law.delta, law.min);
    return true;
}

template <typename Stream> bool serialize_entity(Stream& stream, PlainEntity& entity)
{
    serialize_int(stream, entity.id, 0, kMaxId);
    if (!serialize_law(stream, entity.x, kPosition) || !serialize_law(stream, entity.y, kPosition) ||
        !serialize_law(stream, entity.z, kPosition))
    {
        return false;
    }
    // The struct's Serialize member is written for the demo's stream macros, which return nothing, so its four fields
    // go through serialize's as it lists them: the largest component's index, then the other three.
    compressed_quaternion<kQuaternionBits> turn{};
    std::uint32_t largest = 0U;
    std::uint32_t a = 0U;
    std::uint32_t b = 0U;
    std::uint32_t c = 0U;
    if (Stream::IsWriting)
    {
        turn.Load(vectorial::quat4f(entity.turn[0], entity.turn[1], entity.turn[2], entity.turn[3]));
        largest = turn.largest;
        a = turn.integer_a;
        b = turn.integer_b;
        c = turn.integer_c;
    }
    serialize_bits(stream, largest, 2);
    serialize_bits(stream, a, kQuaternionBits);
    serialize_bits(stream, b, kQuaternionBits);
    serialize_bits(stream, c, kQuaternionBits);
    if (Stream::IsReading)
    {
        turn.largest = largest;
        turn.integer_a = a;
        turn.integer_b = b;
        turn.integer_c = c;
        vectorial::quat4f read;
        turn.Save(read);
        read.store(entity.turn);
    }
    if (!serialize_law(stream, entity.vx, kVelocity) || !serialize_law(stream, entity.vy, kVelocity) ||
        !serialize_law(stream, entity.vz, kVelocity))
    {
        return false;
    }
    serialize_int(stream, entity.health, 0, kMaxHealth);
    serialize_int(stream, entity.stance, 0, 3);
    serialize_bool(stream, entity.firing);
    serialize_bool(stream, entity.has_target);
    if (entity.has_target)
    {
        serialize_int(stream, entity.target, 0, kMaxId);
    }
    else if (Stream::IsReading)
    {
        entity.target = 0U;
    }
    return true;
}

template <typename Stream> bool serialize_snapshot(Stream& stream, PlainSnapshot& snapshot)
{
    serialize_uint32(stream, snapshot.tick);
    serialize_int(stream, snapshot.count, 0, kMaxEntities);
    for (std::uint32_t i = 0U; i < snapshot.count; ++i)
    {
        if (!serialize_entity(stream, snapshot.entities[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename Stream> bool serialize_command(Stream& stream, PlainCommand& command)
{
    serialize_uint32(stream, command.tick);
    serialize_uint16(stream, command.buttons);
    if (!serialize_law(stream, command.yaw, kYaw) || !serialize_law(stream, command.pitch, kPitch))
    {
        return false;
    }
    serialize_int(stream, command.move_x, -127, 127);
    serialize_int(stream, command.move_y, -127, 127);
    return true;
}

template <typename Stream> bool serialize_chat(Stream& stream, PlainChat& chat)
{
    serialize_int(stream, chat.sender, 0, kMaxId);
    serialize_int(stream, chat.room, 0, 3);
    serialize_string(stream, chat.text, static_cast<int>(sizeof(chat.text)));
    serialize_int(stream, chat.mention_count, 0, kMaxMentions);
    for (std::uint8_t i = 0U; i < chat.mention_count; ++i)
    {
        serialize_int(stream, chat.mentions[i], 0, kMaxId);
    }
    return true;
}
// NOLINTEND

template <typename T, bool (*Body)(serialize::WriteStream&, T&)>
std::size_t write(const T& in, std::uint8_t* out, std::size_t capacity)
{
    // serialize's writer works in 32-bit words.
    serialize::WriteStream stream(out, static_cast<std::int64_t>(capacity & ~std::size_t{3U}));
    T copy = in; // its unified functions take the value by reference on both sides
    if (!Body(stream, copy) || !stream.SerializeEnd())
    {
        return 0U;
    }
    stream.Flush();
    return static_cast<std::size_t>(stream.GetBytesProcessed());
}

template <typename T, bool (*Body)(serialize::ReadStream&, T&)>
bool read(const std::uint8_t* bytes, std::size_t size, T& out)
{
    serialize::ReadStream stream(bytes, static_cast<std::int64_t>(size));
    return Body(stream, out) && stream.SerializeEnd();
}

std::size_t encode_snapshot(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainSnapshot, serialize_snapshot<serialize::WriteStream>>(in, out, capacity);
}

bool decode_snapshot(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out)
{
    return read<PlainSnapshot, serialize_snapshot<serialize::ReadStream>>(bytes, size, out);
}

std::size_t encode_command(const PlainCommand& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainCommand, serialize_command<serialize::WriteStream>>(in, out, capacity);
}

bool decode_command(const std::uint8_t* bytes, std::size_t size, PlainCommand& out)
{
    return read<PlainCommand, serialize_command<serialize::ReadStream>>(bytes, size, out);
}

std::size_t encode_chat(const PlainChat& in, std::uint8_t* out, std::size_t capacity)
{
    return write<PlainChat, serialize_chat<serialize::WriteStream>>(in, out, capacity);
}

bool decode_chat(const std::uint8_t* bytes, std::size_t size, PlainChat& out)
{
    if (!read<PlainChat, serialize_chat<serialize::ReadStream>>(bytes, size, out))
    {
        return false;
    }
    out.text_bytes = static_cast<std::uint8_t>(std::strlen(out.text));
    return true;
}

} // namespace

const PeerArm kSerializeArm{"serialize 2.1.3", "bits; compressed floats, smallest three, 32-bit tick",
                            encode_snapshot,   decode_snapshot,
                            encode_command,    decode_command,
                            encode_chat,       decode_chat};

} // namespace serialize_board
