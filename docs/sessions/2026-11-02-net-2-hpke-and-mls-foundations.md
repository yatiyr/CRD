# NET.2: HPKE and MLS's first layers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 50; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [HPKE, H1](../design/network-foundation.md#hpke) and
[MLS, M1 to M4](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch starts NET.2's first Remaining clause, MLS (RFC 9420), at its foundation.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane and the win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

No completed hosted run has a failure this track has not fixed. Push run 41358455047 (33ac38d4) finished two more
jobs since the last batch, win-shipping and win-release, each failing `crd-check-isa-isolation` and
`crd-check-compile-commands` on AWS-LC's MULX assembly and jitterentropy's `/Od`, which b37a4177 fixed after that
commit. Push run 41383412180 (7d4774c2) is pending with no job started; no hosted run contains e1bddcbe..1ff4c37d yet.

## What landed

- **Decision M1, Cerid's own MLS** over the provider interface: the vetted implementations are Rust or C++ with
  exceptions and owning containers over OpenSSL, none fits the engine's allocators and zero-allocation message path,
  and none has the post-quantum suites; the MLS working group's vectors and live OpenMLS and MLSpp interop (M4, the
  TLS driver's pattern on the public-checks lane) hold it to the protocol.
- **Decision H1, HPKE** (`hpke.hpp`, `crypto.hpke`): RFC 9180 base mode, single shot, composed from the provider's
  key agreement, HKDF and AEAD: DHKEM over X25519, P-256 and P-384 with DeriveKeyPair, the key schedule and the first
  message of a context. AWS-LC's own HPKE has neither the NIST-curve DHKEMs MLS suites 2 and 7 need nor
  DeriveKeyPair.
- **Decision M2, the profile:** MLS suites 0x0001, 0x0002, 0x0003 and 0x0007; 0x0004 to 0x0006 (X448, Ed448, P-521)
  are refused. `kdf_hash` joins the provider interface: the hash an HKDF is built on, which MLS and HPKE suites name
  by their KDF.
- **MLS's first layers** (`mls.hpp`, decision M3): the presentation language's minimal vector headers and presence
  octets over the shared `WireReader`/`WireWriter`, array-based tree math for perfect trees of 1 to 2^31 leaves, and
  RefHash, ExpandWithLabel, DeriveSecret, DeriveTreeSecret, SignWithLabel, VerifyWithLabel, EncryptWithLabel and
  DecryptWithLabel.
- **Register and pins.** `crypto.hpke` (implemented) takes the HPKE codepoints from `tls.ech` and adds DHKEM(P-256),
  DHKEM(P-384), HKDF-SHA384 and AES-256-GCM; `mls.protocol` gains the four "MLS Cipher Suites" codepoints, a registry
  the generator now types. RFC 9420 and RFC 9180 were read, and the codepoints checked against IANA's MLS registry
  (updated 2025-11-17) and HPKE registry (updated 2026-04-16) on 2026-11-02. The MLS working group's vectors are
  pinned (`mls-vectors`, mlswg/mls-implementations at 2034fe72, SHA-256 `4c2624f7...`, a testing download read at run
  time); RFC 9180's are read from the pinned AWS-LC. The cerid-package inventory gained `crypto.hpke`; its five
  findings were restated and `gen-crypto-inventory.py --review cerid-package` recorded the review (counsel review
  `counsel.2026-11-01` stays open and blocks nothing). `crypto.groups` joins the capability owners.
- **Shared helpers.** `overlaps` (provider.cpp and capability.cpp each had one), `refuse` and `label_of` moved to one
  internal header with the new `LabelBuffer`.

## Evidence (this PC)

- linux-gcc-debug and win-debug (MSVC): `crd-crypto-tests` 174 cases, 833,532 and 833,534 assertions, all passing;
  the 13 new cases (`[hpke],[mls]` and the KDF hash case) 43,392 assertions: the six RFC 9180 base-mode vectors of the
  profile and 26 refused, the 14 deserialization headers, the 10 tree-math trees node by node, every pair of nodes of
  trees up to 64 leaves and the 2^31-leaf tree, the four suites' crypto-basics vectors (three suites refused) and the
  20 external keys of the key-schedule vectors.
- linux-gcc-shipping (GCC 13, LTO): `crd-crypto-tests` and `crd-fuzz-crypto-mls` link clean and the 12 HPKE and MLS
  cases pass, after its link-time `-Wnull-dereference` flagged one byte flip of a copied buffer in `test_hpke.cpp`
  (now the tag's last byte).
- `crd-fuzz-crypto-mls` (new, six modes): 17 seeds written by `--seed`, replayed clean on both lanes, and 4,000
  seeded random mutants of them replayed clean on both. The other crypto corpora, the register and inventory drift
  tests and `tls_interop.py` (79 of 79 against OpenSSL 4.0.3, after `overlaps` and `refuse` moved) pass.
- `tidy-files.py` (LLVM 20.1.8) clean on all 14 changed C++ files after three nested conditionals and their local
  constants' names in the fuzz target were fixed.
- The CTest guards (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, ISA isolation excluded) on
  linux-gcc-debug: 19 of 19 pass.
- Ten source mutations, each caught by the new cases (four by the MLS corpus too): the varint minimality check, a
  presence octet above 1, the parent formula, the "MLS 1.0 " prefix, the generation in DeriveTreeSecret, the DHKEM
  `eae_prk` label, DeriveKeyPair's counter start, the key schedule's mode byte, an opening's output not zeroed on a
  refusal and a KEM context without the recipient key.

## Next

MLS's next layers in M3's order: the secret tree and its generation window, then the key schedule, transcript
hashes and message protection, each against its vector file.
