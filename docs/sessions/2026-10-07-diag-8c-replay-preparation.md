# DIAG.8c replay preparation, 2026-10-07

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.8c](../ROADMAP.md#slice-diag.8c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-8c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [program inspection under Execute](2026-10-07-diag-8c-program-inspect-under-execute.md).

## Goal

The fifth DIAG.8c batch adds the row's replay-preparation command: for an authored program, what a replay of a run
would need and which of those inputs exist. DIAG.9a, which records inputs, is not started, so the command can only
state precisely why each input is missing. It prepares; it never runs anything and never claims a replay.

The DX12 hardware fault output pasted again at the start of this step (`submit=00000000 wait=00000000
reason=00000000`, an empty removal record, exit 30) is the second run already recorded in
[the user-items note](2026-10-07-user-items-wpr-and-vulkan-loss.md). It adds nothing new; the user's decision stands
and the case was not run again.

## What was there (checked before coding)

- No replay facility, recorder or recipe exists. The crash bundle format has sections for a manifest, the crash
  record, a raw dump, a log tail and module identities, but none for a program identity or any recorded input, so a
  bundle form of this command would only report every input missing. It was left out.
- CEIR already declares what a replay needs: every op kind carries §26 effect families (RandomRead, TimeRead, FileIO,
  Synchronization, GPUCommand and the host/world reads) and a §27 determinism class, and
  `Context::collect_effective_mask` resolves a `func.call` through the module's symbol table to its callee's effects.
  Without a table a call is a conservative ExternalCall barrier; an unregistered op is ExternalCall too (empty is not
  unknown).
- `program.provenance` held the three-form program load (cooked, binary, text under the relative path) inline.

## What changed

- **Shared loader** (`engine/execution/ceir-cook/src/program_load.{hpp,cpp}`, private): the bounded read, the form
  choice by leading bytes, the load into a fresh Context with the host's dialects and the stable-id assignment, moved
  out of `program.provenance` unchanged so both commands load a program the same way.
- **`replay.prepare`** (`crd/ceir/cook/replay_diag.hpp`, `replay_diag.cpp`; `read`, a path): one item per input in a
  fixed order, each with its guarantee, whether it is needed (`yes`, `no`, `unknown`), its state (`available`,
  `missing`, `not-needed`), how many ops need it, the first such op in pre-order (stable id, name, authored
  file:line:col) and the reason:

  | Input | Guarantee | Needed when |
  | --- | --- | --- |
  | `program` | identity | always; available (the content hash, and a cooked file's recorded hash) |
  | `build` | identity | always; a program file names no build |
  | `entry-arguments` | event | always |
  | `random` | event | RandomRead |
  | `clock` | event | TimeRead |
  | `host-state` | event | a host or world read (HostState, Scene, Ecs, Physics, Audio, Document, Constraint, UI) |
  | `external-results` | event | FileIO, NetworkIO, DeviceIO, ExternalCall, AgentAction |
  | `schedule` | schedule | Synchronization, Nondeterministic |
  | `device-tolerance` | numeric | GPUCommand (a declared tolerance or oracle, never bit identity) |

  Each op's effective effects are collected with the module's symbol table and a cycle guard cleared per op, so every
  call to a callee is charged with the callee's effects. An op whose effects include ExternalCall (an unregistered op,
  an unresolved callee, a declared opaque call) is opaque: it needs external results, and when no registered op needs
  another effect-derived input that input is `unknown` and reported missing, never `not-needed`. The summary gives the
  form, size, content and recorded hashes, op, unregistered and opaque counts, the weakest determinism class the
  registered ops claim and how many make no claim, the needed, unknown and missing counts, `replay` (`unavailable`
  while anything is missing) and the comma-separated missing inputs.
- **ceridc:** `bind_diag_commands` registers `replay.prepare` with the same dialects as the other program commands,
  so `ceridc diag` and the MCP `diag` tool serve it. As with `program.provenance`, ceridc registers only arith, core and
  func, so a CHIR-lowered program's task and async ops read as unregistered there, which makes its effect-derived
  inputs `unknown`.

## Tests

- `tests/execution/ceir-cook/test_replay_diag.cpp` (4 cases, 171 assertions), positions from scanning the text:
  - the committed `assets/ceir/inspect_demo.ceir`: 9 items, program available, build and entry arguments missing, the
    six effect-derived inputs not needed with no op blamed, `weakest_claim` bit-exact with the five `func.*` ops
    unclaimed (counted from the text), the root never in the answer, pages of four walking the same items; the binary
    and cooked forms give byte-equal items and the same content hash (the cooked file's recorded hash too);
  - a specimen whose host registers probe ops with TimeRead, RandomRead, SceneRead, FileIO, Synchronization and
    GPUCommand, where `@noise` (random draw and scene read) is defined after `main` and called twice: random and host
    state are blamed on the first call at its scanned line and column, with 3 ops each (two calls and the callee's op);
    clock, external results, schedule and device tolerance on their own ops; a resolved call adds no external results;
  - a program with an op no host registers: it is opaque and needs external results, random, host state, schedule and
    device tolerance are `unknown` and missing and blamed on it, while the clock read is still blamed by name;
  - refusals (record-only grant, no root, an escaping path, a raised cancel) with the command's run and byte counters
    at zero, an oversized file refused unread, and a text that does not parse refused at its scanned line and column.
- `tests/tools/ceridc/test_ceridc_diag.cpp`: the parity sequence gains a `replay.prepare` snapshot and its second page
  (native, verb and in-process MCP byte-equal, seven handler runs), and the real binary's CLI and MCP stdio answers for
  it equal the native document.

## Teeth (win-debug)

Each break was applied warning-clean, the target rebuilt and the cases run; then the source was restored (mtime
bumped), rebuilt and the suites passed again (`crd-ceir-cook-tests` 55 cases, `crd-ceridc-tests` 10).

| Break | Result |
| --- | --- |
| No symbol table (static ExternalCall fallback for calls) | the demo and specimen cases (2 of 4; 20 assertions) |
| The callee cycle guard shared across ops | the specimen case (the second call counted nothing) |
| Opaque ops ignored (an unknown need reported not needed) | the opaque case |
| The size bound not passed to the loader | the refusal case |
| Text parsed under the joined root path | the specimen and opaque cases |
| ceridc not registering `replay.prepare` | the parity and real-binary cases |

The first attempt at the cycle-guard tooth did not compile (`/WX`, an unused parameter) and the run silently reused
the previous exe; it was redone warning-clean before its result was read.

## Evidence

All on the final sources.

- **win-debug:** `crd-ceir-cook-tests` 55 cases (2,682 assertions), `crd-chir-tests` 30 (5,382; the provenance
  command's CHIR case runs the refactored loader), `crd-ceridc-tests` 10 (431, with `CRD_CERIDC_EXE`). CTest runs the
  156 `diag`/replay cases in parallel (`-j 8`), and the 8 repository guard CTests pass.
- **win-shipping, win-clang-cl-shipping:** reconfigured (new sources), the four targets built with clean links, the
  same three suites pass with the same counts.
- **win-asan** (inside vcvars): the same three suites pass with the same counts; no ASan report.
- **WSL:** linux-gcc-debug, linux-gcc-asan and linux-clang-tsan (the hosted lane's `TSAN_OPTIONS`) build the four
  targets and pass the same three suites with the same counts, with no ASan, UBSan or TSan report. They were run a
  second time after the last edits (comment rewraps and one summary string), so these results are on the final
  sources too.
- **Checks:** strict tidy (LLVM 20.1.8) is clean on the six changed C++ sources (the new headers through them).
  clang-format `--dry-run` reports only the repository's hand alignment, include grouping and one-line `case` style;
  its line-wrap findings were applied by hand. The Allman check, check-master-plan and check-repository pass.

## Hosted CI must later show

This batch does not move the row to Needs CI (a local clause remains). When the row is complete, its hosted run must
also show `crd-ceir-cook-tests` (55 cases) and `crd-ceridc-tests` (10, including the real binary's
`replay.prepare` answers) green on all six lanes, and `crd-chir-tests` (30) still green.

## Remaining (local) on DIAG.8c

- A GUI consumer of the same service.
- Not tested: the snapshot item cap (`kDiagMaxSnapshotItems`); `replay.prepare`'s in-walk cancel checks (the service's
  cancel refusal is tested). Not claimed: the synchronous MCP stdio loop does not process `notifications/cancelled`;
  a `program.inspect` request holds the service for the length of its run.
