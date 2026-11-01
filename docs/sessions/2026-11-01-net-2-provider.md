# NET.2: the vetted provider behind crd-crypto

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 33; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the network foundation's
[provider interface](../design/network-foundation.md#provider) and [provider policy](../design/network-foundation.md#crypto).

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". NET.2's text (2026-10-27) asked for approval before provider downloads; this decision and the
  later pre-approval of peer libraries, test servers and network tools give it. AWS-LC, NASM and the Wycheproof vectors
  were pinned under it; the user can object to any of them.
- 2026-11-01, about 14:50: "I wanted you to finish all net rows and you stalled on very currently unnecessary things. Of
  course we think about the laws but it seems you are over complicating everything! I WANT NET DONE PROPERLY AND GOLD
  STANDARD AND WITHOUT ANY DEFERS!" NET.1, NET-STD, COMPLY.0 and COMPLY.1 have every clause proven and wait only on
  table order, so this batch starts NET.2.

## CI first

Nothing to fix from this track. Complete run 41298463221 (13453acb) had one of 30 jobs finished (Preflight, success)
and the Windows Repository checks job running; complete run 41302856153 (92af745a) had not started; every push run since
438b8932 was cancelled before it started or is still pending (41305149530, 4145a2a0).

## What landed

- **Pins** (`cmake/pins.json`): AWS-LC 5.12.0 (tag commit `cd481c20`, released 2026-10-23, 154,548,427 bytes, SHA-256
  `1de549ad...`; LICENSE read: Apache-2.0 and ISC with MIT, BSD-3-Clause and public-domain parts), NASM 2.16.03's
  Windows x64 release (513,543 bytes, `3ee47822...`; the 2.16 line AWS-LC's own Windows CI uses; nasm.us timed out on
  one of four fetches here, which the cold-acquisition lanes will meet too) and C2SP/wycheproof at `d17cc89f` (the test
  vectors, 26,786,397 bytes, Apache-2.0). `check-pins.py` now reads hyphenated package names.
- **The build**: `crd_module(engine/foundation/crypto ... PACKAGES aws-lc ... TEST_PACKAGES wycheproof)`; AWS-LC builds
  libcrypto only from its pregenerated sources (no Perl, no Go, no writes into the source cache), linked privately into
  `crd-crypto` and exported link-only as `Cerid::aws-lc-crypto` (the package config finds Threads for it outside
  Windows); no link-time IL in its objects; Windows x64 assembles with the pinned NASM; `cl.exe` ARM64 takes its
  portable C (decision P3). The configure is warning-free: AWS-LC's 3.5 minimum is raised to 3.10 as for OpenBLAS (CMake
  4.4 ignores `CMAKE_WARN_DEPRECATED` under CMP0218), and its cached `ENABLE_PRE_SONAME_BUILD`, which made every
  reconfigure warn, is dropped before it is added.
- **The interface** ([provider.hpp](../../engine/foundation/crypto/include/crd/crypto/provider.hpp),
  [provider.cpp](../../engine/foundation/crypto/src/provider.cpp)): randomness, constant-time equality, zeroing and
  key identifiers; `AeadKey` (AES-128/256-GCM, ChaCha20-Poly1305) holding the provider's 584-byte context inline, so
  keys and per-packet operations allocate nothing, with exact lengths, in-place operation and refused aliasing;
  HKDF-SHA256/384; `SigningKey` and `ProviderVerifier` for ML-DSA-44/65/87, Ed25519 and ECDSA P-256/P-384 (strict DER);
  `AgreementKey` for X25519, P-256 and P-384. `ProviderVerifier` is now the policy generations' production verifier and
  `provider_algorithms()` their ceiling. Decisions P1 to P6 are in the contract.
- **A defect the tests found in the first draft**: AWS-LC decodes the hybrid point form (0x06, 0x07) at the
  uncompressed length, so the explicit uncompressed-form check is required (TLS 1.3 accepts only 0x04); a
  draft that dropped it as redundant accepted a hybrid peer, and the test of every other form now holds it.
