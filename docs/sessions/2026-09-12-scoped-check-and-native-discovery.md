# Scoped verification and native test discovery

<!-- doc-role: evidence -->
> Dated evidence, 2026-09-12. Live work: [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Scope and implementation

Continue REPO.DEV.3b: complete the focused developer frontend without local repository sweeps, source/index loss,
invented platform qualification or renderer implementation. The advisor capability remains unavailable. No commit,
push, dependency version upgrade or geometry/physics algorithm change was performed.

`dev.py check` now composes the existing environment, File API model, synchronization guard, process supervisor and
evidence APIs. It accepts diagnostic paths/targets, dry-run, one/two compile workers and explicit budgets. It refuses
automatic full-scope execution and more than 32 selected build targets. Fresh build/toolchain creation still uses
the canonical preset helper; an existing stale/missing File API reply triggers guarded reconfiguration.

After a selected build, contained CTest discovery finds actual tests and fixture owners. Newly required fixture
executables build once, then discovery repeats. CTest keeps its own fixture/resource/timeout semantics. A CMake-3.25
compatible index file selects names; JUnit must account for every selected test exactly once. Missing/zero/duplicate
results, failures, skips/disabled tests and changed source/model identities prevent qualification. Documentation-only
checks need no compiler. Repository guards and changed-C++ tidy remain required. Envelopes preserve native exits,
logs, commands, source identity, counts and configuration. Explicit subsets are labeled diagnostic evidence.

`plan` and `check --dry-run` no longer invoke CTest: PRE_TEST discovery can execute test programs and write cached
inventories. These read-only commands now explain the pending build/discovery step instead of calling that work a
read-only query. The production check owns containment and result evidence.

## Reproduced discovery defects and repair

A tiny actual Catch2 **3.7.1** CMake integration fixture built Debug, then Release. Before repair,
`ctest -C Debug --show-only=json-v1` reported a test named Release and invoked the Release executable. Catch's
POST_BUILD mode writes one shared tests file; PRE_TEST writes configuration-specific inventories.

[CrdTestDiscovery](../../cmake/CrdTestDiscovery.cmake) wraps the pinned Catch integration without copying its discovery
algorithm. All 102 test CMake files now call `crd_discover_tests`. It requires PRE_TEST and wraps the actual include
contributed by Catch. An absent executable produces a failing placeholder carrying explicit target/artifact labels,
not a guessed target-name prefix. The selector validates that annotation against the current codemodel. Unrelated
pending targets are excluded; pending fixture providers remain required and selected placeholders cannot qualify.

The fixture also caught flattened list-valued properties. The wrapper applies original properties through Catch's
TEST_LIST after inclusion, preserving labels, fixture declarations, timeouts and GPU locks. Iterating property pairs
by index avoids CMake's destructive POP_FRONT flattening remaining escaped values. The fixture uses tiny executables
implementing Catch's list protocol with the actual Catch CMake modules; it is not a replacement test framework.

Windows VS 2026 Debug/Release and Linux GCC/Ninja fixtures passed configuration selection, unbuilt ownership,
fixture setup and list-valued properties. The complete check fixture also passed on both hosts, with one executed
consumer, explicit fixture guard programs, JUnit reconciliation and verified evidence. Temporary fixtures mock only
the absent Git checkout identity; real checkout identity qualification remains a separate gate. The then-current
38-case suite passed on Windows; Linux passed 37 with the Windows Job Object case not applicable.

## Real consumer investigation

Two frame-cooker files received mechanical strict-analysis repairs: indexing StringView directly and replacing
nested ternaries with equivalent branches. LLVM-20 parsed both and reported clean. The latest published strict-tidy
job had identified those three diagnostics; its exact revision/job is below.

The first real check correctly refused an unreachable attached IDE from the sandbox. Running in the matching desktop
context completed guarded configuration. A subsequent doctor snapshot saw a transient watcher writer-lock conflict;
the watcher returned to watching, with no incomplete transaction. No guard was bypassed.

The next frame-cooker run built its real dependencies, discovered and executed **127/127** selected CTests: **126
passed, one failed, zero skipped**. The failing SIMD-emission guard constructed a Ninja object path in the native VS
tree. The repair uses CMake's TARGET_OBJECTS, configuration-specific ISA expectation, and explicit target ownership
for object-consuming guards. A focused unrelated target must not read another target's stale/unbuilt object.

That build also exposed Catch2 C4530 warnings. The native cache had empty CMAKE_CXX_FLAGS and four base configuration
flag entries; a fresh fixture with the same CMake/compiler had /EHsc and the normal optimization/runtime-check flags.
This establishes cache drift, not a fresh-generator defect. Doctor now rejects empty native defaults. A narrow repair
backs up the cache and unsets only those five verified-empty entries through project-sync configure, allowing CMake
to initialize its defaults. Nonempty settings, presets and source algorithms are preserved.

Logs are under ignored `build/research-dev-workflow-20260912/`; structured real runs are in
`build/win-vs-debug/cerid-dev/runs/`. This record retains the interpretation; an earlier failure envelope remains a
failure. The SIMD guard/cache repairs need their subsequent scoped proof before their owning rows close.

## Published CI refresh

The human published `2ed89c487215c91ffc2f667c19b2d97f01f5598c`. At the recorded observation,
[run 34700773738](https://github.com/yatiyr/CRD/actions/runs/34700773738) remained active. Both repository jobs and
Linux Debug, Release and Debug-SSE2 passed. Strict tidy job **103572020447** failed at frame_work.cpp:25/112 and
frame_asset.cpp:1129; the local repair above is not yet published.

Clang-cl Shipping job **103572020334** completed with the same seven DX12 failures (B18 hair/fur/scattering, B17-c
A-buffer crash, inner coverage, RT-4 and impostors). The new selected-device probe passed and reported:
Microsoft Basic Render Driver, vendor 1414/device 008c, DXGI flags 0, software=false in both DXGI and the engine,
driver 10.0.26100.33296, binding/conservative tiers 3, ROV support, four-lane waves and RT tier 1.1. This is the actual
selected device, independent of WMI display identity. It does not authorize changing oracles or prove the provider
classification mechanism is sufficient; REPO.3c.4–7 retain the investigation.

## Continuation

Finish the real scoped proof after cache/guard repair; complete portable strict-analysis execution and full frontend
qualification, including fresh/unbuilt consumers and failed/changed-state paths. Review the helper's declared limits:
its OS lock prevents competing checks, while raw external builds/other hosts require further coordination proof.
Then continue the approved repository rows and exact human-published CI verification. Stop before renderer review
gates. [Recipe](../recipes/2026-09-12-affected-build-selection.md) and [contract](../design/developer-workflow.md) own
the reusable explanation; only ROADMAP owns statuses.

## Subsequent scoped proof

The five-entry cache repair completed through the existing synchronizer: CMake restored /EHsc, Debug /Od /RTC1,
Release /O2, RelWithDebInfo /O2 and MinSizeRel /O1 defaults. The rebuilt frame-cooker consumer emitted no previous
C4530 diagnostics. Native generated CTest commands now name each configuration's actual SIMD object, with scalar
and SSE2 expectations for their respective profiles.

The watcher snapshot race received a discriminating fix: after a persisted conflict snapshot, execution reruns the
authoritative guarded IDE/journal/generation/source checks. A regression proves a real unsaved-source conflict still
throws; only a successfully revalidated snapshot stops blocking. Standalone doctor retains its read-only observation.

The complete real frame-cooker check then passed **127/127 selected/reported/executed CTests, zero failures, skips or
disabled tests**. Both changed C++ files passed LLVM-20 tidy; documentation/hygiene and unchanged source/model identity
passed. Evidence envelope `20260912T192212-a68e5af9be9a` retains all six phases. Its qualification is explicitly
**diagnostic subset passed**, not all reverse consumers, other configurations or unpublished remote CI. The portable
tool suite now has 41 passing Windows cases; the last Linux increment passed 39/40, with the Windows-only case not
applicable, before the final watcher-revalidation case was added.

The subsequent native math check passed **184/184 selected/executed CTests, zero failures/skips/disabled tests**,
with source/model identity and repository guards intact, in envelope `20260912T193158-e10820814c01`. The SIMD guard
inspected the actual Debug object: 26,270 instructions, 137 YMM references, five YMM FP operations. A preceding
failure found dumpbin absent from the effective PATH. The frontend now loads the generated compiler metadata and
process-local MSVC tools for native generators even when the cache has no compiler entry; doctor reports the actual
MSBuild/compiler paths. This closes the bounded native cache/object-path qualification, not the whole frontend.

Final discovery/check fixtures passed on Windows VS 2026 Debug/Release and Linux GCC/Ninja. The 42-case suite passed
on Windows; Linux passed 40 applicable cases with two explicit Windows-only cases. A later guard-reporting increment
passed 43 Windows cases and 41 applicable Linux cases. Nine controlled decoder scenarios cover nonzero decoder exits,
empty non-IPO output, explicit IPO/NEON skips, and correct/incorrect ISA evidence. This repairs a source-level
skip-as-pass concern without changing numerical algorithms or SIMD expectations. The actual guard/configuration
integration is a separate follow-up proof; REPO.DEV.3b.5 owns it in the only live table.

Direct CTest ownership is now explicit through `crd_test_target`; the DX12 selected-device probe consumes it. The
selector validates the declared executable against the current model and, when present, the actual command. Missing
unannotated commands still fail closed. The real Windows and Linux partial-build fixtures now leave the unrelated
direct executable absent and complete the selected consumer's full check. The resulting suite passed 44 Windows
cases and 42 applicable Linux cases, with two Windows-only cases explicitly not applicable there. Both actual Catch
fixtures also passed again, including Debug-after-Release execution and preserved fixture/list-valued properties.

A later read of run 34700773738 found all six Linux lanes and both repository jobs successful. Windows Debug
(103572020504), Debug-SSE2 (103572020433) and clang-cl Debug (103572020478) each finished with the same seven DX12
failures already owned by REPO.3c.4–7; totals were respectively 6,763, 6,760 and 6,763 CTests. The clang-cl Debug
run no longer reported the repaired FFT stack failures. Win ASan/Release/Shipping remained active at this observation.
No green remote result is attributed to the current unpublished increment.

The next real native check (`20260912T195802-51a1b8695251`) failed before tests: MSBuild invoked automatic CMake
regeneration and the native guard rejected a generation that had not finalized its synchronization baseline. A
readable File API model alone was insufficient readiness evidence. Check now also asks the synchronization owner's
`needs_generation` and pending-generation state, routing changed/unfinished registered native projections through
guarded configure before build. A live generation still refuses overlap. The 45-case Windows suite passes, including
changed, complete, failed and live baseline states; this fixture is distinct from the subsequent real-build proof.

## Final bounded guard proof

Guarded recovery then completed and envelope `20260912T200214-45bfbc9c8c08` passed **184/184**, with zero failed,
skipped or disabled tests and unchanged source/model identity. Native CMake commands carry the actual object path,
ISA and IPO per configuration: Debug/ASan/scalar/SSE2 have IPO 0; Release/RelWithDebInfo/Shipping/ShippingProfile have
IPO 1. This is configuration projection evidence; it is not eight new runtime runs.

An added stderr fixture reproduced Windows PowerShell throwing before exposing a native decoder's exit 9. The guard
now temporarily permits native stderr capture, immediately saves the exit and restores the preference. The final
45-case suite passed on Windows and 43 applicable Linux cases, with two Windows-only cases explicitly not applicable.
The final real Windows SIMD CTest passed 1/1 on the same Debug object. The Linux SIMD CTest passed 1/1 against its
existing Debug object: 23,212 disassembled instructions and 445 YMM references. That Linux run qualifies the repaired
script against the existing artifact; it does not claim a newly rebuilt Linux math module or fresh whole-tree model.

These observations close the bounded REPO.DEV.3b.5 guard-reporting contract. The complete frontend still requires
external-build coordination, portable strict analysis and broader real-consumer/failure-path qualification under its
existing children. Frame-cooker source repairs still need all affected consumers and exact published CI evidence.

Further primary-source research identified Microsoft's documented BasicRender flag exception. The
[research census](../research/2026-09-12-large-cpp-development-and-ci.md#gpu-failure-census) records the source and a
selected-LUID query path for REPO.3c.4. No backend classification, tolerance or renderer algorithm was changed in this
increment. The repository loop remains authorized; renderer review gates and human-only publication remain intact.

Close-out checks pass: 857 roadmap rows, 1,007 documents and 8,576 local links at the recorded validator run; repository
hygiene, six repository-tool fixtures, actionlint and `git diff --check`. The watcher is alive/watching, no incomplete
transactions remain, and generation state is clear. Orientation rules were reviewed and retain their policy; MEMORY
now links the discovery/baseline/decoder lesson. Only context carries the current pointer, now REPO.DEV.3b.2.
