# NET.2: ML-KEM and the table PRF behind the provider

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 34; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the
[provider interface](../design/network-foundation.md#provider), decisions P7 and P8.

## The user's decisions this batch acts on

- 2026-11-01, about 01:25: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO
  NOT BLOCK ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD
  STANDARD AND ELITE". No download was needed: the pinned AWS-LC and Wycheproof cover this slice.
- 2026-11-01, about 14:50: "I wanted you to finish all net rows and you stalled on very currently unnecessary things. Of
  course we think about the laws but it seems you are over complicating everything! I WANT NET DONE PROPERLY AND GOLD
  STANDARD AND WITHOUT ANY DEFERS!" The batch takes NET.2's next two clauses in the row's order.

## CI first

Nothing to fix from this track. Complete-tier run 41311293265 (dac5d27e) had only its Preflight job, still queued;
push run 41311264843 (dac5d27e) was pending; every other push run since 438b8932 was cancelled before it started.

## What landed

- **Register.** `crypto.ml-kem` (FIPS 203, final 2024-08-13; ML-KEM-768 `0x0701` and ML-KEM-1024 `0x0702`, key bits
  9472 and 12544) and `crypto.siphash` (SipHash-2-4 `0x0801`, 128 bits) are implemented entries. Two classes join
  `ALGORITHM_CLASSES`: `0x07` post-quantum key encapsulation (key exchange) and `0x08` keyed pseudorandom function
  (integrity). The standards register gains the spec type `paper` (a peer-reviewed publication no standards body
  republishes; `published` or `withdrawn`; `register-review.py` plans it as a read by hand), since SipHash has no RFC.
  Its DOI was resolved through Crossref to Aumasson and Bernstein, "SipHash: A Fast Short-Input PRF", INDOCRYPT 2012.
  A withdrawn NIST publication or paper now fails the guard while pinned (it passed before, for `nist` too).
- **ML-KEM** ([provider.hpp](../../engine/foundation/crypto/include/crd/crypto/provider.hpp), decision P7):
  `kem_sizes`, `kem_encapsulate` and `KemKey` (`generate`, `from_seed`, `public_key`, `decapsulate`). Private keys are
  the 64-byte seed d || z through AWS-LC's deterministic key generation. AWS-LC's raw public key import only copies the
  bytes, so `kem_encapsulate` runs the provider's key check (FIPS 203 section 7.2) first and answers `BadKey` for an
  unreduced key rather than a provider failure. Decapsulation never fails on a ciphertext of the right length
  (implicit rejection). Outputs that overlap an input or each other are `Overlap`, with nothing written.
- **The table PRF** (decision P8, threat T10): `PrfKey` over AWS-LC's `SIPHASH_24`, a 16-byte key read as the paper's
  k0 and k1 little-endian, generated per table or shared between cooperating processes; a key that holds none answers
  `NotReady`, never a hash.
- **A defect fixed on the way:** `AgreementKey::derive` did not refuse a secret buffer overlapping the peer key, which
  the interface's `Overlap` rule requires; it now does, with a test and a mutation.
- **A gate defect fixed on the way:** the strict-analysis helper analysed a test header through the alphabetically
  first unit of its directory, here a fuzz target that does not link the JSON reader, so the header never parsed
  (ungated, never clean). `scripts/cerid_dev/tidy.py` now prefers a unit that includes the header, with a
  `test-dev-workflow.py` case; `wycheproof.hpp` and `policy_test_signer.hpp` then analyse clean.
- **One key handle.** `SigningKey`, `AgreementKey` and `KemKey` share `detail::ProviderKeyHandle` (ownership, move,
  release), replacing three copies of the same move and clear code.
- **Tests** share [wycheproof.hpp](../../tests/foundation/crypto/wycheproof.hpp) (the vector reader, lifted out of
  `test_provider.cpp`). The encapsulation vectors fix the randomness m, which `kem_encapsulate` never takes (FIPS 203
  section 6.2), so `test_kem_prf.cpp` links the provider and checks their known answers through its deterministic
  encapsulation, the same code `kem_encapsulate` runs with a fresh m.
- **Classification.** The cerid-package inventory gained ML-KEM and SipHash; its five findings and the counsel
  question of `counsel.2026-11-01` name them, and `gen-crypto-inventory.py --review cerid-package` recorded the review
  (inventory `5269f2e6...`). The functions did not change (key exchange and integrity were already performed).

## Evidence (cloud machine, linux-gcc-debug, GCC 13.3)

- Wycheproof, every ML-KEM-768 and ML-KEM-1024 file: 100 keys from seeds each (encapsulation key equal, round trip);
  161 and 162 decapsulations equal to the vector's K (implicit rejection of the strcmp, malleable, bit-flipped and
  random ciphertexts included), 20 seeds of other lengths and 20 ciphertexts of other lengths refused each; 133
  encapsulations each against the known answers, with 112 and 116 unreduced keys refused as `BadKey` and 20 lengths as
  `BadLength`, outputs zeroed; 9 expanded decapsulation keys each refused as outside the profile. All 40 SipHash-2-4
  vectors.
- `crd-crypto-tests`: 42 cases, 92,106 assertions (the 24 NET.2 cases 91,110); 8 new cases, among them aliasing,
  zeroing, moves, seeds that fix the encapsulation key (d) and the rejection secret (z), and one KEM key and one PRF key
  used from four threads.
- `crd-fuzz-crypto-provider`: three new modes with oracles independent of the provider (a KEM round trip with a changed
  bit and arbitrary ciphertexts; encapsulation keys judged by a restated FIPS 203 modulus check; the PRF against a
  restated SipHash-2-4) and 4 new deterministic seeds; the 14-seed corpus replays clean and the 10 earlier seeds are
  unchanged.
- Nine source mutations, each caught: the modulus check dropped, a refused key's outputs kept, output aliasing
  unchecked, the ciphertext length unchecked, a long seed accepted, `derive`'s overlap unchecked, the PRF key halves
  swapped, the key read big-endian and a hash without a key.
- The scoped CTests (crypto suites and corpora, `crd-check-registers`, `crd-check-capability-owners`,
  `crd-check-source-routes`, the inventory drift) pass; the configure prints no warning; LLVM 20.1.0 strict analysis of
  the changed C++, the two test headers included, is clean. The Python cases of the changed scripts pass (73 register
  cases with two new ones for the `paper` spec type and the new classes, 60 dev-workflow cases with one new).

## Remaining on NET.2

As the row states, starting with the hybrid groups against an independent peer's key shares, then the TLS 1.3 and
QUIC handshake integration.
