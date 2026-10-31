# CRC folding and XXH3 in the checksum family

<!-- doc-role: reference -->
> Technique reference. Live owner: [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1).

Code: [checksum.hpp](../../engine/foundation/containers/include/crd/containers/checksum.hpp),
[crc_kernels.cpp](../../engine/foundation/containers/kernels/crc_kernels.cpp),
[hash_kernels.cpp](../../engine/foundation/containers/kernels/hash_kernels.cpp),
[checksum.cpp](../../engine/foundation/containers/src/checksum.cpp); tests:
[test_checksum.cpp](../../tests/foundation/containers/test_checksum.cpp);
evidence: [session](../sessions/2026-10-31-hw-resilience-checksum-family.md).

## Parameters

| Quantity | Value | Contract |
|---|---|---|
| CRC-32 (ISO-HDLC) | P = 0x04C11DB7, reflected 0xEDB88320, init and final xor 0xFFFFFFFF | check "123456789" = 0xCBF43926 |
| CRC-32C (Castagnoli) | P = 0x1EDC6F41, reflected 0x82F63B78, same init and xor | check = 0xE3069283; the SSE4.2 CRC32 instruction |
| Kernel register | the CRC value inverted | chains: crc(b, crc(a)) = crc(a ++ b) |
| Fold constant K(n) | reflect32(x^n mod P) << 1, 33 bits | a lane D bits ahead: lo64 · K(D+32) xor hi64 · K(D−32) |
| Distances | 128 (one lane), 256, 512, 1024, 2048 bits | four lanes or registers per step, then one |
| Barrett | P' = reflect33(P with x^32), μ = reflect33(floor(x^64 / P)) | 64 to 32 bits; CRC-32: 0x1DB710641, 0x1F7011641 |
| XXH3 secret | 192 bytes, the specification's kSecret | a seed derives a secret: +seed and −seed by 8-byte word |
| XXH3 classes | 0, 1–3, 4–8, 9–16, 17–128, 129–240, over 240 bytes | only the last is a kernel (64-byte stripes, 16 per block) |

## The CRC fold

A CRC is the remainder of the message times x^32 modulo P over GF(2). A 128-bit block B that sits D bits before the
end of the data still to come contributes B · x^D mod P, and a carry-less product computes that without bit loops:
split B into its two 64-bit halves and multiply each by a constant that moves it D bits. In the reflected (LSB-first)
representation the first half needs x^(D+32) and the second x^(D−32), each reflected and shifted once because the
carry-less product of two reflected values lands one bit low (Gopal et al., "Fast CRC Computation for Generic
Polynomials Using PCLMULQDQ Instruction", Intel, 2009). Four independent lanes hide the multiply's latency; the lanes
then fold onto each other 128 bits at a time, 128 bits reduce to 64 with K(64) and to the 32-bit register with
Barrett's quotient estimate μ. The wide variants fold 256- and 512-bit registers by K(1024) and K(2048) and finish on
the same 128-bit path.

Pitfalls:

- PCLMULQDQ and VPCLMULQDQ are in no x86-64 psABI level, and ARM64's CRC32 and PMULL are optional in ARMv8.0, so the
  folds and the ARM64 CRC instructions are kernel variants (ADR-0134 §1); a CPU without them runs the table below.
- One 128-bit fold serves PCLMULQDQ and PMULL through a few primitives (products of the low or high halves, a byte
  shift). `vmull_p64` differs by compiler: ACLE takes two `poly64_t` scalars, cl takes and returns vector registers.
- Constants are derived at compile time from P and checked against the published CRC-32 ones (`static_assert`); a
  hand-typed table is the usual source of a wrong CRC-32C.
- The register is injected into the first lane's low 32 bits; the wide paths must zero the rest of the register.
- Inputs under 64 bytes, and the tail under 16, go to the slicing tables (CRC-32) or the CRC32 instruction (CRC-32C
  on x64, both polynomials on ARM64).

## XXH3

XXH3-64 (Collet, xxHash 0.8 specification) keeps 8 u64 accumulators. Each 64-byte stripe adds every input word to its
neighbour's accumulator and the product of the low and high 32 bits of input xor secret to its own; after 16 stripes
the accumulators are scrambled (x ^ x >> 47 ^ secret, times 0x9E3779B1). The last stripe overlaps the end. Integer
lanes only, so SSE2, AVX2, AVX-512 and NEON compute the same bits; the 32 × 32 → 64 products are `pmuludq`
(`vmull_u32` on NEON). The short classes are scalar mixes of the secret and the length.

## Verification

Reference vectors from libxxhash 0.8.2, Python's zlib and a bit-serial CRC; every level against the bitwise register
or the portable accumulators at every length class and alignment; avx512 under Intel SDE (`icx` for VPCLMULQDQ);
the ARM64 variants on the hosted ARM64 lanes.
