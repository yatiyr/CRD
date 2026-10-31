# NET-STD: algorithm identifiers and signed crypto policy generations

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31. Row: [NET-STD](../ROADMAP.md#slice-net-std) (track C, batch 4; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the
[policy generation](../design/network-foundation.md#policy-wire) and the
[Cerid identifiers](../design/registers.md#cerid-identifiers).

## CI first

No track-C commit has a hosted result yet. Every GitHub Actions run since 3bb87c7f (41210564727 to 41216490620) was
cancelled by a later push with no job scheduled (`jobs: []`), and run 41216954976 for 625bcb77 was still pending
with no job when this batch began; the own CI builds only at operator checkpoints (c2645814). Reading the Python
guards locally instead found a defect from batch 3: `scripts/fuzz.py` had no entry for `crd-fuzz-net-negotiation`,
so `test_every_registered_fuzz_target_has_a_driver_entry` failed. The driver now names it (and the new crypto
target), and `test_fuzz_driver_bounds_every_run_and_names_its_corpora` lists both.

## What landed

- **`crd-crypto`** (`engine/foundation/crypto`, decision D2): created by NET-STD, as `crd-net` was, with no network
  dependency. The capability registry lands `crypto.agility` (its API: `register_ids.hpp`, `verifier.hpp`,
  `policy.hpp`); `crypto.providers` moves from planned to assigned (NET.2 creates its API) and gains signatures that
  refuse a cryptographic library header (`openssl/`, `sodium.h`, `bcrypt.h`, CommonCrypto, `SecRandom.h`,
  `sys/random.h`) or an operating-system or library randomness call outside the module.
- **Algorithm identifiers.** The register gains a "Cerid algorithm identifiers" registry: the high byte is the class
  (hybrid key exchange, traditional key exchange, post-quantum signature, traditional signature, AEAD, KDF), the low
  byte numbers the algorithm. Seventeen identifiers: the three hybrid groups on `tls.hybrid-kex`, and new entries
  `crypto.ecdh` (RFC 7748 and the NIST curves), `crypto.ml-dsa` (FIPS 204), `crypto.eddsa` (RFC 8032), `crypto.ecdsa`
  (FIPS 186-5), `crypto.aes-gcm` (RFC 5116), `crypto.chacha20-poly1305` (RFC 8439) and `crypto.hkdf` (RFC 5869), each
  with its IANA codepoints marked unverified (TLS groups and signature schemes, AEAD numbers) where they exist. The
  register holds 73 entries, 57 pinned; `cerid.policy-generation` is implemented with its evidence and the RFC 7696
  items 2.1, 2.3 and 2.6 name theirs.
- **Generator.** `scripts/gen-register-ids.py` turns each Cerid identifier registry into an enumeration with its
  table and entry lookup (one code path for `ProtocolId` and `AlgorithmId`), adds `AlgorithmClass` and
  `algorithm_class`, writes crypto's header, types the TLS group, signature scheme, extension, AEAD and object
  identifier registries (object identifiers have their own form check), refuses an algorithm identifier outside the
  class table or numbered 0, and groups the cases of one entry under one return (clang-tidy's branch-clone check).
  The net header is byte-identical. `crd-crypto-register-ids-drift` and the provenance manifest hold the new header.
- **Shared wire integers.** The bounded big-endian writer and reader moved out of `negotiation.cpp` into
  `crd/containers/wire_bytes.hpp` (capability `containers.wire-bytes`), so the negotiation and the policy codec share
  one owner. A refused take now consumes nothing; three Catch2 cases cover byte order, the bounds and short reads.
- **Verification interface** (`verifier.hpp`): `SignatureVerifier::verify(algorithm, public key, message,
  signature)` returning valid, invalid or unsupported; pure signing of the whole message. Providers implement it in
  NET.2; Cerid writes no primitive.
- **Policy generations** (`policy.hpp`, `policy.cpp`): the codec (`write_policy_body`, `write_policy`,
  `decode_policy`, `validate_content`), the trust configuration (`validate_config`), `PolicySnapshot` (freshness,
  algorithm status, protocol minimums) and `PolicyStore` (`apply`, `snapshot`, `floor_generation`), with the eight
  numbered decisions of the [contract](../design/network-foundation.md#policy-wire): hybrid trust where every trust
  key signs, monotonic numbers with a persisted floor and rollback refused before verification, issue skew, expiry
  and offline grace, migration windows, the remote disable, the build's ceiling (a generation cannot enable an
  algorithm the build lacks), live swap by snapshot value and exact bounded decoding. ADR-0035 records that the module
  exists.

## Verification (track machine, linux-gcc-debug)

- Build of `crd-crypto-tests`, `crd-net-tests`, `crd-containers-tests` and both fuzz targets: 0 warnings.
- `crd-crypto-tests`: 17 cases, 914 assertions, all passing (codec round trip at the bounds, every validation and
  decoding refusal, the writers' refusals, the hybrid trust set, every signature failure, monotonic adoption with a
  restart, skew, expiry and grace including a saturating grace end, windows, the ceiling, unknown identifiers, the
  remote disable against a running session's snapshot, protocol minimums, a concurrent applier and reader, error
  names, and the generated identifiers).
- `crd-fuzz-crypto-policy`: 15 seeds written by `--seed` (the committed corpus is that output, compared with `diff`)
  and replayed. Oracles: a restatement of the layout and the validation order decides every decode; a model of the
  adoption rules decides every apply and the floor; every snapshot's freshness, status and minimums are checked at
  the window and validity boundaries. The lifecycle seed adopts, conflicts, disables, refuses an earlier issue, a
  forgery, adopts in grace, refuses an expired generation and a rollback.
- Teeth: eleven source mutations of `policy.cpp` (no early rollback, a missing signature skipped, conflict as
  already-current, expiry without grace, no ceiling, refusal one second late, no issue order, a non-hybrid trust set
  accepted, a signature length bound off by one, freshness one second late, no issue skew) each fail the unit tests;
  the fuzz replay catches eight of them (the early rollback and a sub-hour issue skew are not observable to it, and
  its trust set is fixed).
- CTest with `crd-crypto|crd-net|crd-containers|crd-fuzz-crypto|crd-fuzz-net|^crd-no-|^crd-check`: 58 of 58 passed,
  among them both drift tests, the include budget, the container ban, capability owners, source routes and the
  registers guard. `crd-net-tests` (17 cases, 123 802 assertions) and the negotiation replay pass on the shared reader.
- Strict analysis: `scripts/tidy-files.py` with the pinned clang-tidy 20 parsed all eleven changed C++ files clean
  after the findings were fixed (a lookup loop became `std::ranges::find`, the generated switch groups one entry's
  cases, test signer bits became constants, a nested conditional became a function).
- Python: `test-repository-tools.py` `RegisterIds` (5 tests: the crypto header, grouped returns, object identifier
  and class refusals), `Registers`, `CapabilityOwners` on Linux and the whole suite on Windows.
- After the rebase onto origin/main (which moved FNV-1a onto crd-core; the test signer now takes it from
  `crd/core/fnv1a.hpp`) the same build and the 58 scoped CTests passed again; `crd-check-isa-isolation` first judged
  three objects of other tracks' new sources that the scoped build had not compiled, and passed once those targets
  were built.
- Machine time: 0.50 h (16:18 to 16:41 and 16:44 to 16:52 UTC; TERMINATED).

## Remaining in NET-STD

The scheduled re-research tooling; the first snapshot refresh from the RFC Editor, the datatracker and the IANA
registries (it also verifies the `iana::unverified` codepoints of both headers), which needs the user's approval;
and Done once NET.1 is Done (it waits on OWN, track B).
