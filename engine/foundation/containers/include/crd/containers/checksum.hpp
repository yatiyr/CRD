#pragma once

// The engine's checksum family (HW-RESILIENCE.1; the one owner OWN.1 records): CRC-32, CRC-32C and the 64-bit XXH3
// hash. Every cooked asset, compressed block, crash bundle, document and network payload that carries a checksum
// computes it here. Each is a published format with fixed bits: every CPU level computes the same value
// (crd/containers/checksum_kernels.hpp), so a value written on one machine verifies on any other.
//
// The CRCs chain: crc32(b, crc32(a)) == crc32(a ++ b), so a stream is checked block by block without a copy.

#include <crd/containers/span.hpp>
#include <crd/core/types.hpp>

namespace crd::containers
{

// CRC-32/ISO-HDLC, the CRC of PNG, ZIP, gzip and Ethernet: polynomial 0x04C11DB7 reflected, initial value and final
// xor 0xFFFFFFFF. crc32("123456789") == 0xCBF43926; crc32({}) == 0. `crc` is the value of the bytes before `data`.
[[nodiscard]] crd::u32 crc32(ConstSpan<crd::u8> data, crd::u32 crc = 0U) noexcept;

// CRC-32C (Castagnoli; iSCSI, ext4, SCTP): polynomial 0x1EDC6F41 reflected, initial value and final xor 0xFFFFFFFF.
// crc32c("123456789") == 0xE3069283. `crc` chains as crc32's does.
[[nodiscard]] crd::u32 crc32c(ConstSpan<crd::u8> data, crd::u32 crc = 0U) noexcept;

// XXH3-64 of the xxHash 0.8 specification with the default secret, the 64-bit hash zstd frames and many asset formats
// use: xxh3_64({}, 0) == 0x2D06800538D394C2. A non-zero seed derives the secret as the specification does.
[[nodiscard]] crd::u64 xxh3_64(ConstSpan<crd::u8> data, crd::u64 seed = 0U) noexcept;

} // namespace crd::containers
