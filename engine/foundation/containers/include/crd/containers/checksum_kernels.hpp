#pragma once

// The checksum family's per-level kernels (HW-RESILIENCE.1, SIMD-DISPATCH, ADR-0134 §5). Two families, each a table
// per CPU level that bit-exactly computes its formats (crd/containers/checksum.hpp; the level the process runs:
// crd/containers/checksum_dispatch.hpp):
//
//   crc:  portable (slicing by 8), sse4.2 (CRC-32C on the CRC32 instruction), and the carry-less multiply folds as
//         variants: sse4.2 + PCLMULQDQ (128-bit), avx2 + VPCLMULQDQ (256-bit) and avx512 + VPCLMULQDQ (512-bit).
//         PCLMULQDQ is in no psABI level, so a CPU without it runs the sse4.2 or portable table.
//   hash: XXH3's long-input loop (over 240 bytes) on portable, sse2, avx2, avx512 and neon vectors; the short inputs
//         and the final merge are scalar general code.
//
// The ARM64 CRC32 and PMULL instructions are optional ARMv8.0 features that crd-core does not detect yet, so neon runs
// the portable CRCs.

#include <crd/core/types.hpp>

namespace crd::containers
{

struct CrcKernels
{
    // The CRC register over n bytes at data (any alignment): `state` is the register after the bytes before (the CRC
    // value inverted), the result likewise; a CRC value is the final register inverted.
    crd::u32 (*crc32)(crd::u32 state, const crd::u8* data, crd::usize n) noexcept;
    crd::u32 (*crc32c)(crd::u32 state, const crd::u8* data, crd::usize n) noexcept;
};

struct HashKernels
{
    // XXH3's long-input loop over n > 240 bytes at data (any alignment) with the 192-byte secret: the stripes and
    // scrambles of every block, the last partial block and the last stripe, on the 8 accumulators acc holds (the
    // caller sets their initial values and merges them; any alignment).
    void (*xxh3_accumulate)(crd::u64* acc, const crd::u8* data, crd::usize n, const crd::u8* secret) noexcept;
};

// The levels with a body; the dispatchers refuse a build that compiles any other.
extern const CrcKernels kCrcKernelsPortable;
extern const CrcKernels kCrcKernelsSse42;
extern const CrcKernels kCrcKernelsSse42Pclmulqdq;
extern const CrcKernels kCrcKernelsAvx2Vpclmulqdq;
extern const CrcKernels kCrcKernelsAvx512Vpclmulqdq;
extern const HashKernels kHashKernelsPortable;
extern const HashKernels kHashKernelsSse2;
extern const HashKernels kHashKernelsAvx2;
extern const HashKernels kHashKernelsAvx512;
extern const HashKernels kHashKernelsNeon;

namespace detail
{
// The slicing-by-8 tables of each reflected polynomial (0xEDB88320, 0x82F63B78): table k maps a byte to its CRC
// register contribution k bytes ahead. General code owns them; every level's byte loops read them.
struct CrcSlices
{
    crd::u32 table[8][256]; // NOLINT(modernize-avoid-c-arrays): a constant table
};
extern const CrcSlices kCrc32Slices;
extern const CrcSlices kCrc32cSlices;
} // namespace detail

} // namespace crd::containers
