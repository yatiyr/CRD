#pragma once

// CEIR-31b-1a-iii / 31b-4-b-ii-2 — the scalar hash ORACLE for the committed assets/ckir/ui_tint_noise.ckir noise term.
// LIFTED out of tests/kir/test_ckir_asset.cpp's anonymous namespace so BOTH the eval-verify gate (#4138, eval==C++) AND
// the on-device arm (e) gate (test_ui_frosted_glass_gpu.cpp, GPU==eval) compute the reference from ONE definition. The
// integer hash is the EXACT sequence the .ckir carries at n17..n29 (all dtype=U32 => the emitter lowers to unsigned uint
// `>>` and the C++ Mul wraps mod 2^32 — the u32-wrap scar); the noise tail is n30..n32 (Cast<F32> then *(1/2^32)).
// ⛔ noise is computed in FLOAT, not double: `float(uint)` on the GPU rounds a 32-bit int to a 24-bit mantissa, and the
// oracle must lose the SAME bits before the downstream *255 UNORM step ([[feedback_oracle_must_round_every_elementary_op]]).
// The KGraph builders (emit_hash_u32 / emit_noise_from_hash / build_hash_scalar) STAY in test_ckir_asset.cpp — they are
// eval fixtures (a Fragment FragCoord kernel cannot eval; the scalar evaluator is compute-only), not device oracles.

#include <crd/core/types.hpp> // crd::u32

namespace crd::tests
{
// h1 = x*0x9E3779B1 ^ y*0x85EBCA77 (the mix operand of the FIRST >>15 shift; also the bit-31 coverage probe).
inline crd::u32 ref_hash_stage1(crd::u32 x, crd::u32 y) { return (x * 0x9E3779B1U) ^ (y * 0x85EBCA77U); }
// h3 = (h1 ^ (h1>>15)) * 0x27D4EB2F (the operand of the SECOND >>13 shift — a DIFFERENT value than stage1).
inline crd::u32 ref_hash_stage3(crd::u32 x, crd::u32 y)
{
    crd::u32 h = ref_hash_stage1(x, y);
    h ^= h >> 15U;
    return h * 0x27D4EB2FU;
}
// h4 = the full hash; PLAIN `>>` (mirrors eval's i64 >> with NO `& 31` mask — moot for shifts 15/13, exact for < 32).
inline crd::u32 ref_hash(crd::u32 x, crd::u32 y)
{
    crd::u32 h = ref_hash_stage1(x, y);
    h ^= h >> 15U;
    h *= 0x27D4EB2FU;
    h ^= h >> 13U;
    return h;
}
// noise in [0,1) = (f32)h * (1/2^32). ⛔ FLOAT arithmetic in the SAME expression order the shader uses (float(uint) then
// *inv): double here would keep all 32 bits and round differently at the downstream *255 UNORM8 quantization.
inline float noise_from_hash_f32(crd::u32 h)
{
    const float nf = static_cast<float>(h);
    return nf * 2.3283064365386963e-10F; // 1/2^32
}
} // namespace crd::tests
