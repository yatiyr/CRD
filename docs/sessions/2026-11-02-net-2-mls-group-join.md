# NET.2: MLS groups, validation against a group and the Welcome

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 54; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layer 8 and decisions M8 and
M9](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with its eighth layer.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Push run 41384275913 (1ff4c37d) has one failed job, win-release: one of its 9,497
tests, `crd-perf-kernels-at-avx512-sde-skx` (the hardware check found no pinned CPU under SDE, `cpus_pinned` 0), which
is not this track's code; fb8452af on main, after the run, confirms a CPU pin from the thread's affinity. Its
win-debug job is still running. Push run 41407549025 (80e95b01) was cancelled, so no completed hosted run contains
81e99526 or later yet and the previous batches' audit carries forward.

## What landed

- **`mls_group.hpp`, M3 layer 8.** The group's extensions (each type once, required_capabilities, external_senders,
  the default types); a leaf node checked against its group (section 7.3) and a whole tree's credential types
  (13.3); a key package checked against its group (10.1), its signature and KeyPackageRef; GroupInfo, GroupSecrets
  and Welcome with their codecs, GroupInfoTBS signed and verified; `seal_welcome` (the committer's side) and
  `open_welcome`; `Group::create` (section 11) and `Group::join` (12.4.3.1), a refusal leaving the group empty.
- **Decisions M8 and M9**: the caller's clock and maximum lifetime (OpenMLS's twelve weeks and an hour by default),
  the Authentication Service and the member's store as interfaces, keys checked once when they enter a group, the
  capabilities' version and suite not checked (sections 7.3 and 10.1 do not ask it, and the working group's suite-7
  welcome key package does not list 0x0007: the first run of the vectors found this), the join's order, reinit and
  branch PSKs refused until layer 9.
- `check_agreement_public_key` in the provider (an EC point on the curve, an X25519 key not of small order), held to
  every Wycheproof X25519 and ECDH vector's verdict; `take_group_context`, `welcome_secret` and `welcome_key` in
  `mls.hpp` (a joiner needs the GroupInfo's key before it can read the GroupContext); `EpochSecrets::clear`.
- Shared instead of repeated: the codec helpers (`Size`, `take_list`, the credential codec) moved into
  `mls_detail.hpp`; byte equality, order and `repeats` into `byte_spans.hpp` (quic.cpp's own copy removed); the MLS
  tests' vector, suite, identity and generator helpers into `mls_test_support.hpp` (four files had copies).
- **A new fuzz target `crd-fuzz-crypto-mls-group`** (24 seeds), registered in `fuzz.py`, its tool test and
  [test instruments](../design/test-instruments.md).

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 213 cases, 1,162,091 assertions, all passing; the 11 new cases in
  `test_mls_group.cpp`: the 4 welcome vectors of the profile (each Welcome decoded and written back, its key package
  checked, the Welcome opened with the init key, the GroupInfo verified under the signer's key, the epoch's
  confirmation tag); the 32 passive-client-welcome vectors of the profile (16 with the tree fetched, 16 with it in the
  GroupInfo, 16 with external PSKs), each joined to its epoch authenticator and then refused as Replayed by a member
  already in the group; every messages vector's Welcome and GroupInfo written back; a creator's group in every suite
  welcoming three joiners from real key packages with a path and the tree carried, then without a path, with two
  external PSKs and the tree fetched, every epoch secret, the tree, the held keys and the interim hash equal to the
  committer's; every refusal of a join, each leaving the group empty (config, decoding, suite, package and init key,
  GroupSecrets and GroupInfo changed or malformed, tag, signer and tree, path secrets, PSKs missing, repeated or of
  usage reinit or branch, a group already joined, the Authentication Service, lifetimes, the leaf key, a key package
  whose leaf is not the tree's, an external sender refused, an unsupported GroupContext extension, and a hostile tree
  whose root key is a small-order X25519 point under valid parent hashes and signatures); a group's creation;
  section 7.3's leaf checks in order, 13.3's credential types, 10.1's key package checks and KeyPackageRef; extension
  lists (pairwise and bitmap), capabilities, required capabilities and external senders. The welcome and key-schedule
  vectors also check `welcome_secret`, `welcome_key` and `take_group_context`; every Wycheproof X25519 and ECDH vector
  checks `check_agreement_public_key` against the agreement's verdict.
- The first run of the welcome vectors found that the working group's suite-7 key package does not list 0x0007 in its
  capabilities; the check that a leaf lists the group's version and suite, which RFC 9420 does not ask, was removed
  (decision M8).
- win-debug (MSVC): the MLS, HPKE and provider cases (76 cases, 463,070 assertions) and the group corpus; no warning.
- linux-gcc-asan: the same 76 cases, the four MLS corpora and 4,000 seeded random mutants of the group corpus, clean
  under AddressSanitizer and UBSan.
- `crd-fuzz-crypto-mls-group`: 24 seeds written by `--seed` and committed (a second `--seed` run writes identical
  files); 4,000 seeded random mutants replayed clean on linux-gcc-debug and under ASan.
- Twenty source mutations in `mls_group.cpp`, each built and run against the MLS cases and the group corpus; nineteen
  caught after two tests were added (a key package whose leaf shares only its encryption key with the tree's, and an
  external sender the Authentication Service refuses); one was equivalent: the joiner's own check of section 13.4 is
  check_leaf on its own leaf, which is its package's, so the separate check was removed.
- `tidy-files.py` (LLVM 20.1.0 on the lane) clean on the seventeen changed C++ sources after two fixes (a local
  constant's name, a loop as `std::ranges::any_of`).
- The CTest guards with the crypto corpora and register tests (`^crd-no-|^crd-check|^crd-diag-coverage$|
  ^crd-diag-seeded-faults$|crypto.*corpus|register`, ISA isolation excluded): 197 of 197.

## Next

MLS's layer 9 in M3's order: proposals (section 12.2's validation, 12.3's application), commits (12.4.1 and 12.4.2),
external joins (12.4.3.2), proven against `passive-client-handling-commit.json` and `passive-client-random.json`.
