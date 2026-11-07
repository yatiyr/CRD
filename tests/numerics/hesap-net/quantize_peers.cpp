// The two peers of quantize_peers.hpp. serialize.h is included as the release ships it. The compressed_quaternion
// structs are cut verbatim from the pinned networked-physics Snapshot.h at configure time (tests/numerics/hesap-net/
// CMakeLists.txt) and compile on the few stand-ins below; `quat4f` holds the four floats, and `normalize` is the
// identity because that source's is an SSE reciprocal square root estimate, which differs by CPU and no wire carries.
//
// Built with the engine's floating-point contract (no contraction), as both sources require of a conforming build.

#include "quantize_peers.hpp"

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the release header reads this macro.
#define SERIALIZE_ENABLE_TESTS 0
#include <cmath>
#include <cstdint>
#include <cstring>
#include <serialize.h>

namespace vectorial
{

// NOLINTNEXTLINE(readability-identifier-naming): the name the cut source spells.
struct quat4f
{
    float v[4];

    quat4f() = default;

    quat4f(float x, float y, float z, float w) : v{x, y, z, w} {}

    void store(float* out) const { std::memcpy(out, v, sizeof(v)); }

    static quat4f identity() { return quat4f(0.0F, 0.0F, 0.0F, 1.0F); }
};

inline quat4f normalize(const quat4f& q)
{
    return q;
}

} // namespace vectorial

namespace math
{

inline float floor(float value)
{
    return std::floor(value);
}

} // namespace math

// Peer code runs only on well-formed bit widths.
#define CORE_ASSERT(condition)
// The structs' bit stream members are never instantiated here.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,bugprone-macro-parentheses): a declaration the cut source spells.
#define PROTOCOL_SERIALIZE_OBJECT(stream) template <typename Stream> bool Serialize(Stream& stream)

#include "networked_physics_quaternion_slice.inc"

