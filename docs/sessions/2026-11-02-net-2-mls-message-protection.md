# NET.2: MLS message framing, protection and transcript hashes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 52; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layer 6 and M6](../design/network-foundation.md#mls);
[provider](../design/network-foundation.md#provider).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with its sixth layer.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane and the win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

No completed hosted run has a failure this track has not fixed. Push run 41384275913 (1ff4c37d) is the first hosted
run with the capability, early-data and message-schema code; when this batch started, its finished jobs
(linux-gcc-release, linux-clang-arm64-debug, win-arm64-debug, win-vs and both repository checks) had passed and the
rest were running. Push run 41392493046 (30efd427) is pending; no hosted run contains 0dd2b5ab or later yet.

## What landed

- **`mls_message.hpp`, M3 layer 6.** The structures a handshake message carries (credentials, capabilities and
  extensions, leaf nodes, key packages, the seven proposals, commits and update paths), decoded as views of the input
  and written back byte for byte; a credential or proposal of a type this build does not know is its type and an
  opaque vector, as OpenMLS reads it. Framing: FramedContent, the signature over FramedContentTBS (laid out once
  behind its label), PublicMessage with a member's membership tag, MLSMessage, and `framing_admits`, the senders and
  content types section 6 lets each wire format carry, checked where a message is protected or verified and never
  where it is decoded. PrivateMessage: the sender's next handshake or application generation, a fresh random reuse
  guard in the nonce, the content sealed under the message's own header (no copied associated data) and the sender
  data under the keys of the ciphertext's sample; opening reads the sender data, then the key the tree keeps, checks
  the padding is zero and a commit's tag Nh long, and only then spends the generation. Protecting and opening
  allocate nothing and open in place. The transcript hashes and the confirmation tag.
- **Decision M6: Padme padding** (`padme_padding`): a padded length reveals O(log log L) bits for at most 11.6 % more
  bytes; a receiver accepts any run of zeros and refuses any other byte.
- **The provider's `kdf_mac`**: HMAC with SHA-256 or SHA-384 over AWS-LC, MLS's MAC, proven against Wycheproof; a new
  `CryptoError::Malformed` names an authentic message that does not decode or a sender its framing refuses;
  `PreSharedKeyID`'s codec is public (proposals carry it); `WireReader::rest()` lets a decoder view what a structure
  spans.
- **A new fuzz target `crd-fuzz-crypto-mls-message`** (31 seeds): every decoder's acceptance writes back, a private
  message changed in any byte or hostile is refused with its plaintext zeroed and nothing spent, a public message
  verifies exactly when unchanged.
- Fixed on the way: three lines over 120 columns in this track's earlier test sources (`test_mls.cpp`,
  `test_register_ids.cpp`, `test_provider.cpp`) and the missing `crd-fuzz-crypto-mls` row of the fuzz-target table
  in [test instruments](../design/test-instruments.md).

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 192 cases, 1,051,619 assertions, all passing; the 11 new cases (10 in
  `test_mls_message.cpp`, the provider's MAC case) with the other MLS cases: 25 cases, 255,901 assertions. Every
  transcript-hashes vector of the four suites (the AuthenticatedContent written back, the tag, both hashes and the
  restated input); every message-protection vector (public messages verified and protected again, byte for byte in
  the two Ed25519 suites; application data refused as a public message; private messages opened, replayed and
  protected again); the 300 messages vectors' key packages, proposal bodies, commits and messages written back.
- win-debug (MSVC): `crd-crypto-tests` 192 cases, 1,051,620 assertions (after the fix below); both MLS corpora; no
  warning.
- linux-gcc-asan (a new tree on the track's lane): the MLS, HPKE and provider cases (174 cases, 1,050,622
  assertions), the wire-bytes cases, both MLS corpora and the mutants below, clean under AddressSanitizer and UBSan.
  A test written first held views of a temporary's bytes; the vectors' replay check caught it before ASan ran.
- `crd-fuzz-crypto-mls-message`: 31 seeds written by `--seed` and committed (a second `--seed` run writes identical
  files); with 280 real messages from the vectors (key packages, commits, public and private messages, updates),
  replayed clean, and 4,000 seeded random mutants of the 310 clean on linux-gcc-debug, under ASan and on
  linux-gcc-shipping.
- The decoding oracle's finding, from the review before handover: the five bytes `00 00 01 00 03` (an MLSMessage
  holding a Welcome with no body) decoded, but no Welcome or GroupInfo encodes to nothing, so the message could not
  be written back and the target aborted (reproduced on the first build). `take_message` now refuses an empty Welcome
  or GroupInfo; two decoder refusals and the seed `decode-bare-welcome` hold it.
- linux-gcc-shipping (GCC 13 with link-time optimization, which once flagged quic.cpp's `-Wnull-dereference`): the
  crypto suite and both fuzz targets link without a warning; the MLS and HPKE cases (29 cases, 256,574 assertions),
  both MLS corpora and the 4,000 mutants pass.
- `test-instruments.md` lists the target and the previously missing `crd-fuzz-crypto-mls`.
- `tidy-files.py` (LLVM 20.1.0 on the lane) clean on the nine changed C++ sources; the first run flagged nested
  conditionals, a `const_cast` and a raw loop in the new tests, all rewritten.
- The CTest guards (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, ISA isolation excluded) 19
  of 19 and the register drift and fuzz-corpus tests 5 of 5; the first guard run caught the non-ASCII "Padme" test
  name.
- Thirteen source mutations in `mls_message.cpp`, each built and caught by the MLS cases (three by the committed
  corpus too): a new member's commit signed without the GroupContext, the reuse guard left out of the nonce, the
  generation spent before the content opens, the padding unchecked, application data admitted in a PublicMessage,
  the membership tag over a PrivateMessage TBS, the sender data sealed under the whole header, an unknown proposal
  type refused, the wire format left out of the confirmed transcript hash, a commit's tag length unchecked on
  opening, Padme keeping one bit fewer, a leaf node's signed fields taken past its signature, and the sender data's
  leaf read as the generation.
- Validators after the fix: master-plan, repository, Allman braces, generated sources, docs, capability owners,
  registers, pins, register identifiers and the crypto inventory pass; `test-repository-tools.py` passed 316 tests
  (3 skipped) twice and once failed one case whose output was not kept, which the next run passed (this host
  misexecutes about one Python run in ten, per the lessons).
- Not reached by a test: `random_bytes` failing for the reuse guard and the provider refusing an AEAD key; each is
  one pass-through of the provider's verdict.

## Next

MLS's next layer in M3's order: the ratchet tree with tree and parent hashes, its validation and TreeKEM, proven
against `tree-operations.json`, `tree-validation.json` and `treekem.json`.
