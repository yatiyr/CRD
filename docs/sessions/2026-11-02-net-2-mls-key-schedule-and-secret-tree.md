# NET.2: MLS's key schedule and secret tree

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-02. Row: [NET.2](../ROADMAP.md#slice-net.2) (track C, batch 51; the
[parallel tracks](2026-10-31-parallel-tracks.md)). Contracts: [MLS, M3 layers 4 and 5 and
M5](../design/network-foundation.md#mls); [WSL source mirror](../design/build-performance.md#wsl-source-mirror).

## The user's decisions this batch acts on

- 2026-11-01, evening: "I WANT NETWORK PARTS TO BE COMPLETELY DONE AND TESTED! I WANT FULL AND SECURE AND GOLD
  STANDARD NETWORK MODULE!" This batch continues NET.2's first Remaining clause, MLS, with its next two layers.
- 2026-11-01, night: "DO EVERYTHING TO REDUCE THE COSTS ... SAVE MY MONEY!" Everything ran on this PC (the track's
  WSL lane and the win-debug tree); no cloud machine ran (0 hours, 0 USD).

## CI first

No completed hosted run has a failure this track has not fixed; every failed job was read again:

- Push run 41358455047 (33ac38d4), ten failed jobs. linux-gcc-release and linux-gcc-relwithdebinfo: GCC 13's link-time
  `-Wnull-dereference` at quic.cpp:271, fixed by b5160de6. win-release, win-relwithdebinfo, win-shipping,
  win-shipping-profile and win-clang-cl-shipping: `crd-check-isa-isolation` and `crd-check-compile-commands`, fixed
  by b37a4177. win-tidy (the QUIC sources): 964487d5; linux-clang-tsan (AWS-LC's dispatch-test instrumentation):
  3a59a528; linux-gcc-shipping (the same `-Wnull-dereference`): b5160de6.
  win-relwithdebinfo also fails `crd-ci-orchestrator-e2e` (an HTTP read timing out in the cancellation case of
  `test-ci-orchestrator-e2e.py`): the CI platform's own test, outside this track, as recorded in batch 49.
- Complete-tier run 41342960677 (59a3347e), eight failed jobs. linux-gcc-debug: the certificate-compression case
  (`3 == 1`, zstd against zlib for an ECDSA certificate); 6e5dbb36 changed that row to accept either form after
  this run's commit (it is not an ancestor of 59a3347e), and the choice of the smaller form stays exact in the
  Ed25519 row and the next case. The other seven: the ISA-isolation, compile-commands and TSan causes above.
- Push runs 41384275913 (1ff4c37d) and 41389014829 (8845f1d9) are queued and pending; no hosted run contains
  0dd2b5ab or later yet.

## What landed

- **The key schedule** (`mls.hpp`, M3 layer 4): the GroupContext's encoding; `EpochSecrets::advance` (a member's next
  epoch from the previous init secret, the commit secret and the PSK secret, safe to call with its own init secret)
  and `join` (a joiner's from the Welcome's joiner secret), holding the eleven secrets that outlive the derivation
  and zeroing the member and epoch secrets inside it; `psk_secret` over external and resumption PreSharedKeyIDs and
  their PSKLabels; MLS-Exporter.
- **The secret tree** (M3 layer 5): the sender data keys and `SecretTree`, which derives each parent's two children
  the first time a leaf below them is needed, zeroes every secret once what it derives is held (section 9.2), and
  answers within **decision M5**'s window: OpenMLS's 5 generations behind and 1000 ahead, settable per tree; a key is
  read without changing the tree and spent only after its message authenticated, so a forged message spends
  nothing; generation g lives in slot g mod the window, so a spent generation is Replayed and a dropped or too
  distant one OutOfWindow (a new `CryptoError`) in constant time; one allocation per epoch, nothing on the message
  path, at most 2^20 leaves; a sender's ratchet is Exhausted past 2^32 - 1.
- **Tree math** without unchecked optionals: Appendix C's formulas for nodes already inside a tree are file-local
  helpers that the public functions and the secret tree share (the LLVM 20.1.0 tidy on the lane flagged the
  optional dereferences, including two in `direct_path` and `copath` from batch 50).
- **The WSL source mirror** times a copied file at the copy. An edit made while a lane build ran kept the checkout's
  time, older than the object that build had just compiled from the previous bytes, and Ninja kept the object: the
  fuzz target ran without its new seeds. A sync now copies only paths whose bytes differ and times them at the copy;
  a new `SourceMirror` case reproduces the race and fails on the old script.

## Evidence (this PC)

- linux-gcc-debug: `crd-crypto-tests` 181 cases, 863,428 or 863,429 assertions (the randomized cases vary), all
  passing; the 7 new cases (`test_mls_schedule.cpp`) with the HPKE and MLS cases 19 cases, 73,269 assertions: 20
  key-schedule epochs (four suites, five each; three suites outside the profile skipped), 44 psk_secret vectors, 12
  secret-tree vectors (164 leaves, each ratchet in three trees), the window, the one-leaf tree, the empty window and
  every refusal.
- win-debug (MSVC): `crd-crypto-tests` 181 cases, 863,428 assertions, and the 22-seed corpus,
  all passing.
- `crd-fuzz-crypto-mls`: the secret-tree mode (mode 6) against a model of the window and a restatement of section 9;
  22 seeds written by `--seed` (5 new) and committed, replayed clean, and 4,000 seeded random mutants of them clean.
- `tidy-files.py` (LLVM 20.1.0 on the lane) clean on the five changed C++ sources, after the optional dereferences in
  the tree code were replaced by Appendix C's formulas for nodes known to lie inside the tree.
- Thirteen source mutations in `mls.cpp`, each built and caught by the MLS cases (seven by the committed corpus too):
  left and right swapped when a path opens, the window one shorter, the forward bound one shorter, a late key not
  spent, a skipped key derived at the next generation, the ratchet stepped with the next generation, the sender data
  sample taken from the whole ciphertext, the member secret extracted the other way round, every PSKLabel at index 0,
  the exporter context unhashed, a spent generation read again, the GroupContext without its version, and a sent
  generation not spent.
- `test-repository-tools.py SourceMirror`: 2 cases pass; the new race case fails on the previous script (its copy's
  time stays four seconds older than the simulated object).
- After the mutations, the lane rebuilt the restored `mls.cpp` without a touch (the mirror's new timing): the crypto
  suite, the corpus, the register drift tests and `crd-fuzz-crypto-mls`'s CTest (4 of 4), and the CTest guards
  (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, ISA isolation excluded) 19 of 19 pass.

## Next

MLS's next layers in M3's order: transcript hashes with message framing and protection (with padding), proven
together against `transcript-hashes.json` and `message-protection.json`, then the ratchet tree.
