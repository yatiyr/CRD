#pragma once

#include <cstddef>
#include <cstdint>

namespace crd
{
/// Signed 8-bit integer (–128 … 127).
using i8 = std::int8_t;
/// Signed 16-bit integer (–32 768 … 32 767).
using i16 = std::int16_t;
/// Signed 32-bit integer (–2 147 483 648 … 2 147 483 647).
using i32 = std::int32_t;
/// Signed 64-bit integer.
using i64 = std::int64_t;

/// Unsigned 8-bit integer (0 … 255). Also the byte type.
using u8 = std::uint8_t;
/// Unsigned 16-bit integer (0 … 65 535).
using u16 = std::uint16_t;
/// Unsigned 32-bit integer (0 … 4 294 967 295).
using u32 = std::uint32_t;
/// Unsigned 64-bit integer (0 … 18 446 744 073 709 551 615).
using u64 = std::uint64_t;

/// 32-bit IEEE 754 single-precision float. Exactly 4 bytes on all targets.
using f32 = float;
/// 64-bit IEEE 754 double-precision float. Exactly 8 bytes on all targets.
using f64 = double;

/// Unsigned pointer-sized integer. Use for sizes, indices, and counts.
using usize = std::size_t;
/// Signed pointer-sized integer. Use for pointer differences and signed offsets.
using isize = std::ptrdiff_t;

static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");
static_assert(sizeof(usize) == sizeof(void*), "usize must be pointer sized");
} // namespace crd
