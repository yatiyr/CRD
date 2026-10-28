// The SIMD-type probe of one CPU level (SIMD-DISPATCH, ADR-0134 §5, §7): kernels/simd_level_probe.cpp is compiled
// once per level by crd_add_cpu_kernels, so each table runs the crd::math::simd types of that level's backend over the
// same inputs. test_simd_levels.cpp compares every level the host executes with the portable one. The header holds
// only data declarations, so a kernel unit compiles nothing from it.

#pragma once

#include <crd/core/types.hpp>

namespace crd::test::simd_probe
{

// Lanes per input array: a multiple of every vector width.
inline constexpr crd::usize kLanes = 64;
// The most result words one operation writes for one input.
inline constexpr crd::usize kMaxWords = 8 * kLanes;

struct Input
{
    alignas(64) crd::f32 a[kLanes];
    alignas(64) crd::f32 b[kLanes];
    alignas(64) crd::f32 c[kLanes];
    alignas(64) crd::f64 da[kLanes];
    alignas(64) crd::f64 db[kLanes];
    alignas(64) crd::f64 dc[kLanes];
    alignas(64) crd::i32 ia[kLanes];
    alignas(64) crd::i32 ib[kLanes];
};

// How two levels' results of one operation compare.
enum class Compare : crd::u8
{
    Bits,     // every bit, NaN payloads included (data movement, bit operations, min/max, integers)
    F32Value, // f32 words; any two NaNs agree (IEEE 754 leaves a two-NaN operation's payload open)
    F64Value, // f64 words; any two NaNs agree
};

struct Op
{
    const char* name;
    Compare compare;
    // Writes the operation's results for `in` into `out` (at most kMaxWords) and returns how many it wrote.
    crd::usize (*run)(const Input& in, crd::u64* out) noexcept;
};

struct Table
{
    const char* backend;   // crd::math::simd::backend_name() in the unit
    crd::usize lane_width; // crd::math::simd::kNativeLaneWidth in the unit
    const Op* ops;
    crd::usize op_count;
};

extern const Table kSimdProbePortable;
extern const Table kSimdProbeSse2;
extern const Table kSimdProbeSse42;
extern const Table kSimdProbeAvx2;
extern const Table kSimdProbeAvx512;
extern const Table kSimdProbeNeon;

} // namespace crd::test::simd_probe
