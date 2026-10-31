# 2026-10-31 — NET-STD: the negotiation codec and the crd-net module

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Track C of the [parallel tracks](2026-10-31-parallel-tracks.md), third batch. Row:
[NET-STD](../ROADMAP.md#slice-net-std); previous batch: [standards register](2026-10-31-net-std-standards-register.md).

## CI first

No completed CI run exists for this track's commits yet. GitHub Actions run 41212753727 (8de630f7) was still pending
with no job started when the batch began, and 2762ed4b had not reached the GitHub mirror; every earlier run of this
track was cancelled by a later push before a job finished. This batch's push cancels the pending run again. The own CI
now builds only at operator checkpoints (c2645814, user order), so the next hosted evidence for these commits is the
first GitHub run that is not superseded, or the next checkpoint build.

## What landed

- **Module.** `crd-net` (`engine/foundation/net`, `DEPENDS containers core`, tests `foundation/net` and the fuzz
  harness), the owner decision D1 of the [network foundation](../design/network-foundation.md#d1) assigned. It sits in
  the foundation family beside the planned `crd-crypto`: one module owns every path to another machine, so no new
  family was needed. The OWN.1 registry's `network.transport` moved from planned to landed with its API and two
  signatures (socket and name-resolution calls; socket headers); a probe source in crd-core calling `socket()` was
  refused by `check-capability-owners.py` and removed.
- **Negotiation** ([negotiation.hpp](../../engine/foundation/net/include/crd/net/negotiation.hpp), contract
  [The negotiation messages](../design/network-foundation.md#negotiation-wire)): `write_offer`, `decode_negotiation`,
  `select_negotiation`, `write_answer`, `verify_answer` and `write_transcript`, with `validate_policy` and one error per
  refusal. Decisions taken in the contract:
  1. The answer carries the server's available versions (the RFC 9368 pattern), so the client detects an offer
     stripped in flight (`Downgrade`) and a self-contradicting selection (`InconsistentAnswer`); the transcript bytes
     (`crdneg01`, both messages with u32 lengths) are what NET.2's handshake binds, because the in-band check is only as
     strong as the answer's integrity.
  2. Ignore-unknown holds in both directions; only an extension the client knows and did not offer is refused in an
     answer.
  3. GREASE reserves `0x?A?A` as extension identifiers and as version majors (the register's "Cerid GREASE values"
     registry, its value normalised from prose to the machine-readable pattern). Every message carries one GREASE
     major and two distinct GREASE extensions of 0 to 7 bytes; positions, values and lengths come from 24 bytes of
     caller entropy, so NET-SIM can replay a run and the transport takes them from the CSPRNG. No generator constant
     was written in crd-net (`random.seed-generators` is still OWN.1's).
  4. Decoding is exact and allocation-free: 8 versions, 32 extensions, 1024-byte values, 4096-byte messages.
- **Generated identifiers.** [scripts/gen-register-ids.py](../../scripts/gen-register-ids.py) writes
  [register_ids.hpp](../../engine/foundation/net/include/crd/net/register_ids.hpp) from the standards register:
  `ProtocolId` and `protocol_register_id`, `kGreaseIds` with `is_grease` generated from the pattern, and the QUIC and
  ALPN codepoints, the unverified ones in `iana::unverified`. Codepoints of `crypto` entries wait for NET.2's module;
  a registered owner without an output fails the generator. The header is in
  [generated-sources.json](../../scripts/generated-sources.json) and the `crd-net-register-ids-drift` CTest runs
  `--check`. The [registers contract](../design/registers.md#cerid-identifiers) records it.
- **Register.** `cerid.negotiation` is `implemented` with its tests as evidence.

## Bench machine fixes

Two `ci/bench_vm.py sync` defects surfaced on the first sync of this batch and were fixed with tests in
`scripts/test-ci-platform.py` (97 tests):

- The bundle's ref, `refs/bench/sync`, was one ref for every machine, but the tracks' worktrees share `refs/`, so two
  tracks syncing at once moved or deleted each other's ref (the bundle creation failed with git's exit 128). Each
  machine now syncs through `refs/bench/sync-<machine>`.
- The remote fetch updated the ref without `+`, so a sync after a rebase or an amend was a refused non-fast-forward
  (plink's exit 1; the [optimization track](2026-10-31-simd-perf-bench-machine-fft-f32-profile.md) recorded the
  workaround). The refspec is now forced.

## Verification (track machine, linux-gcc-debug)

- `crd-net-tests`: 17 cases, 123 802 assertions, all passing, among them an exhaustive GREASE-pattern check over all
  65 536 values and 4096 entropy draws covering every GREASE slot, length and value.
- `crd-fuzz-net-negotiation`: 16 seeds written by `--seed` (the corpus is that output) and replayed. Oracles: a
  restatement of the wire layout decides every decode result and field; any accepted offer is answered so that a
  client that could have sent it accepts; the fixed client's verdict on any answer is restated from the contract;
  generated policy pairs run the whole exchange.
- Teeth: seven source mutations (no downgrade check, no ignore-unknown, equal GREASE identifiers, no unsolicited
  refusal, no duplicate refusal, the server's minor instead of the lower, no trailing-byte refusal) each fail the unit
  tests and, after the downgrade, unsolicited and duplicate seeds were added, the fuzz replay as well.
- CTest with `crd-net|crd-fuzz-net-negotiation|^crd-no-|^crd-check`: 20 of 20 passed, among them the include budget,
  the container ban, capability owners, source routes, registers and ISA isolation. Build warnings: 0.
- Strict analysis: `scripts/tidy-files.py` with the pinned clang-tidy 20 (installed by `install-clang-tidy.py`) parsed
  all six changed C++ files clean, after two findings were fixed (a lookup loop became `std::ranges::find`; the
  generated `is_grease` became a pattern test, and the 16-bit `ProtocolId` carries a reasoned `NOLINTNEXTLINE`).
- Python: `test-repository-tools.py` `RegisterIds`, `Registers`, `CapabilityOwners` (17 tests) on Linux and Windows.
- Local validators: `check-master-plan.py`, `check-repository.py`, `check-allman-braces.py`, `check-generated.py`,
  `check-registers.py`, `check-capability-owners.py`, `check-docs.py`.

## Remaining in NET-STD

The algorithm identifiers and the signed, monotonic policy generation mechanism behind the `crd-crypto` verification
interface; the scheduled re-research tooling; the first snapshot refresh from the RFC Editor, the datatracker and
the IANA registries (it also verifies the `iana::unverified` codepoints), which needs the user's approval; and Done
once NET.1 is Done (it waits on OWN, track B).
