#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap
{
// -----------------------------------------------------------------------
// CRDR FourCC pin for hesap-dense on-disk artifacts.
//
// 'HDV0' = Hesap Dense V0. Used by the v0f load/save round-trip when the
// dense BLAS infrastructure has shape (~5wk into the cluster). Pinning
// here at v0a so the constant has one home from day 1.
//
// FourCC byte order: 'H'=0x48, 'D'=0x44, 'V'=0x56, '0'=0x30. Stored
// little-endian on disk to match the rest of CRDR (ADR-0037).
// -----------------------------------------------------------------------

inline constexpr crd::u32 kHesapDenseFourCC =
    (static_cast<crd::u32>('H')) | (static_cast<crd::u32>('D') << 8) | (static_cast<crd::u32>('V') << 16) |
    (static_cast<crd::u32>('0') << 24);

// Compile-time sanity: little-endian bytes spell "HDV0".
static_assert((kHesapDenseFourCC & 0xFF) == 'H', "kHesapDenseFourCC byte 0 must be 'H'");
static_assert(((kHesapDenseFourCC >> 8) & 0xFF) == 'D', "kHesapDenseFourCC byte 1 must be 'D'");
static_assert(((kHesapDenseFourCC >> 16) & 0xFF) == 'V', "kHesapDenseFourCC byte 2 must be 'V'");
static_assert(((kHesapDenseFourCC >> 24) & 0xFF) == '0', "kHesapDenseFourCC byte 3 must be '0'");

} // namespace crd::hesap
