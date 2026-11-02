# NET.2: MLS's post-quantum suites and GREASE

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 59; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [MLS, M2, M3 layers 10d and 10e, decision
M13](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" and "AS SECURE AND FAST AND COMPACT AS POSSIBLE WITH THE LATEST ALGORITHMS AND
  TECHNIQUES": MLS groups now run in the post-quantum suites.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Push run 41416720316 (8f0770cc, MLS groups) passed all 30 jobs: the first hosted
MSVC, clang-cl, ARM64, win-asan and win-tidy compile of `mls_group.hpp`, `mls_group.cpp` and their tests. Scheduled
run 41429790527 (4f30bb98, MLS commits): 16 passed, 14 running, none failed. Push run 41462446381 (d7371489) is
queued.

## What landed

- **The suites** (decision M2): the eleven of draft-ietf-mls-pq-ciphersuites-07, as its tables give them, through
  `iana::unverified` (IANA does not list them yet; `mls.pq-ciphersuites` stays planned until it does). The draft was
  read again for the KEM, KDF, AEAD and signature of each suite and its ML-DSA rules (pure, empty context, FIPS 204
  key encoding).
- **Nenc, not Npk.** Every kem_output was sized by the KEM's public key, which only a DHKEM's encapsulation matches;
  an ML-KEM-768 encapsulation is 1088 bytes against a 1184-byte key. The UpdatePath's ciphertexts, the Welcome's
  EncryptedGroupSecrets and the ExternalInit's check now use `hpke_sizes(kem).encapsulation`.
- **Sizes**: `kMaxNodeKeyBytes` and `kMaxEncapsulationBytes` (1665, MLKEM1024-P384), `kMaxSignatureKeyBytes` (2592) and
  `kMaxSuiteSignatureBytes` (4627, ML-DSA-87) in `mls.hpp`, replacing the 97-byte node key, a file's 104-byte leaf
  signature and a signature key copied into a node-key buffer; an UpdatePath's up to 31 node keys take one allocation
  (52 KiB would have been a stack frame).
- **GREASE** (section 13.5, decision M13): `grease` and `random_grease`; an empty extension of a random GREASE type in
  every GroupInfo Cerid signs (`GroupInfoOptions::grease`, on by default, none added beside an application's own);
  capabilities compared without GREASE values (`supports_extension`, `supports_proposal`, the new
  `supports_credential`), while a leaf's own extensions need only be listed, so a listed GREASE leaf extension is
  accepted. `each_extension` moved to `mls_detail.hpp` (the GroupInfo builder needs it too).
- **Fuzzing**: the honest groups of `crd-fuzz-crypto-mls-commit` (both wire formats) and `-mls-group` are made in every
  suite; `-mls-message`'s signers covered four suites while its selector reached all of them, which would have read
  past the array, and its buffers now hold an ML-DSA-87 message.

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 238 cases, 1,440,011 assertions, all passing. The suite loops now run every
  behavioural MLS case in fifteen suites: creators' groups, commits as public and private messages, external joins
  and their refusals, reinitialization and branching, the TreeKEM model runs. Their first run failed in the
  post-quantum suites on the test side only (buffers sized for 1 KiB messages and Npk-long encapsulations; an
  all-zero key, which FIPS 203's check accepts, used as the unusable external key; a model run whose seed left no
  unmerged leaf in two suites, now run with fresh seeds until it reaches one, at most four times).
- New and changed cases: the suite table restated from the draft and the four maxima held to it; every test member's
  capabilities list a GREASE value, its leaf carries a GREASE extension and its key package another; a GREASE case:
  the fifteen values over the 16-bit space, 1,500 random draws reaching each, a GroupInfo's single GREASE extension
  (none when asked not to, none beside the application's own), a Welcome and an external join through GREASE, and the
  refusals of a GREASE GroupContext extension (at creation and in `check_leaf`), GroupContextExtensions proposal and
  credential type.
- linux-gcc-asan: the MLS and HPKE cases (638,282 assertions) clean under AddressSanitizer and UBSan after a fix to
  the new test (it decoded a GroupInfo from a temporary it then read); the three changed fuzz targets' corpora clean.
- win-debug (MSVC): the MLS and HPKE cases (638,282 assertions), no warning.
- Ten source mutations, each built and run against the MLS cases: all caught (Npk instead of Nenc in the UpdatePath,
  the ExternalInit check and the Welcome; 0xFAFA taken as GREASE; fourteen values drawn; GREASE supported as a
  credential type and as an extension; a leaf's own extensions judged GREASE-blind; no GroupInfo GREASE; GREASE added
  beside the application's own).
- `tidy-files.py` (LLVM 20.1.0 on the lane) on the twenty changed C++ files: clean. CTest guards with the crypto
  corpora (`crd-fuzz-crypto|^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, and the register-id
  drift check): 31 of 31.
- Fuzz seeds: 159 new seeds written by the targets themselves (`--seed`) into `corpus-mls-commit` (125),
  `corpus-mls-group` (12) and `corpus-mls-message` (22); every target's seeds replayed with its committed corpus on
  linux-gcc-debug, no input rejected.

## Next

M3 layer 10f: live OpenMLS and MLSpp interop (M4). Read the pinned peers' sources for the post-quantum codepoints
they implement before planning their interop.
