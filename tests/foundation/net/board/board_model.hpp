// The serializer board's messages as an application holds them, in plain types every arm reads and writes
// (tests/foundation/net/schemas/serialize_board.json declares them for crd-net; board.proto for protobuf's format).

#pragma once

#include <cstddef>
#include <cstdint>

namespace serialize_board
{

inline constexpr std::size_t kMaxEntities = 64U;
inline constexpr std::size_t kMaxText = 200U;
inline constexpr std::size_t kMaxMentions = 8U;

// The schema's bounds, which the bit-packing arms declare as crd-net's schema does.
inline constexpr float kPositionMin = -512.0F;
inline constexpr float kPositionMax = 512.0F;
inline constexpr float kPositionResolution = 0.001F;
inline constexpr float kVelocityMin = -32.0F;
inline constexpr float kVelocityMax = 32.0F;
inline constexpr float kVelocityResolution = 0.01F;
inline constexpr float kYawMin = -180.0F;
inline constexpr float kYawMax = 180.0F;
inline constexpr float kPitchMin = -90.0F;
inline constexpr float kPitchMax = 90.0F;
inline constexpr float kAngleResolution = 0.01F;
inline constexpr int kQuaternionBits = 9;
inline constexpr std::uint16_t kMaxId = 1023U;
inline constexpr std::uint8_t kMaxHealth = 100U;

struct PlainEntity
{
    std::uint16_t id;
    float x;
    float y;
    float z;
    float turn[4]; // x, y, z, w
    float vx;
    float vy;
    float vz;
    std::uint8_t health;
    std::uint8_t stance; // 0 to 3
    bool firing;
    bool has_target;
    std::uint16_t target;
};

struct PlainSnapshot
{
    std::uint32_t tick;
    std::uint32_t count;
    PlainEntity entities[kMaxEntities];
};

struct PlainCommand
{
    std::uint32_t tick;
    std::uint16_t buttons;
    float yaw;
    float pitch;
    std::int8_t move_x; // -127 to 127
    std::int8_t move_y;
};

struct PlainChat
{
    std::uint16_t sender;
    std::uint8_t room; // 0 to 3
    std::uint8_t text_bytes;
    char text[kMaxText + 1U]; // NUL after text_bytes, as serialize's strings need
    std::uint8_t mention_count;
    std::uint16_t mentions[kMaxMentions];
};

// One peer's codec: encode returns the bytes written (0 when `out` is too small), decode refuses malformed bytes as its
// library does and fills the message as the application then holds it.
struct PeerArm
{
    const char* name;
    const char* format; // how its bytes are laid out, for the board's table
    std::size_t (*encode_snapshot)(const PlainSnapshot& in, std::uint8_t* out, std::size_t capacity);
    bool (*decode_snapshot)(const std::uint8_t* bytes, std::size_t size, PlainSnapshot& out);
    std::size_t (*encode_command)(const PlainCommand& in, std::uint8_t* out, std::size_t capacity);
    bool (*decode_command)(const std::uint8_t* bytes, std::size_t size, PlainCommand& out);
    std::size_t (*encode_chat)(const PlainChat& in, std::uint8_t* out, std::size_t capacity);
    bool (*decode_chat)(const std::uint8_t* bytes, std::size_t size, PlainChat& out);
};

// Each in its own unit, built as peer code (no engine headers, its library's warnings off).
extern const PeerArm kSerializeArm; // mas-bandwidth/serialize, networked-physics' smallest three
extern const PeerArm kBitseryArm;   // bitsery with bit packing
extern const PeerArm kZppBitsArm;   // zpp_bits
extern const PeerArm kProtozeroArm; // protobuf's wire format through protozero
extern const PeerArm kFlatBuffersArm; // FlatBuffers through flatc's C++
extern const PeerArm kCapnProtoArm;   // Cap'n Proto's packed form through capnp's C++

} // namespace serialize_board
