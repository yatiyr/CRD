# NET.2: MLS interoperability with MLSpp

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 60; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [MLS, M3 layer 10f and the amended decision
M4](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!": Cerid's MLS now runs groups with an independent implementation, hostile input included.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane); no cloud machine ran (0 hours, 0 USD).
- Testing downloads are pre-approved (2026-11-01): MLSpp and nlohmann_json are pinned from their official GitHub
  archives with SHA-256 and size.

## CI first

Nothing from this track to fix. Scheduled run 41429790527 (4f30bb98): 23 of 30 jobs passed, 7 running, none failed.
Push runs 41462446381 (d7371489) and 41470966450 (52c54be2) are queued and pending; no completed hosted run includes
8a5f08e2 yet.

## The peers' post-quantum codepoints (read first)

Read from the pinned sources on 2026-11-02: MLSpp's main (`include/mls/crypto.h`) implements 0x0008
MLKEM768X25519_AES256GCM_SHA384_Ed25519, 0x0009 MLKEM768P256_AES256GCM_SHA384_P256 and 0x000A
MLKEM1024P384_AES256GCM_SHA384_P384, with HPKE KEM ids 0x0041, 0x0042, 0x0050, 0x0051 and 0x647A (X-Wing); OpenMLS
0.9.0 (`traits/src/types.rs`) 0x004D (X-Wing, ChaCha20-Poly1305), 0x004E and 0x004F (X-Wing with AES-256 and AES-128)
and 0x0906 (ML-KEM-1024 with ML-DSA-87), and none of the P-256 or P-384 hybrids. Cerid's are
draft-ietf-mls-pq-ciphersuites-07's (0x0010 to 0x0017, 0x0020 to 0x0022). Three implementations, three provisional
assignments: a suite's codepoint is hashed into every key package, GroupContext and reference, so no alias can make
them agree without a non-standard codepoint in Cerid's table. Decision M4 is amended: interop runs on the four RFC 9420
suites all share, and the post-quantum suites interoperate once the peers take the draft's or IANA's codepoints
(`mls.pq-ciphersuites`, still planned for that reason).

## What landed

- **Pins**: `mlspp` (main at fc724c31, 2026-09-21; its only tag is v0.1.0 of 2019; BSD-2-Clause, its
  `third_party/variant.hpp` BSL-1.0) and `nlohmann_json` 3.12.0 (MIT; MLSpp's CMake requires its package). Both
  gated on the new `CRD_BUILD_MLS_VS_MLSPP`, which the `linux-gcc-public-checks` preset turns on.
- **Build**: the interop OpenSSL 4.0 project is now built once for either test (`tests/foundation/crypto/CMakeLists.txt`);
  nlohmann_json installs its package and MLSpp builds by its own CMake against that OpenSSL (its library and the three
  it links, no tests), so its post-quantum code is compiled too.
- **The protocol and the peers**: `crd-mls-peer` (crd-crypto) and `crd-mlspp-peer` (MLSpp and OpenSSL only) each hold
  several members and speak one line protocol (member, create, join, psk, propose, commit, process, protect,
  group_info, external_join, state). Cerid's members carry GREASE in every capabilities list, a GREASE leaf extension
  and a GREASE key package extension; MLSpp adds its own. Two adaptations to MLSpp's interface, stated in its peer:
  MLSpp caches a proposal only when it handles its message, so a member handles its own at once (as the delivery
  service would return it); and MLSpp has no outcome for a commit that removes the member and refuses to cache a Remove
  of the member itself, so the peer reads those Removes after MLSpp unwrapped (verified) the messages.
- **The driver** (`mls_interop.py`, the `crd-crypto-mls-interop-mlspp` CTest): per suite, a group script with each
  library creating, handshakes public and private (16 scripts), a refusal case and the TP-9 reproduction: 24 cases.
  Each script adds by value (tree in the Welcome), commits an Update and an Add by reference for a joiner who fetches
  the tree, an Update and a Remove by reference, an external PSK and an Add without a path, an empty commit, external
  joins from a GroupInfo with and without the tree, and a resync by external commit, the roles alternating between the
  libraries; after every epoch every member's epoch, authenticator, exporter, tree hash and roster are equal and all
  read each other's application data. Every proposal, commit and application message is first delivered forged in
  each authenticated field (signature, membership tag, confirmation tag, sender data, content); an application message
  delivered again and one of the past epoch are refused; a committer refuses an endless key package (decision M8,
  Cerid) and one of another suite (both). Independently of both libraries the driver reads each key package (suite, key
  lengths, identity, lifetime, GREASE in capabilities and extensions), each Welcome (suite, one entry per joiner
  addressed by the KeyPackageRef it computes, Nenc-long encapsulations) and each GroupInfo (GroupContext, the tree
  exactly when asked for, external_pub for joiners, Cerid's one GREASE extension, the tree hash every member reports).
- **mls_group.hpp**: `Group::process` now states that a member a commit removes checks no confirmation tag (the path
  that keys the new epoch is not encrypted to it, section 12.4.2), which the forged-tag deliveries showed is true of
  both libraries.

<a id="tp-9"></a>
## TP-9: MLSpp spends a generation on forged content

A PrivateMessage whose content seal is forged makes MLSpp's member refuse the genuine message after it ("Request for
expired key"): the generation's key is taken before the seal authenticates, though RFC 9420 section 9.2 consumes a key
that "was used to encrypt or (successfully) decrypt a message", so anyone on the delivery path can make a
member lose any message. Registered as [TP-9](../third-party-defects.md#tp-9) and let be; `forged_content_spends`
reproduces it in a group of two MLSpp members in each suite (a Cerid member added afterwards refuses the same forgery
of the next message and reads it genuine), and fails when MLSpp is fixed. MLSpp's members get every other forgery.

## Evidence (this PC, linux-gcc-public-checks through the track's WSL lane)

- `crd-crypto-mls-interop-mlspp`: 24 of 24 cases; one traced run made 10,364 requests, 3,012 messages processed and
  5,385 refusals (3,050 Cerid `forged`, 248 `replayed`, 24 `out-of-window`, 4 `expired`, 4 `unsupported`; MLSpp's AEAD,
  membership tag, signature, confirmation and epoch refusals).
- `crd-crypto-tls-interop-openssl` after the shared OpenSSL block: 79 of 79.
- CTest guards (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`) on the lane tree: all pass but
  `crd-check-diag-runbooks`, which names tests of executables this partly built tree has not built (no runbook, CTest
  or diagnostic command changed).
- `tidy-files.py` (LLVM 20.1.0) on both peers: clean after two findings fixed (constant names, `starts_with`, and
  `main` of the MLSpp peer no longer able to throw).

## Next

M3 layer 10g: live OpenMLS interop. First: whether `docs/design/pinned-inputs.md` takes OpenMLS 0.9.0's `Cargo.lock`
(crates.io checksums) as the pin of its crate set, and the pinned Rust toolchain for the lane.
