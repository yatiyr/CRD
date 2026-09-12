# Large C++ development and CI — Cerid recommendations

<!-- doc-role: reference -->
> Dated research and proposed implementation contract, 2026-09-12. Live state: [ROADMAP](../ROADMAP.md).
> Rules: [AGENTS](../../AGENTS.md). Research owner: [REPO.DEV.0](../ROADMAP.md#slice-repo.dev.0).

## Verdict and scope

Cerid should keep C++20, CMake, its preset matrix, modular targets, Catch2/CTest and its source/IDE synchronizer.
The next investment is reliable, dependency-aware development: a short local feedback cycle, trustworthy CI
selection, reproducible toolchains, useful failure artifacts and explicit hardware qualification. A build-system
migration or a larger mandatory reading list would not address the observed failures.

The proposed programme repairs the currently observed verification failures and improves repository infrastructure.
It does not implement the hundreds of retained renderer, UI, physics or scientific features. Their existing master
rows remain authoritative. A clean infrastructure close means all declared infrastructure acceptance tests pass;
it cannot establish that arbitrary future code is bug-free or that untested hardware is supported.

Confirmed requirements are human-only commits/pushes, scoped local verification, continued cross-platform design,
one roadmap and updated session/document evidence. The initial [REPO.DEV.1](../ROADMAP.md#slice-repo.dev.1) review subsequently approved tiered CI, existing hosted
CI/workstation only, independent work while human publication waits, and a stop at repository closure before renderer
review. The operational recommendations are the approved programme direction; unimplemented mechanisms remain
proposals until their owning children qualify. The session records the explicit loop authorization.

## Evidence boundary

The source census used commit `9045eebb5c072b6025343c67343a57a57c79bc85`, pushed to `main`, with a clean working
tree at investigation entry. There are 96 module CMake files under `engine/<family>/<module>`, 20 visible configure
presets and eight native MSVC configurations. Counts describe this revision, not permanent architectural limits.
The investigation inspected build/test orchestration, synchronizer ownership, documentation and completed CI job
logs; it was not a new exhaustive audit or execution of every engine algorithm.

Remote evidence is [run 34694952926](https://github.com/yatiyr/CRD/actions/runs/34694952926) at that revision and
[run 34688030261](https://github.com/yatiyr/CRD/actions/runs/34688030261) at
`2a82134759c25e5f7af5e9aaf0f53c10d3721955`. Both were still running when sampled. Completed jobs can be inspected
independently; an unfinished workflow is neither a pass nor a demonstrated hang. The
[session](../sessions/2026-09-12-large-cpp-research-and-loop-plan.md) records individual job IDs and diagnostics.
Temporary full logs are in ignored `build/research-dev-workflow-20260912/`; durable conclusions link the remote jobs.

## Lessons from large projects

The projects below have different products and infrastructure budgets. Their mechanisms are useful evidence;
their exact configuration counts, organizational processes and claimed speedups are not Cerid targets.

| Project | Primary-source observation | Application to Cerid |
|---|---|---|
| Chromium | Presubmit scripts select affected files and directory-owned checks. GPU infrastructure selects actual OS/GPU machines and separates some longer tests by capacity/change scope. [1][2] | Reuse module ownership for focused checks. Separate software-provider correctness from real-adapter qualification. Keep broad checks to catch interactions selection misses. |
| LLVM | Unit, regression and whole-program testing have distinct roles. CMake supports selected projects and independent compile/link/code-generation concurrency limits. [3][4] | Retain focused CTest targets; bound memory-heavy links separately. Validate optimized code generation and real consumers in CI. LLVM does not establish a universal rule that developers never run broad tests. |
| Firefox | Its build frontend controls configurations and output directories. Compiler caching can help warm builds, while the first cached build may be slower. Unified builds can hide missing includes and namespace coupling. [5][6] | Offer a thin common developer command and stable preset directories; measure caching. Keep standalone translation-unit/header checks and preserve a non-unified path. |
| Qt | Configure can select submodules plus dependencies; developer builds and distributable builds have different purposes. Cross compilation separates host tools from target binaries. [7] | Add dependency-closed module selections without removing full presets. Prove a downstream application outside the source checkout. Separate native cookers/code generators from target runtime artifacts. |
| Blender | Its CMake testing helpers support isolated suites and shared test binaries, carry test assets/environment explicitly, and distinguish performance executables. [8] | Keep existing module tests; package each suite's assets and required environment. Decide test process granularity using measured startup/runtime costs. No wholesale test-framework replacement. |
| Unreal Engine | Adaptive unity excludes actively edited files; build options can test headers independently and control PCH use. [9] | Optimize the edit/build path separately from clean builds. Use compile traces before considering unity or PCH restructuring; preserve actual Shipping/LTCG semantics. |
| Bazel | Hermeticity requires declared inputs/tools and avoiding build-time writes into shared source trees. [10] | Apply these properties to CMake. A Bazel migration is unnecessary to fix source-cache mutation, missing dependencies or machine-specific inputs. |

LLVM's own CI guidance additionally recommends explicit runner versions, action hashes, read-only defaults and
disabled checkout credential persistence. Cerid already sets workflow `contents: read`; preserve that strength
and address the remaining reproducibility gaps. An OS label pins an image family, not every installed tool. [11]

## Verified failures and source concerns

The census below describes the published investigation baseline. Subsequent local repairs and their limits are
recorded in [the CI environment session](../sessions/2026-09-12-repository-ci-environment.md); ROADMAP owns live state.

### Environment and strict analysis

The current Windows repository job requests `Visual Studio 17 2022`. Its recorded image is
`windows-2025-vs2026`, version `20260907.229.1`, and CMake reports no VS 2022 instance. The workflow hard-codes
that generator in both native fixture steps. GitHub announced the VS 2026 transition for `windows-latest` and
`windows-2025`; the job log directly confirms that transition affects this run. This is a fixture/environment
mismatch, not evidence that the native profile implementation fails to compile under its intended toolchain. [12]

All six Linux compiler jobs failed in setup. The inspected Debug and Shipping logs both show HTTP 403 from
`urllib.request.urlopen` while obtaining the pinned Vulkan validation archive. That prevents the engine tests from
starting. The checksum and exact-member extraction in [the installer](../../scripts/install-vulkan-validation.py)
are useful protections. The HTTP rejection's underlying endpoint/request cause remains unproven. Capture it and
qualify a supported acquisition/cache path; do not disable validation, accept an unverified mirror or simply retry
until green. Keep system drivers and the selected validation layer distinct.

Current strict-tidy CI reproduces two diagnostics in
[work_build.cpp](../../engine/execution/ceir-gpu/src/work_build.cpp): unnecessary `data()` before subscripting and
a nested conditional expression. These require a scoped source repair and actual LLVM-20 parsing, with warnings
remaining errors. The full workflow's unfinished status does not hide this completed failing job.

### GPU failure census

The preceding Windows Debug job reports seven failed tests among 6,760 discovered cases: three B18 hair/fur
scattering cases, the B17-c atomic A-buffer case with a reported segmentation exception, B1-f inner coverage,
RT-4 area-light NEE/MIS and CEIR-18p impostor engagement. These are real test failures on that recorded revision;
their mechanisms and applicability to the latest revision require targeted investigation.

The host video-controller census reports Microsoft Hyper-V Video, driver `10.0.26100.1150`. That is not an
authoritative record of the D3D12 adapter selected by each test. Capture selected adapter identity, software flag,
feature queries, compiler/driver versions, exact input/output, validation and device-removal diagnostics. Existing
software-adapter-specific floating-point bars must be examined rather than enlarged to manufacture a pass.
Likewise, `WARN` followed by an early return on missing devices can appear successful without executing the workload.
Mandatory qualification must separately count executed, unsupported, skipped and failed tests.

The repository runner API returned zero registered self-hosted runners. This does not inventory every computer
available to the project. Hosted software devices and this workstation cannot prove Intel/AMD/NVIDIA hardware
coverage or native Linux desktop presentation. Hardware requirements need named resources and explicit owners.

### Infrastructure observations

| Observation at the inspected revision | Implication and owning work |
|---|---|
| CI runs on main pushes and main-targeted pull requests; no scheduled/manual entry or concurrency grouping appears in the workflow. | Introduce deliberate trigger tiers and cancellation semantics; preserve full qualification runs. REPO.DEV.5. |
| Repository checks and costly compiler jobs start independently. | Cheap invalid-state detection should precede expensive work where useful; avoid making slow native fixtures unnecessarily block all fast CPU feedback. REPO.DEV.5. |
| Four ordinary configure presets have no direct CI invocation: Windows RelWithDebInfo, ShippingProfile, DebugScalar and Linux DebugScalar. Native fixtures cover some equivalent profile semantics, not whole-engine equivalence. `win-tidy-local` is a diagnostic variant, while `win-vs` needs native integration coverage. | Generate an explicit preset-to-tier coverage contract; classify aliases/diagnostics instead of inventing missing production lanes or quietly deleting presets. REPO.DEV.5. |
| GPU suites already share `RESOURCE_LOCK crd_gpu_device`; tooling and some expensive numerical tests already have timeouts. Full CI CTest commands lack a consistent bounded scheduling policy. | Reuse locks, audit their coverage and measure durations before adding resource-aware parallel execution. REPO.DEV.5. |
| CI caches dependency downloads, not compiler outputs. | Measure cold/warm compile and link costs before selecting cache settings. REPO.DEV.7. |
| Root CMake configures broad dependencies/modules; an ImGui Vulkan fix writes into its downloaded source tree. | Establish optional dependency closure and immutable source-cache patching. Concurrent corruption is a concern, not a reproduced failure. REPO.DEV.4 and REPO.DEV.6. |
| The synchronizer already reads CMake File API targets/dependencies. It derives dependency names by splitting opaque target IDs and filters interface targets. | Reuse shared primitives, resolve IDs through the codemodel's explicit mapping, and add interface/generated dependency handling for affected-build selection. Do not reuse structural filters as a complete build-dependency graph. REPO.DEV.3. |
| No public package-export/config helpers were found in the inspected root, module and CMake files. | Public separability needs an external consumer/relocation proof, not only a working sandbox. REPO.DEV.8. |
| Workflow permissions are read-only, but action tags float and checkout credentials retain their default behavior. | Pin reviewed action revisions and disable unnecessary credential persistence. REPO.DEV.6. |
| Existing build documentation can be read as requiring local Debug, ASan and additional platform/provider runs on every slice. | Replace that ambiguity with one risk-based local decision table and explicit CI obligations. REPO.DEV.2. |

The File API documents target IDs as opaque and supplies name/ID mappings; these are the appropriate interface
for dependency resolution. It describes the configured graph, not every conditional graph on other platforms. [13]

## Local development contract

### One short path for humans and agents

`python scripts/dev.py plan` now provides read-only selection ([contract](../design/developer-workflow.md),
[evidence](../sessions/2026-09-12-developer-selection.md)). `doctor`, `check` and `evidence` remain interface sketches
until their owning rows qualify. Implement a thin Python frontend over the existing configure/build/CTest/tidy/synchronizer
helpers. Do not create a second build engine or separately authored module inventory. It must also work without an
IDE, print exact underlying commands, support dry-run and machine-readable output, preserve exit codes, and let
developers invoke CMake/CTest directly.

`doctor` reports resolved compiler/CMake/Ninja/Python/SDK paths, preset eligibility, cache identity, runtime DLLs,
disk/RAM budget and synchronization conflicts. Detection is read-only; no automatic global installs or cleanup.
`plan` explains changed files, owning targets, affected consumers, tests/guards and extra risk-triggered lanes.
`check` executes that accepted scope sequentially where memory or device ownership requires it. `evidence` emits
revision/working-tree content identity, toolchain tuple, selected/executed counts, durations and result paths.

| Change | Normal local evidence | Additional scope when justified |
|---|---|---|
| Documentation only | Documentation/roadmap validation and scoped diff | Repository guard if layout/links/tool instructions changed; no engine compilation. |
| Python/CMake/IDE tooling | Changed-tool regression fixtures and a representative generated target | Native profile matrix for configuration semantics; Windows/Linux CI for portable tooling. |
| Private C++ implementation | Owning target, affected linked executables, selected CTests/guards and incremental tidy on the primary host | Sanitizer for lifetime/bounds; optimized configuration for optimization-sensitive code. |
| Public/template headers, generated APIs, ABI | Reverse-dependent compile/tests and a public-header consumer on the primary host | Windows/Linux compiler CI; scalar/SIMD and Shipping/LTCG where the contract is affected. |
| Allocators, fibers, concurrency | Focused lifecycle/adversarial cases and applicable instrumentation | Relevant sanitizer lane, scheduler stress and CI architecture coverage; validate instrument compatibility first. |
| CKIR/CEIR, shader/layout, GPU providers | The changed real provider path, CPU oracle and validation on available hardware | Affected backend/OS lanes in CI; local second backend/platform only to discriminate a mechanism or reproduce its failure. |
| Intrinsics, OS APIs, endian/packing, compiler/SDK/build flags | Explicit affected-platform risk analysis and the best available discriminating local test | Corresponding CI lanes are required before qualification. Common build changes broaden CI selection. |

One successful local configuration is normal iteration evidence. It is not full cross-platform certification.
Conversely, unavailable secondary hardware is not a reason to rebuild all unaffected local configurations.
If CI identifies an additional failure, capture that exact environment and narrow the next experiment accordingly.
No test exclusions, reduced warnings, weakened precision or repeated broad retries substitute for a fix.

### Dependency-aware selection

Start with source ownership and the configured CMake target graph; traverse reverse dependencies to consumers.
Include public/header-only targets, generated headers, code generators, fixtures, resources and authored assets.
Header include dependencies and asset runtime dependencies require augmentation; target links alone are incomplete.
Treat root build rules, toolchain flags, unknown files, deleted/renamed ownership, stale codemodels and unsupported
conditions conservatively. Explain the fallback instead of reporting an empty green selection.

During local work include tracked modifications, staged changes and relevant untracked files; do not assume
`HEAD` is the session baseline. CI selects against the actual event base/merge revision and records both identities.
Test the selector using temporary fixture repositories, including renames, interface edges, platform conditions,
generated outputs and schema/asset changes. A broad CI comparison must show no missed required test before
selection replaces an existing full gate. Retain an explicit full mode and periodic full coverage thereafter.

The project synchronizer remains the sole owner of bidirectional structural transactions. Planning must not
regenerate over unsaved IDE edits or incomplete transactions. Native configurations retain equivalent profile
semantics and file membership; compiler-specific environments remain separate CMake presets.

## CI design

### Three execution tiers with one coverage contract

**Tier 1: preflight.** Validate repository structure, documentation, workflow syntax, generated CEIR/capability
artifacts, profile contracts and changed tooling. Emit the affected-target plan and expected matrix before heavy
work. A failed required preflight cannot be hidden by skipped dependent jobs.

**Tier 2: change qualification.** On pushes/PRs, run affected Windows MSVC and Linux GCC targets, strict analysis,
relevant sanitizers and contract-triggered optimized/backend tests. Preserve stable required-check names with an
aggregate that verifies every expected job actually ran and passed. A docs-only event still emits a meaningful
required result. A changed workflow/build system qualifies itself with conservative scope.

**Tier 3: complete qualification.** Scheduled/manual and explicit cluster/release checks cover every supported
non-diagnostic preset, full suites, native solution profiles, real hardware and clean consumers. Long numerical
and performance workloads receive dedicated resource budgets. A fast Tier 2 pass does not close a gate requiring
Tier 3. Version the tier mapping alongside presets and fail validation if a supported preset has no owner.

Use workflow/ref-specific cancellation for superseded ordinary feedback runs. Preserve designated full qualification
and release evidence so frequent pushes cannot starve it. GitHub supports concurrency groups and conditional
in-progress cancellation; configuration must distinguish these intents. [14]

### Scheduling and evidence

Retain existing GPU locks. Introduce suite labels and measured process/memory budgets, then bounded CTest parallelism
for independent CPU work. Distinguish CTest processes from each test's internal worker pool. GPU locks apply inside
one CTest invocation; separate CI jobs or local processes need runner/device exclusivity too. Use workload-specific
timeouts and capture stack/device diagnostics before concluding that a timeout identifies an engine deadlock.

CTest already offers JSON discovery, labels, JUnit output and resource allocation in the documented baseline.
Use these supported interfaces instead of parsing human progress lines. Record zero matches as failure when work
was expected. Preserve fixture dependencies and resource locks while sharding. Runtime-derived durations inform
shards; do not split solely by alphabetical count. [15]

Artifacts should contain the selected revision/tree hash, workflow/preset/toolchain fingerprints, selected adapter,
test inventory, JUnit, first-failure logs, relevant emitted IR/shaders and minimized inputs or dumps. Retain assets
needed for reproduction with content hashes. Produce a concise job summary and machine-readable conclusion without
requiring agents to download every successful test log. Refresh URLs and summaries when the published revision changes.

Shared hosted-runner timings remain diagnostic; existing soft performance budgets must not become performance
qualification. Dedicated measured hardware owns hard performance comparisons and full boards in `docs/bench/`.
Report skips and missing capabilities separately. A provider's software fallback can qualify its declared correctness
contract, never hardware throughput or all physical adapters.

## Build speed, reproducibility and module consumption

### Dependency closure and host tools

Add opt-in module selection that closes over dependencies while preserving the default full configuration and all
presets. Prove a numerics/foundation consumer can configure without graphics SDKs and an unrelated physics link.
Renderer selections pull their actual execution/GPU/resource dependencies automatically. A missing required edge is
an actionable configure error. Disabled modules are omitted deliberately, not represented by empty integration stubs.

Cookers and reflection/IR generators run on the host; their outputs target a separately declared platform/schema.
Declare dependencies, output/byproduct paths and tool versions. Avoid timestamps or absolute host paths in semantic
asset identity. Verify incremental rebuilds after editing inputs and a no-op rebuild without edits. This groundwork
supports later browser/macOS cross compilation without claiming those runtime platforms qualify now.

### Pinned inputs and cache integrity

Centralize reviewed SDK/tool/dependency pins, hashes and license provenance. Share acquisition helpers across CI
lanes; allow verified local archives for offline setup. Extract/install transactionally, detect partial/corrupt
downloads and mismatched tools before compilation. Keep patch inputs versioned and apply them to a build-owned copy
or a distinct content-addressed patched source, never a shared downloaded tree. Test two configurations consuming
one source cache without writers changing each other's inputs.

Preserve read-only workflow tokens, use reviewed immutable action hashes, disable persisted checkout credentials,
and separate untrusted PR caches/artifacts from privileged consumers. Do not connect the development workstation
as an unrestricted public-PR runner. Hardware workers need explicit trust boundaries and cleanup; repository
content, tool output and downloaded artifacts never expand authority. These recommendations follow GitHub's
documented workflow threats and mitigations. [16]

### Measure compiler caching and parallelism

Trial a pinned compiler cache on Ninja MSVC/clang-cl and Linux GCC before changing defaults. Measure cold build,
warm rebuild, single-source edit, public-header edit, configure, link, cache hit/miss reasons, RAM and disk. Record
the entire board, including regressions. Bound compiler and linker parallelism independently using measured memory
pressure. Keep debug symbols, `/WX`, sanitizers, ISA levels and Shipping/LTCG behavior intact.

Sccache documents MSVC debug-information constraints and CMake policy/settings for embedded debug information;
PCH and actual selected versions need verification. CMake's compiler-launcher property supports Make/Ninja, so
setting it alone cannot establish native Visual Studio caching. Native MSBuild integration requires its own proof;
otherwise retain a truthful uncached IDE path. Do not publish Mozilla's cache speedups as Cerid measurements. [17][18]

Global unity builds, C++20 named-module migration, distributed compilation and a Bazel/GN replacement are not the
first implementation steps. Keep them as measured options if profiling later justifies their integration cost.
Header fan-out, unwanted dependency setup, repeated linking and poor test scheduling are more directly evidenced
here. Any optimization must retain standalone source correctness and debugging ergonomics.

### Public consumption and stronger instruments

Add self-contained public-header compile checks without PCH/unity for selected changed surfaces, plus a periodic
complete lane. Enforce public/private include and dependency boundaries. Export a small real Cerid package first,
install it, move it, and compile/run a downstream consumer outside both source and build trees; extend dependency
closure to selected module families. Test static/shared constraints and generated headers per supported profile.
Relocatable CMake packages must not embed source-machine dependency paths. [19]

Create reusable bounded fuzz targets for existing CEIR text and CKIR graph/binary ingestion, preserving malformed
inputs and minimized regressions. Extend the same harness when later consumers arrive. LibFuzzer provides a
coverage-guided in-process entry point and persistent corpus workflow; inputs need size/time/allocation limits. [20]
Audit ASan/UBSan and concurrency instrumentation before adding lanes. ThreadSanitizer exposes explicit fiber
create/switch/destroy interfaces; a custom fiber scheduler requires a verified model, including the intended
happens-before relationships. A silent uninstrumented run is not a concurrency proof. [21]

The immediate infrastructure slice qualifies the test instruments and current ingestion consumers. It does not
authorize rewriting allocator/job algorithms or pulling geometry/physics development ahead of their owning rows.
Any discovered real defect gets an explicit owner and a scope check if its repair crosses those boundaries.

## Documentation and ownership

Keep `START_HERE` as the route, `AGENTS` as conduct, `BUILDING` as commands/verification, `context` as the current
pointer and `ROADMAP` as the only state table. Describe the local risk table once in BUILDING and link it everywhere
else. A session records only actual changes, commands, evidence and the next discriminating action. Update affected
facts rather than rewriting every document or rereading all historical research each session.

Add a compact contribution path for a module, bug fix, asset/program and build-system change, reusing existing
templates and systems index. Ownership means a public module/contract and a human review route; do not fabricate
teams or grant agents merge rights. Validate code-generated documents, example commands, case-sensitive paths and
single-roadmap references. Dependency/security scans and license manifests supplement, not replace, review.

**Agents never commit or push, including through helper scripts, automation, bots or delegated work.** They prepare
reviewable diffs and a suggested Conventional Commit message. A machine may record content hashes and patch bundles
for provenance; that does not create a Git commit or publish changes. Human publication is an explicit boundary.

## Execution and finite close

Every task is a child in the existing master table. The sequence is environment/tidy repairs, GPU diagnosis and
repairs, fast local instructions, affected selection, module closure, CI tiers, dependency integrity, measured build
speed, public consumers/instruments, documentation and exact-revision qualification. Work may continue on an
independent child while another waits for hardware or publication; that gate remains open and visible.

The proposed unattended loop reads the current pointer and exact row, inspects workspace/synchronizer/CI state,
performs one bounded increment, runs its scoped checks, writes evidence and updates affected docs. It re-anchors
after every increment. Preserve human edits, cap host load, never run full local matrices, never commit/push,
never relax an oracle and never infer approval of ADR-0107/RAH-0. No purchases, runner registration or publication
settings changes are inferred from a general quality goal. Avoid duplicate loops and overlapping writers.

After initial decisions, routine implementation choices proceed without repeated questions. A new material boundary
gets recorded; continue other authorized work. Quiet monitoring resumes CI inspection when the human publishes.
An old revision's green result cannot qualify a changed working tree. If all remaining work requires publication,
hardware or an explicit architectural decision, report that condition rather than manufacturing completion.

Closure requires all current confirmed failures resolved with regressions; every selected supported preset assigned
and exercised at its tier; selector safety tests and broad comparison; native configuration/synchronizer proofs;
measured development improvements; clean dependency/consumer/reproducibility checks; accurate compact docs; and
the required full CI results on the human-published candidate revision. No arbitrary speedup percentage is declared
before measurement. The loop then stops at repository closure, unless a later explicit decision expands its scope.

The initial decisions are: tiered versus every-push-full CI; publication handoff while unattended; available GPU
resources/cost boundary; and this finite repository boundary. They do not reopen human-only commit policy.

## Sources

Primary material retrieved 2026-09-12. Moving documentation/source branches describe the retrieved revision;
refresh version-sensitive details when implementing. CMake 3.25 references deliberately match the declared base
tooling; native VS 2026 has its separate CMake 4.2+ requirement. Inline numbers identify these source notes.

1. [Chromium — Presubmit Scripts](https://www.chromium.org/developers/how-tos/depottools/presubmit-scripts/): affected-file checks and directory ownership; selective checking has limits.
2. [Chromium — GPU Testing](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/gpu/gpu_testing.md): hardware pools, scoped try jobs and real test executables.
3. [LLVM — Testing Infrastructure Guide](https://llvm.org/docs/TestingGuide.html): unit/regression/whole-program roles and focused execution.
4. [LLVM — Building with CMake](https://llvm.org/docs/CMake.html): selected projects, job pools, memory limits and host tools.
5. [Firefox — Configuring Build Options](https://firefox-source-docs.mozilla.org/setup/configuring_build_options.html): frontend/configuration/output isolation and caching costs.
6. [Firefox — Unified Builds](https://firefox-source-docs.mozilla.org/build/buildsystem/unified-builds.html): hidden include dependencies and changing unity groups.
7. [Qt — Configure Options](https://doc.qt.io/qt-6/configure-options.html): submodule dependency closure, developer builds and host/target separation.
8. [Blender — CMake testing helpers](https://raw.githubusercontent.com/blender/blender/main/build_files/cmake/testing.cmake): suite granularity, test environments/assets and performance targets. Handbook fetch was unavailable; the primary implementation was read instead.
9. [Epic — Unreal Build Configuration](https://dev.epicgames.com/documentation/en-us/unreal-engine/build-configuration-for-unreal-engine): adaptive unity/PCH and independent header compilation.
10. [Bazel — Hermeticity](https://bazel.build/basics/hermeticity): declared inputs, source-tree writes and reproducibility checks; principles adopted without a build-system migration.
11. [LLVM — CI Best Practices](https://llvm.org/docs/CIBestPractices.html): runner/action pinning, permissions and checkout credentials.
12. [GitHub runner-images — Windows VS 2026 transition](https://github.com/actions/runner-images/issues/14017): announced runner-label migration, independently confirmed by Cerid's job logs.
13. [CMake 3.25 — File API](https://cmake.org/cmake/help/v3.25/manual/cmake-file-api.7.html): codemodel, opaque target identifiers, dependencies and versioned queries.
14. [GitHub — Workflow concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency): grouped and conditional cancellation.
15. [CMake 3.25 — CTest](https://cmake.org/cmake/help/v3.25/manual/ctest.1.html): JSON discovery, JUnit, labels, fixtures, resource allocation and bounded execution.
16. [GitHub — Secure use reference](https://docs.github.com/en/actions/reference/security/secure-use): action hashes, untrusted code/artifacts, token permissions and ownership review.
17. [Mozilla — sccache](https://github.com/mozilla/sccache): supported compilers, MSVC debug information, cache behavior and limitations.
18. [CMake 3.25 — Compiler launcher property](https://cmake.org/cmake/help/v3.25/prop_tgt/LANG_COMPILER_LAUNCHER.html): generator support boundary.
19. [CMake 3.25 — Packages](https://cmake.org/cmake/help/v3.25/manual/cmake-packages.7.html): exported targets and relocatable consumers.
20. [LLVM — libFuzzer](https://llvm.org/docs/LibFuzzer.html): coverage-guided input harness, corpus and instrumentation.
21. [LLVM compiler-rt — ThreadSanitizer interface](https://raw.githubusercontent.com/llvm/llvm-project/main/compiler-rt/include/sanitizer/tsan_interface.h): fiber lifecycle/switch interfaces and synchronization semantics.

Used by: [research and loop planning session](../sessions/2026-09-12-large-cpp-research-and-loop-plan.md).
