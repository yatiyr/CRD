# 2026-11-04: HESAP-NET, GF(2^8) and the RFC 5510 Reed-Solomon code (Track C batch 94)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (Partial). NET.2 waits only on hosted evidence, so the next row's
code starts now (the user's 2026-11-01 order: table order never slows a row).

## CI first

Dispatch 41754591577 (f387ced1) was still running: 7 jobs passed, none failed; it predates 0bec6134, so its ARM64
lanes cannot show crd-crypto. Dispatch 41763564869 (edea8d7d), the first with the ARM64 closure, was still queued.

## What landed

- `crd/hesap/comms/gf256.hpp`: RFC 5510 §8.1's field (0x11D, alpha = 2) as constexpr exponent and logarithm tables;
  multiply, inverse, divide, alpha powers, and `gf256_expand` (a coefficient's two nibble tables).
- Kernel family `hesap.erasure` (`erasure_kernels.hpp`, `kernels/erasure_kernels.cpp`): one GF(2^8) dot product,
  several outputs at once. AVX2 and AVX-512 multiply with vpshufb nibble lookups, and NEON with tbl, one vector
  column of up to four outputs at a time, so each source vector is split into nibbles once per group. SSE2 has no
  byte shuffle: it XORs sign-masked broadcast products c·2^b over 1 KiB tiles. The known-answer check uses five
  outputs of three 141-byte sources, then an accumulating pass that must give zero.
- `crd/hesap/comms/erasure.hpp`: `ReedSolomonCode`, RFC 5510 §8.2's systematic code, k <= n <= 255. Column j of
  GM = V(k,k)^-1 V(k,n) is the Lagrange basis at alpha^j. Encoding evaluates it, and decoding interpolates through the
  k received points, both with barycentric weights in the log domain: O(k^2) per pattern, then O(k) per column, with
  no Gauss-Jordan step and no allocation. Malformed requests are refused before anything is written.
- Capability `numerics.erasure-codes` (owner hesap-comms; signature: the 0x11D polynomial).

## Evidence

- `crd-hesap-comms-tests "[erasure]"`: 1169 assertions in 11 cases on win-debug and linux-gcc-debug, with no
  warnings.
  - Every product checked against a shift-and-add multiply.
  - GM checked entry by entry against a Gauss-Jordan V(k,k)^-1 V(k,n) for eight shapes up to (255, 255).
  - Repair symbols checked against s x GM.
  - Every k-subset of (1,4), (3,7), (4,8) and (5,10) recovers the block, as do random patterns up to (200, 255).
  - Every refusal is checked.
- Kernel arms `crd-hesap-comms-kernels-at-{sse2,sse42,avx2,avx512}` pass. The host (i9-14900K) has no AVX-512, so
  the avx512 body ran under Intel SDE 10.13.1 `-skx`: all tests passed, with the level listed.
- Guards `^crd-no-|^crd-check` all pass, after a non-ASCII test name was fixed.

## Next

The bit-exact oracle and the budget against ISA-L and OpenFEC (pinned testing downloads), a fuzz target over decode
requests, then FlexFEC (RFC 8627), RaptorQ (RFC 6330) and the interleavers.