- **Registries**: `crypto.providers` is landed; `crypto.ecdh`, `crypto.ml-dsa`, `crypto.eddsa`, `crypto.ecdsa`,
  `crypto.aes-gcm`, `crypto.chacha20-poly1305` and `crypto.hkdf` are implemented with `test_provider.cpp` as evidence;
  the cerid-package inventory now performs confidentiality, so its five findings were restated (5D002 under the
  Cryptography Note, ENC with the (b)(1) or (b)(2) question, Wassenaar's Note, Article 39 of Law No. 5809, Law No.
  7545), reviewed with `gen-crypto-inventory.py --review`, and its counsel question rewritten in the open review
  `counsel.2026-11-01`. The dependency manifest and the generated-sources record follow.
- **The ISA isolation guard** gained the `provider` role (ADR-0134 section 6): AWS-LC's objects are not Cerid code, the
  manifest lists its archive, a map image names it as a library and an ELF image leaves out its functions (`nm`), so
  Cerid code in the same image is still judged. Before the role the debug tree's guard listed hundreds of AWS-LC
  routines (AES-NI, VAES, AVX2, AVX-512 GCM, SHA extensions); with it, none.

## Evidence (cloud machine, linux-gcc-debug, GCC 13.3)

- `crd-crypto-tests`: 34 cases, the 16 provider cases with 70,315 assertions: every Wycheproof AES-GCM (133 in profile,
  183 refused outside it), ChaCha20-Poly1305 (316 and 9), HKDF-SHA256 (83 valid) and -SHA384 (80), X25519 (31 zero
  secrets refused, the rest equal), ECDH P-256 and P-384 (25 and 19 refused), Ed25519 (181 judged), ECDSA P-256 and
  P-384 (174 and 194 valid, 310 each refused) and ML-DSA-44/65/87 vector (172, 202 and 233 verify vectors; 83, 102 and
  93 keys derived from their seeds equal to the vectors' public keys; 70, 80 and 71 messages signed and verified), RFC
  8032's two deterministic signatures, FIPS 180-4's SHA-256 example, concurrent sealing and signing, and a policy
  generation signed by real ML-DSA-65 and Ed25519 keys adopted by `PolicyStore`, its forgery refused.
- `crd-fuzz-crypto-provider`: 10 seeds in `tests/foundation/crypto/fuzz/corpus-provider`, replayed clean; the AEAD
  re-seal, HKDF prefix and sign-then-verify oracles.
- Eight source mutations, each caught by the suite: the AEAD's partial aliasing unchecked, a refused EC peer's secret
  kept, an all-zero X25519 secret accepted, the hybrid point form accepted, HKDF's bound one block late, a hybrid group
  listed as implemented, P-384 ECDSA hashed with SHA-256, a 12-byte AEAD tag.
- The scoped CTests (the crypto suites and corpora, `crd-check-registers`, `crd-check-capability-owners`,
  `crd-check-source-routes`, the `crd-no-*` guards) pass; LLVM 20.1.0 strict analysis of the four changed C++ files is
  clean; the configure prints no warning. The ISA isolation guard judged 1,946 general and 143 kernel objects with no
  AWS-LC finding; its 8 findings are objects of other tracks' new sources this incrementally built tree had not
  compiled (stale-tree reports, none in crd-crypto).
- The Python cases of the changed scripts pass (pins, ISA guard with a new provider case, fuzz driver, registers).

## Remaining on NET.2

As the row states: the KEM and the hybrid groups, the keyed PRF, the TLS 1.3 and QUIC handshake integration with the
profile's conformance tests, certificate compression, raw public keys, resumption, address validation and ECH,
identities and capability tokens, anti-replay, server-authorized schemas, MLS with interop, the key store, the first
policy generation and its offline keys, the verified model, the key-logging guard, Windows ARM64 assembly and prefixed
symbols, the boards against OpenSSL and BoringSSL, and the hosted evidence of this batch's complete-tier run (Windows,
clang-cl, ARM64, the sanitizers and the IPO lanes build AWS-LC for the first time there).
