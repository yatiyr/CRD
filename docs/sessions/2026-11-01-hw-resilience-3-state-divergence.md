# 2026-11-01 — HW-RESILIENCE.3: state-hash ledgers and the divergence check

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.3](../ROADMAP.md#slice-hw-resilience.3). Before:
[the CUDA device check](2026-11-01-hw-resilience-3-cuda-device-check.md).

## CI read

- No completed run has a failed job; push runs up to 41308959544 were cancelled by concurrency.
- Complete-tier run 41295647725 (`43963cb2`, in progress) failed linux-gcc-arm64-asan and win-asan 1 of 2 on the
  three Shewchuk oracle cases: ASan's stack-buffer-overflow in `fast_expansion_sum_zeroelim`'s look-ahead read, which
  a0c9d534 fixed on main. Every queued complete-tier run (41298463221 at `13453acb`, 41302856153 at `92af745a`)
  predates that fix, so [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) need a
  green complete-tier run at a0c9d534 or later; three are queued or running, so none was dispatched.
- Actions names: the three `FTL_*` secrets are present (the Firebase lane is the CI rows'); no `AMD_CLOUD_*` or
  `MAC_CLOUD_*`, so those lanes wait on their accounts.

## What landed

`crd/perf/state_hash.hpp` in crd-perf, registered as `diagnostics.state-divergence` (owner perf):

- `StateHashLedger`: one participant's per-step hashes, the state's `xxh3_64` (seed 0) or the caller's u64, steps
  strictly increasing. It keeps the last `capacity` steps (default 65,536, at most 2^22) and the chain
  `chain_n = xxh3_64(LE64(chain_{n-1}) ++ LE64(step) ++ LE64(hash))` from 0 over every step, with the chain before
  its oldest kept step, so memory stays bounded for any session length.
- The `CSL1` payload (name, capacity, first step, steps recorded, base chain, kept steps). The decoder refuses a wrong
  magic, version or flags, a name or count over its bound, counts that disagree (all steps kept with a non-zero base,
  fewer kept than a full window), steps out of order and trailing bytes; it rebuilds the chain.
- `StateDivergenceMonitor`: participants are added with their ledger and hardware identity. `update()` walks each
  ledger from where it stopped and judges each step once every participant has reached it. A participant whose next
  step is later did not record the step, which is itself a divergence. Steps a ledger no longer keeps are walked
  without a judgement (`skipped`), and its base chain carries their history. The chains are compared when every
  ledger starts at one step: hashes that agree with chains that do not mean "diverged at or before" this step.
- The verdict latches at the first disagreement. `diverged` against the reference (a recorded run) or a hash held by
  more than half of three or more participants; `no-majority`; `undecided` for two without a reference, since
  either could be wrong.
- The cause: `machine` when an agreeing machine has the diverging one's processor (vendor, signature and model), so
  that machine is the likely cause; `processor-difference` when no agreeing machine has any diverging machine's
  processor, which points at code whose results depend on the processor; `unknown` otherwise. Each diverging
  machine's advisory and microcode state come from `classify_fault`, not a second copy of the table.
- `state_divergence_message` names the step, each diverging participant with its processor, microcode and board,
  the values, the cause, `ceridc doctor --hardware` for the machine and the BIOS fix for an advisory before its fix.
- `append_hex` moved from fault_classification.cpp into the shared `json_writer.hpp`, which both now use.

## Tests and evidence

`test_state_hash.cpp` in crd-perf-tests, 9 cases:

- The hash and chain bits against `xxh3_64` and a hand-written little-endian buffer.
- The bounded ledger: refusals, window, `find`, base chain and chain against a fold.
- The payload: round trip at two capacities, every prefix refused, a trailing byte, and each refusal by name.
- Online majority: each step is judged only when the lagging participant reaches it, gamma diverges at step 7 and
  the cause is `processor-difference`.
- `machine`: two machines with the same processor, one diverging, its microcode before the fix in the message. An
  unrecorded machine gives `unknown`.
- A reference against `undecided`, and `no-majority`.
- A missing step.
- A divergence at step 3 that no window keeps, found at step 7 through the chains. A wide window walks six steps
  without a judgement. Different first steps compare hashes only.
- Decoded ledgers judged as live ones.

Teeth: comparing hashes without the chains fails the window case, and a monitor without its floor fails the wide
window case; restored, rebuilt, rerun. Passed on win-debug and on linux-gcc-debug through the WSL mirror. The ctest
guards passed on the third run; each of the first two lost a different Python guard (capability-owners failed with
no output kept, isa-isolation ended on a segfault) and check-master-plan once raised an impossible `ord()`
TypeError, each clean when rerun: this host's known misexecution of native Python validators, not a finding.

## Left

The ledger as a bundle section that flight bundles carry and `bundle.inspect` reports, and a diag command that
judges the ledgers of several bundles against a recorded run or the majority.
