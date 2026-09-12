# Repository CI environment repairs

<!-- doc-role: evidence -->
> Dated evidence, 2026-09-12. Live work: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

## Contract

Execute the approved repository loop from [the research/decision session](2026-09-12-large-cpp-research-and-loop-plan.md).
First owners are REPO.3c.1 (hosted VS fixture), REPO.3c.2 (Linux validation acquisition) and REPO.3c.3 (strict tidy).
Scope is infrastructure and confirmed failure repairs; retained engine algorithms and renderer design gates stay
with their owners. No advisor capability is available. No commits or pushes are allowed.

## Hosted Visual Studio fixture

Published revision `9045eebb5c072b6025343c67343a57a57c79bc85` fails native fixture configuration in
[job 103556726280](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726280): it requests VS 2022
on the recorded `windows-2025-vs2026` image. CMake never reaches a compile. The fix selects VS 2026 in both native
fixture commands and explicitly selects that image family; the repository Linux fixture likewise names Ubuntu 24.04.
This family pin is not an immutable software image; the recorded image/toolchain tuple remains necessary.

The existing native profile and structural fixtures provide the discriminating regression. Run them on the
installed VS 2026 with standalone CMake, sequentially and with their existing bounded subprocesses. No full engine
matrix is involved. Remote confirmation of the edited workflow awaits the human-published revision in REPO.3d.

Local results: `test-native-build-profiles.py --generator "Visual Studio 18 2026"` passes seven portable tests and
compiles/executes all eight profiles, including the no-IPO exception. `test-project-sync-native.py` with the same
generator passes add/remove/delete/recover, reference/module moves, empty folders and watcher/regeneration checks,
compiling Debug and Release. Both fixtures ran sequentially using CMake 4.3.2 and installed MSVC 19.51 on Windows.
Existing actionlint 1.7.12 accepts the changed workflow. Synchronizer status is watching, with no pending generation
or incomplete journal. Local fixture evidence closes the repair; exact hosted confirmation belongs to REPO.3d.

## Linux validation acquisition

The SDK endpoint rejects `Python-urllib/3.12` with HTTP 403 while the same public URL accepts an explicitly
identified client. A truthful `Cerid-SDK-Installer/1.0 (+https://github.com/yatiyr/CRD)` request returned HEAD 200
and a 32-byte range GET 206 with the XZ magic. This isolates request identity as the observable cause; it does not
speculate about the service's internal rule. No browser impersonation, credentials or alternative archive was used.

The installer now supplies that identity while retaining the existing timeout, exact-member extraction and SHA-256
`3bf0f762afb6c79bc6a9d9fb5998745ccff928800a29619b501ed9de7fd9789b`. Its new regression intercepts the request,
checks the identified official client and confirms that untrusted returned bytes still fail checksum before install.
All five repository-tool regressions pass on Windows Python 3.14.4 and WSL Python 3.12.3.

A full 288,040,252-byte archive was fetched into ignored build output by the actual installer and passed its hash.
The same archive was independently verified/extracted under Linux to
`/home/yatiyr/cerid-build/validation-repair-20260912`. Its layer/library paths were used with the existing scoped
`ctest --test-dir /home/yatiyr/cerid-build/linux-gcc-debug/tests/ceir-gpu-vulkan -R '^ceir 13z' --timeout 180
--no-tests=error --output-on-failure` probe: nine cases pass. The complete LastTest.log has zero VUIDs, zero device
skip warnings and assertion counts 6/72/9/74/10/24/23/23/2. This is validation-tooling evidence on that existing
WSL binary/provider, not a fresh rebuild or hardware qualification of all Linux engine code.

## Strict tidy and affected consumers

`work_build.cpp` now indexes the StringView directly and selects produce/consume/compact through ordinary
conditionals, preserving the previous mapping and algorithm. Existing descriptor/emission/executor tests cover
these branches; no new algorithm or weakened check was introduced.

Builds pass for `crd-ceir-gpu-tests`, `crd-ceir-gpu-vulkan-tests` and `crd-ceir-gpu-dx12-tests` in win-debug. The
provider consumer binaries were explicitly relinked before final evidence. LLVM-20.1.8 `tidy-files.ps1` reports the
changed TU parsed and clean. Final scoped CTest regex
`^(ceir 20b:|ceir 20c-1:|crd-repository-|crd-master-plan|crd-no-non-ascii-test-names)` passes 17/17, including real
Vulkan/DX12 work execution and repository/documentation guards. An earlier invocation found the in-progress
BUILDING/MEMORY size-budget violation; that was corrected before the final pass. No engine failure was hidden.

