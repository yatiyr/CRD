# 2026-11-01 — HW-RESILIENCE.3: the ledger in flight bundles and state.divergence

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3), now Done. Before:
[the state-hash ledgers](2026-11-01-hw-resilience-3-state-divergence.md).

## CI read

- No completed run has a failed job; push runs 41310778561 to 41311748202 were cancelled by concurrency.
- Complete-tier run 41295647725 (`43963cb2`, in progress) failed linux-gcc-arm64-asan, linux-clang-arm64-asan,
  linux-gcc-asan 2 of 2 and win-asan 1 of 2 on the Shewchuk oracle cases (fixed by a0c9d534); TP-5's mikktspace
  case is the registered failure. Run 41311293265 (`dac5d27e`, queued) is the first complete-tier run that contains
  a0c9d534, so it is the one to audit for [DIAG.11c](../ROADMAP.md#slice-diag.11c) and
  [DIAG.12](../ROADMAP.md#slice-diag.12); with three queued or running, none was dispatched.
- Actions names: the three `FTL_*` secrets; no `AMD_CLOUD_*` or `MAC_CLOUD_*`.

## Windows configure after AWS-LC

01159147 broke every Windows x64 configure, fresh or incremental: the generate step failed with
`CMAKE_ASM_MASM_COMPILE_OBJECT` (28 times) and `CMAKE_ASM_COMPILE_OBJECT` (twice) not set. CMake gives an extension
to the assembler language enabled last, in every directory: zstd's ASM claims `.asm`, `.s` and `.S`, crd-jobs'
ASM_MASM `.asm`, AWS-LC's ASM_NASM `.asm`. crd-jobs enables MASM after AWS-LC enables NASM, so AWS-LC's 28 `.asm`
sources became MASM in directories that only enabled NASM, and `hrss/asm/poly_rq_mul.S` became zstd's ASM. The fix
names each language explicitly: AWS-LC's `.asm` sources are ASM_NASM, its `.S`/`.s` sources (Linux-only code, never
assembled by AWS-LC's own MSVC build, where NASM claims neither extension) are HEADER_FILE_ONLY, and crd-jobs'
context switch is ASM_MASM. A fresh configure then succeeds, the ninja rules show NASM for AWS-LC and MASM for
crd-jobs, and crd-crypto-tests pass (71,244 assertions).

## What landed

- `BundleSectionTag::StateHashLedger` (9) carries a process's ledger. `FlightCommandHost::attach_state_ledger` names
  it; `state_ledger` (flight.write) encodes it waiting, `try_state_ledger` (a forward sink, the fatal path included)
  tries the attachment lock (the module snapshot's, now `SnapshotLock`) and the ledger's own lock a bounded number of
  times. `write_flight_bundle` takes a `FlightStateLedger`: none attached, no section; attached but unread, an
  explicit absent section the manifest declares.
- The ledger holds a lock word that `record_hash` and the encoders take (`try_encode_state_hash_ledger` only tries
  it); moves take the steps, never the lock. `kMaxStateLedgerSteps` is 2^21, and a static assertion holds a full
  ledger's payload inside one bundle section (2^22 steps would have been stored truncated and refused).
- The importer decodes the section (`has_state_ledger`, `state_ledger_refused`, `state_ledger_error`);
  `bundle.inspect` reports them and a `state-ledger` item (name, capacity, first and recorded steps, kept window,
  last hash, base chain and chain).
- `state.divergence` (Read, a directory path, `reference=<ledger name>`) judges the `*.cdb` bundles of a directory
  under the root in name order, each ledger with its own bundle's hardware identity: summary verdict, step, cause
  and sentence; one item per participant with its value at the step, one per bundle that takes no part with why.
  At most 64 bundles, together within `max_bundle_bytes`, measured before any read, each read through crd-platform's
  ranged read (the I/O fault seam sees it). The command fuzzer knows its argument check and has a seed.

## Tests and evidence

- crd-perf-tests (`[state-hash]`, 12 cases): the bundle carries, declares absent or omits the ledger, and a damaged
  one is refused with its reason; a recording thread and a reader of both encoders (300 reads after the loop passed
  its window, overlapping it) see only whole ledgers; `state.divergence` over six bundles (three ledgers, gamma
  diverging at step 7, plus one without a ledger, one declared absent and one not a bundle) by majority and by
  reference, its refusals (an unknown reference, another argument, a file path, bundles past the limit, read 0 bytes)
  and `bundle.inspect`'s ledger item and absent section.
- Teeth: without the lock in `record_hash` every read was torn (299 and 300 of 300 in three runs); without the
  manifest's absent tag and without `set_reference` the absent and reference checks failed; restored, rebuilt, rerun.
- win-debug: crd-perf-tests (368 cases), the ceridc CTests and the command fuzzer's 58-input corpus pass;
  linux-gcc-debug (WSL mirror) passes the state-hash, command and flight cases.

## Row

Every clause of HW-RESILIENCE.3 now holds: kernel and device known-answer checks with their fallbacks and reports,
`ceridc doctor --hardware`, and state hashes that detect a divergence and name the machine, live and from bundles.
The wire exchange stays NET.3's and 4.2c's. Next in this track: [HW-RESILIENCE.4](../ROADMAP.md#slice-hw-resilience.4).
