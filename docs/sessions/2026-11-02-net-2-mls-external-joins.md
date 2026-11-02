# NET.2: MLS external joins

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 56; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layer 10a and decision
M11](../design/network-foundation.md#mls); [HPKE, H1](../design/network-foundation.md#hpke).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with external joins.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Dispatch run 41409186119 (fb8452af, complete tier), the first hosted run with MLS
through TreeKEM (035756d4), has no failed job: 27 of its 30 jobs passed (win-tidy, win-release, win-debug,
win-shipping and linux-gcc-debug among them); win-asan 1/2, win-public-checks and linux-clang-tsan are still
running. Push runs 41416720316 (8f0770cc) and 41428427048 (4f30bb98) and run 41429790527 (4f30bb98) are queued, so
no completed hosted run contains 1bc8a676 (layer 8) or later yet.

## What landed

- **HPKE's exporter** (RFC 9180 section 5.3): `HpkeKey::export_secret` and `hpke_export` (SetupBaseR and SetupBaseS
  with the context's Export). The encapsulation, decapsulation and key schedule are now each written once and shared
  by sealing, opening and exporting.
- **External joins, M3 layer 10a** (RFC 9420 sections 8.3 and 12.4.3.2; new decision M11):
  - `Group::group_info` publishes the epoch's signed GroupInfo. `GroupInfoOptions` (which a commit's GroupInfo uses
    too) carries the tree and, only when asked, the external_pub extension: DeriveKeyPair(external_secret)'s public
    key, which no one outside the group can derive. So a group is closed to external joins until a member publishes
    the key.
  - `Group::join_external` joins from such a GroupInfo. It checks the GroupInfo as a Welcome's, exports the init
    secret to the external key, adds an optional resync Remove and PSKs, takes the leftmost free leaf
    (`RatchetTree::reserve_leaf`, with merge_path and create_update_path told the leaf is a joiner's), and signs the
    commit as a PublicMessage. It checks its own commit as the members will, then holds the new epoch and its GroupInfo
    (`PathKeys::start`). A refused commit means the application joins again from a newer GroupInfo.
  - `Group::process` takes external commits: the path leaf's key signs; the list is one ExternalInit (kem_output Npk
    long), at most one Remove and PSKs, all by value; the key schedule starts from the init secret decapsulated with
    the epoch's external key.
- **A defect fixed on the way: section 5.3.1's successor rule was not enforced.** A member could replace its leaf with
  any credential the Authentication Service accepted on its own, through an Update or a commit path. The new
  `AuthenticationService::valid_successor` is now asked for every replaced leaf: an Update (also when a committer
  chooses the cached proposals it prefers), a commit's path leaf, and an external joiner's resync. An external
  joiner's resync is how a stranger would otherwise take a member's place.
- The model group, its proposals and its hostile framing moved from `test_mls_commit.cpp` into `mls_model.hpp`. The
  commit suite and the new `test_mls_external.cpp` share them.
- `crd-fuzz-crypto-mls-commit` gains two modes: the honest external commit changed in any byte, and the published
  GroupInfo changed in any byte, which a joiner must refuse and be left empty. It now has 69 seeds (24 new).

## Evidence (this PC)

- The working group's vectors hold no external commit. None of the 382 commits of the passive-client vectors has a
  new_member_commit sender (scanned this batch). So the joiner and the members are held to each other and to the RFC,
  and live OpenMLS and MLSpp interop (M4) stays the outside check.
- HPKE against RFC 9180: the 18 exported values of the profile's six base-mode vectors match, and exports of 1, Nh and
  255 Nh bytes agree on both sides of six suites.
- linux-gcc-debug: `crd-crypto-tests` 228 cases, 1,206,423 assertions, all passing. The 7 new cases:
  - groups in every suite that new members join externally: with and without the tree, with an external PSK, as the
    tree doubles; members resyncing into leaves 1 and 0 (the second injecting a resumption PSK its store kept); a
    former state processing its own removal; two joiners racing (the loser OutOfWindow, then joining again);
  - the joiners committing and welcoming (the Welcome's GroupInfo carrying the external key, which its joiner
    ignores), with application data each way;
  - the published GroupInfo, and a commit's GroupInfo carrying the new epoch's key when asked;
  - 24 joiner refusals and 19 member refusals, each leaving the joiner empty or the group unchanged;
  - the tree operations;
  - the successor rule for Updates (both the committer's choice and the receiver's check) and for commit paths.
- win-debug (MSVC): the MLS and HPKE cases (65 cases, 404,716 assertions); no warning.
- linux-gcc-asan: the MLS and HPKE cases, the five MLS corpora, and 4,000 seeded mutants of the 69 commit seeds,
  clean under AddressSanitizer and UBSan. The same mutants are clean on linux-gcc-debug.
- Twenty-eight source mutations of the slice, each built and run against the MLS and HPKE cases and the commit
  seeds: twenty-seven caught. Two of them (the guard that keeps a joiner from marking itself removed, and the store
  fallback for a resumption PSK of the joined epoch) are reached only by a resync of leaf 0 with a store-held
  resumption PSK, which the tests gained this batch for that reason. The 28th is equivalent: an export of zero bytes
  is refused by the provider's HKDF-Expand too; the early check stays because it spares the key agreement.
- `tidy-files.py` (LLVM 20.1.0) on the nine changed C++ files: clean after two fixes (two spans renamed that read as
  swapped arguments, a using-declaration the model's move left unused). Two pre-existing lines of `test_mls_commit.cpp`
  past 120 columns were rewrapped.
- The CTest guards with the crypto corpora and register tests (`^crd-no-|^crd-check|^crd-diag-coverage$|
  ^crd-diag-seeded-faults$|crypto.*corpus|register`, ISA isolation excluded): 198 of 198.

## Next

M3 layer 10b: ReInit and subgroup branching (sections 11.2 and 11.3, with their reinit and branch resumption PSKs),
then GREASE (section 13.5) and the post-quantum suites, with live OpenMLS and MLSpp interop (M4).
