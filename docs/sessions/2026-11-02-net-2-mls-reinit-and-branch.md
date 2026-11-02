# NET.2: MLS reinitialization and subgroup branching

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 57; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contract: [MLS, M3 layer 10b and decision
M12](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with ReInit and branching.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

Nothing from this track to fix. Dispatch run 41409186119 (fb8452af, complete tier, MLS through TreeKEM) has 28 of 30
jobs passed and none failed; win-asan 1/2 and linux-clang-tsan are still running. Scheduled run 41429790527
(4f30bb98, the first with layers 8 to 9) has all 30 jobs queued, and push run 41439561058 (396de664) is pending, so
no completed hosted run contains 1bc8a676 or later yet.

## What landed

- **ReInit** (RFC 9420 sections 12.1.5, 12.2 and 12.4.2): `propose` sends one; a commit holds a ReInit only alone, and
  its version must be no older than the group's and its extensions named once. A member's default list takes the latest
  valid cached ReInit only when nothing else is committed. Merging a ReInit's commit ends the group (`Group::ended`,
  `reinit_proposal`): it keeps its last epoch's secrets, sends nothing (NotReady) and takes no message (OutOfWindow),
  and a ReInit's commit never publishes the external key.
- **The new group** (sections 11.2 and 11.3): `Group::reinit` (from a group a ReInit ended, by any of its members) and
  `Group::branch` (a subgroup of a live group) create the group and merge its first commit at once: an Add per member,
  the PSK of usage reinit or branch naming the prior group's current epoch under a fresh Nh-byte nonce, with the prior
  group's resumption secret, and a path. The creator checks its new group as its joiners will.
- **Joining it** (section 12.4.3.1): `open_welcome` and `Group::join` take the prior group (`prior`). A reinit or
  branch PSK is accepted once and only from that group at its current epoch; the joiner then checks epoch 1, the
  ReInit's group id, version, suite and extensions (reinit) or the prior group's suite (branch), and the members.
  "According to the application" is the Authentication Service's `valid_successor`: each new member matches a distinct
  prior member, and for a reinit every prior member is matched. A reinitialization may keep the group id; the store's
  repeat check then spares it.
- A reinit or branch PSK in any other commit or Welcome is refused (Malformed in a commit, NotReady in a Welcome that
  names no prior group holding it), as section 12.1.4 says; before this batch both were Unsupported.
- The test suites' hand-made committer (`commit_adds`) moved from `test_mls_group.cpp` into `mls_committer.hpp`, so
  `test_mls_resume.cpp` builds a hostile creator from it.
- `crd-fuzz-crypto-mls-commit` gains two modes: the honest ReInit commit changed in any byte, and the resumed Welcome
  changed in any byte, joined with the receiver's ended group as the prior group. It now has 97 seeds (28 new).

## Evidence (this PC)

- No working-group vector holds a ReInit commit or a resumed Welcome: the passive-client vectors processed every
  commit and Welcome to completion while ReInit and reinit or branch PSKs were refused. So creators and joiners are held
  to each other and to a hand-made creator, and live OpenMLS and MLSpp interop (M4) is the outside check.
- linux-gcc-debug: `crd-crypto-tests` 233 cases, 1,218,910 assertions, all passing. The 5 new cases:
  - reinitialization in every suite into the next suite with new extensions, the id changed or kept, created by another
    member than the committer; the new group exchanging data and committing;
  - the ReInit selection rules, an external sender's ReInit among them, and a cached branch PSK never committed;
  - subgroups branched from a group that goes on, and one from a group a ReInit ended;
  - 13 creator refusals and 19 joiner refusals, each left empty.
- win-debug (MSVC): the MLS and HPKE cases (70 cases, 417,204 assertions); no warning.
- linux-gcc-asan: the MLS and HPKE cases (417,204 assertions), the commit and group corpora, and 4,000 seeded mutants of the 97 commit seeds,
  clean under AddressSanitizer and UBSan. The same mutants are clean on linux-gcc-debug.
- Thirty-five source mutations, each built and run against the MLS cases and the commit corpus and mutants:
  - thirty-four caught. Three of them first survived, and each got a test: the ReInit's version at the joiner, the
    prior group's id for a reinit key, and a cached ReInit beside a by-value proposal;
  - the thirty-fifth showed that the resumed PSK's identity check could not fail (only `resume` supplies that PSK), so
    the check was removed.
- `tidy-files.py` (LLVM 20.1.0 on the lane) on the ten changed C++ files: clean.
- CTest guards with the crypto corpora (`crd-fuzz-crypto|^crd-no-|^crd-check|^crd-diag-coverage$|
  ^crd-diag-seeded-faults$`): 30 of 30, run again after the last code change.

## Next

M3 layer 10c: GREASE (section 13.5) and the post-quantum suites, with live OpenMLS and MLSpp interop (M4).
