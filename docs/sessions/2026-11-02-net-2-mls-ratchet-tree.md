# NET.2: MLS ratchet tree and TreeKEM

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 53; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layer 7 and M7](../design/network-foundation.md#mls).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with its seventh layer.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lanes and the worktree's win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

No completed hosted run has a failure this track has not fixed. Push run 41384275913 (1ff4c37d), the first hosted
run of the capability, early-data and message-schema code, is still running: 23 of its 30 jobs passed (win-tidy,
win-vs, win-clang-cl-shipping, both ARM64 pairs, linux-gcc-asan, linux-gcc-shipping among them), none failed. Push
run 41399997646 (45f3ecde) is pending; no hosted run contains 81e99526 or later yet, so the previous batch's audit
carries forward.

## What landed

- **`mls_tree.hpp`, M3 layer 7.** The ratchet_tree extension decoded (each node of its position's type, the last one
  present, extended with blanks; at most 2^20 leaves) and written without trailing blanks; resolutions and filtered
  direct paths; tree hashes held per node with two occupancy bits and recomputed along the path a change touches;
  parent hashes, the original sibling tree hash computed with the parent's unmerged leaves blanked and dropped; the
  join-time checks (unmerged lists, unique encryption and signature keys, LeafNodeTBS signatures, every non-blank
  parent parent-hash valid relative to exactly one descendant); Add, Update and Remove with doubling and truncation;
  the UpdatePath merge with its section 12.4.2 checks before anything changes; TreeKEM's committer side
  (`create_update_path`), receiver side (`process_update_path`, each derived public key checked against the path's)
  and a Welcome's path secret (`PathKeys::implant`).
- **Decision M7**: tree state and staged keys. A refusal leaves a tree as it was; ProviderFailure empties it. A
  commit's keys wait in a `PathUpdate` until `PathKeys::apply`, so a commit refused at its confirmation tag changes no
  key; `prune` drops keys of nodes a proposal blanked.
- `Commit`'s path is now an `UpdatePath` with its own codec; the leaf node's signed fields, Appendix C's children and
  `copy_bytes` moved into `mls_detail.hpp`, shared by the three MLS units instead of repeated.
- **A new fuzz target `crd-fuzz-crypto-mls-tree`** (20 seeds), registered in `fuzz.py`, its tool test and
  [test instruments](../design/test-instruments.md).
- Fixed on the way: three wire-codepoint enums in `mls_message.hpp` that LLVM-20 tidy flags when it analyses the
  header in its module (`performance-enum-size`): each keeps the 16-bit registry width with a stated suppression.

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 202 cases, 1,145,748 assertions, all passing; the 10 new cases in
  `test_mls_tree.cpp`: the 56 tree-validation vectors of the profile (every node's resolution and tree hash, the
  join-time checks), the 5 tree-operations vectors (two adds, one growing; an update; two removes, one truncating),
  the 44 treekem vectors (each member's state checked against the tree, every update path merged to its tree hash and
  decrypted by every other member to its path secret and commit secret, then written again by its sender and
  decrypted by the others); model groups in each suite (160 commits in 0x0001, 80 in each other) that add, update,
  remove and commit at random, every member's tree, commit secret and held keys compared with TreeKEM's invariant at
  every epoch, every joiner validating the tree and implanting its path secret; doubling, truncation and trailing
  blanks; every refusal of the decoder, the join-time checks, the merge, the decryption, the tree changes and the
  path keys, each leaving the tree unchanged.
- win-debug (MSVC): the MLS cases (35 cases, 350,031 assertions) and the tree corpus; no warning.
- linux-gcc-asan: the MLS, HPKE and provider cases (39 cases, 350,702 assertions), the three MLS corpora and 4,000
  seeded random mutants of the tree corpus, clean under AddressSanitizer and UBSan. Its first run found a real
  use-after-free: `create_update_path` wrote the path's leaf through a template that viewed the sender's leaf in the
  tree it had just replaced; the path now writes the leaf as the tree holds it, and the header says the template may
  view the tree. It also found a test holding a view of a temporary.
- `crd-fuzz-crypto-mls-tree`: 20 seeds written by `--seed` and committed (a second `--seed` run writes identical
  files); 4,000 seeded random mutants replayed clean on linux-gcc-debug and under ASan.
- Sixteen source mutations in `mls_tree.cpp`, each built and run against the MLS cases and the tree corpus; fifteen
  caught: the original sibling hash left unadjusted, the filtered direct path unfiltered, resolutions without
  unmerged leaves, Add listing no unmerged leaf, Remove not truncating, LeafNodeTBS with another index, the merge
  skipping the leaf's parent hash, derived keys unchecked against the path, the commit secret from the first path
  secret, path keys held elsewhere accepted, intermediate nodes not listing an unmerged leaf, a chain claiming no
  parent hash, hashes not recomputed up to the root, `apply` keeping replaced levels (caught after a test of `apply`
  was added), and the decoder accepting a trailing blank. One is equivalent: parent-hash validity relative to "at
  least one" instead of "exactly one" child, since a parent valid through both children needs each child's parent
  hash to cover the other's tree hash, a hash cycle no input reaches; the code keeps RFC 9420's "exactly one". The
  check that a chain starts only at a commit leaf proved redundant (any other leaf carries no parent hash) and was
  removed.
- `tidy-files.py` (LLVM 20.1.0 on the lane) clean on the nine changed C++ sources after the fixes above and a nested
  conditional, a constant's name and a multiple declaration in the new code.
- The CTest guards with the fuzz corpora and register tests (`^crd-no-|^crd-check|^crd-diag-coverage$|
  ^crd-diag-seeded-faults$|corpus|register`, ISA isolation excluded): 257 of 257.

## Next

MLS's next layer in M3's order: key packages' and leaf nodes' validation against the group, proposals and commits,
proven against the rest of `messages.json` and `welcome.json`.