## Fast local policy — REPO.DEV.2

BUILDING now owns one risk table: primary local target/consumer tests and incremental tidy, additional local lanes
only for discriminating risks/failures, broader qualification in CI. It explicitly distinguishes the approved tier
policy from the still-unimplemented scheduling change. Commands preserve exits, bounded tests, nonzero counts and
native configuration selection. AGENTS, START_HERE, SANITY, MEMORY and the quality contract route the same policy.
Existing troubleshooting knowledge and detailed historical evidence remain reachable; the entry budgets are retained.

No agent commits or pushes, including through helpers or automation. Independent work continues while publication
waits, and new published results must match the candidate. No new CI resources or renderer design approval is assumed.
Next owner: REPO.3c.4, complete failed-lane/provider census before the remaining GPU repairs.

## Expanded failure census

The preceding revision's additional completed jobs reproduce the same seven DX12 failures in
[Release](https://github.com/yatiyr/CRD/actions/runs/34688030261/job/103538355057),
[Debug SSE2](https://github.com/yatiyr/CRD/actions/runs/34688030261/job/103538355018) and
[clang-cl Shipping](https://github.com/yatiyr/CRD/actions/runs/34688030261/job/103538354974).
[clang-cl Debug](https://github.com/yatiyr/CRD/actions/runs/34688030261/job/103538355069) adds six FFT failures:
four-step f64/f32, standalone hierarchical f32, scheduled radix-8/16, batched transforms and even-batch AoS.
Catch2 identifies stack overflow in all six, not a generic numerical mismatch. REPO.3c.8 owns this additional repair;
the request to fix all remaining CI items includes it. No geometry/physics change is authorized by that finding.

The A-buffer exception likewise reports stack overflow. Hair/fur failures use the tight hardware bars even though
older comments assumed hosted WARP. RT-4 reports worst error 0.0524184 against 0.05; inner coverage reports 312 white
and zero black pixels; the impostor path records one impostor draw but zero GPU instances/pixel delta while CPU
visibility counts 25. These are diagnostic observations, not established shared root causes. Preserve exact provider
identity and each existing oracle; changing precision thresholds or silently skipping unsupported execution is not a repair.

The current published `9045eebb` revision confirms the same census in completed
[clang-cl Debug](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726254) (seven DX12 plus six FFT)
and [clang-cl Shipping](https://github.com/yatiyr/CRD/actions/runs/34694952926/job/103556726234) (seven DX12).
Other MSVC lanes were still running at this inspection; their eventual results are not inferred from these lanes.

## FFT stack repair

All six clang-cl failures reproduce locally after a focused rebuild. LLVM COFF unwind records show
`execute_batched` stack allocations of 2,149,688 bytes for f32 and 2,033,048 bytes for f64; the executable's PE
stack reserve is 1,048,576 bytes. Forced inlining of generated leaves, including the hierarchical family, merges
their temporary storage into oversized caller frames. Changing only the batched generator policy left the
hierarchical contribution and all six failures; the final candidate covers both families. Existing optimized
inlining/arithmetic and numerical thresholds are preserved. Final `execute_batched` frames are 736 bytes (f32)
and 880 bytes (f64), with the executable reserve unchanged.

The new header was imported automatically by the live Visual Studio watcher into `cmake/project-structure.json`.
The first local regeneration could not access the desktop; a subsequent attempt correctly refused overlap with
native generation. After that generation completed, the scoped clang-cl rebuild resumed. No sync guard was bypassed.
LLDB's missing Python runtime prevented that instrument from starting; the artifact inspection above uses
`llvm-readobj`, not an invented debugger trace.

The [recipe](../recipes/2026-09-12-generated-codelet-stack-safety.md) captures the mechanism and reproduction.
The hierarchical header's named scratch generator is absent. REPO.DEV.10 owns restoring generated-source
provenance. Its existing expressions and execution schedule remain intact; mechanical declaration/name cleanup
was applied to the surviving artifact, without inventing a recovered algorithm generator.

### Generated-source quality and final local proof

Direct header tidy exposed 67,398 combined declarations, 2,048 naming diagnostics and generated-function size
diagnostics. The small `fft_codegen_style.py` helper now emits separate declarations, preserves nested expression
commas/pointer constness and canonicalizes the three invalid local names. The batched generator uses the same
helper; the surviving hierarchical artifact was normalized with it. This explains the large mechanical header
diff. The tests cover expression commas, braced/shift expressions, pointer constness, idempotence and rejection
of malformed input. No numerical operation, literal, schedule, public function name or tolerance was changed.

Only `readability-function-size` has per-function annotations for these generated scheduled DAGs: retaining their
fused scope is intentional. All naming, declaration, bugprone and other checks remain enabled; there is no global
tidy suppression. The three edited/new headers parse and pass the LLVM-20.1.8 incremental helper.

The final clang-cl rebuild includes FFT and all five directly linked numerical consumer test targets, sequentially
with two compile workers. Scoped CTest results: FFT 29/29; DSP FFT consumers 16/16; CWT 4/4; OFDM 4/4;
Chebyshev/trigonometric interpolation 2/2; spectral differentiation 1/1. This includes the existing thread/replay
oracles and totals 56 runtime cases. Six relevant repository/documentation/numeric/test-name CTest guards pass.
The six portable tooling fixtures also pass on Windows Python 3.14.4 and WSL Python 3.12.3.

The batched generator reproduced the output byte-for-byte before style cleanup and under Python development
checks after cleanup. One intervening WSL invocation raised `AttributeError: 'list' object has no attribute 'id'`
inside the unchanged scheduler's `score(nd)`. That observation has no established root cause and is not declared
fixed by a later pass. REPO.DEV.10 also owns resolving this generator/tooling reproducibility concern. No engine
or hardware defect is inferred from it. Exact published-revision qualification of these source edits remains
REPO.3d; unavailable backend/hardware obligations remain open.

Three subsequent ordinary WSL invocations also reproduced the normalized batched header byte-for-byte. That
narrows the observation to an intermittent/unreproduced tooling symptom; it does not establish its cause or close
the separate reproducibility finding. The C++ stack mechanism and its discriminating regressions are independently
resolved; REPO.3c.8 closes locally, with publication retained under REPO.3d. Its prerequisite is corrected to
environment/tidy readiness (REPO.3c.3): this CPU-only repair is independent of completing the GPU device census.
The full census remains required by the GPU repair children and the repository parent.

## Selected DX12 device census

REPO.3c.4 now has a small [native probe](../../tests/gpu/gpu-context-dx12/device_info.cpp), registered as
`crd-dx12-device-info`. It creates the real Cerid facade and the same default D3D12 device selection, resolves
the adapter by device LUID and checks the facade name/software classification against its DXGI descriptor.
It prints vendor/device/revision/flags, node count, package driver version, resource-binding/conservative-raster/
ROV and wave/RT query values with their HRESULTs. Failed queries stay distinguishable from unsupported features.
A missing facade returns CTest skip code 77, explicitly leaving hardware qualification open; other census failures
fail the test. This is identity/capability evidence, not a rendered-output or performance proof.

Selection follows [D3D12CreateDevice's documented default](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice),
not the WMI display-controller label. Driver version uses
[CheckInterfaceSupport with IDXGIDevice](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiadapter-checkinterfacesupport),
retaining an explicit unavailable result if the query fails. No software classification by adapter-name heuristic,
threshold change, forced provider or rendering algorithm change was added.

All Windows test jobs now run this one probe verbosely before their existing CTest suite. Both exits are preserved;
a failed probe cannot turn the job green, and the main suite still runs to collect its own failures. Linux jobs
are unchanged. Actionlint accepts the edited workflow; hosted probe output awaits the human-published revision.

The TU parses clean under LLVM-20.1.8 tidy. Its native VS 2026 Debug target builds with two workers and its bounded
CTest passes: NVIDIA GeForce RTX 4070 Ti SUPER, vendor `10de`, device `2705`, revision 161, flags/software 0,
driver `32.0.15.9579`, one node; all reported feature queries succeed. LUID was `00000000:0001307b` for this run.
Binding/conservative tiers were 3, ROV/wave support true, wave range 32–32 and raw RT tier 12. These values qualify
this census implementation on this tuple only, not a GitHub runner or every adapter.

Initial Ninja/native attempts were refused by the IDE coordination guard (inaccessible desktop or
`RPC_E_CALL_REJECTED`). The watcher had already generated the native target from CMake; building that existing
target in the matching desktop context succeeded with the guard enabled. No saved buffer, sync journal or guard
was discarded. The final driver-reporting edit was rebuilt/retested through that same native target.
