# Sequential acceptance audit of the parked REPO.DEV rows

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.DEV.2](../ROADMAP.md#slice-repo.dev.2) through [REPO.DEV.3b.2](../ROADMAP.md#slice-repo.dev.3b.2).
> Rules: [AGENTS](../../AGENTS.md). Preceding batches: [guard/tidy repairs](2026-09-13-ci-guard-tidy-repairs.md),
> [route and pinned WARP](2026-09-13-inner-coverage-route-and-pinned-warp.md).

The [strict-order amendment](2026-09-12-strict-slice-order.md#preserved-later-work) moved these rows from Done or
In progress to Partial as a sequencing hold, with their implementation and evidence intact, to be audited at their
turn. Their turn arrived once every REPO.3c child holds only a published-CI wait. This audit inspects each contract
against the current source and documents, reuses proof that is still valid and runs the checks the hold left open.
Same user assignment as the preceding batches; no commit or push.

## Evidence shared by every row

- Hosted revision `0b858a6` ([run 34726528230](https://github.com/yatiyr/CRD/actions/runs/34726528230)): both repository
  jobs passed every step, including the tooling regression checks, the affected-selection contracts, project
  synchronization, canonical profile contracts, the native MSVC profile compile and execution, the native Visual
  Studio round trips and the Linux projection/compile.
- Local today, Windows Python 3.14: `test-dev-workflow.py` **48/48**, `test-project-sync.py` **52/52**,
  `test-native-build-profiles.py` **7/7**, `test-repository-tools.py` **16/16**.
- `python scripts/dev.py doctor --build build/win-debug` resolves CMake 4.x, MSVC 14.51, Ninja, Python, the pinned
  clang-tidy 20.1.8, eleven eligible presets, RAM/disk budgets and the process-local environment, executing nothing.
- `python scripts/dev.py plan --build build/win-debug` on today's working tree lists every changed source, header,
  script and CMake file, broadens conservatively where the configured model cannot prove ownership ("configure model
  first" instead of an empty green selection), names the tidy set, both repository guards and four risk gates, and
  refuses to launch a local sweep. That is the contracted fallback behaviour, not a defect.

## Row by row

**REPO.DEV.2 — compact instructions.** [BUILDING](../BUILDING.md) carries the one-primary-configuration workflow, the
risk-triggered decision table, the CI obligations and the "never sweep locally" rule; [AGENTS](../../AGENTS.md) carries
the scoped verification, human-only commit/push and CI-wait rules; [START_HERE](../../START_HERE.md) routes both. The
research [contract table](../research/2026-09-12-large-cpp-development-and-ci.md#local-development-contract) and the
BUILDING table agree row for row. Today's BUILDING edits stayed within the 7,000-byte orientation budget and the
validator passes. Accepted: Done.

**REPO.DEV.3a — conservative selector.** The [selection session](2026-09-12-developer-selection.md) records opaque-ID
resolution, propagated interface includes, generated sources, fixtures, disabled tests, utility safety, stale/foreign/
corrupt replies, rename/deletion/untracked handling and empty/guard-only rejection, with tiny real CMake fixtures on
Windows and WSL. The suite has since grown to 48 cases and passes locally and on both hosted repository jobs; today's
`plan` shows the stale-model fallback in the actual repository. Accepted: Done.

**REPO.DEV.3b.1 — configuration-specific discovery.** [CrdTestDiscovery](../../cmake/CrdTestDiscovery.cmake) wraps the
pinned Catch2 3.7.1 integration with PRE_TEST discovery, failing placeholders carrying target/artifact labels for
unbuilt executables and preserved list-valued properties; every test CMake file calls `crd_discover_tests`, and the
[scoped-check session](2026-09-12-scoped-check-and-native-discovery.md#reproduced-discovery-defects-and-repair) records
the reproduced Debug/Release inventory defect and the Windows/Linux fixtures. `test-project-sync.py` and the hosted
native round trips exercise the same generated inventories today. Accepted: Done.

**REPO.DEV.3b.2 — scoped check execution.** The same session's [scoped proof](2026-09-12-scoped-check-and-native-discovery.md#subsequent-scoped-proof)
and [final guard proof](2026-09-12-scoped-check-and-native-discovery.md#final-bounded-guard-proof) record bounded
native-status recovery, source/model identity checks, JUnit count reconciliation, guard ownership and the workspace
lock that serializes overlapping processes; the [status-read repair](2026-09-12-atomic-abuffer-emitter-repair.md) is
linked from the row. The reversible WARP wrapper used throughout today's DX12 work runs inside that same lock and
identity check (`Source changed during qualification` guard) and completed nine times with verified restoration.
Accepted: Done.

## Boundary

The acceptance above is retained proof. Later the same day the whole-suite qualification withdrew the WARP 1.0.20 pin
from CI ([evidence](2026-09-13-inner-coverage-route-and-pinned-warp.md#whole-suite-qualification-and-withdrawal)), so
REPO.3c.10 holds Partial again and the order rule returns these four rows to Partial behind it; nothing in their
implementation or evidence changed. REPO.DEV.3b.3 (portable strict LLVM-20 analysis on Linux and other hosts) remains
the first Open row after them: no implementation exists and none is claimed; REPO.DEV.3b.4 and 3b.5 keep their retained
proof as Partial behind it. The pointer is REPO.3c.10.
