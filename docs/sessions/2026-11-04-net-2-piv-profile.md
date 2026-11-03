# 2026-11-04: NET.2, the PKCS #11 device's PIV profile (Track C batch 91)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.2](../ROADMAP.md#slice-net.2) (Partial). Design: [K14](../design/network-foundation.md#key-store).

## CI first

Dispatch 41717350998 (1bb5ab7c): its four failed jobs (both Repository checks, windows-native, android) and
win-arm64-debug all fail crd-license-manifest or crd-repository-tools on batch 89's unrendered license manifest,
fixed by 8a66ef13 and fddede9a. Nothing new. win-arm64-debug configured no AWS-LC: `CRD_MODULES_WITHOUT=jobs` drops
crd-crypto on ARM64 (crd-platform links crd-jobs), so P3's Windows ARM64 assembly has no hosted proof yet; the row
now says so.

## What landed

- `Pkcs11Token::piv` and the "pkcs11-piv" device, read against yubico-piv-tool 2.7.3's ykcs11 sources: the security
  officer's secret (tried once, never on its final try), the first retired slot (`CKA_ID` 5 to 24) without a token
  certificate, an 18-byte reference whose tag binds store, slot and public key and is checked before any login,
  forgetting by overwrite plus certificate deletion. Both profiles now require `CKA_LOCAL` after generation, and
  share one template builder (`generate_pair`).
- `engine/foundation/crypto/src/cryptoki.hpp`: OASIS's header as Cerid includes it, shared with the test module.
- `tests/foundation/crypto/piv_module.cpp` (`crd-crypto-piv-module`): a PKCS #11 module presenting one PIV card as
  ykcs11 does (fixed slots and labels, SO-only generation and deletion, the certificate written with each key,
  attestations as session objects, the login closed with the last session), its signatures from the test's AWS-LC
  keys and eleven faults behind `piv_module.hpp`; pushed to its mirror on Android.

## Evidence

- win-debug: `[key-store]` 28 cases, 10087 assertions passed (the three PIV cases: 193); tidy (LLVM 20.1.8) clean on
  every changed file; the 22 `crd-no-*`/`crd-check*` guards pass.
- WSL linux-gcc-debug and linux-gcc-asan: every key store CTest passes, the PIV cases included; linux-gcc-debug's
  `crd-crypto-pkcs11-softhsm` passes with `CKA_LOCAL` required.
- Teeth (win-debug), each caught then restored: `CKA_LOCAL` not checked (6 failures), the tag compare emptied
  (both binding cases fail), a slot with a certificate counted free (7), forget keeping the certificate (7).

## Next

The macOS lane for the Keychain and Secure Enclave on `macos-15-intel` (crd-jobs builds there with the Mach-O guards
and a wait-on-address semaphore; Apple silicon needs JOBS-PORT.2's ARM64 fiber context), then the complete-tier run.
