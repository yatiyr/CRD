# CEIR execution foundation

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CEIR owns the canonical executable IR: operations, values, blocks, regions, types, effects, stable identity,
serialization, verification, compiler analyses/transforms and provider execution. CHIR owns high-level language
semantics and lowers into CEIR; CKIR owns device kernels/shaders. See
[ADR-0109](../decisions/0109-ceir-chir-ckir-ownership-and-module-placement.md).

## Modules and source

- [ceir](../../engine/execution/ceir/include/crd/ceir/): GPU-independent IR, verifier, dialects and compiler infrastructure.
- [ceir-host](../../engine/execution/ceir-host/include/): host execution/provider mechanisms.
- [ceir-gpu](../../engine/execution/ceir-gpu/include/): GPU lowering/materialization/provider bridge, depending on the generic
  gpu-context facade and CKIR, not a concrete Vulkan/DX12 backend.
- [ceir-cook](../../engine/execution/ceir-cook/include/): authored execution assets and cook integration.
- [Dialect definitions](../../engine/execution/ceir/ops/): generated operations; regenerate through the canonical generator.

## Recorded completion

CEIR-1…35 is closed on its recorded contracts: IR/control/effects/lifecycle, compiler/execution planning, GPU/resource/
render/frame convergence, native tensor/ML/autodiff/optimization, transform/rewrite strategy assets, autotuning,
native launch graphs, distribution, media/UI/audio bridges, CHIR-0 and legacy deletion/qualification.
The [CEIR history](../detours/D-007-ceir-tracker.md) and
[35 close](../sessions/2026-09-11-ceir-35z-band-close.md) contain the evidence.

CEIR-33 C2 closed domain schemas/contracts, with the widget implementation retained in I2D-9. CEIR-35's production
work qualified the execution substrate and routed feature quality to its owning bands; it did not close the full
renderer/UI library. Its internal A/B boards are not renderer peer-crush claims.

## Runtime and lifecycle boundaries

Authored frame graphs converge through CEIR under [ADR-0127](../decisions/0127-ceir-frame-dialect-and-converter.md).
Providers bridge to host, Vulkan/DX12 graphics/compute and CUDA compute where implemented. Provider selection is
capability-dependent. Emitting a dialect or shader for a backend is not sufficient evidence of its full execution.

Cook/cache keys, compiled plans, state-schema compatibility and migration are reusable mechanisms. The standalone
plan-cache API's existence does not imply every host uses it. Consumer integration is checked in the owning slice.
Source reload in scene-render still needs the RAH-7 atomic/granular registry work. Audio's CEIR-31 acyclic proof does
not complete real-time feedback execution. CHIR-0 is not a full application language.

Runtime inspection ([DIAG.8b](../design/runtime-diagnostics.md#diag-8b)): `crd/ceir/inspect.hpp` pauses the compiled
plan (`plan::RunControl`) or the reference interpreter (its step hook) at authored `file:line` breakpoints, steps,
cancels and answers typed value snapshots from the paused thread, bound to one program generation. The crd-jobs
`HostProvider` runs under a session too: pooled launches keep running through a pause and are reported as pending,
one cancel stops the pool work, and bodies on its own sub-interpreters count breakpoint hits instead of pausing.
GPU dispatches recorded by `execute_lowered` under a session never pause, whatever its scope: a breakpoint there is
counted, a pause request is refused and a cancel stops the recording before the next dispatch.
crd-ceir-cook's `InspectHost` (`crd/ceir/cook/inspect_host.hpp`) is the composition a consumer uses: it cooks the
authored text into a ReloadSet generation, compiles one entry, binds the session and runs the plan on its own executing
thread, so the consumer's thread stays the controller. `ceridc inspect` is its headless consumer (a JSON report of
every stop, the watched lines' typed values and the scripted steps). `crd-sandbox --inspect`
is its sandbox consumer: the program loads app-first (`SceneRenderer::resolve_program_text`, `app://ceir/<name>` over
`engine://ceir/<name>`) and the frame loop polls it each frame without waiting, with a window to step, pause and cancel.

Diagnostic commands ([DIAG.8c](../design/runtime-diagnostics.md#diag-8c)): crd-ceir-cook registers
`program.provenance` (`crd/ceir/cook/program_diag.hpp`) into crd-perf's typed command service. Given a CEIR text,
binary or cooked program under the host's root, it answers every op with its authored file:line:col, its CHIR origin
and its native binding, plus one item per origin for ops merged from several; `ceridc diag` and the MCP `diag` tool
serve it. `program.inspect` (`crd/ceir/cook/inspect_diag.hpp`) needs the separate `execute` grant: it runs an authored
text program to a script given as named arguments (breakpoints, watched lines, steps, a stop bound) through the same
scripted inspection as `ceridc inspect` (`crd/ceir/cook/inspect_script.hpp`) and answers each breakpoint, stop and
watched value as an item; it is how an agent transport inspects a program. `replay.prepare`
(`crd/ceir/cook/replay_diag.hpp`, `read`) answers what a replay of a run of an authored program would need: one item
per input (program identity, build, entry arguments, random streams, clock, host state, external results, schedule
choices, a device tolerance), whether the program's effective effects need it (a call charged with its callee's;
`unknown` when an opaque op exists), the first op that needs it at its authored position, and why it is missing.
A program file holds no run, so it reports the replay unavailable and names the missing inputs; given a run record
it answers the build and arguments the record holds.

Run records ([DIAG.9a](../design/runtime-diagnostics.md#diag-9a)): `replay.record` (`execute` and `record`) runs an
authored program's entry once through the compiled-plan executor and writes an immutable record
(`crd/ceir/cook/replay_record.hpp`): the cooked program blob and its content hash, the build, the arguments, each
input's need and state (missing when nothing captures it), a bounded trace of every dispatched instr with its results,
and the outcome. `replay.run` (`execute`) replays a record from its own blob in any later process, never from the
checkout, and reports the first divergent event, value, count, outcome, result or cell at its authored position, or
that the run reproduced; with `program=` it replays the same inputs against an edited file. A record from another
build, with a missing input, a bad checksum or a blob that is not its recorded content is refused before anything
runs. The committed `assets/ceir/replay_demo.ceir` fails on a seeded argument for the tests to reproduce.
`InspectHost::start` can record the execution it runs under a debug session (`HostRecording`): the trace is taken by
the session's safe-point observer (`inspect::Session::run` with an observer), so breakpoints, steps and value reads
leave it identical to an unobserved run's, and `InspectHost::record` gives the record with the host's asset id and the
ReloadSet generation that ran (schema 2), still that generation's program after a reload. A cancelled run gives no
record. `ceridc inspect --record <file>` and crd-sandbox's `--inspect-record <file>` write one; `write_record_file`
never overwrites.

## Maturity and further work

The [manifest](../capabilities/gpu-platform-capabilities.toml) and
[generated matrix](../generated/gpu-platform-capability-matrix.md) own exact feature counts/levels/provider evidence.
Do not copy a dated count here. RAF and CEIR levels are distinct axes governed by the recorded C2/qualification
choices; none is a blanket “the whole engine is finished” claim.

Future rendering/UI/science/media work is tracked only in [ROADMAP](../ROADMAP.md#master-table), with source gaps
in the [audit](../research/2026-09-12-system-audit.md). The
[pre-audit overview](../archive/2026-09-12-superseded-plans.md#system-ceir) is preserved as a historical record.
