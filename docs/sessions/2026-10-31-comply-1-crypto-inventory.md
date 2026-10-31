# COMPLY.1: the cryptography inventory of every export and its reviewed classification

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-10-31. Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 8; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: the registers'
[cryptography inventory and classification](../design/registers.md#crypto-classification).

## CI first

No failure is attributed to this track, and there is still no hosted result for any of its commits. The push run for
e532ee73 (41230540315) was pending with no job scheduled, every earlier push run of the track (b562ba6e and 88624a49
included) was cancelled by the next push to main before its jobs were scheduled, and the operator's checkpoint run
41215667488 on c2645814 (batches 1 and 2) has no failed job: 24 jobs green, win-public-checks, linux-gcc-asan,
win-asan, win-release and win-debug still running. crd-net, crd-crypto and crd-comply therefore still have no MSVC,
clang-cl, ARM64 or sanitizer build; the next checkpoint the user starts on current main gives them one.

NET.1, NET-STD and COMPLY.0 wait only on [OWN](../ROADMAP.md#slice-own) (track B) and on the user's approval of the
register reads, so this batch starts COMPLY.1, the next row of the scope.

## What landed

- **Cryptography is a register fact.** Every one of the 73 standards entries now declares `cryptography`, the
  functions it performs in the canonical order authentication, confidentiality, integrity, key exchange, key
  derivation (empty for 47 of them: the protocols that ride on a protected transport, the guidance and the formats).
  QUIC v1 and v2, `quic.tls`, TLS 1.3 and MLS name all five; QUIC-LB names confidentiality (it encrypts server ids);
  ICE, STUN and TURN authentication and integrity; SFrame confidentiality and integrity; ECH confidentiality and key
  exchange; the Cerid session control authentication and key exchange, the media control key exchange, the service
  and relay allocation protocols and the policy generations authentication. `ALGORITHM_CLASSES` in
  `scripts/gen-register-ids.py` now carries each class's functions, so an algorithm's class fixes what its entry must
  name, and each of the 17 "Cerid algorithm identifiers" codepoints carries `key_bits`, one length per component: AES
  128 and 256, ChaCha20-Poly1305 256, X25519, Ed25519 and P-256 256, P-384 384, HKDF 256 and 384, ML-DSA-44, -65 and
  -87 10496, 15616 and 20736 (FIPS 204 public keys), and the hybrids `[256, 9472]`, `[256, 9472]` and `[384, 12544]`
  (FIPS 203 encapsulation keys). The generated headers are unchanged.
- **The inventory.** `scripts/gen-crypto-inventory.py` reads the exports from the build (the relocatable package,
  `cerid-package`, from the 15 `crd_package_library` calls; the programs `asset_cooker`, `ceridc`, `shader_cook`,
  `kir_autotune`, `cprof_export`, `runtime` and `sandbox` from the registry), walks each closure through `DEPENDS`
  and writes `assets/comply/crypto-inventory.json`. Today the package performs authentication only (crd-crypto's
  signed policy generations, through a caller-supplied verifier, no algorithm implemented) and no program performs any
  cryptographic function; every algorithm is still planned (NET.2). `--check` is the new
  `crd-comply-crypto-inventory-drift` CTest and the file is in `scripts/generated-sources.json`.
- **Classification records.** `crypto_classifications` in `cmake/legal-register.json` holds a head record per export
  reviewed on 2026-10-31 against its inventory digest, with a finding for each of the five obligations whose check is
  now this script (`us.ear.encryption`, `eu.dual-use`, `int.wassenaar`, `tr.cybersecurity.products`,
  `tr.btk.encryption`; each check flipped from planned to active). The findings are proposals with their basis in
  plain words and `counsel.needed` true: no cryptography for data confidentiality, so Category 5 Part 2 does not
  describe any export (EAR99 where the EAR applies, NLR; not listed in Annex I of Regulation 2021/821; not on the
  Wassenaar Dual-Use List; no Turkish cybersecurity-product permit or BTK encryption duty), whether the EAR reaches a
  Turkish publisher's software at all is a counsel question, and the Category 5 Part 2 note excluding
  authentication-only cryptography is cited from knowledge until the texts are read.
- **The guard.** `scripts/check-registers.py` (`crd-check-registers`) checks every entry's `cryptography` and every
  algorithm's `key_bits`, recomputes the inventories and fails an export without records, a changed inventory without
  a review, findings that miss, repeat or misname an obligation, counsel answers out of order, malformed or repeated
  releases, a release recorded before counsel reviewed it, and a head record kept for a former export.
- **Review, releases and facts.** `--review <export>` records a restated classification against the current
  inventory and sends it back to counsel; `--release <export> <version>` freezes the current, counsel-reviewed head
  record; `--facts <export>` prints what COMPLY.8's store kit turns into the Apple export answer
  (`uses_non_exempt_encryption` is whether the export performs confidentiality) and the EAR self-classification report
  (the findings that owe one), and refuses a stale inventory.
- **China.** `cn.commercial-cryptography` (the Cryptography Law and the Commercial Cryptography Administration
  Regulations: import licensing, export control and product certification) is watched until a product enters China;
  `oscca.gov.cn` joins China's official hosts.
- **Rule generations.** The five changed checks are reviewed as generation 2 of TR, EU, US and INT. The ceridc comply
  test now reads the committed generation numbers instead of spelling generation 1, so a reviewed rule change no
  longer breaks it.

## Verification

On the track machine (c4-standard-8, GCC 13.3, `linux-gcc-debug`, the committed tree 2c39c5b8):

- Build of `crd-comply-tests`, `ceridc` and `crd-ceridc-tests`: 0 warnings.
- CTest `comply|^crd-check-registers$|register-ids-drift|^crd-no-|^crd-check`: 40 of 40, among them
  `crd-comply-crypto-inventory-drift`, `crd-check-registers`, the rule and register-id drift tests and the comply
  suites; `tests/ceridc`: 26 of 26.
- Teeth in the real tree: marking `crypto.aes-gcm` implemented failed `crd-check-registers` ("cerid-package's
  cryptography inventory changed since its classification was reviewed on 2026-10-31", beside the unverified IANA
  codepoints) and the drift test; restoring the register passed both again.
- Python: `test-repository-tools.py` gains `CryptoInventory` (7 cases: the entries' functions and key lengths with
  every malformed form, the inventory from a fixture build with packaging, closure, packages, planned and implemented
  algorithms and three malformed registries, drift, the guard over a review and a changed inventory, every record and
  finding rule, head and release rules, and review, release and facts with each refusal); with `Registers`,
  `LegalRegister`, `ComplyRules`, `RegisterReview`, `RegisterIds` and `CapabilityOwners` 49 cases pass on Windows and
  Linux. Seventeen mutations of the guard and the generator (no digest comparison, no required-findings check, no
  release counsel check, no class functions, any key length, no counsel consistency, a former export's head kept, a
  repeated finding, counsel days out of order, no closure, no package, a constant Apple answer, counsel kept on
  review, planned entries inventoried, an unchanged review accepted, a repeated release, commented registrations
  read) are each caught.
- Strict analysis: `scripts/tidy-files.py` with the pinned clang-tidy 20 parsed the changed ceridc test clean after
  its one finding was fixed (a local constant renamed); clang-format 20 with the repository's `.clang-format`
  reproduces the file, and the rebuilt `crd-ceridc-tests` passed 26 of 26 again with 0 warnings.
- `check-registers.py`, `check-generated.py`, `check-docs.py`, `check-repository.py`, `check-master-plan.py` and
  `check-allman-braces.py` pass; `gen-register-ids.py --check` and `gen-comply-rules.py --check` are current.

## Remaining in COMPLY.1

- **User action:** approve the download of the legal register's first read (the official texts
  `python scripts/register-review.py --register cmake/legal-register.json plan --out <file>` lists, which include the
  EAR, Regulation 2021/821, the Wassenaar List, Laws No. 7545 and 5809 and the sanctions instruments). The read dates
  each consolidated text, replaces the home-page URLs and identifies the Ministry of Trade communiqué that pins
  `tr.dual-use-export`.
- **Counsel review (user action):** the eight head classifications (the EAR's reach, the Category 5 Part 2 reading
  and the Turkish permit questions).
- **Sanctions and embargo screening** for distribution, accounts and payments (OFAC, EU restrictive measures, the UN
  list, UK OFSI and Turkey's asset-freeze regime): the list generations and the screening query of `crd-comply` that
  the store rows consume.
- The NET.2 providers make algorithms implemented; each such change re-reviews the exports it reaches through the
  guard.
- Done once [COMPLY.0](../ROADMAP.md#slice-comply.0) is Done.
