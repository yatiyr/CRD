// HESAP-NET's quantisation peers, behind plain calls: mas-bandwidth/serialize's compressed_float through its own
// streams and networked-physics' compressed_quaternion, cut verbatim from the pinned Snapshot.h (quantize_peers.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

namespace quantize_peers
{

// serialize's wire constants for a conforming declaration.
void float_params(float min, float max, float resolution, std::uint32_t& max_code, int& bits, float& delta);

// The code serialize writes for `value`, read back from its bit stream; by the declaration or from the constants.
[[nodiscard]] std::uint32_t float_write(float value, float min, float max, float resolution);
[[nodiscard]] std::uint32_t float_write_precomputed(float value, std::uint32_t max_code, int bits, float delta,
                                                    float min);

// What serialize reads from a stream holding `code`; false when it refuses the code.
[[nodiscard]] bool float_read(std::uint32_t code, float min, float max, float resolution, float& value);
[[nodiscard]] bool float_read_precomputed(std::uint32_t code, std::uint32_t max_code, int bits, float delta, float min,
                                          float& value);

// The board's arms: a batch through one stream, returning the bytes it took.
[[nodiscard]] std::uint64_t float_write_batch(const float* values, std::size_t count, std::uint32_t max_code, int bits,
                                              float delta, float min, std::uint8_t* buffer, std::size_t bytes);
[[nodiscard]] std::uint64_t float_read_batch(const std::uint8_t* buffer, std::size_t bytes, std::uint32_t max_code,
                                             int bits, float delta, float min, float* values, std::size_t count);
[[nodiscard]] std::uint64_t bits_write_batch(const std::uint32_t* codes, std::size_t count, int bits,
                                             std::uint8_t* buffer, std::size_t bytes);
void bits_read_batch(const std::uint8_t* buffer, std::size_t bytes, int bits, std::uint32_t* codes, std::size_t count);

struct QuaternionWord
{
    std::uint32_t largest;
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
};

// compressed_quaternion<bits> (compressed_quaternion_64<bits> above ten bits) for bits 2 to 16: its Load, and its Save
// before its final vectorial::normalize, whose SSE estimate is the CPU's and no part of the wire.
[[nodiscard]] QuaternionWord quaternion_load(int bits, const float* xyzw);
void quaternion_save(int bits, const QuaternionWord& word, float* xyzw);

// The board's arms, over words laid out as crd/hesap/net/quantize.hpp's.
void quaternion_load_batch(int bits, const float* xyzw, std::size_t count, std::uint64_t* words);
void quaternion_save_batch(int bits, const std::uint64_t* words, std::size_t count, float* xyzw);

} // namespace quantize_peers
