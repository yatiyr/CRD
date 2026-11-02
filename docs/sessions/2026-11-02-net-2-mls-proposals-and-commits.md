# NET.2: MLS proposals and commits

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 55; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layer 9 and decision
M10](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with its ninth layer.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Push run 41384275913 (1ff4c37d) still has one failed job, win-release, whose cause
(`crd-perf-kernels-at-avx512-sde-skx`, no pinned CPU under SDE) is not this track's and fb8452af fixed; its win-debug
job is still running. Dispatch run 41409186119 (fb8452af, complete tier) is the first hosted run with MLS through
TreeKEM (80e95b01): 16 of its 30 jobs have passed, none failed (win-clang-cl, win-clang-cl-shipping, win-vs,
win-arm64-debug, the four ARM64 Linux lanes, linux-gcc-shipping, linux-gcc-asan 1/2, linux-gcc-release,
linux-gcc-relwithdebinfo, linux-gcc-debug-scalar and both repository checks among them). Push run 41415861936
(6ad873b6) was cancelled, so no completed hosted run contains 1bc8a676 (layer 8) or later yet.

## What landed

- **`mls_group.hpp` and `mls_commit.cpp`, M3 layer 9.** `Group::process`: a public or private message of the epoch
  (the membership tag, or the PrivateMessage opened in place), the sender's key (a member's leaf, an external sender
  the group lists, a new member's key package), the signature; a proposal cached under its ProposalRef when its
  sender may send its type; application data read; a commit's list checked (section 12.2), applied in section 12.3's
  order to a copy of the tree with each proposal checked against the new extensions (12.1), its path checked, merged
  and decrypted under the provisional GroupContext, the new tree's keys and credential types, the key schedule with
  the PSKs, the confirmation tag: a `StagedCommit` that `Group::merge` makes the group's epoch. The committer's side:
  `Group::commit` (the cached proposals section 12.2 prefers and its own by value, a path when required or asked for,
  the GroupInfo signed, the Welcome with each joiner's path secret and every PSK), `propose`, `propose_update` (its
  key kept until a commit adopts the leaf) and `protect`.
- **Decision M10**: staged commits merged in constant work; a refused message changes nothing but a PrivateMessage's
  spent generation; per group, at most 256 cached proposals per epoch (Exhausted past it) and the resumption PSKs of
  32 past epochs; the member's own commit echoed back is Replayed; ReInit and external commits refused as Unsupported
  until layer 10.
- Supporting changes: `EpochSecrets::take`, `PathUpdate::clear`, `PathKeys::replace_leaf`, `process_update_path`'s
  leaf key for an own Update a commit applied, `RatchetTree::check_unique_keys` public; `check_psk_id`,
  `supports_context`, `private_content_bytes`, `view`, `copy_into`, `fresh_buffer` and `Secret` shared through
  `mls_detail.hpp` instead of repeated; the test identities, joiners, Authentication Service and store moved into
  `mls_test_support.hpp`; the group fuzz targets' reader, member and key package into `mls_fuzz_support.hpp`.
- **A new fuzz target `crd-fuzz-crypto-mls-commit`** (45 seeds), registered in `fuzz.py`, its tool test and
  [test instruments](../design/test-instruments.md).

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 221 cases, 1,193,874 assertions, all passing; the 8 new cases in `test_mls_commit.cpp`: all 52
  passive-client-handling-commit vectors of the profile (104 epochs) and the passive-client-random vector (200
  epochs), each epoch's authenticator reached, every proposal Replayed when sent again and every commit OutOfWindow
  in the next epoch; Cerid groups in every suite as public and private messages (four joiners from one Welcome, an
  Update adopted, a member processing its own removal, an external and a resumption PSK, an empty and a partial
  commit with the tree fetched, required_capabilities, application data each way and replayed); two external senders
  the group lists and a new member proposing (a Remove, Adds and a PSK, cached by every member and committed by
  reference; a sender index past the list, the other sender's key and a new member signing with another key refused);
  every refusal of
  sections 6, 12.1, 12.2 and 12.4.2 with the group unchanged and still taking a valid commit, the list rules again
  under a real path; the sending, committing and merging refusals; the proposal bound and the resumption ring.
- The vectors' members already in the group hold key packages living from March 2024 to March 2025: every lifetime is
  checked at a time inside them.
- win-debug (MSVC): the MLS and HPKE cases (58 cases, 392,167 assertions) and the commit and group corpora; no warning.
- linux-gcc-asan: the same cases, the five MLS corpora and 4,000 seeded random mutants of the commit corpus, clean
  under AddressSanitizer and UBSan; the mutants clean on linux-gcc-debug too.
- `crd-fuzz-crypto-mls-commit`: 45 seeds written by `--seed` and committed (a second `--seed` run writes identical
  files).
- Twenty-five source mutations of `mls_commit.cpp`, each built and run against the MLS cases and the commit corpus:
  all caught, five only after the list-rule case under a real path was added (the committer's own Update, two
  GroupContextExtensions, an Update keeping its key, the committer's own Update and a repeated Remove left out of a
  commit's selection), two by the external-sender case (the sender list's bound, a new member's key).
- The review before handover found that a commit's selection deduplicated Adds' signature keys and PSKs' encodings in
  one sorted list, so a key and an encoding of the same bytes would have collided; each is now keyed by its type too.
- `tidy-files.py` (LLVM 20.1.0 on the lane) clean on the fourteen changed C++ files after three fixes (an unused
  using-declaration, a local named after a keyword, a nested conditional).
- The CTest guards with the crypto corpora and register tests (`^crd-no-|^crd-check|^crd-diag-coverage$|
  ^crd-diag-seeded-faults$|crypto.*corpus|register`, ISA isolation excluded): 198 of 198 after one rerun;
  `crd-check-capability-owners` (a Python check) died once with SIGSEGV and passed when run again, this host's known
  misexecution (MEMORY: host misexecutes), not a code defect.

## Next

M3 layer 10: external joins (section 12.4.3.2: the external_pub extension, ExternalInit, the joiner's path), ReInit
and subgroup branching (sections 11.2 and 11.3) and GREASE (13.5).