namespace quantize_peers
{
namespace
{

template <int Bits, template <int> class Word> QuaternionWord load_as(const float* xyzw)
{
    Word<Bits> word{};
    word.Load(vectorial::quat4f(xyzw[0], xyzw[1], xyzw[2], xyzw[3]));
    return QuaternionWord{static_cast<std::uint32_t>(word.largest), static_cast<std::uint32_t>(word.integer_a),
                          static_cast<std::uint32_t>(word.integer_b), static_cast<std::uint32_t>(word.integer_c)};
}

template <int Bits, template <int> class Word> void save_as(const QuaternionWord& in, float* xyzw)
{
    Word<Bits> word{};
    word.largest = in.largest;
    word.integer_a = in.a;
    word.integer_b = in.b;
    word.integer_c = in.c;
    vectorial::quat4f q;
    word.Save(q);
    q.store(xyzw);
}

} // namespace

void float_params(float min, float max, float resolution, std::uint32_t& max_code, int& bits, float& delta)
{
    serialize::serialize_compressed_float_params(min, max, resolution, max_code, bits, delta);
}

std::uint32_t float_write(float value, float min, float max, float resolution)
{
    std::uint32_t max_code = 0U;
    int bits = 0;
    float delta = 0.0F;
    float_params(min, max, resolution, max_code, bits, delta);
    std::uint8_t buffer[64] = {};
    serialize::WriteStream writer(buffer, sizeof(buffer));
    (void)serialize::serialize_compressed_float_internal(writer, value, min, max, resolution);
    writer.Flush();
    serialize::ReadStream reader(buffer, sizeof(buffer));
    std::uint32_t code = 0U;
    (void)reader.SerializeBits(code, bits);
    return code;
}

std::uint32_t float_write_precomputed(float value, std::uint32_t max_code, int bits, float delta, float min)
{
    std::uint8_t buffer[64] = {};
    serialize::WriteStream writer(buffer, sizeof(buffer));
    (void)serialize::serialize_compressed_float_precomputed_internal(writer, value, max_code, bits, delta, min);
    writer.Flush();
    serialize::ReadStream reader(buffer, sizeof(buffer));
    std::uint32_t code = 0U;
    (void)reader.SerializeBits(code, bits);
    return code;
}

bool float_read(std::uint32_t code, float min, float max, float resolution, float& value)
{
    std::uint32_t max_code = 0U;
    int bits = 0;
    float delta = 0.0F;
    float_params(min, max, resolution, max_code, bits, delta);
    std::uint8_t buffer[64] = {};
    serialize::WriteStream writer(buffer, sizeof(buffer));
    (void)writer.SerializeBits(code, bits);
    writer.Flush();
    serialize::ReadStream reader(buffer, sizeof(buffer));
    return serialize::serialize_compressed_float_internal(reader, value, min, max, resolution);
}

bool float_read_precomputed(std::uint32_t code, std::uint32_t max_code, int bits, float delta, float min, float& value)
{
    std::uint8_t buffer[64] = {};
    serialize::WriteStream writer(buffer, sizeof(buffer));
    (void)writer.SerializeBits(code, bits);
    writer.Flush();
    serialize::ReadStream reader(buffer, sizeof(buffer));
    return serialize::serialize_compressed_float_precomputed_internal(reader, value, max_code, bits, delta, min);
}

QuaternionWord quaternion_load(int bits, const float* xyzw)
{
    switch (bits)
    {
        case 2:
            return load_as<2, compressed_quaternion>(xyzw);
        case 3:
            return load_as<3, compressed_quaternion>(xyzw);
        case 4:
            return load_as<4, compressed_quaternion>(xyzw);
        case 5:
            return load_as<5, compressed_quaternion>(xyzw);
        case 6:
            return load_as<6, compressed_quaternion>(xyzw);
        case 7:
            return load_as<7, compressed_quaternion>(xyzw);
        case 8:
            return load_as<8, compressed_quaternion>(xyzw);
        case 9:
            return load_as<9, compressed_quaternion>(xyzw);
        case 10:
            return load_as<10, compressed_quaternion>(xyzw);
        case 11:
            return load_as<11, compressed_quaternion_64>(xyzw);
        case 12:
            return load_as<12, compressed_quaternion_64>(xyzw);
        case 13:
            return load_as<13, compressed_quaternion_64>(xyzw);
        case 14:
            return load_as<14, compressed_quaternion_64>(xyzw);
        case 15:
            return load_as<15, compressed_quaternion_64>(xyzw);
        default:
            return load_as<16, compressed_quaternion_64>(xyzw);
    }
}

void quaternion_save(int bits, const QuaternionWord& word, float* xyzw)
{
    switch (bits)
    {
        case 2:
            save_as<2, compressed_quaternion>(word, xyzw);
            break;
        case 3:
            save_as<3, compressed_quaternion>(word, xyzw);
            break;
        case 4:
            save_as<4, compressed_quaternion>(word, xyzw);
            break;
        case 5:
            save_as<5, compressed_quaternion>(word, xyzw);
            break;
        case 6:
            save_as<6, compressed_quaternion>(word, xyzw);
            break;
        case 7:
            save_as<7, compressed_quaternion>(word, xyzw);
            break;
        case 8:
            save_as<8, compressed_quaternion>(word, xyzw);
            break;
        case 9:
            save_as<9, compressed_quaternion>(word, xyzw);
            break;
        case 10:
            save_as<10, compressed_quaternion>(word, xyzw);
            break;
        case 11:
            save_as<11, compressed_quaternion_64>(word, xyzw);
            break;
        case 12:
            save_as<12, compressed_quaternion_64>(word, xyzw);
            break;
        case 13:
            save_as<13, compressed_quaternion_64>(word, xyzw);
            break;
        case 14:
            save_as<14, compressed_quaternion_64>(word, xyzw);
            break;
        case 15:
            save_as<15, compressed_quaternion_64>(word, xyzw);
            break;
        default:
            save_as<16, compressed_quaternion_64>(word, xyzw);
            break;
    }
}

std::uint64_t float_write_batch(const float* values, std::size_t count, std::uint32_t max_code, int bits, float delta,
                                float min, std::uint8_t* buffer, std::size_t bytes)
{
    serialize::WriteStream writer(buffer, static_cast<std::int64_t>(bytes));
    for (std::size_t i = 0U; i < count; ++i)
    {
        float value = values[i];
        (void)serialize::serialize_compressed_float_precomputed_internal(writer, value, max_code, bits, delta, min);
    }
    writer.Flush();
    return static_cast<std::uint64_t>(writer.GetBytesProcessed());
}

std::uint64_t float_read_batch(const std::uint8_t* buffer, std::size_t bytes, std::uint32_t max_code, int bits,
                               float delta, float min, float* values, std::size_t count)
{
    serialize::ReadStream reader(buffer, static_cast<std::int64_t>(bytes));
    for (std::size_t i = 0U; i < count; ++i)
    {
        (void)serialize::serialize_compressed_float_precomputed_internal(reader, values[i], max_code, bits, delta, min);
    }
    return static_cast<std::uint64_t>(reader.GetBytesProcessed());
}

std::uint64_t bits_write_batch(const std::uint32_t* codes, std::size_t count, int bits, std::uint8_t* buffer,
                               std::size_t bytes)
{
    serialize::WriteStream writer(buffer, static_cast<std::int64_t>(bytes));
    for (std::size_t i = 0U; i < count; ++i)
    {
        std::uint32_t code = codes[i];
        (void)writer.SerializeBits(code, bits);
    }
    writer.Flush();
    return static_cast<std::uint64_t>(writer.GetBytesProcessed());
}

void bits_read_batch(const std::uint8_t* buffer, std::size_t bytes, int bits, std::uint32_t* codes, std::size_t count)
{
    serialize::ReadStream reader(buffer, static_cast<std::int64_t>(bytes));
    for (std::size_t i = 0U; i < count; ++i)
    {
        (void)reader.SerializeBits(codes[i], bits);
    }
}

void quaternion_load_batch(int bits, const float* xyzw, std::size_t count, std::uint64_t* words)
{
    const auto shift = static_cast<std::uint32_t>(bits);
    for (std::size_t i = 0U; i < count; ++i)
    {
        const QuaternionWord word = quaternion_load(bits, xyzw + (4U * i));
        words[i] = (std::uint64_t{word.largest} << (3U * shift)) | (std::uint64_t{word.a} << (2U * shift)) |
                   (std::uint64_t{word.b} << shift) | std::uint64_t{word.c};
    }
}

void quaternion_save_batch(int bits, const std::uint64_t* words, std::size_t count, float* xyzw)
{
    const auto shift = static_cast<std::uint32_t>(bits);
    const std::uint64_t mask = (std::uint64_t{1} << shift) - 1U;
    for (std::size_t i = 0U; i < count; ++i)
    {
        const std::uint64_t word = words[i];
        const QuaternionWord fields{
            static_cast<std::uint32_t>(word >> (3U * shift)), static_cast<std::uint32_t>((word >> (2U * shift)) & mask),
            static_cast<std::uint32_t>((word >> shift) & mask), static_cast<std::uint32_t>(word & mask)};
        quaternion_save(bits, fields, xyzw + (4U * i));
    }
}

} // namespace quantize_peers
