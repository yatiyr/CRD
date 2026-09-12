# Previous orientation documents

<!-- doc-role: archive -->
> Historical reference; old status/schedules/grants are not current instructions. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Historical source record. Current rules: [AGENTS](../../AGENTS.md); current status/order: [ROADMAP](../ROADMAP.md).
Old loop grants, Next lists, grades and build/format instructions below are not current instructions.

<a id="agents"></a>
## Source: AGENTS.md

<a id="agents-cerid-engine--agent-rules"></a>
### Cerid Engine — Agent Rules

> Rules of engagement for any AI agent (Claude Code, OpenCode, etc.) working on Cerid.
> Concise by design. **Build/test commands + verification + troubleshooting** → `docs/BUILDING.md`. **Module index** → `docs/systems/README.md`. **Live state** → `context.md`. **Master plan** → `docs/ROADMAP.md`. **Memory index** → `MEMORY.md`.

<a id="agents-what-is-cerid"></a>
#### What is Cerid

Cerid is a **general-purpose C++20 real-time engine substrate**. Games are one consumer; **simulation (incl. robotics), medical visualization, DAW-class creative tools, CAD/CAM, and offline cinematic pipelines** are equal-class consumers. The architecture serves all of them; no domain is privileged.

Module inventory — the source-backed per-module index is **`docs/systems/README.md`** (this file does NOT carry a
module inventory; a second copy of it always rots). The broad strokes: the foundation
(`core/log/vm/memory/containers/math/units/time/platform/app/config/jobs/perf/resources/scene/imgui/profile`), the
GPU platform (`gpu-context{,-vulkan,-dx12,-cuda}` + `kir` + the RAF render-asset stack + `draw` — the former
`rhi/renderer/shader` stack is **retired**, ADR-0105), 13 `geometry-*` sub-modules, and the `crd-hesap` numerical
substrate (v0–v16 shipped: dense → sparse → eig → opt → ODE → FFT → DSP/wavelet/comms → special/stats →
interp/quad/diff/motion → tensors → autodiff). **The current focus lives in `context.md`, never here.** Planned:
`eylem` resume (paused at v1b) / `sdf` / `font` / `audio` / `scripting/language (Cerid-owned CEIR/CHIR + CR-D007 visual + first-class C++ hot-reload, ADR-0108)` / `ui` / editor (CR-D007).

<a id="agents-engineering-principles-non-negotiable"></a>
#### Engineering Principles (non-negotiable)

Pinned in `docs/PRINCIPLES.md`. Don't re-litigate; deviations require an ADR.

- **Modular by default.** Every subsystem is a separable module with a clear public surface. A DAW build that doesn't need physics/animation must omit them at link time.
- **Vertical slice over horizontal completeness.** Walk a small path end-to-end before widening.
- **Authoring text, runtime binary.** Human-edited data is TOML/JSON/glTF. Engine-consumed data is cooked binary. Runtime never imports source assets.
- **One-way module dependencies.** Cycles are bugs. New edges require review.
- **Real workload before optimization.** No SIMD, fiber, GPU-allocator, ECS rewrite, or render-path swap without a measured baseline and target.
- **API stable across backends.** Public surfaces (RHI, physics, audio, renderer) are designed assuming multiple implementations even when only one exists. Vendor types do not leak.
- **Tak-çıkar (plug-out) third-party.** External deps (glslang/shaderc, spirv-reflect, ImGui, toml++) sit behind Cerid-owned interfaces. Core surfaces (renderer, eylem physics, audio) are Cerid-native — no vendor wraps.
- **Determinism is optional but reachable.** Fixed-step physics, deterministic RNG, replay-friendly event log. Not the default; never out of reach.
- **No owning STL containers in engine/tool code.** Use `crd::containers::Array`/`String`/`HashMap`. Non-owning views (`StringView`, `ConstSpan`) and `<algorithm>` functions are permitted.
- **Two-layer typed architecture (ADR-0078 §5).** Every public API / ECS field / config / cooker / UI uses `Quantity<D, T>`. SIMD / math inner ops / geometry algorithm bodies / numerical kernels / GPU writes stay raw `f32`/`f64`. Bridges at the API surface only (`.value`, `to_raw_vec`, `from_raw_vec`, strip-compute-retag).
- **The engine is allowed to be slow before it is allowed to be wrong.**

<a id="agents-agent-conduct-non-negotiable"></a>
#### Agent Conduct (non-negotiable)

Rules for AI agents working on Cerid. As binding as the engineering principles.

- **⛔⛔⛔ FOLLOW THE PURPOSE — RE-ANCHOR EVERY PROMPT (user 2026-08-15, in anger).** Every task serves a purpose; NAME it before you act and RE-READ it each prompt. The purpose of the entire CEIR line is one thing: **everything executable is an authorable asset CEIR** — the host/renderer knows NOTHING about the algorithms it runs. You may drift against the purpose mid-task; the discipline is to CATCH AND CORRECT yourself every prompt, never to finish the wrong thing well. (Scar: I built a Forward+ light cull as a hand-written C++ `KGraph` under a slice literally titled "as an authored asset" — see the authored-programs rule below + `feedback_everything_is_an_authorable_asset_ceir`.)
- **Never silently reduce a slice's scope.** The phase doc row + relevant ADR sections + the prior session log's "Next" pointers define the contract. If you think a deliverable should be deferred, surface it as a scope-check question to the user BEFORE writing code. Wait for confirmation.
- **Treat "elite" / "no shortcuts" as a quality multiplier, not a scope reducer.** Ship the proper architectural choice even when the slice could ship with less.
- **NEVER defer failures to debt — SOLVE them.** A red test/config blocks the slice. Bisect, root-cause, fix. "Pre-existing" is not a defense. (Memory: `feedback_never_defer_solve`.)
- **Every measured benchmark board is written to `docs/bench/` AT MEASUREMENT TIME** as its own file (convention + naming in `docs/bench/README.md`: machine/config, tracked harness path, the FULL peer board incl. losses, verdict line). Session logs and phase tables LINK to the bench file, never restate the table. Part of the DoD for any perf/crush claim.
- **⭐ STUDY SOMETHING → WRITE A RECIPE.** Whenever we study a technique, algorithm, method, or device feature from a paper (or several) and turn it into working code, write an **educative recipe** to `docs/recipes/` (convention in `docs/recipes/README.md`). A recipe TEACHES the subject end to end so that a human or an agent who reads it once understands it completely — **PARAMETERS FIRST** (a full table of every knob: meaning, units, default, range), then the physics/maths (papers cited precisely), the full assembly (rebuildable from the doc), the traps (every scar, why it happened, the symptom), the measured numbers (link the bench board), and where the code lives. The bar: *if I read the recipe, everything must be fully understood.* This is knowledge capture, distinct from a session log (what we did) or a bench board (the numbers) — it is the LESSON. Not optional for anything we genuinely learned; skip it only for trivial mechanical work. (Origin: 2026-07-21, the hair renderer — offline + real-time recipes.)
- **Numerical/perf work: FULL crush, no deferrals, never accept near-parity.** Every benchmark carries the FULL peer board (scipy + MATLAB + Boost + **GSL** — install the missing peer; state N/A *with the check*, never drop a column). A measured loss OR a *tie* vs a reference library is an OPEN bug, not a closed slice (SANITY #9): parity-with-the-same-algorithm is never the wall — a per-operation cost (a heavy `pow`, a per-call recompute of integrand-independent nodes/weights/error-coefficients) always is, and precomputing it once flips the loss. **Reconstruct-and-verify-in-python FIRST** — fetch the reference's actual source (`gh`: scipy `__quadpack.c`/`_interpnd.pyx`, QUADPACK constants) + verify the algorithm bit-exact before porting one C++ line. (Memory: `feedback_full_victory_beat_all_gold_standards`, `feedback_bench_all_peers_never_cherry_pick`; SANITY Ledger 2026-06-30.) **The recurring crush levers + traps live in `docs/hints/crush-playbook.md` (living) — read it before a crush, and APPEND the new lesson after one.**
- **⛔⛔ KERNEL/PERF WORK IS GOVERNED BY `docs/KERNEL-CRUSH-MANDATE.md` — BINDING ORDERS, read it before touching a hot kernel.** THE PRIME LAW: **if a peer (cuBLAS/MKL/cuDNN/oneDNN/a published kernel) reaches a number on the SAME hardware, that number is PROVEN achievable — "impossible" / "we can't reach it" is FORBIDDEN; the gap is YOUR implementation's shortfall until measured otherwise; you do not stop until PARITY or CRUSH.** You may NOT invoke a "wall / nerf / ceiling / SASS-limit / memory-wall / diminishing-returns" to stop UNTIL you have reproduced the best public hand-written result, still have a gap, AND empirically proven the limit (Order 3). Pin the peer target FIRST, profile-then-fix-the-one-limiter, exhaust every lever, reverse-engineer + deep-research the peer on plateau, autotune. Honest = keep going, never = declare the crush impossible. (Memory: `feedback_never_invoke_a_wall_a_peer_already_beat`. Origin: 2026-07-08 GEMM chase — I stopped at 58% of peak citing a "wall" that public CUDA-C beats at ~90%.)
- **Substrate work ships proactively; speculative paths defer.** Filed follow-ons with settled designs + cheap tests ship in-line when the harness is fresh. Follow-ons with unsettled design tradeoffs only a consumer can resolve defer until that consumer arrives. (Memory: `feedback_ship_at_consumer_template_from_day_one`.)
- **Document paper-divergence explicitly.** When implementing a canonical algorithm with a different sub-step (D124 SAT-vs-Mamou-centroid, D129 voxel-fraction-vs-Hausdorff, D94 super-tet ordering), pin the divergence as a numbered Dxxx + rationale paragraph in the ADR amendment + system doc.
- **Append new pure-virtuals at the END of an interface.** Inserting in the middle shifts vtable slots and silently dispatches to the wrong method in win-release LTCG. (Memory: `feedback_vtable_stability_append_at_end`. Case study: rhi-compute v0-close SEGV 2026-05-17.)
- **⛔⛔⛔ HARD RULE (user, 2026-07-25, RESTATED IN ANGER AFTER I BROKE IT) — WE WILL ONLY USE OUR AUTHORED
  FRAME GRAPHS.** *"THAT'S THE WHOLE REASON WHY WE BUILD THE SYSTEM! WHY WE DID THAT! ... PLEASE WRITE THAT RULE
  EVERYWHERE YOU CAN! WE WILL ONLY USE OUR AUTHORED FRAME GRAPHS!"*

  **Every rendering technique ships as an AUTHORED FRAME-GRAPH ASSET (`.frame.toml` → cooked `.crdr`), loaded and
  executed by `execute_frame_graph`.** Not as C++ that builds passes. Not as a `draw_*` sequence. An **asset**.
  Shadow cascades, sky bakes, IBL prefilter, HDR/tonemap chains, TAA resolve, GI, OIT, editor overlays, picking,
  debug viz — every one of them. The engine's own rendering ships in the **built-in pack** that mounts first;
  applications override a technique purely by shadowing its name.

  ⛔ **`FrameGraphBuilder` (programmatic construction) is for TESTS, node editors, and runtime-generated graphs
  ONLY.** It is NOT an acceptable way to ship an engine technique. An earlier, weaker wording of this rule said
  the two provenances were "equal and interchangeable"; that was read as licence to hardcode, and REN-3.2-b's CSM
  was built as C++ in `scene_renderer.cpp` while REN-36's whole authoring stack sat unused beside it. **That
  wording is retracted.** Interchangeable means *the asset path must produce identical pixels* — it does not mean
  you may choose the C++ path for engine features.

  **The test, applied to any rendering work before it is called done:** *can a user change this technique — pass
  order, resource formats, cascade count, an inserted pass — by editing an asset and WITHOUT recompiling the
  engine?* If no, it is not done. A step that genuinely cannot be expressed is a missing **`FramePassKind`**: add
  the kind with its own gate, then author the technique on top of it. Never route around the system.

- **⛔⛔⛔ THE SAME LAW BINDS THE PROGRAMS, NOT JUST THE GRAPHS (user 2026-08-15, in anger).** The authored-frame-graph
  rule above governs pass STRUCTURE; this one governs the PROGRAM each pass runs.
  **⛔ FORM (2026-08-15, ESCALATED — user: "CEIR AND CKIR must be authorable"):** the authorable form is the IR
  ITSELF — a CEIR program (`crd::ceir` `parse`/`print`) or a CKIR `.ckir` NODE-GRAPH asset (`ckir_write`/`ckir_read` — a node-editor-readable TOML `[[node]]`/`[[stmt]]` graph) plus
  the KGPH `ckir_serialize` blob). Cookers/declarations (`.crdl`/`.crdv`/`.crdp`) and C++ builders are NON-PRIVILEGED
  FRONTENDS that all emit the SAME canonical IR (tracker §38-39); a C++ builder may exist ONLY as a BOOTSTRAP whose
  PRINTED output is committed as the asset, NEVER as the runtime source. **"Declaration + cooker" as the TERMINAL bar
  is RETRACTED.** Every rendering/compute ALGORITHM —
  lighting, culling, deferred, post, TAA, HZB, GI, skinning, Forward+/clustered — ships as an **authorable CEIR/CKIR
  asset cooked disk-first** (a declaration on disk + a reusable cooker that builds the graph + a memoized disk-first
  cook in the host), **loaded by default for every application, and partially modifiable OR completely replaceable by
  an app asset that shadows its name — without recompiling the engine.** `scene_renderer` (and any host) KNOWS NOTHING
  ABOUT RENDERING: it cooks-from-disk and executes; it NEVER hand-builds an algorithm graph. A `ensure_*_program` /
  `ensure_*_kernel` that writes `KGraph` nodes in C++ is the anti-pattern (exemplar: `ensure_deferred_lighting_program`);
  every one is a conversion+deletion target (DELETION IS THE PROOF). C++ / CEIR-from-C++ is allowed only as the COOKER
  MECHANISM, never as the shipping form. **The test before "done":** can an application replace this algorithm by
  editing or shadowing an asset, WITHOUT recompiling the engine? If no, it is not done.
  **⛔⛔⛔ THIS IS THE GOLD-STANDARD DoD FOR EVERY CEIR SLICE + THE LOOP (user, reaffirmed relentlessly 2026-08-15):** every
  C++-written CEIR and CKIR is CONVERTED to a disk-read asset (a `.ckir` NODE-GRAPH kernel via `ckir_write`/`ckir_read`, a
  CEIR program via `.frame.toml` / `crd::ceir` `parse`) and the hand-written builder is DELETED. A CEIR slice does NOT
  close, and a loop tick does NOT advance past it, while any program is still hand-built in engine C++. (Memory:
  `feedback_everything_is_an_authorable_asset_ceir`.)

  **Why this is not negotiable:** the entire REN-36 investment exists so applications, tools, and agents can
  invent rendering techniques (deferred, Forward+, NPR, whatever) without engine changes. Every technique
  hardcoded in C++ is one the authoring system provably cannot express, and it turns the contract into a lie.
- **No dual code paths for "demo" vs "real" content.** When the sandbox uses the engine, it goes through the same surface a downstream consumer would. If a legacy path exists, the slice adding the new path replaces the legacy — does not run alongside it.
- **Hook-based contracts > explicit-call APIs.** When a prior slice left a cleanup contract for a follow-up to pin (e.g. per-component drop callback), build the proper hook. Don't paper over with an explicit-call API the consumer remembers to invoke.
- **Stub targets are not integration.** A consumer (e.g. `IPresetTarget`) must consume at least one real field that drives observable behaviour. "Display the value in ImGui" is observability, not integration.
- **Phase doc deliverables are the contract.** Aspirational-sounding lines remain in scope until the user explicitly defers them. Author the TOML, wire the cooker, ship the UI.
- **Call `advisor` on every non-trivial slice plan before implementing.** Catches silent-narrowing reliably.
- **⛔⛔ Iterate locally; the WHOLE-REPO sweep is CI's job (user, 2026-08-15, after I burned hours on it).** Build + run ONLY the module(s) you changed + their blast radius (`build-target.bat build/win-debug <target>` → the specific `[tags]`/named tests + the WSL Linux legs for GPU code). **Never** run `per-slice-check.ps1` / `full-sweep.ps1` over the whole repo on this host — a whole-repo sweep is a multi-hour job CI parallelizes on dedicated hardware. Still FIX every bug you see (the scope rule is about not re-running untouched modules, never about ignoring a defect). Bound any local GPU run with `ctest --timeout N`. (Memory: `feedback_whole_repo_build_and_test_is_cis_job_not_local`; BUILDING.md §Per-slice.)
- **⛔⛔ Verify your instrument before you claim a defect (user, 2026-08-15, after I FABRICATED one).** A surprising failure is YOUR harness until proven otherwise — never report failed / hung / broken / timeout, to the user OR in a doc, before ruling out the tooling; an unverified defect written as fact is a lie (worse than the misread, in a never-disguise-failure repo). *Slow ≠ hung*: compute expected duration (tests × per-test) and check CPU-climb on the RIGHT process before calling a hang; never kill a run you haven't proven wedged. Known phantoms: `Select-Object -First` → fake exit 255; win-asan exe w/o the ASan DLL PATH → `0xC0000135`; raw clang-tidy on an MSVC PCH → false "0 warnings". (SANITY #11; Memory: `feedback_powershell_select_first_kills_native_exe_exit_code`.)

<a id="agents-tech-stack"></a>
#### Tech Stack

- C++20, no compiler extensions
- CMake 3.25+ with Ninja generator (CMakePresets.json)
- MSVC 2026 (primary; VS 18), clang-cl (verified in CI), GCC (Linux in CI)
- Test framework: Catch2 v3 (via CPM.cmake)
- Format: clang-format (`.clang-format` in repo root)
- Lint: clang-tidy (`.clang-tidy` in repo root); `WarningsAsErrors: '*'` flipped 2026-05-17
- MSVC `/Zc:preprocessor` required (for `__VA_OPT__` in log macros)
- Config substrate: `toml++` (single-header, exceptions-free mode)
- Graphics: GLFW 3.4 + Vulkan 1.3 + shaderc + spirv-reflect
- Debug UI: Dear ImGui (docking, v1.92.0)

<a id="agents-build--test"></a>
#### Build & Test

Full reference in `docs/BUILDING.md`. Quick reminders:

```powershell
cmake --preset win-debug && cmake --build --preset win-debug && ctest --preset win-debug
clang-format -i <file>
clang-tidy -p build/win-debug <file>
```

Per-slice DoD helper — ⛔ **this is the CI recipe; do NOT run whole-repo on this host** (see Agent Conduct "Iterate locally"). Locally use `build-target.bat build/win-debug <target>` + only the tests you changed:

```powershell
.\scripts\per-slice-check.ps1 -Parallel                  # 4-config (CPU slices)   — CI
.\scripts\per-slice-check.ps1 -IncludeRelease -Parallel  # 5-config (GPU-sensitive) — CI
.\scripts\full-sweep.ps1                                 # 18-config (cluster close) — CI
```

<a id="agents-project-structure"></a>
#### Project Structure

```
engine/<module>/
    include/crd/<module>/    public headers (.hpp for C++, .h for C-only)
    src/                     implementation
tests/<module>/              Catch2 tests + CMakeLists.txt
runtime/                     startup skeleton + crd-sandbox
runtime/examples/            per-module smoke executables (smoke_log, smoke_memory, ...)
runtime/configs/             authoring TOML configs (imgui_layer.toml, ...)
runtime/examples/shaders/    GLSL sources cooked to SPIR-V at build time
docs/ROADMAP.md              master plan: phases, decision log, detour queue
context.md                   live "where we are now" (project root)
docs/PRINCIPLES.md           engineering principles + pinned cornerstones
docs/phases/<phase>.md       one file per phase
docs/design/<slice>-*.md     per-slice IMPLEMENTATION SPEC (reuse audit + increments + gates);
                             the slice's ROW links it; index at docs/design/README.md
docs/sessions/               one file per session (YYYY-MM-DD-<slug>.md)
docs/systems/                one short overview per shipped module
docs/decisions/<NNNN>-*.md   per-decision ADRs; index at decisions/README.md
docs/research/               research dossiers
docs/debt.md                 open follow-on slices + known cleanup
docs/detours/                side-mission registry (D-NNN-*.md)
docs/<module>/<MODULE>_FILE.md  long-form deep-dive for major modules
MEMORY.md                    external agent memory index (location in docs/README.md; not a root file)
```

<a id="agents-coding-standards"></a>
#### Coding Standards

This section is CANONICAL (enforced by `.clang-format` / `.clang-tidy`):

| Element        | Style       | Example                        |
| -------------- | ----------- | ------------------------------ |
| Namespace      | lower_case  | `crd`, `crd::detail`           |
| Class / Struct | CamelCase   | `LogManager`, `Vec3`           |
| Enum / value   | CamelCase   | `LogLevel::Trace`              |
| Function       | lower_case  | `platform_name()`              |
| Variable       | lower_case  | `max_size`                     |
| Member         | m_lower_case | `m_name` (m_ prefix)          |
| Constexpr var  | lower_case  | `default_capacity`             |
| Global const   | kCamelCase  | `kMaxLogFiles` (k prefix)      |
| Template param | CamelCase   | `ValueType`                    |
| Macro          | UPPER_CASE  | `CRD_ASSERT`, `CRD_OS_WINDOWS` |

**Style:** Allman braces · 4-space indent · 120-char column · pointer-left · `#pragma once` · include order project → `<crd/...>` → `<...>`.

**Hard rules:** RAII only; no raw `new`/`delete` · `std::span` / `std::string_view` / `std::optional` over raw pointers · `noexcept` on moves + dtors · `[[nodiscard]]` on factories/accessors · Concepts/`requires` over SFINAE · no `using namespace` in headers · containers take `IAllocator*` as constructor arg, not template parameter · no commented-out code · no TODOs unless asked · `CRD_ASSERT`/`CRD_VERIFY` for assertions · `static_cast<T>(literal)` for non-exact-representable defaults in `<MathScalar T>` template code (avoid `T{double_literal}` — gcc-linux `-Wfloat-conversion -Werror`).

<a id="agents-architectural-cornerstones"></a>
#### Architectural Cornerstones

Pinned decisions from `docs/decisions/`. Don't re-litigate; circumstances change → new ADR.

- **The graphics layer IS `crd-gpu-context` (ADR-0103, ADR-0105 — supersedes the ADR-0001/0080 rhi split).** One
  device, one facade, one IR: `crd-gpu-context` owns every GPU program + pipeline; no module outside a backend names
  a shading language or a bytecode (I1/I2 grep gates). The former `crd-rhi`/`crd-rhi-vulkan`/`crd-rhi-compute`/
  `crd-shader`/`crd-renderer` modules were retired at RET-8 (2026-07-23). Vtable-stability discipline unchanged:
  append new virtuals at END (D135).
- **Rendering is asset-driven (ADR-0106; supersedes the ADR-0016/0017 IRenderPath plan).** Every technique is an
  authored frame-graph asset executed by `crd-render-graph`; render "paths" (forward/deferred/vis-buffer) are the
  post-RAF RPL proof library, not C++ path classes. See the ⛔⛔⛔ authored-frame-graphs rule above.
- **Hybrid scene model (ADR-0020 + ADR-0049-0061).** Spatial Hierarchy (scene tree) + SoA component storage. UI uses a separate retained `UiWorld` with distinct `UiNodeId` (user-chosen ADR-0107 D2); world-space UI integrates through explicit scene attachments. The full ADR-0107 remains Proposed. 8-layer slot-shaped ECS substrate (Phase 3.0 ✅).
- **Two-layer typed architecture (ADR-0078).** Upper layer = `Quantity<D, T>` at every API/config/cooker/UI surface. Lower layer = raw `f32`/`f64` in SIMD/math/geometry/numerical kernels + GPU writes. Bridges only at the boundary. Mars Climate Orbiter bug class is a compile error.
- **Physics — Cerid-native (eylem) from day 1 (ADR-0062, ADR-0063).** No third-party wrap; `crd-eylem` substrate IS the interface. Deterministic by construction, ECS-native, fiber-jobified, multi-domain (games + robotics + medical + cinematic + DAW), templated 2D + 3D, GPU-extensible. Phase 3.1 v0–v9 (~30 slices); v0–v1b ✅; v1c+ paused (resumes after detour D-007 + hesap per the locked sequencing).
- **Geometry-before-physics sequencing (ADR-0076 §12).** `crd-geometry` (substrate of 11 sub-modules) ships full BEFORE eylem v1c resumes, so eylem v1c+ consumes geometry from day 1 with no deferred-refactor debt.
- **Numerical substrate (ADR-0065).** `crd-hesap` MATLAB-class numerical substrate, Phase 3.1.6 — **v0–v16 shipped** (dense/sparse/iterative/direct/eig/opt/ode/fft/dsp/wavelet/comms/special/stats/interp/quadrature/diff/motion/tensor/autodiff); v17 GPU compute grew into detour D-007 (hesap-GPU is its last stop); v18 notebook+MCP planned.
- **Authoring vs runtime.** Configs and scenes authored in TOML; scenes cooked to binary for runtime. Configs parsed directly (small, not hot-path).
- **ImGui's role.** Debug-only forever. After `crd-ui` ships, ImGui never grows into editor or game surfaces.
- **Reference counting split.** Generic intrusive ref-counting in `crd-memory`. Resource-facing shared references (eviction, lazy loading, hot-reload ownership) in `crd-resources`.

<a id="agents-definition-of-done"></a>
#### Definition of Done

Every shipped slice must pass **all** of these:

1. Compile clean — zero warnings (`/WX` on MSVC, `-Werror` on GCC/Clang).
2. Pass clang-tidy + clang-format for changed files. **Run tidy INCREMENTALLY, per file, as part of testing EACH
   slice — never defer it to cluster close.** The moment you add or edit a test/header, run
   `powershell -File scripts/tidy-files.ps1 <the .cpp/.hpp files you touched>` (the CI-faithful LLVM-20 gate with
   `--warnings-as-errors=*`) right alongside the module's test run, and fix any hit before moving on. A whole cluster's
   worth of `readability-isolate-declaration` / `readability-identifier-naming` violations is trivial to fix one file
   at a time and miserable to fix 200-at-once at the end. **Scar (2026-07-07):** the `win-tidy-local` gate silently
   broke (its `CMakeCache` `CMAKE_COMMAND` got rewritten to the VS-bundled CMake — see docs/BUILDING.md §"Ninja
   `#deps 0`"), so an entire autodiff cluster (v16-c…h) was written UNGATED and accumulated 200+ tidy violations
   discovered only at close. Rule: if the tidy gate ever appears to pass trivially or errors on configure, VERIFY it
   is actually running (`scripts/tidy-files.ps1` uses clang-tidy directly and cannot be silently disabled); a broken
   gate is a DoD failure, not a convenience.
3. Have unit tests. All existing tests pass.
4. **Per-slice quality pass via `scripts/per-slice-check.ps1`:**

   | Config | When |
   |---|---|
   | win-debug | always |
   | win-asan | always |
   | win-shipping | always |
   | win-release | opt-in via `-IncludeRelease` for GPU / LTCG-sensitive slices |
   | win-tidy | always |

   Cluster-close slices additionally run the **18-config full sweep** (`scripts/full-sweep.ps1`): 11 Windows + 7 Linux configs.

5. **Per-slice verification runs `ctest --preset <X>`, NOT the test binary directly.** Guard tests (`crd-no-non-ascii-test-names`, `crd-simd-emission-check`, `crd-no-std-math-check`, `crd-no-std-sort-check`, `crd-no-untagged-physical-numeric`) are ctest-registered and don't appear in any test binary's `--list-tests`. A test binary saying "All tests passed" can coexist with a failing guard — both must be green.
6. **GPU slices** additionally use the v9-prereq-test-harness discipline: wrap setup in `crd::gpu::ValidationCapture` (gpu-context; the DX12 twin closes by debug-layer counter) → assert 0 errors/warnings; `bit_compare`/`ulp_compare` GPU output vs CPU oracle; `gpu_determinism_check` 3 rounds if claiming determinism; `CRD_PERF_BUDGET_LE` per published budget.
7. Public API change → update `context.md`; `docs/systems/<module>.md` updated if relevant.
8. Architectural decision → ADR file under `docs/decisions/` + entry in `docs/decisions/README.md` index + tag in `docs/ROADMAP.md` Section 4.
9. Commit message follows Conventional Commits: `feat(<module>): ...`, `fix(<module>): ...`, `refactor(<module>): ...`.

<a id="agents-session-re-entry-prompt"></a>
#### Session Re-entry Prompt

Use when starting the next session and asking the assistant to pick up where the project left off:

```text
You are working on the Cerid Engine. Read context surgically.

<a id="agents-mandatory-reads-always"></a>
### Mandatory reads (always)

Follow the canonical reading order in docs/README.md (the Documentation Map / Start
Here): AGENTS.md -> docs/BUILDING.md -> PRINCIPLES -> SANITY -> context.md -> ROADMAP. The map
also points to every other doc area (ADRs, systems, research, debt, detours).

<a id="agents-then-one-master-table-contract"></a>
### Then ONE master-table contract

From context.md "Current focus", find the active row in docs/ROADMAP.md. Open its linked
implementation contract and relevant ADR. Old phase/detour tables are historical requirements,
not competing status or scheduling authorities. A user-requested whole-system audit may read broadly.

<a id="agents-lazy-load-everything-else"></a>
### Lazy-load everything else

- Past decision → docs/decisions/README.md tag index → fetch ONLY the matching ADR.
- Last session detail → the session log linked under "Last shipped milestone".
- Module surgery → docs/systems/<module>.md, then deep-dive only if needed.
- Open follow-ons → docs/debt.md.
- Detour rules → docs/detours/README.md.
- Reusable engineering lessons → MEMORY.md index (one-line entries with file pointers).

<a id="agents-session-start-ritual"></a>
### Session start ritual

After mandatory reads:

1. Five-bullet summary:
   - Last shipped milestone (one line + session file ref)
   - Current focus (phase + slice from context.md)
   - Active detour, if any
   - Top 1–3 items in "Next up"
   - Open questions blocking progress

2. Ask, in priority order:
   a. "Continue with the planned next item: <name>?" Propose a concrete plan
      for THIS session. Wait for approval before implementation.
   b. If user wants a detour: ask for title/why/scope/exit, create
      docs/detours/D-NNN-<slug>.md, update context.md "Active detour".
   c. If undecided: surface 2–3 candidates from "Next up" or docs/debt.md.

3. Honor PRINCIPLES.md throughout. Don't re-litigate cornerstones.

<a id="agents-session-end-ritual"></a>
### Session end ritual

When user says session-end:
1. Write docs/sessions/YYYY-MM-DD-<slug>.md
2. Update context.md "Last shipped milestone" (one-paragraph summary + link)
3. Architectural decision → new ADR file under docs/decisions/ + entry in
   docs/decisions/README.md + tag in docs/ROADMAP.md
4. Slice status flip → update the one docs/ROADMAP.md row with evidence; do not duplicate live status in phase files
5. Phase finished → archive note at top of phase file
6. Surprising engineering lesson → add a memory entry (one-line MEMORY.md
   pointer + a feedback_*.md file with rule + Why + How-to-apply)
7. NEVER run git commit / push. Propose Conventional Commits message in chat;
   user commits themselves.
```

<a id="agents-detour-queue"></a>
#### Detour Queue

Side missions that interrupt the main roadmap. Active detour is named in `context.md`. Detour files in `docs/detours/`.

**Rules:**

- A detour pauses the main roadmap; `context.md` records "Active detour: D-NNN".
- Each detour has: title, why, scope, exit criteria.
- Same DoD applies (per-slice DoD + 18-config sweep at close).
- When done: close the detour file. If it changed architecture → new ADR. The main roadmap then resumes.
- Detours that grow beyond their exit criteria become real phase slices — promote them, don't let them quietly take over.

<a id="agents-session-expectations"></a>
#### Session Expectations

- Keep compile warnings at **zero**.
- For graphics/platform slices, check runtime behavior + validation-layer output, not just compile/test success.
- If a smoke/example reveals a real runtime issue, fix it or document exactly why it is intentionally deferred (and file in `docs/debt.md`).
- Use `crd::gpu::ValidationCapture` to assert validation silence on every GPU test. It's the authoritative oracle for "did I drive Vulkan correctly?" (DX12: the debug-layer counter twin.)

<a id="agents-documentation-conventions"></a>
#### Documentation Conventions

The full doc-system map — every doc area, its purpose, its index file, and the doc-design
rules (two classes: *living/scannable* get size budgets, *append-only historical* records
don't) — lives in **`docs/README.md`** (the single home; not duplicated here). Quick frame:
`AGENTS.md` + `docs/BUILDING.md` = rules · `PRINCIPLES`/`SANITY` = compass · `context.md` = live
state · `ROADMAP` = hub · then `decisions/` (ADRs, indexed in its `README.md`) · `systems/`
· `phases/` · `sessions/` · `research/` · `debt.md` · `detours/` · `MEMORY.md`.

After a system ships, preserve dated evidence in its session log. Correct an overview when it no longer describes
current code. ROADMAP alone owns live slice/subslice status; context is a pointer. Superseded planning content is
reference evidence, never a second execution queue (ADR-0129).

<a id="agents-platform"></a>
#### Platform

- Windows 11, PowerShell 7 primary.
- Use PowerShell-compatible commands. Avoid `cat`/`grep`/`sed`/`rm -rf`. Use `Get-Content`/`Select-String`/`Remove-Item -Recurse -Force` (cautiously).
- Running executables in PowerShell: use absolute path with `& "..."`. Relative paths sometimes fail in PS invocation contexts.

<a id="agents-git-policy"></a>
#### Git Policy

- **Agents NEVER run `git commit` or `git push`.** The user commits themselves.
- Agents may freely run `git status`, `git log`, `git diff`, `git show`, `git branch`.
- Conventional Commits: `feat(<module>): ...`, `fix(<module>): ...`, `refactor(<module>): ...`, `docs(<module>): ...`, `test(<module>): ...`, `build: ...`, `ci: ...`.
- When an agent finishes, it proposes a commit message in chat. The user runs commit themselves.
- **NO AI co-author trailers — ever** (user direction 2026-07-02). Proposed commit messages must NOT contain
  `Co-Authored-By: Claude ...` or any AI attribution line; only humans appear in the contributors graph. This
  overrides any harness default that appends such a trailer.
- **Never skip hooks** (`--no-verify`) or bypass signing unless the user explicitly asks. If a hook fails, fix the underlying issue.

---

<a id="claude"></a>
## Source: CLAUDE.md

<a id="claude-claudemd--local-agent-pointer-untracked"></a>
### CLAUDE.md — local agent pointer (UNTRACKED)

> This file is deliberately **not tracked** (user direction 2026-07-02: the repo is generic; all
> project knowledge lives in generic tracked docs). This is only the local entry pointer for
> AI-assisted sessions. If you change project rules, change the GENERIC docs — never this file.

<a id="claude-read-in-order-the-canonical-onboarding--same-as-docsreadmemd"></a>
#### Read, in order (the canonical onboarding — same as docs/README.md)

1. **`AGENTS.md`** — the rulebook: engineering principles, agent conduct (scope discipline, never-defer,
   full-crush benchmark policy, NO AI co-author trailers), coding standards, DoD, git policy, project
   structure, session re-entry/exit rituals.
2. **`docs/BUILDING.md`** — build presets, per-slice verification, smoke protocol, platform notes,
   and ALL troubleshooting (Raptor Lake build caps, ASan DLL PATH, LTCG hazards, the VS-CMake
   `#deps 0` landmine + helper scripts).
3. **`docs/PRINCIPLES.md`** + **`docs/SANITY.md`** — the compass + the scar→rule→check doctrine
   (read every session; both short).
4. **`context.md`** — live state: current focus, last shipped, next up.
5. **The active phase doc** under `docs/phases/` (named in context.md) — the slice contract.
6. **`docs/README.md`** — the full documentation map (ADRs, systems, sessions, research,
   `docs/bench/` benchmark convention, debt, detours).

<a id="claude-session-rituals"></a>
#### Session rituals

Follow AGENTS.md §"Session Re-entry Prompt" on entry (five-bullet summary → propose a concrete plan →
wait for approval) and §"Session end ritual" on close (session log, context.md update, ADR/phase-row
updates, memory entry for surprising lessons, propose a commit message — the user commits).

<a id="claude-hard-local-reminders"></a>
#### Hard local reminders

- Agents NEVER `git commit`/`git push`; propose Conventional-Commits messages **without any AI
  co-author trailer** (this overrides any harness default).
- Benchmarks: every measured board → `docs/bench/` at measurement time (convention in its README).
- Windows PowerShell 5.1 mangles UTF-8 text I/O — see docs/BUILDING.md §Platform notes.
- Iterate locally (module tests only); close globally (`scripts/per-slice-check.ps1`, sequential,
  Ninja-capped — the host-instability doctrine in docs/BUILDING.md).

---

<a id="docs-principles"></a>
## Source: docs/PRINCIPLES.md

<a id="docs-principles-cerid-engine--engineering-principles"></a>
### Cerid Engine — Engineering Principles

> Non-negotiable. Every slice respects them; deviations are explicit,
> justified, and recorded as an ADR under `docs/decisions/`.
>
> Read this every session. It's short and it's the architectural compass.

<a id="docs-principles-identity"></a>
#### Identity

Cerid is a **general-purpose C++20 real-time engine substrate**. Games are
one consumer; **simulation (incl. robotics), medical visualization,
DAW-class creative tools, and offline cinematic pipelines** are equal-class
consumers. The architecture serves all of them; no domain is privileged.

| Domain                       | Why Cerid fits                                                                |
| ---------------------------- | ----------------------------------------------------------------------------- |
| Games                        | Real-time renderer, physics, animation, scripting, scene/entity model         |
| Simulation (incl. robotics)  | Deterministic option, swappable physics, math depth, sensor/actuator hookable |
| Medical visualization        | High-quality rendering, large-volume data, deterministic playback             |
| DAWs / creative tools        | Custom retained-mode UI, node editors, plugin/script extensibility, low jitter |
| Offline cinematic pipelines  | Same render path, scriptable, deterministic, scene-graph aware                |

<a id="docs-principles-principles"></a>
#### Principles

- **Modular by default.** Every subsystem is a separable module with a clear
  public surface. A DAW build that doesn't need physics/animation must be
  able to omit them at link time.
- **Vertical slice over horizontal completeness.** Walk a small path
  end-to-end before widening. The first triangle gate is a permanent example.
- **Authoring text, runtime binary.** Human-edited data is text (TOML / JSON
  / glTF). Engine-consumed data is cooked binary. Runtime never imports
  source assets.
- **Everything executable is an authorable asset.** The rule above covers
  DATA; this covers CODE. Every algorithm the engine runs — rendering,
  culling, lighting, post, GI, compute, numerical — is an authorable CEIR
  program asset: cooked disk-first, loaded by default, partially modifiable
  or completely replaceable by an application WITHOUT recompiling the engine.
  The host/renderer knows nothing about the algorithms it executes; it cooks
  and runs assets. A C++ graph-builder is a cooker *mechanism*, never the
  shipping form. (AGENTS.md §Agent Conduct pins the enforcement + the
  `scene_renderer` hand-built-`ensure_*` scar.)
- **One-way module dependencies.** Cycles are bugs. The dependency graph is
  reviewed at every module boundary change.
- **Real workload before optimization.** No SIMD, fiber, GPU-allocator, ECS
  rewrite, or render-path swap without a measured baseline and target.
- **API stable across backends.** Public surfaces (RHI, physics, audio,
  render path) are designed assuming multiple implementations even when
  only one exists. Vendor types do not leak.
- **Tak-çıkar (plug-out) third-party.** Where Cerid uses an external
  (glslang/shaderc, spirv-reflect, ImGui, toml++), the integration is a
  backend behind a Cerid-owned interface. Core simulation surfaces
  (renderer, physics/eylem, audio) are Cerid-native — no vendor wraps.
- **Determinism is a first-class option.** Not the default, but reachable:
  fixed-step physics, deterministic random, replay-friendly event log.
- **Every shipped slice ends green on Debug + Release + ASan.** Three
  flavours. No exceptions.
- **The engine is allowed to be slow before it is allowed to be wrong.**
- **Sanity is a practice, not a phase.** Root-cause over workaround; verify
  the *shipped* artifact; test boundaries, not just volume; know what your
  diagnostics can't see; honest scoreboards — including about ourselves. The
  full doctrine (each rule tied to the bug that taught it) and the living
  **Sanity Ledger** every agent contributes to live in **`docs/SANITY.md`**
  — read every session.
- **Units live at the API surface, raw scalars live in the inner loop.**
  Cerid runs a **two-layer typed architecture** (formalised at Phase 3.1.7.5
  close; ADR-0078 §5):

  - **Upper layer — typed.** Every public API, every ECS component field,
    every config key, every cross-module function signature, every cooker /
    loader public surface, every UI display path uses `Quantity<D, T>`
    (`Length<T>` / `Mass<T>` / `Force<T>` / `Velocity<T>` / `Torque<T>` / …).
    The dimensional check happens here, at compile time, on the SI value.
  - **Lower layer — raw.** SIMD kernels, math primitives (Vec / Mat / Quat
    inner ops), numerical algorithms (BLAS / LAPACK / closest-point /
    raycast bodies / Möller-Trumbore / Vatti clipper / etc.), GPU command-
    buffer writes, file and wire byte buffers, on-disk asset payloads
    operate on raw `f32` / `f64`. No dimensional tag rides through an
    `_mm256_*` intrinsic or a `vkCmdPushConstants` call.

  The two layers meet at **the API surface**, and only there. Crossings
  use `.value`, `to_raw_vec` / `from_raw_vec` (constexpr — compile away),
  or a documented strip-compute-retag wrapper. Each crossing is one line
  with a one-line comment naming the boundary (e.g. `// GPU push constant
  — raw f32 by ADR-0078 §3 D22`).

  **The internal canonical unit is SI base** (meters / kg / seconds /
  radians / kelvin / ampere / candela / mole) at the typed layer. Asset /
  file / UI boundaries normalise to SI at load (`get_length("65_mph")` →
  `Velocity32{29.0576F}`); runtime never sees non-SI. The user-facing
  display layer (`UnitPreferences` + 11 discipline presets) translates SI
  back to authoring-convention strings for the UI only.

  **Precision tier (f32 / f64) is orthogonal to the dimensional type.**
  Same `Length<T>`; games + runtime pick `f32`; aerospace large-world +
  CAD micrometer + scientific pick `f64`. Zero-overhead layout
  (`sizeof(Quantity<D, T>) == sizeof(T)`) means the precision choice is
  the only one that affects storage; the dimensional tag is compile-time
  metadata.

  **There is no opt-out at the upper layer.** Bare-float-for-physical-
  quantity at any API surface is a code-review block and a CI-guard
  violation (`crd-no-untagged-physical-numeric`). The lower layer stays
  raw on purpose — pretending an `_mm256_load_ps` carries a `Length`
  dead-ends in the first lane shuffle.

  → `crd-units` (Phase 3.1.7.5 ✅ CLOSED 2026-05-15); ADR-0078 §1-§5.

<a id="docs-principles-architectural-cornerstones-pinned"></a>
#### Architectural Cornerstones (pinned)

These come from accepted ADRs and are not re-litigated in routine sessions.
If circumstances genuinely change, open a new ADR or escalate to `@heavy`.

- **Render path — SUPERSEDED BY EVENTS (annotated 2026-08-07):** the original cornerstone ("Renderer v1
  ships Clustered Forward+ behind an `IRenderPath` interface; Deferred / Visibility-Buffer land later as
  additional implementations" → ADR-0016) described the retired `crd-renderer` (deleted at RET-8,
  ADR-0105). **Today's cornerstone:** rendering is **asset-driven** — every technique is an authored
  frame-graph asset executed by `crd-render-graph` (ADR-0106); the forward/deferred/visibility "paths" are
  the post-RAF **RPL proof library** of authored renderers, not C++ path classes. ADR-0106/0105 now govern
  runtime ownership and retirement. The original ADR-0016 remains historical evidence.
- **Culling — realized on the new stack (annotated 2026-08-07):** the intent of ADR-0017 (frustum → BVH →
  Hi-Z occlusion; per-light culling) survives and is partially landed as GPU-driven culling (REN-40
  device-side cull; HZB + LOD/SSE tracked in the post-RAF GVA band); the "part of clustered Forward+"
  framing referred to the retired renderer. → ADR-0017
- **Scene + ECS:** **Hybrid model.** SoA component storage for cache-friendly
  iteration; hierarchical scene tree for traversal/authoring. Not pure ECS,
  not naive scene graph. → ADR-0020
- **UI owns a separate retained world.** `UiWorld` / `UiNodeId` are distinct from
  `SceneWorld` / `EntityId` (user-chosen ADR-0107 D2, 2026-08-07). World-space UI uses
  explicit attachment/input projection; standalone UI has no required gameplay ECS.
  This supersedes the UI-tree clauses of ADR-0020/0023/0057; the full ADR-0107 remains Proposed.
- **Physics — Cerid-native (eylem) from day 1.** No third-party wrap. The
  `crd-eylem` module is built deterministic-by-construction (compile +
  runtime FP contract), ECS-native, fiber-jobified, multi-domain (games
  + robotics + medical + cinematic + DAW), templated 2D + 3D from a
  single substrate, and GPU-extensible. → ADR-0062, ADR-0063
  (supersedes ADR-0018; phase plan: `docs/phases/phase-3.1-eylem.md`;
  research: `docs/research/cerid-eylem.md`)
- **Authoring vs runtime:** Configs and scenes authored in TOML; scenes
  cooked to binary for runtime. → ADR-0012, ADR-0013
- **ImGui's role:** Debug-only forever. After `crd-ui` ships, ImGui never
  grows into editor or game surfaces. → ADR-0023
- **Reference counting split:** Generic intrusive ref-counting in
  `crd-memory`. Resource-facing shared references in `crd-resources`. →
  ADR-0014
- **Agent-native engine: CLI / RPC is the source of truth.** Every
  engine operation a human user, artist, engineer, or scientist
  performs is reachable from CLI + JSON-RPC + **Anthropic MCP (exact
  compatibility, not adjacent)**. The GUI is a visualization layer
  that emits CLI commands when the human clicks. AI agents (Claude
  Code, Claude desktop, OpenAI / Gemini Function Calling agents)
  drive the engine end-to-end via the same surface. Capability-based
  security + transactional sessions + sandbox isolation +
  deterministic replay (ADR-0063 + ADR-0078) make agent sessions
  safe + reproducible. **Per-slice Definition of Done** includes
  shipping the CLI command schemas alongside the C++ API from
  2026-05-19 forward; the `crd-cli` parser substrate itself lands in
  the CMD/REFLECT/DOC service rows before full UI/editor work (ADR-0129 supersedes
  the older eylem-first sequence). Existing `ceridc` and hesap commands are a partial
  implementation, not proof that every operation is exposed. Local UI invokes shared
  typed commands in-process; it does not spawn a CLI for each event. → ADR-0081 (Proposed); research
  `docs/research/cerid-agent-native-engine.md` +
  `docs/research/cerid-hesap-2026-update.md`. Strategic bet: the
  next decade of creative + engineering + scientific work happens
  with AI agents as peer collaborators; the engine substrate built
  for that wins.

- **Cerid owns its executable-program language stack (CEIR/CHIR);
  C++ is one first-class authoring surface, not the only one.**
  Reusable algorithms are versioned, inspectable, serializable,
  hot-reloadable program ASSETS, authored as four projections of one
  semantic model: text (CEIR execution IR and the initial CHIR high-level
  frontend; full CHIR authoring is still planned), a CR-D007 visual graph, a domain frontend, or a C++
  builder. C++ stays first-class for native extension, providers, and
  programmatic authoring — the C++ builder emits ordinary canonical
  CEIR — and C++ DLL hot-reload remains supported. **No Lua / Python /
  JavaScript / GDScript / WrenScript embedded interpreter — CHIR is
  Cerid's OWN language, not a wrapped third-party VM.** Why the
  earlier C++-ONLY rule was reversed (ADR-0108): an algorithm-as-asset
  must be inspectable, semantically diffable, verifiable, partially
  evaluable, and lowerable to the best legal CPU/GPU/NPU/provider
  strategy — a compiled `.crds.cpp` DLL is none of those, so "C++ is
  the only authorable program" would block the CEIR mission. The
  determinism (ADR-0063), no-marshaling, full-debugger, and zero-FFI
  benefits of C++ survive as versioned native intrinsics + the C++
  builder. → ADR-0108 (surgically supersedes ADR-0081 §9), ADR-0081,
  ADR-0034 (subsumed).

- **Schema versioning + backwards-compat for the agent surface.**
  Every CLI / MCP command schema is versioned (major/minor). Major
  bump = breaking change to params or output; old schema stays
  registered for ≥ 2 minor versions with `Deprecated` status before
  removal. Minor bump = additive (new optional param, additional
  output field). Backwards-compat CI test: any schema removed before
  its deprecation window expires triggers a build failure. Schema
  export (`meta.export-mcp-tools`) produces a versioned MCP tool
  catalog suitable for committing alongside agent prompts. This
  discipline is what lets AI agents author scripts they can trust
  across Cerid versions. → ADR-0081 §2.

<a id="docs-principles--the-deletion-is-the-proof-ren-3738-2026-07-27"></a>
#### ⭐⭐ The deletion is the proof (REN-37/38, 2026-07-27)

When a capability moves from C++ into an authored asset, the slice is not done when the asset COOKS — it is done
when **the C++ it replaced is deleted and the engine still renders**. Anything less leaves two paths, and the one
that actually draws the frame is the old one.

Three things this band showed, each of which had been true for months without anyone noticing:

1. **A parallel vocabulary always wins by default.** `assets/materials/*.mat.toml` → GLSL files rendered while
   `.crdm` merely cooked. Two vocabularies for one thing is not redundancy to tidy up later; it is a decision
   about which one is real, made silently.
2. **"It cooks" and "it draws" find different bugs.** Every defect in this band that could destroy an image — a
   varying pair that disagreed, a uv width the emitter rejected, a light direction negated in the wrong place —
   was invisible until something rendered. A cook-layer gate is necessary and is not sufficient.
3. **An unreachable library is indistinguishable from a missing one.** `ckir_lighting.hpp` held 1100 lines of
   gold-standard shading — LTC area lights, split-sum IBL, PCSS/EVSM/MSM — while the technique ABI carried
   exactly one directional light. None of it was unfinished. There was simply no vocabulary to name it, and a
   capability nothing can name does not exist.

Corollary for reviews: ask **what would have to change for this to be wrong, and can anything see it?** If the
answer is "nothing on either backend", the check has to move to cook time and be a DECLARED contract.

---

<a id="docs-sanity"></a>
## Source: docs/SANITY.md

<a id="docs-sanity-cerid--engineering-sanity-doctrine"></a>
### Cerid — Engineering Sanity Doctrine

> **Read this every session.** It is short on purpose. Its job is to keep the engine
> *core* solid by encoding the lessons we paid for in real debugging time, so we don't
> pay for them twice. The target is an **A++ core**; this is the *path*, not a claim of
> arrival. Honest self-assessment as of 2026-06-09: a strong numerical stack on a **B+
> core** with one freshly-closed foundation bug. We get to A++ by *practice*, recorded
> in the **Sanity Ledger** at the bottom — every agent adds to it, a little at a time.
>
> Each rule below is **scar → rule → check**: the real bug that taught it, the rule, and
> the concrete thing you actually do. If a rule ever feels abstract, re-read its scar.

---

<a id="docs-sanity-the-rules"></a>
#### The rules

<a id="docs-sanity-1-root-cause-never-work-around-no-debts"></a>
##### 1. Root-cause, never work around. ("No debts.")
- **Scar:** the multifrontal-LU flaky AV was almost shipped as a single-chunk `factor_pool`
  band-aid that *hid* a bug in `TlsfAllocator` — the engine's most-used allocator.
- **Rule:** a fix that *avoids* a symptom without naming its mechanism is a debt, not a fix.
- **Check:** before you call it fixed, state the mechanism in one sentence. If you can't,
  you haven't root-caused it — say so, don't dress it up. Workarounds get deleted once the
  root is found (the `factor_pool` was reverted; the "remaining debt" turned out not to exist).

<a id="docs-sanity-2-verify-the-shipped-artifact-not-a-green-you-remember"></a>
##### 2. Verify the *shipped* artifact, not a green you remember.
- **Scar:** a stale PCH (`C1853`) let "All tests passed" report on an **un-rebuilt** binary;
  an earlier session *fabricated* benchmark numbers from memory.
- **Rule:** a green checkmark you can't trust is worse than a red one — it hides bugs.
- **Check:** clean-rebuild before trusting a result after header/PCH changes; run **`ctest`**,
  not the bare test binary (guard tests live only in ctest — see CLAUDE.md DoD §8); capture
  perf/test numbers **to a file**, never quote them from memory. Verify the *clean* artifact
  (no diagnostics, no temporary workaround), not the intermediate you debugged with.

<a id="docs-sanity-3-boundary-adversaries-not-volume"></a>
##### 3. Boundary adversaries, not volume.
- **Scar:** `init_pool` placed the end sentinel 16 B too early; **820 K+** random-stress
  assertions sailed over it for a long time — random alloc/free almost never fills a pool to
  its *last* block, which is the only place the bug lived. (`crd::containers::String` bit us
  the same way once: `capacity == allocation size` off-by-one.)
- **Rule:** volume of assertions ≠ coverage of edges. Latent bugs hide at boundaries.
- **Check:** foundational modules get tests that hit edges *on purpose* — fill-to-tail,
  fragment-to-end, `capacity == allocation size`, empty, single-element, the last valid
  index — and **pre-poison** memory where a bug needs poison to show (the TLSF regression
  fills a `0xCD`-poisoned buffer to its tail; see `tests/memory/test_tlsf_allocator.cpp`).

<a id="docs-sanity-4-know-what-your-diagnostic-cant-see"></a>
##### 4. Know what your diagnostic CAN'T see.
- **Scar:** poison/quarantine gave **false positives** (TLSF coalesce reuses freed payloads);
  page-heap **masked** the bug (it altered alloc timing); ASan can't see *intra-pool* TLSF
  overruns; the `{1..16}` determinism moat + ASan can't catch a UMR (resident pages are
  byte-identical). A **sound structural validator** (walk the block chain, check in-pool +
  free-list links + bitmap consistency) is what actually found it.
- **Rule:** a clean run from a tool that is blind to the bug class proves nothing.
- **Check:** match the diagnostic to the bug. Allocator structure → structural walk. UMR →
  NaN-poison (`0xFF`-fill) on a *big* problem. Race → TSan. Heap edge → ASan. Name what your
  tool is blind to before you trust its silence.

<a id="docs-sanity-5-measure-before-you-optimize-refute-your-own-hypothesis-first"></a>
##### 5. Measure before you optimize; refute your own hypothesis first.
- **Scar:** "fill-margin wins" (bmwcra) was **false** — fill == flop. `adaptive-ℓ`, staircase,
  panel-BLAS-2, amalgamation: all **refuted by the profile**, not by argument.
- **Rule:** the first thing you measure is whether your own theory is wrong.
- **Check:** no perf claim without a file-captured measurement; profile to find the real lever
  before touching a kernel (the CHOLMOD gap was *serial symbolic*, not the BLAS-3 we assumed).

<a id="docs-sanity-6-honest-scoreboards--no-asterisks-including-about-ourselves"></a>
##### 6. Honest scoreboards — no asterisks, including about ourselves.
- **Scar:** parallel-Cerid-vs-serial-peer is the forbidden asterisk; we report the MUMPS
  *losses* (ns3Da 0.64×) right next to the wins.
- **Rule:** an overstated win erodes trust in every other number we report.
- **Check:** fair peer at its best, matched accuracy, same thread count; state losses plainly;
  don't grade the core A++ when it's B+. The doctrine *earns* the grade, it doesn't assert it.

<a id="docs-sanity-7-dont-rabbit-hole--time-box-change-tools-escalate"></a>
##### 7. Don't rabbit-hole — time-box, change tools, escalate.
- **Scar:** the flaky-AV chase burned a long arc through blind tools before the sound
  validator (advisor-gated) ended it in one step.
- **Rule:** grinding the same lever that isn't converging is how rabbit holes form.
- **Check:** when an approach stalls, call `advisor`, switch diagnostic, or write down what
  you've *ruled out* — don't repeat the failing move with more force.

<a id="docs-sanity-8-search-the-engine-before-you-build-reuse--reimplement"></a>
##### 8. Search the engine before you build. (Reuse > reimplement.)
- **Scar:** v12 reimplemented **erf/erfc/lgamma** (already in `crd::math::deterministic`,
  Cephes, and hesap-special *links crd-math*) and **misplaced f64 SIMD log/exp** into
  `hesap-special/detail` when crd-math is their home (it already has the f32 `log/exp` +
  `Vec4d`); a self-contained tridiagonal **QL** was about to be written when hesap-dense
  already ships Sturm/dqds/**MRRR**. All three because the engine wasn't grepped first.
- **Rule:** before implementing *any* solver, kernel, or utility (eigensolver, FFT, RNG,
  special function, SIMD transcendental, container, allocator…), FIRST check whether the
  engine already provides it. Reuse it; if it's close-but-not-quite, extend it *in its home
  module*; only build new when nothing fits — and say so explicitly.
- **Check:** grep `engine/*/include` for the capability (and its synonyms — `eig`/`eigen`/
  `tridiag`/`steqr`; `log`/`exp`/`Vec4d`; `bessel`/`i0`; `gamma`/`erf`) *before* writing the
  first line. New code lands in the module that owns that capability (a reusable SIMD math
  primitive → crd-math, not a consumer's `detail/`). Module-edge concern? confirm acyclicity
  (does the provider depend on you?) and prefer the edge or a shared module over a duplicate.
  A self-contained reimplementation is justified only after the search comes up empty.

<a id="docs-sanity-9-a-documented-loss-is-an-open-bug-not-a-closed-slice-solve-dont-just-disclose"></a>
##### 9. A documented loss is an open bug, not a closed slice. Solve, don't just disclose.
- **Scar:** v12-d's cold transcendental tail *lost* to Boost (Lambert-W **0.05×** = 20× slower, K/E ~0.23×, E1 0.46×).
  Instead of fixing it, I wrote "Boost's decades of minimax tuning win" in the docs and moved on — then in a
  status report presented the disclosure itself as "honesty." The user's verdict: *that is not honesty, that is not
  doing the work.* The standing mandate (`feedback_full_victory_beat_all_gold_standards`) is FULL victory, every
  gold standard, honestly. A loss recorded in a doc is still a loss.
- **Rule:** honest reporting (rule #6) is *necessary but not sufficient*. Reporting a loss does not retire it. Every
  measured loss against a gold standard is an OPEN problem to solve — not an accepted endpoint you get credit for
  disclosing. "We lose but I documented it" is a silent failure dressed as candor. A 0.05× gap is almost never a
  fundamental wall — it's an algorithm/iteration/initial-guess problem (Boost beats you with *rational minimax* and
  *good seeds*, so use those: better initial guess → fewer Halley steps, a tuned rational instead of a generic series).
- **Check:** before calling any perf slice done, list every peer you lose to and **fix or escalate each** — fix it,
  or bring the user the *measurement* that proves it's a genuine wall (and let them decide), never bury it in prose.
  "Honest about losing" is the start of the work, not the end of it. Beating Boost means doing what Boost does
  (minimax rationals, Fritsch/Halley with a good seed) — reach for that, don't fall back to the textbook series.

<a id="docs-sanity-10-a-ci-only-failure-is-a-config-specific-miscompile--confirm-the-toolchain-name-the-blind-spot-pinpoint-fix-at-the-source"></a>
##### 10. A CI-only failure is a config-specific miscompile — confirm the toolchain, name the blind spot, pinpoint, fix at the source.
- **Scar:** the `wpt` test SegFaulted *only* on win-release/shipping in CI (3/3 runs), never locally (27/27 + gcc green).
  Three guesses cost real time: "older CI MSVC" (it was *newer* — 19.51/14.51 vs local 14.50), the `level-- > 0` reverse
  idiom (rewriting it changed nothing), and a NOINLINE (didn't help). The green **win-asan** run was *misleading* — MSVC
  ASan builds carry **no LTCG**, so they are structurally blind to an LTCG miscompile (rule #4). The real bug: 19.51
  `/O2`+LTCG miscompiled the inline `crd::usize{1} << level` in three hot loops to a garbage stack-address value (markers
  printed `count = 140698301264476` for `level == 2`) ⇒ ~1e14-iteration inner loop ⇒ OOB ⇒ SegFault.
- **Rule:** a deterministic pass-here/fail-there is a codegen/toolchain bug, not luck. Don't theorize from clean source —
  get runtime facts from the *failing* config, and never trust a green from a build that doesn't exercise the failing
  codegen path.
- **Check:** (1) pull the CI compiler version first (`cl` banner) — the local-vs-CI delta is the lead, and CI can be
  *newer*; (2) spin a **minimal temporary CI job** (one failing config × one target × one test, `branches-ignore: [main]`
  so the full matrix stays silent — ~3 min vs ~30) for a tight loop; (3) pinpoint with **flushed-stderr markers**
  (`fputs`+`fflush`; catch2's crash handler gives only the TEST_CASE line), escalating to printing the **actual variable
  values** when the structure looks right but the result is wrong (that exposed the garbage `count`); (4) **fix
  engine-side** — eliminate the miscompiled construct for all consumers (here: drop the in-loop `1 << level`, iterate by
  heap id), never mask by lowering the *test's* optimization; (5) strip every marker + the temp job before close (grep).

<a id="docs-sanity-11-suspect-your-own-instrument-before-you-claim-a-defect-verify-the-harness-not-the-vibe"></a>
##### 11. Suspect your own instrument before you claim a defect. (Verify the harness, not the vibe.)
- **Scar (2026-08-15):** in ONE session I manufactured **four phantom "failures"** and reported them as real — a PowerShell
  `| Select-Object -First` that closed the pipe and killed the exe (fake exit **255**); a healthy but SLOW whole-repo `ctest`
  I misread as "hung" (I queried the WRONG process name and never computed the ~3 h expected duration), then KILLED it at
  4175/6384 and wrote a **fabricated "2-hour ASan deadlock"** into four docs and told the user; a win-asan binary run without
  the ASan DLL on PATH (fake `0xC0000135`); and a raw `clang-tidy` over an MSVC PCH that FALSE-CLEANED (`0 warnings` — it never
  parsed). Zero were real; `B14-c`, the "hung" test, passes in **3.5 s**.
- **Rule:** a surprising failure is YOUR TOOLING until proven otherwise. Never state a defect — failed / hung / broken /
  timeout — to the user or in a doc before ruling out the harness. An unverified defect written as fact is a lie, and in a
  repo whose first rule is never-disguise-failure it is worse than the original misread.
- **Check:** *slow ≠ hung* — before "hung", compute expected duration (tests × per-test), confirm CPU-climb / last-completion
  times on the RIGHT process, and never kill a run you haven't proven wedged (this is the 2026-07-02 ledger rule, re-broken).
  `$LASTEXITCODE` after a native pipe is valid ONLY if nothing downstream early-closes it (no `Select-Object -First`; use
  `-Last` / collect-then-filter / `Out-Null`). win-asan exe ⇒ MSVC ASan DLL dir on PATH (`0xC0000135` = missing DLL,
  BUILDING.md §win-asan). A tidy "0 warnings" is real ONLY if it PARSED — confirm "N warnings generated" (raw clang-tidy on an
  MSVC PCH errors and false-cleans; pass `/Y-`). And **whole-repo build/test is CI's job** — locally build+run only what you
  changed + its blast radius; a whole-repo sweep is a multi-hour job you must not launch on the host.

---

<a id="docs-sanity-how-to-contribute-to-sanity-a-little-at-a-time"></a>
#### How to contribute to sanity (a little at a time)

Any agent with slack can pick **one** small hardening and append it to the ledger. Small is
the point — the core gets to A++ by accretion, not by a heroic pass.

- Add a **boundary-adversary test** to a foundational module (memory, containers, jobs).
- Add or tighten a **CI guard** (`add_test(NAME …)`) that makes a past mistake impossible.
- Convert a **workaround into a root fix** (then delete the workaround).
- **Trim** an over-long doc/memory line, or delete a stale/wrong memory (see scar #3's bloat).
- Replace a **remembered number** with a file-captured one.

---

<a id="docs-sanity-sanity-ledger-append-only-dated-one-line-each-actions-not-philosophy"></a>
#### Sanity Ledger (append-only; dated; one line each; *actions*, not philosophy)

- 2026-08-15 — **Added rule #11 (suspect your own instrument)** after manufacturing FOUR phantom failures in one session:
  a `Select-Object -First` fake-255; a healthy ~3 h whole-repo win-asan `ctest` misread as "hung" then KILLED at 4175/6384,
  with a fabricated "2-hour ASan deadlock" written into four docs and told to the user (all retracted — `B14-c` passes in
  3.5 s); a win-asan run without the ASan DLL PATH (fake `0xC0000135`); a raw clang-tidy over an MSVC PCH that false-cleaned.
  Reinforced the user directive **whole-repo build/test = CI's job** (AGENTS.md conduct + BUILDING.md §Per-slice banner) and
  scrubbed the four false records.

- 2026-08-07 — **Repository-wide doc-hygiene pass (rules #2/#6 applied to the docs themselves):** `context.md`
  2,012 → 108 lines (history moved VERBATIM to `docs/sessions/2026-08-07-context-md-history-archive.md` — the
  2026-07-06…16 + 08-01/02 blocks were the ONLY narrative record of those days); `debt.md` 982 → ~360 (closed
  entries deleted when session-log-homed, orphans salvaged into the hygiene log's appendix); ROADMAP status table
  rebuilt honest (D-007 named as the live front; renderer-era rows annotated RETIRED); systems index refreshed + the four
  retired-module overviews (rhi/rhi-compute/renderer/shader) DELETED per user direction (git history keeps them;
  the index's Retired note points at successors); AGENTS/PRINCIPLES cornerstones annotated to
  gpu-context/RAF reality; ADR tag index extended 0076–0107; ADR-0032's recorded supersession (by 0106) struck in
  place per the SUPERSEDED rule; 36 research dossiers stamped with outcomes; BUILDING smoke list purged of the 9
  RET-deleted smokes; 5 broken doc links fixed; a stale scope comment in `deterministic.hpp` corrected (only
  non-doc touch). Canonical source-of-truth table added to `docs/README.md`. Full report:
  `docs/sessions/2026-08-07-doc-hygiene-pass.md`. Rule reinforced: **a living doc that restates status owned by
  another doc WILL go stale — state facts in one home, point everywhere else.**

- 2026-07-25 — **A "content hash" that memcpys a POD hashes STACK HISTORY, not content — and the tool that was blamed
  was innocent twice over (rules #1/#4/#5/#10).** The parallel-cook dedup failure (D3/D5/D6/D8/D10/D12 red on
  win-asan/win-shipping, green on win-debug) had been handed over as "address-dependent ordering in the KIR→GLSL emitter",
  with a glslang serialization mutex added on the theory that shaderc carried process-global state. Both wrong.
  `serialize_graph` blasted the POD pools raw, so every INDETERMINATE PADDING byte of `KNode`/`KStmt`/`KType`/`KEntry`
  entered the hash — the builders default-initialize (`KNode n;`), so the "content" hash was a function of whatever the
  stack held. A purpose-built gate (build the same graph twice with `dirty_stack(0xAA)` / `dirty_stack(0x55)` between)
  reproduced it in one run and named the byte: **offset 33 = `KNode+1`, the hole between `KOp op` and the 2-aligned
  `KType type`.** win-debug was structurally blind because MSVC `/RTC1` 0xCC-fills locals *deterministically* (rule #4:
  name what your diagnostic cannot see). Root fix = a canonical packed padding-free encoding (bonus: the artifact is now
  ABI-independent). A SECOND instance in the same slice survived that fix — `reflect()`'s `ShaderReflection` is written
  RAW into the `REFL` chunk, and diffing the two cooked cache files pinned the divergence at **file offsets 1881..1883**,
  the 3-byte hole after `KStage stage`; note `T r{}` is NOT sufficient under MSVC (it runs the implicit default ctor and
  leaves the holes), only an explicit `memset` is. The mutex was deleted once the root was found (rule #1). Rules: a POD
  that becomes a hash or an artifact must be serialized FIELD BY FIELD; and when a bug appears only under ASan/parallel,
  suspect *layout*, not *ordering*, before you blame a third-party compiler.

- 2026-07-25 — **The "non-deterministic upstream clang-tidy crash" was the HOST running out of commit — and the tidy gate
  had been analysing a configuration we do not ship (rules #2/#4/#6).** win-tidy died with a different check on a
  different SIMD file every run; two checks had been disabled in `.clang-tidy` to dodge it, and the third instance was
  written off as "not deterministically fixable". Reading the actual log ended it: the message is `LLVM ERROR: out of
  memory` with `Exception Code: 0xC000001D` (LLVM's own abort), **not** the reported `0xC0000005`. Measurements: a
  clang-tidy edge peaks at 200–300 MB; the crashed files run **0/5 standalone** and **0/6 through the exact
  `__run_co_compile` command**; the host sat at **83 GB of a 96 GB commit limit** with Visual Studio (8.7 GB), clangd
  (8.0 GB) and a DAW resident. `malloc_allocator.cpp` — a tiny file — crashing at `<eof>` is the clincher: ambient
  pressure, not TU complexity (a failing heap request gives the OOM; a stack growth that cannot commit gives the AV).
  Both disables reverted; both sweep scripts gained a commit-headroom preflight that measures free commit and clamps
  `-BuildJobs`, so this can never again read as a code failure. **The bigger find underneath it:** clang-tidy silently
  DROPS every `/`-spelled MSVC flag arriving via the compile command — proven with an `#error`-guarded probe through
  the exact CMake path — so `/EHsc` and `/arch:AVX2` never reached the TU: exceptions looked disabled (any `try` = hard
  error, the real `filesystem.cpp` failure, and `bugprone-exception-escape` could never fire) and `__AVX2__` was
  UNDEFINED, meaning **every AVX2-guarded path was preprocessed out and never analysed** — rule #2's exact shape, a
  green you cannot trust. Fixed via `--extra-arg=`, the ISA flag exported by the same CrdSimd branch that sets the real
  one. Correcting it unmasked **120 findings** that had accumulated while the build died at edge ~18/1070; all fixed.
  Rule: when a gate crashes at random, read its actual exit signature before theorising — and periodically prove the
  gate sees the flags you think it does.

- 2026-07-18 — **A green test binary is not a green slice — the close-out full ctest+shipping peeled a five-layer onion the
  binaries had hidden (rules #2/#10; B16 close).** Closing B16 = "just tidy + sweep" turned into: 12 non-ASCII TEST_CASE names
  (guard red + ctest-unselectable under CP1254), 3 `std::pow` in `ckir_ocean.hpp`, and a win-shipping-ONLY `C4789` — all
  pre-existing, all missed because prior B14/B15 sessions verified with `crd-*-tests.exe` not `ctest`+shipping. The C4789 was
  the `#deps 0` landmine again (win-shipping alone carried the English `msvc_deps_prefix` + a VS-bundled `CMAKE_COMMAND` it
  re-armed on a GLOB reconfigure ⇒ a stale pre-`m_stmts` 136-byte `KGraph` obj; every OTHER config had the Turkish prefix,
  which is why only shipping failed) — wiped + reconfigured with standalone CMake, clean rebuild green. The untagged-physical
  guard was resolved by a **full units typing** of the CKIR sim/GI configs — the gold-standard rule crystallized: *type where a
  real dimension exists (Length/Velocity/Acceleration/Angle + a custom InverseLength for 1/km extinction), keep genuinely
  dimensionless tuning knobs raw with an honest marker; `Dimensionless<f64>` on a knob is ceremony that catches nothing.* All
  km/(1/km) round-trips are bit-exact (×1000⇄÷1000 verified) so GPU==oracle held. Verified: win-debug full 5061 + a **focused**
  build+test of only the 7 touched targets on shipping/asan/release/tidy (CI owns the full matrix — don't re-grind untouched
  modules for a localized diff). Transient LTCG `C1001` cleared on retry (known). Rule: at a real slice close, run `ctest` +
  the LTCG configs, not the binary; and a config-specific build failure on ONE dir is a stale-obj/deps fingerprint, not code.

- 2026-07-15 — **A missing upload barrier is a grid-size time bomb (rules #4/#10; B16-a-3 multi-cascade):** the shared
  `dispatch_fft2d` harness copied inputs then dispatched pass 0 with **no `TransferDst→ShaderRead` barrier**, so a large-enough
  grid raced the still-in-flight upload and read STALE data. Latent for the whole GPU-FFT campaign because every prior 2-D test
  was small-batch (single image / batch ≤ 8) and the upload happened to finish first; B16-a-3's batched IFFT at batch = 4·C
  (C≥3 ⇒ batch ≥ 9, grid > device occupancy) first exposed it — **flaky, ~all-wrong past the threshold, bit-exact below**. The
  non-determinism (bad counts differed run-to-run) is what fingered it as a race, not an offset bug; the `[.ocean-ifft-bench]`
  self-verifying at batch 64 proved the KERNEL correct and localized it to the harness. Root-fixed (the exact `dispatch_1wg`
  upload-barrier scar, [[feedback_dispatch_1wg_missing_upload_barrier_race]]); batch 8–16 now bit-exact on Vulkan + DX12.
  Lesson: a multi-pass GPU harness needs the upload→first-read barrier as much as the between-pass barriers; small-grid tests
  never prove it.

- 2026-07-10 — **The ORACLE was more accurate than the kernels it certifies — a reference that cannot be matched cannot certify (rules #2/#4/#9; D-007 B0 fan-out):** `ckir_eval`'s A3 vec/mat corpus (Dot/VecLen/Normalize/Cross/MatVecMul/MatMatMul/Determinant/MatInverse/OuterProduct/geometric/quats/Slerp) accumulated in **f64 and rounded only on store**, while the elementwise ops and `Contract` rounded **every step**. For an F32 graph the oracle therefore computed an f64 dot rounded once, where any f32 GPU rounds each multiply-add ⇒ ~1 ULP delta with the reference wrong-by-being-better, leaving **ADR-0098's T1 certified-bit-exact core unreachable for vec/mat — i.e. for most of a shader**. It hid because **every Vulkan/DX12 vec test compares against ANALYTIC references with tolerances, never against the oracle** — nobody *could* gate bit-exactness against it, so nobody noticed it couldn't be matched (a structural blind spot: the reference itself was un-referenced). Surfaced only when the CUDA fan-out — scalarized, hence emitting explicit elementary ops with `--fmad=false` — first pointed the oracle at a vector graph and asserted `==` (320 mismatches). Fixed at the root (`eval_detail::rnd`; `mat_det`/`mat_minor_det` take the dtype): every elementary IEEE op rounds to the node dtype. Safe to retrofit — `round_dtype` is the identity for F64, so the 200-assert CPU suite passed unchanged first try. **Payoff:** CUDA vec3/mat3/bvec/struct now gate **bit-exact (`==`)** vs the oracle, the strongest gate in the repo. **Named, bounded gap:** GLSL/HLSL/WGSL call `dot()`/`normalize()`/`inverse()` **builtins with implementation-defined internal order**, so those stay ULP-tolerant until ADR-0098 §5's `float_controls` audit — stated, not assumed. Rules: bit-exactness needs BOTH (a) an oracle that rounds per elementary op and (b) a backend emitting the same ops in the same order with no FMA contraction; and *a reference whose tests only ever compare against analytic values has never been proven matchable.* Same session, same shape: the tidy gate that printed PASS without parsing, and `KirBackendCpu` ignoring `comps()` (vec graphs heap-overflowed) — three instruments, none of which had been pointed at themselves.

- 2026-07-05 — **MSVC /O1+/O2 auto-vectorizes a per-lane conditional TWO-ARRAY update WRONGLY — write lane logic as manual vector select chains (rules #4/#5/#10 compound; v14-h LU):** the batched-LU lane tier returned provably-false pivot comparisons on raw data, win-shipping only; /Od + gcc green, **ASan structurally blind (wrong-code, no bad access)**, fprintf-in-loop suppressed it (heisen). THREE plausible theories measured and KILLED first (stack UMR via poison-fill; alias-reorder via atomic_signal_fence; LTCG inline mis-scheduling via noinline seams — each zero effect); a 60-line standalone repro + flag bisection then pinned the true construct: MSVC's auto-vectorization of `if (v > best[q]) { best[q] = v; pr[q] = i; }` (masked blends over two arrays) — NOT LTCG (reproduces without /GL), NOT /O2-specific (/O1 too). Root fix at the construct for all consumers: pivot scan = pure manual-vector argmax (cmp/select, indices in f64 lanes); the same conditional-two-array shape hardened in the chol/LU singular checks. The fix measured FASTER than the miscompiled loop (LU 1.27–2.87× → 1.76–3.81× vs MKL). Rules: per-lane scalar `if (...) { two updates }` loops next to vector code are FORBIDDEN in lane kernels — express them as select chains; and a theory is not a fix — each of the three plausible mechanisms would have shipped as a lie without the measured refutation.

- 2026-07-05 — **A borrowed-lifetime member in a returned object is a cross-config time bomb (rules #1/#2/#4 compound; v14-g):** `HyperTree` stored `const HyperNet*` for index metadata; the driver's finalist path built trees from a lambda-LOCAL net ⇒ every later `stats()` read freed memory. **gcc -O3 was fully green (heap-reuse luck), MSVC-debug SEGV'd at a distant destructor, win-asan converted it to a precise OOB assert** — localized with rule-#10 flushed markers then ASan (match the tool to the bug class). Root fix: the tree OWNS copies of sizes/appearances — never a borrowed lifetime in anything that outlives a call (the allocator-outlives-borrowers scar, reference-member edition). ⚠ Rule-#2 tail: the pre-fix bench board contained garbage-derived values that looked BETTER; re-measured on the fixed artifact + corrected. Same session: an exact-value gate caught a pool-reallocation UAF in `merge_legs` (reserve-before-spans) — two UAFs, both found by discipline (exact values + cross-config), neither by the green gcc suite.

- 2026-07-05 — **The /Od stack-bomb scar has an LTCG sibling: `__forceinline` generated kernels are a link-time compiler-heap bomb (rules #1/#5):** the win-shipping FFT link died after ~40 min with C1002 (out of heap, pass 2) at `fft.hpp(895)` — MSVC honors `__forceinline` under LTCG, so 56 generated codelets (bodies to ~9K lines, 143K-line header) + both `execute_ip4aos` instantiations inlined into the ONE `execute()`; the mega-function exhausted pass-2 codegen heap (17 GB WS observed; system RAM never the limit). Root fix at the emitter, MSVC-scoped: `CRD_FFT_GEN_INLINE` (MSVC = plain `inline` — its /O2 cost model declines giant bodies; gcc/clang keep always_inline ⇒ measured boards untouched) in `batched_codelets_gen.hpp` + `gen_fft_batched.py`, plus a `__declspec(noinline)` seam on `execute_ip4aos`. Relink green, shipping ctest 29/29. Rule: giant generated straight-line kernels must never carry an unconditional force-inline — every unbounded inliner (debug stack, LTCG heap) eventually detonates; put the attribute policy IN the generator.

- 2026-07-03 — **A giant generated straight-line kernel is a debug-build stack bomb — measure the frame, fix at the emitter (rules #5/#10):** the first win-debug run of the FFT standalone-hier/deep-split paths SEGFAULTed (0xC00000FD); the mechanism was MEASURED before fixing (dumpbin `sub rsp` probe): at `/Od` MSVC gives every expression temporary its own un-reused slot ⇒ the 256-point codelet frames are **1.17–1.4 MB EACH** (~236 B/SSA value) — one call overflows the 1 MB Windows default stack; linux never sees it (8 MB stacks) and `#pragma optimize("gt",on)` in a /Od compiland does nothing (tested — frames stayed ~1 MB). Root fix at the GENERATOR, all consumers: dual-body emission — SIMD tiles under `NDEBUG||__OPTIMIZE__`, a lane-scalar edition (same DAG, same order per column ⇒ bit-identical; `0−x` not unary minus for signed-zero identity) otherwise, frames 23–57 KB measured. ⚠ the fix's own scar: the first gate (`NDEBUG` only) silently sent ad-hoc `g++ -O3` bench builds (no `-DNDEBUG`!) to the scalar bodies — caught by re-running the board after the regen (rule #2: verify the shipped artifact), closed with `__OPTIMIZE__`.

- 2026-07-02 — **A std concurrency primitive lost the wake — own the primitive (rules #1/#4/#5 compound):** the red 18-config CI (a DIFFERENT jobs-parallel moat test timing out >1500 s each run, Linux-only) was root-caused by *reproducing* it (4-CPU-pinned taskset loop in WSL, hangs at iters 6-76) then *dumping* it: CPU-ticks-over-5s == 0 killed every spin/livelock theory in one number; gdb stacks showed main in `shutdown()→join()` + one worker in `counting_semaphore::acquire→futex`; `/proc/tid/syscall` + `x/dw` on the futex word proved the worker asleep with **expected==1 while the counter word read 1** — a token present, no wake ever coming. Mechanism: GCC 13.3 libstdc++ preloads the futex expected BEFORE the predicate spin (`_S_do_spin`) and skips the wake when the counter was already >0 (`_M_release`, which carries its own FIXME) — the PR104928 class; our protocol above it was sound. Fix at the root: `crd::jobs::detail::Semaphore` (futex/WaitOnAddress, sleep only at observed-0, release always wakes) — the LAST std concurrency primitive in the jobs hot path is now Cerid-owned like the fibers/deques/MPMC. Repro loop: 300 clean post-fix. Lesson: a "can't-be-our-code" hang deserves the same forensics as our code — and a paper-correct protocol proof does NOT extend to the primitive it stands on (rule #4: name what your proof is blind to).
- 2026-07-02 — **A timeout is not a hang proof (rule #5's face of the binomial scar, inverted):** test 2661 (the fat-front NODE-PARALLEL moat) "hung" win-asan — a 150 s standalone timeout + a killed ~40-min sweep "confirmed" it, and the first diagnosis ("genuinely hangs, fiber/ASan bug") was WRONG. The real evidence: the win-debug baseline was **456 s** (a huge /Od test), the "hung" process's CPU climbed linearly, and thread stacks (a purpose-built DbgHelp `stackdump.exe` — no cdb on this host) showed the main thread hot in `gemm_microkernel_avx2_f64` with `_asan_loadN` on every SIMD load (ASan-on-debug ≈ 5-6×) and all workers parked in `WaitOnAddress` — **slow, not stuck; no deadlock, no fiber annotation gap**. Root fix, not a timeout bump: replaced grid3d(28) with `bordered_spd(24,48,560)` (dense border = a 560-col root supernode receiving cmod from 24 block supernodes — the SAME divergent paths, guard-enforced by `maxnc≥512` + a new `nsuper≥min` REQUIRE) ⇒ **456 s → 9.3 s debug, ~40 min → <1 min asan**, 53,214 asserts green both configs. Check before ever calling a hang: (1) the same test's baseline in a lighter config, (2) CPU-climb over minutes, (3) thread stacks.
- 2026-06-30 — **v13 1-D SCALAR quadrature ENGINE (v13-g/h/i) closed with ZERO open comparisons** (⚠ the quadrature MODULE is NOT done — the oscillatory v13-j {QAWO/QAWF/QAWS/QAWC/Levin} + multi-D cubature v13-k {Genz-Malik/Smolyak/Lebedev} rows are still pending; honest scope, not "module complete") — every IMPLEMENTED method (v13-g composite/Gauss·Lobatto/Radau/Newton-Cotes + v13-h adaptive QUADPACK QNG/QAG/QAGS/QAGP/QAGI + v13-i DE tanh/exp/sinh-sinh/Clenshaw-Curtis/Fejér/Romberg) beats EVERY available frontier peer: scipy + MATLAB + Boost + **GSL 2.7.1** (installed mid-session the moment the user demanded the full board). **The recurring crush lever (a fresh face of #5/#9): integrand-INDEPENDENT work must be precomputed ONCE, never per call** — it was the inefficiency behind FOUR losses that all flipped to wins: (1) the GK error estimate's `crd::math::pow(·,1.5)` (heavy double-double) → `x·√x` (one hardware sqrt) = QAGS 0.88×→**1.29×** / QAGI 0.89×→**1.36×** GSL; (2) Gauss symmetric-pair = parity→**edge** vs Boost `gauss<10>`; (3) the DE convergence estimate `d²/dₘ₋₁` (the double-exponential rate, halving the levels) = exp_sinh 0.48×→**1.17×** Boost; (4) the Clenshaw-Curtis O(N²) clencurt weights recomputed per call → precomputed `CcAdaptiveRule` = 0.59×→**2.05×** GSL-cquad. **Meta-scar: the user REFUSED "near-parity with the reference C is the ceiling"** ("I don't accept near-parity, CRUSH") — and was right every time: parity-with-the-same-algorithm is NEVER the wall; a per-operation cost (a heavy pow, a per-call recompute) always is. **Method that found every win: reconstruct-and-verify-in-python FIRST** — fetched scipy's `_interpnd.pyx` / `__quadpack.c` (the dqagse/dqelg/dqng C) / `_rules/_gauss_kronrod.py` + QUADPACK constants via `gh`, verified the algorithm bit-exact in python before porting a single C++ line (caught a Clough-Tocher gradient sign-flip + the GK roundoff-floor pre-port; faithful goto-preserving transliteration = the v7 NLopt-port discipline). Full peer board (scipy+MATLAB+Boost+GSL) on EVERY row; N/A stated with the check, never dropped (`feedback_bench_all_peers_never_cherry_pick`).

- 2026-06-25 — **tx-a audit caught the crd-math transcendental cluster's premise before building it** (search+measure-before-build, rule #8): `crd::math::deterministic` ALREADY ships sin/cos/tan/exp/log/pow (f32/f64/SIMD) + the newer `crd_exp1/crd_log1` cores exist — NOT a green-field. Measured: `crd_log1` ~1–2 ulp + 1.6× faster than libm (good); `crd_exp1` 1.4× faster but only ~1e-13 (Taylor, not ≤1 ulp) + denormal-broken; **`deterministic::sin` is 2.5× SLOWER than `std::sin`** (it traded speed for bit-exactness). ⇒ the cluster is UPGRADE+UNIFY+complete+route, not build; the hard "no std:: math" guard must wait until the lib is actually faster-than-libm. The Cerid Math Mandate (use `crd::math::*`, implement-if-missing) recorded in `reference_cerid_math_mandate` + the mpmath ulp-gate harness shipped. Premature enforcement would have *slowed* the engine (routing to the slower deterministic:: trig).


- 2026-06-25 — Added **rule #9** (a documented loss is an open bug, not a closed slice) after the user caught me presenting v12-d's Boost losses (Lambert-W 0.05×, K/E 0.23×) as "honest disclosure" instead of fixing them.
- 2026-06-25 — **Rule #9 PROVED: crushed ALL 7 v12-d functions vs Boost** (were 0.05×–0.91× losses) via generated minimax rationals (Chebyshev fit + Lawson reweighting → monomial `.inc`, gated ≤ each function's tolerance): E1 **7.98×**, Ei **6.55×**, zeta **20.5×** (replaced 8 `std::pow` Euler-Maclaurin terms), lambertW0 **1.17×** (3-piece rational, no Halley), ellint_K/E **1.05×** (full-range Cody form A+L·B), Carlson_RF **1.78×** (loosened `errtol` to the 1e-12 gate ⇒ one fewer duplication). Accuracy preserved (special 402081, DSP 27069, stats 317795 green on gcc+MSVC). **Meta-scar: my eval-cost pessimism was wrong 7/7 times** — I predicted K/E/lambertW "can't beat Boost" from hand-estimated ns, then each WON when measured. Lesson: estimate to *prioritize*, never to *conclude a wall* — wire it and measure (SANITY #5). The "fundamental wall" instinct lost every time to actually doing the work.
- 2026-06-09 — Root-caused + fixed `TlsfAllocator::init_pool` end-sentinel (was 16 B early → tail-slack overshoot); reverted the LU `factor_pool` workaround; closed the misdiagnosed `growable-tlsf-multichunk-freelist` debt.
- 2026-06-09 — Added `[memory][tlsf][boundary]` exact-fit-to-tail regression test (0xCD-poisoned buffer; drives a free block below the 512 B small-block boundary so a no-split alloc reaches the tail) — **verified it bites**: SIGSEGV on the buggy one-liner, passes (debug + ASan) on the fix. First two drafts were theater (passed on buggy code too) — see rule #2/#3.
- 2026-06-09 — Wrote this doctrine + trimmed the bloated MEMORY.md index line that motivated rule #3.
- 2026-06-09 — Created `docs/README.md` (the Documentation Map / Start Here); consolidated the three parallel onboarding lists (CLAUDE checklist, AGENTS re-entry prompt, ROADMAP "core docs") to point at it (one canonical reading order); defrosted the stale public `README.md`.
- 2026-06-09 — Pruned `context.md` to its own stated dashboard shape: **274 KB → 6.4 KB** (34 lean lines). Removed the stacked historical `As of DATE` / `Earlier same day` snapshots (all already in session logs + memory); kept live focus + coming-up + detour + last-shipped + a one-line-per-cluster recent history. Old version recoverable via git.
- 2026-06-10 — Root-caused the win-shipping phantom link failure to **`#deps 0` ninja deps** (`msvc_deps_prefix` English vs Turkish cl.exe ⇒ header changes silently never recompile — rule #2's exact class: a stale artifact can fail OR pass for the wrong reason); audited all 13 build dirs (win-shipping + win-tidy-local broken) and **FIXED them in-session** (wipe + standalone-CMake reconfigure; verified `#deps 0 → #deps 95` + suite green — no debt filed, per rule #1); CLAUDE.md Troubleshooting entry added; also fixed a pre-existing tidy naming error (`kTriPanel`) that had slipped into `b261478` the same way (stale tidy dir view).
- 2026-06-10 (later, v7-j) — The `#deps 0` fix was INCOMPLETE (rule #1's own test: the first "root cause" was a symptom) — a mixed-struct-layout shipping SIGSEGV exposed that **any in-build regenerate re-broke the dir**; the real root is the **VS-bundled CMake fork (`4.2.3-msvc3`, first on PATH under vcvars) storing an English `showIncludes` detection on this Turkish-locale host**. Durable fix executed: explicit standalone-CMake invocation policy + purged all `CMakeFiles/*-msvc*` stored detections + wiped/rebuilt win-shipping (verified `#deps 108` + incremental header rebuilds work); CLAUDE.md entry rewritten with the true mechanism.

- 2026-06-24 — Root-caused a win-debug **infinite loop** (595 s CPU, never terminating) in v12-f `binomial_inversion`: it was not reflection-aware, so the dispatcher's {n=500, p=0.95} (routed to inversion because n·min(p,1−p)=25<30) used raw p ⇒ `q^n = 0.05^500` underflows to 0 ⇒ the inversion `px` stays 0 ⇒ x climbs past `bound` ⇒ x>n ⇒ outer loop retries forever. Fix = reflect internally like `binomial_btpe` already does (sample Binomial(n, 1−p), return n−x). **Boundary-adversary (rule #3):** 282 K passing assertions sailed over it — only the single adversarial param p>0.5-at-large-n ({500,0.95}) reached the dead branch; the optimized full-suite run is what exposed the non-termination (debug just hid it behind slowness). Also: an optimized green is the artifact to trust (rule #2) — the debug "still running" was a *hang*, not slowness.
- 2026-06-21 — Root-caused a SIGSEGV in `crd-hesap-dense` `eig_real_impl`/`eig_complex_impl`: both guarded **n==0** but not **n==1** (the balance/Hessenberg pipeline assumes n ≥ 2), so a 1×1 matrix crashed in `hessenberg`. Exposed by v11-q `residuez` calling `roots()` on a degree-1 polynomial (1×1 companion). Fixed at the root (a 1×1 matrix's single entry IS its eigenvalue, eigenvector [1]) in both paths — benefits every eig consumer, not just `roots`. Boundary-adversary class (rule #3): the bug lived only at the smallest valid size, which random/large tests never hit. (Confirmed via grep: NO malloc/new/std-container in the new DSP code — crd containers only.)
- 2026-06-28 — Added **rule #10** + root-caused/fixed the `wpt` CI SegFault: MSVC 19.51/14.51 `/O2`+LTCG miscompiled the inline `crd::usize{1} << level` in `WaveletPacket`'s ctor, `best_basis`, and `reconstruct` to a garbage stack-address value (`count=140698301264476` at `level=2`) ⇒ ~1e14-iteration inner loop ⇒ OOB ⇒ SegFault. **win-release/shipping only**; local MSVC 14.50 + gcc green; **win-asan blind (ASan disables LTCG)**. Found via a minimal `branches-ignore:[main]` CI job + flushed-stderr markers (escalated to numeric values when structure looked right). Fix: iterate by heap id (ctor + best_basis) / halving counter (reconstruct) — no in-loop variable shift; **verified green on the CI's exact 14.51**. Also fixed a clang-cl `-Wunused-lambda-capture` (constexpr `cap`). **3 wrong guesses first** (older-MSVC, reverse-idiom, NOINLINE) — the numeric marker was decisive.

- 2026-07-10 — **Two Accepted ADRs contradicted each other, and the plan followed the wrong one.** ADR-0101 says backend languages are outputs only, never authored or stored; **ADR-0099 §6 says `crd-shader` IS the shared GLSL/HLSL→SPIR-V/DXIL compiler** that CKIR routes through. Both Accepted. The D-007 B3 plan cited §6 and proposed gating the raster emitters on `crd::shader::compile_glsl(Stage::Vertex)` — making CKIR *depend on* a GLSL compiler, the exact inversion ADR-0101 exists to delete. Caught by the user, not by review. **Scar → rule:** an ADR that supersedes part of another must **strike the superseded clause in place, in the old document** (done: 0099 §6 is struck through and points at ADR-0103), because the next reader lands on the old text and follows it. A "Superseded" line in an index nobody reads is not a check. Corollary of rule #2 (verify the *shipped* artifact): the shipped artifact of a decision is the sentence someone will read six months later.
- 2026-07-10 — **A green test binary is not a green ctest.** `crd-kir-tests.exe` printed "All tests passed" while **6 of its cases could not be selected by ctest at all**: their `TEST_CASE` names contained an em-dash, and Windows decodes ctest's argv through the Active Code Page (CP1254 here), so Catch2's filter matched nothing and ctest reported `Failed`. The repo already has a ctest-registered guard for exactly this (`crd-no-non-ascii-test-names`) and it was **red**, plus `feedback_ascii_only_test_names` in memory — the rule, the check, and the memory all existed; only *running ctest* was skipped. 9 names fixed (kir + kir-vulkan); guard green. Reinforces `feedback_per_slice_run_ctest`: **guards are ctest-registered, so a binary-direct run cannot see them.** Also: a bare `ctest` outside vcvars fails `crd-simd-emission-check` (`dumpbin` not on PATH) — use `scripts/run-ctest.bat`, which sources vcvars, or you will chase a phantom.

- 2026-07-27 — **A shader pair can disagree and there is no layer that can tell you.** The scene’s SKINNED vertex program emitted 2 of the 4 varyings every cooked fragment program reads, so a skinned draw shaded from UNDEFINED interpolants at locations 2 and 3 — it linked, it bound, it rendered, and neither Vulkan validation nor the DX12 debug layer can see it. Found only when the varying set became DECLARED and a cook-time contract check (38-D4) compared the two by name, location, width AND interpolation. **Rule:** when two artifacts must agree and no runtime can check it, the agreement has to be a DECLARED contract verified at cook time — a convention is not a check. A name-only check is not one either: it passes a vec2-at-location-3 against a vec3-at-0.
- 2026-07-27 — **A helper’s documented "caller error" arm builds a graph that compiles the wrong thing.** `lighting::pcf_shadow` takes a **vec2** uv (a 2-D shadow map); every atlas in this engine is LAYERED. Passing a vec3 hits `nodes::detail::bin`’s *"two mismatched vectors — a caller error; leave to the shape checker"* branch, which emits a shape-invalid node: the COOK returns a valid node id and the SHADER fails to compile, with nothing in the message pointing at the uv width. **Rule:** a library arm that says "caller error" and returns a value anyway is a silent failure at the call site — read the operand WIDTHS a helper assumes, not just its parameter names. Rule #2’s shape: the artifact to trust is the emitted shader, not the graph that built cleanly.
- 2026-07-27 — **A convention documented one function away is a convention you will still walk into.** `scene_renderer.cpp` carries an explicit comment that the frame header stores the direction TOWARD the light while `lighting::directional_light` wants the direction light TRAVELS (it negates internally) — and I copied the header value straight into the new light record anyway. N·L ≤ 0 everywhere: a uniformly dark frame that still DRAWS. **Rule:** cross a convention boundary in exactly ONE place and name it there; the second copy of a negation is the one that is missing. Same class as the CSM shadow-camera inversion — two libraries, two conventions, no type to catch it.
- 2026-07-27 — **`parse_*_toml` never reset its output descriptor**, so parsing a second asset into a reused one APPENDED: overlapping names surfaced as `DuplicateName` (an error naming the wrong thing) and distinct names as a silently MERGED layout. Present in two independently written cookers. **Rule:** a parse-into-out-param owns the WHOLE of that object — reset first. Any tool with a load button hits this on the normal path, not as an edge case, which is why "the tests construct a fresh desc each time" hid it.
- 2026-07-27 — **Two argument kinds spelled identically in C++, and only a coverage gate could see it.** In `ckir_nodes.hpp` a WIRE (node id) and a COMPILE-TIME ATTRIBUTE (`extract`’s channel index, `convert_f_vec`’s width, `place2d`’s order, a geometric reader’s varying `location`) are both `int`. A generated op registry passed node ids into all of them: type-checks, builds, swizzles component 47. The same gate found `kMaxNodeInputs = 5` while the widest node takes SEVEN (making `gooch_shade`/`range` unauthorable, rejected for a reason that named the wrong thing). **Rule:** when a registry is generated from signatures, the generator must know what each argument MEANS — and the gate that proves it must CONSTRUCT every entry from the registry’s own account of its slots, not merely list them.
- 2026-07-27 — **Four gates of my own that could not fail.** Probe constants that also occur in the baseline (0.25 is a tint component; 0.8 is `surface_defaults`’ base colour); an IES baseline whose record already declared `ies_index` (comparing the feature with itself); a displacement check that only asserted the literal was PRESENT (passes with the node built and never wired — fixed by running both graphs through B7 `lower_entry` so DCE removes what the entry cannot reach); and a clustering check that would have passed with the cluster list declared and ignored (fixed by asserting the unrolled program gets SMALLER). **Rule:** before trusting a gate, ask what value would make it fail — if the answer needs the baseline to change too, the check is measuring nothing.
- 2026-07-27 (audit pass) — **The checker a comment defers to must EXIST, and its rules must come from the oracle.** Built `ckir_shape.hpp` (the checker `detail::bin` had always named); on its FIRST corpus run it found two latent defects in shipped light-cook paths: the PCSS blocker search reading depth through the COMPARISON sampler (an overload that does not exist in GLSL — the cook succeeded, the shader never could compile) and contact shadows feeding SCALARS to the 4-tap `contact_shadow` helper (lanes 1..3 of a 1-wide value; the close gate had measured node counts). **Rule:** derive validator rules from the oracle's actual read loops, wire the check at the cook boundary with a POINTING error, then run it over everything that already exists — the first catches are usually real.
- 2026-07-27 (audit pass) — **Byte-identity round-trips are blind to fields BOTH sides drop.** Frame blob v3 silently lost every post-REN-36 pass field (the RT pipeline's three program names, VRS, queue, sampler, filter) and the vertex emitter lost the per-stage parameter sections — while both round-trip gates stayed green, because a field dropped by the writer AND the reader round-trips "byte-identically". And the shipped `.crdl` was outright CORRUPT because it was an inert copy nothing ever parsed. **Rules:** serializer gates assert FIELD SURVIVAL (parse → cook → read → field-by-field against the parsed original), and two copies of one declaration get a CANONICAL-FORM drift gate the day the second copy is born.
- 2026-07-27 (audit pass) — **A backend half proven by compile only hides its whole dispatch path.** A16's DX12 RT pipeline had never been RUN by a device gate (HLSL-lowering compile only) — so the HLSL emitter's missing any-hit entry arm, the lazy `supports_rt_pipeline()` (the exact scar A16 fixed on Vulkan, alive in its DX12 twin), and the opaque-geometry any-hit skip all sat unfound until the first real DispatchRays gate ran. **Rule:** "both backends" requires one EXECUTING gate per backend with a distinguishable assertion; capability queries answer from the feature check, never from lazily created state.

<a id="docs-sanity-open-sanity-backlog-small-claimable"></a>
##### Open sanity backlog (small, claimable)
- Harden the remaining in-loop `crd::usize{1} << <loop-var>` shifts (swt.hpp `dil`/`step`, modwt.hpp `dil`) against the rule-#10 MSVC-LTCG miscompile — they pass on 14.51 today (CI-proven), but are the same fragile pattern; prefer a non-shift form (doubling counter / heap-id iteration) when next touched.
- Trim `MEMORY.md` back under its session-load limit — do it incrementally, don't risk losing info. **2026-07-10: 20,756 → 18,699 bytes by tightening hooks; all 175 links verified present before/after (`comm` on the extracted link sets). Still ~1.2 KB above the 17.1 KB advisory target — closing that gap needs entries MERGED or DROPPED, not hooks trimmed (593 non-ASCII chars cost ~1.2 KB alone, and the ⛔/⭐ scan markers earn their bytes). Hard read limit is 24.4 KB, so the index loads fully today.**
- Adversarial boundary-test pass on `crd-containers` (String/Array/HashMap capacity-edge cases).
- Confirm (don't assume) no other heavy-churn `GrowableTlsfAllocator` consumer was relying on the old behaviour now that `init_pool` is fixed.
- **Doc bloat (living/scannable class only — never truncate session logs/ADRs/dossiers):**
  - ~~Collapse `docs/ROADMAP.md` status-table rows to one line per phase~~ — ✅ DONE 2026-08-07 (doc-hygiene pass; the discipline stays: phase history belongs in the phase doc, never in the hub).
  - ~~Prune **closed** entries from `docs/debt.md`~~ — ✅ DONE 2026-08-07 (982 → ~360 lines; salvage in the hygiene session log). The rule is RECURRING: prune at every close, don't re-accumulate.

---

<a id="docs-building"></a>
## Source: docs/BUILDING.md

<a id="docs-building-building-testing--troubleshooting"></a>
### Building, Testing & Troubleshooting

The canonical build/verification reference for Cerid. Coding standards, engineering principles,
and contributor conduct live in [`AGENTS.md`](../../AGENTS.md); the doc map in [`docs/README.md`](../README.md).

<a id="docs-building-requirements"></a>
#### Requirements

- **C++20** (no compiler extensions) · **CMake ≥ 3.25** + **Ninja** (`CMakePresets.json`)
- **MSVC 2022/2026** (primary; VS path `C:\Program Files\Microsoft Visual Studio\18\Community\`),
  clang-cl (verified in CI), GCC (Linux in CI)
- **Vulkan SDK 1.3+** (`$env:VULKAN_SDK` must be set for shader compilation)
- Test framework: **Catch2 v3** (via CPM.cmake) · Format: clang-format · Lint: clang-tidy
- MSVC `/Zc:preprocessor` required (for `__VA_OPT__` in log macros)
- Config: toml++ · Graphics: GLFW 3.4, Vulkan 1.3, shaderc, spirv-reflect · Debug UI: Dear ImGui (docking)

<a id="docs-building-quick-start"></a>
#### Quick start

```powershell
<a id="docs-building-windows-with-msvc-primary"></a>
### Windows with MSVC (primary)
cmake --preset win-debug      # Debug + asserts + profiling
cmake --build --preset win-debug
ctest --preset win-debug

<a id="docs-building-other-key-presets-win-release-lto--win-relwithdebinfo--win-clang-cl-"></a>
### Other key presets: win-release (LTO) · win-relwithdebinfo · win-clang-cl ·
<a id="docs-building-win-asan-see-the-asan-dll-note-below--win-shipping--win-tidy-tidy-runs-during-build"></a>
### win-asan (see the ASan DLL note below) · win-shipping · win-tidy (tidy runs during build)
<a id="docs-building-linux-ci-mirrors-linux-gcc-debugreleaserelwithdebinfoasanshippingdebug-sse2"></a>
### Linux (CI mirrors): linux-gcc-{debug,release,relwithdebinfo,asan,shipping,debug-sse2}
```

```powershell
<a id="docs-building-format--lint-a-single-file"></a>
### Format / lint a single file
clang-format -i <file>
clang-tidy -p build/win-debug <file>

<a id="docs-building-single-test-binary-note---reporter-not---reporters"></a>
### Single test binary (note: --reporter, not --reporters)
& "D:\Dev\cerid\build\win-debug\tests\<module>\crd-test-<module>.exe" --reporter compact
& "...\crd-<module>-tests.exe" "[tag]"     # filter by tag
```

<a id="docs-building-per-slice-verification-definition-of-done"></a>
#### Per-slice verification (Definition of Done)

> ⛔⛔ **WHOLE-REPO verification is CI's job, NOT the local host (user directive 2026-08-15; SANITY #11).** Locally, build +
> run ONLY the module(s) you changed + their blast radius — e.g. `scripts/build-target.bat build/win-debug <target>` then the
> specific `[tags]`/named tests you touched, plus the WSL Linux legs for GPU code. Do **NOT** run `per-slice-check.ps1` /
> `full-sweep.ps1` over the whole repo on this machine: a whole-repo sweep is a **multi-hour** job (e.g. win-asan ctest ≈ 3 h
> over 6384 ASan-instrumented tests) that CI runs in parallel on dedicated hardware. The commands below are the **CI recipe**,
> kept here for reference. Still FIX every bug you see — the scope rule is about not re-running untouched modules, never about
> looking away from a defect. If a local GPU test must run, bound it (`ctest --timeout N`) so it can never wedge the session.

```powershell
<a id="docs-building-4-config-dod-debug--asan--shipping--tidy--default-for-cpu-only-slices"></a>
### 4-config DoD (debug + asan + shipping + tidy) — default for CPU-only slices.
<a id="docs-building-run-sequentially-no--parallel-and-ninja-capped--see-host-instability-below"></a>
### Run SEQUENTIALLY (no -Parallel) and Ninja-capped — see "Host instability" below.
.\scripts\per-slice-check.ps1

<a id="docs-building-5-config-adds-win-release--gpu--ltcg-sensitive-slices"></a>
### 5-config (adds win-release) — GPU / LTCG-sensitive slices
.\scripts\per-slice-check.ps1 -IncludeRelease

<a id="docs-building-cluster-close-the-18-config-full-sweep-11-windows--7-linux"></a>
### Cluster-close: the 18-config full sweep (11 Windows + 7 Linux)
.\scripts\full-sweep.ps1

<a id="docs-building-linux-presets-from-windows-wsl2-builds-to-cerid-build-on-native-ext4"></a>
### Linux presets from Windows (WSL2; builds to ~/cerid-build/<preset> on native ext4)
.\scripts\wsl-build.ps1 linux-gcc-release
```

**Verification runs `ctest --preset <X>`, NOT the test binary directly.** Guard tests
(`crd-no-non-ascii-test-names`, `crd-simd-emission-check`, `crd-no-std-math-check`,
`crd-no-std-sort-check`, `crd-no-untagged-physical-numeric`, `crd-no-std-transcendental-check`,
`crd-hesap-v13-no-exceptions`, …) are registered only with CTest and never appear in a test binary's
`--list-tests`. A binary saying "All tests passed" can coexist with a failing guard — both must be green.

**Smoke tests** are standalone executables in `build/<preset>/runtime/` and are NOT registered with
CTest. Headless set (no GPU/window): `smoke_config smoke_containers smoke_filesystem smoke_frame_clock
smoke_jobs smoke_log smoke_math smoke_memory smoke_meshgen smoke_resources smoke_resources_async
smoke_resources_reload smoke_resources_stream smoke_texture smoke_mesh smoke_hesap_substrate
smoke_hesap_blas1 smoke_hesap_blas2 smoke_hesap_blas3 smoke_hesap_sparse smoke_hesap_matrix_resource
smoke_hesap_solve_cli smoke_hesap_tensor smoke_virtual_memory smoke_virtual_memory_allocator`.
GPU/window set (run manually): `smoke_app smoke_window` + **`crd-sandbox --smoke-test N`** (the real GPU
smoke — validation ON, N seconds, zero-validation-output gate; `--headless` for CI). The nine
retiring-stack smokes (`smoke_shader/renderer/imgui_overlay/rhi_api/rhi_vulkan_bootstrap/material/
resources_render/asset_import/depth_prepass`) were DELETED at RET-7 (2026-07-23, ADR-0105) — their living
coverage is the sandbox smoke + the gpu-context test suites.

**Benchmarks:** every measured board is written to [`docs/bench/`](../bench) at measurement time
(convention + naming in `docs/bench/README.md`).

<a id="docs-building-adding-a-module--test"></a>
#### Adding a module / test

1. `engine/<name>/include/crd/<name>/<name>.hpp` (umbrella) + `src/` + `CMakeLists.txt` (copy an
   existing module's pattern); `add_subdirectory(engine/<name>)` in the root CMakeLists.
2. `tests/<name>/CMakeLists.txt` + at least one Catch2 test; auto-discovered by CTest.
3. Smoke in `runtime/examples/smoke_<name>.cpp` where relevant.
4. Document in `docs/systems/<name>.md` once shipped.

<a id="docs-building-platform-notes-powershell"></a>
#### Platform notes (PowerShell)

- Use PowerShell-compatible commands (`Get-Content`, `Select-String`, `Remove-Item`), not POSIX tools.
- Run executables with absolute paths + the call operator: `& "D:\Dev\cerid\build\...\smoke_foo.exe"`
  (relative `.\build\...` fails in some invocation contexts).
- **PowerShell 5.1 text I/O mangles UTF-8** (`Get-Content -Raw`/`Set-Content -Encoding utf8` reads
  ANSI + writes BOM ⇒ mojibake). For scripted edits use
  `[IO.File]::ReadAllText($f, [Text.Encoding]::UTF8)` + `WriteAllText` with
  `[Text.UTF8Encoding]::new($false)`.

<a id="docs-building-troubleshooting--known-issues-and-permanent-fixes"></a>
#### Troubleshooting — known issues and permanent fixes

<a id="docs-building-host-instability-i9-14900k-raptor-lake--cap-builds-never-run-all-core"></a>
##### Host instability: i9-14900K Raptor Lake — cap builds, never run all-core

The dev host's 14900K carries the documented Vmin-shift instability defect; sustained all-core builds
trigger `0x0000000A` bugchecks. **Software cap = harm reduction; the hardware fix is BIOS Intel Default
Settings + IPDT + (if failing) Intel's 5-year RMA.** Mandated workflow: run the DoD **sequentially**
(never `-Parallel`); builds are Ninja-capped (`per-slice-check.ps1` / `full-sweep.ps1` default
`-BuildJobs` to half the logical cores; tuning ladder 16 → 12 → 8 → 6); set a persistent user env var so
ad-hoc builds are capped too: `[Environment]::SetEnvironmentVariable('CMAKE_BUILD_PARALLEL_LEVEL','16','User')`
(WSL: `export CMAKE_BUILD_PARALLEL_LEVEL=16` in `~/.bashrc`). Benchmarks + `parallel_for`-saturating test
runs are the same hazard class. Inspect dumps with WinDbg `!analyze -v`.

<a id="docs-building-win-asan-ctest-fails-with-exit-0xc0000135-dll-not-found"></a>
##### win-asan: CTest fails with exit 0xc0000135 (DLL not found)

Add the MSVC tools dir to PATH before ctest (every session):

```powershell
$asanDir = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.50.35717\bin\Hostx64\x64"
$env:PATH = "$asanDir;$env:PATH"
ctest --preset win-asan
```

<a id="docs-building-win-release-ltcg-interprocedural-bugs"></a>
##### win-release: LTCG interprocedural bugs

- MSVC LTCG can mis-propagate a `nullptr` store across inlined TUs (the ResourceManager eviction case):
  keep `CRD_NOINLINE` on `evict_block_locked` / `try_evict_to_budget`.
- **Append new pure-virtuals at the END of an interface** — inserting mid-interface shifts vtable slots
  and surfaces as win-release-only crashes. If you must reorder: full clean rebuild + `-IncludeRelease`.
- MSVC 19.51 LTCG miscompiled in-loop `crd::usize{1} << loop_var` (the `wpt` CI SegFault) — avoid the
  in-loop variable-shift pattern in hot loops (iterate by id / doubling counter).
- Visual Studio IDE: after header changes that grow a class, use **Rebuild** (stale-`.obj` C4789).

<a id="docs-building-ninja-deps-0-header-changes-silently-never-recompile-never-use-the-vs-bundled-cmake"></a>
##### Ninja `#deps 0`: header changes silently never recompile (NEVER use the VS-bundled CMake)

On this Turkish-locale host, the VS-bundled CMake fork stores an English `msvc_deps_prefix` while cl.exe
emits localized include-notes ⇒ **zero header deps recorded** ⇒ stale objects, phantom greens, mixed-layout
SIGSEGVs. It also RE-ARMS itself by rewriting `CMAKE_COMMAND` in `CMakeCache.txt` on any regenerate it runs.
**Policy:** always invoke the standalone CMake explicitly — use the helper scripts, which bake it in:
`scripts/configure-preset.bat <preset>` · `scripts/build-target.bat <build-dir> <targets...>` ·
`scripts/check-deps.bat <build-dir> <obj>` · `scripts/run-ctest.bat <build-dir> <regex>`.
Diagnose: `ninja -t deps <obj>` → `#deps 0` = broken; audit `CMakeCache.txt` `CMAKE_COMMAND` must be
`C:/Program Files/CMake/bin/cmake.exe`. A broken dir must be wiped + reconfigured.

<a id="docs-building-assorted-permanent-fixes"></a>
##### Assorted permanent fixes

- **ResourceManager destructor**: two-pass unload-then-free (payloads holding `ResourceHandle`s) — do not
  collapse to one pass (use-after-free).
- **`crd::containers::String`**: `set_heap_capacity` stores usable chars, not allocation size (the
  `capacity == allocation size` heap-overflow class).
- **jobs::init() in test binaries**: one owner per binary (double-init crashes).
- **`String` in log macros**: pass `.c_str()`/`StringView` (no `std::formatter` for `String`).
- Catch2 flag is `--reporter` (not `--reporters`); a `[` in a TEST_CASE *name* breaks `catch_discover`
  registration (tags only).
- Transient MSVC LTCG `C1001` / clang-tidy AV crashes that clear on a retry-clean are known upstream
  bugs: close on retry-PASS, file as debt, do not re-sweep.
- **PowerShell `native.exe | … | Select-Object -First N` KILLS the exe mid-run** (StopUpstreamCommands) →
  `$LASTEXITCODE` reads a phantom **255/-1** even when every assertion passed. Read the true exit with
  `-Last`, `Out-Null`, or `$out = & exe args 2>&1; $LASTEXITCODE` (collect-then-filter). `ctest` runs each
  test to completion, so a `-First`-induced 255 in a manual harness does NOT mean the gate fails. (SANITY #11)
- **Single-file `clang-tidy` on a PCH build false-cleans:** raw `clang-tidy -p build/win-tidy <file>` can't
  read the MSVC-generated `cmake_pch.cxx.pch` (`not a valid precompiled PCH … doesn't start with AST file
  magic`) → it errors out and reports **0 warnings** (the unparsed-file false-clean). Pass `--extra-arg=/Y-`
  (ignore PCH) `--extra-arg=-Wno-unused-command-line-argument`, and CONFIRM it actually parsed via the
  "N warnings generated." / "Suppressed N warnings" footer — an empty output is a non-analysis, not a pass.

---

<a id="docs-readme"></a>
## Source: docs/README.md

<a id="docs-readme-cerid-documentation"></a>
### Cerid documentation

**Follow [one master table](../ROADMAP.md#master-table).** [Context](../../context.md) is the short current pointer.
The [system audit](../research/2026-09-12-system-audit.md) explains the current architecture, gaps and consolidation.

<a id="docs-readme-reading-order"></a>
#### Reading order

[AGENTS](../../AGENTS.md) → [BUILDING](../BUILDING.md) → [PRINCIPLES](../PRINCIPLES.md) → [SANITY](../SANITY.md) →
[context](../../context.md) → [ROADMAP](../ROADMAP.md). Then open the active row's contract and ADR; do not reload the
entire historical corpus each session. A whole-system audit may inspect more broadly when requested.

The agent memory index is environment-owned, not a root repository file. On the current workstation it is
`C:/Users/abici/.claude/projects/D--Dev-cerid/memory/MEMORY.md`; follow its referenced memories as needed.
Do not create a second repository memory index merely because `MEMORY.md` is absent here.

<a id="docs-readme-where-each-fact-belongs"></a>
#### Where each fact belongs

| Question | Home |
|---|---|
| What happens next; which slices/subslices are done? | [ROADMAP](../ROADMAP.md#master-table), the only live table |
| Where is this session focused? | [context](../../context.md), one pointer |
| What must renderer/UI/editor deliver? | [Execution contract](../design/renderer-ui-execution-contract.md) and [inherited catalogue](../design/rendering-ui-contracts.md) |
| What exists and where is the code? | [Systems/source map](../systems/README.md) |
| Why was architecture chosen? | [ADR index](../decisions/README.md) |
| What did this audit find? | [Audit evidence](../research/2026-09-12-system-audit.md) |
| How is a large slice implemented? | [Design notes](../design/README.md), linked from its master row |
| What did a past session prove? | [Sessions](../sessions), dated records |
| What was measured? | [Benchmarks](../bench/README.md), full captured boards |
| What did we learn? | [Recipes](../recipes/README.md), [lessons](../lessons/README.md), [research](../research/README.md) |
| What older scope must survive? | [Phase contracts](../phases), [debt reference](../debt.md), [archives](README.md); scheduled only in ROADMAP |
| What capability is actually qualified? | [Manifest](../capabilities/gpu-platform-capabilities.toml) and its [generated view](../generated/gpu-platform-capability-matrix.md) |
| How do we build/check? | [BUILDING](../BUILDING.md); broad sweeps are CI work |

<a id="docs-readme-maintenance"></a>
#### Maintenance

One fact has one owner. ROADMAP owns status and order; do not copy live Next lists into phase, detour, research or
system docs. All additional subslices enter that same table before implementation. A completed row links to evidence.
The capability matrix is generated from its manifest and is not a second manually edited task tracker.

Keep context and navigation lean. The single master table may be large because it preserves all retained work;
its cells summarize and link, while details live in contracts. A short summary never silently reduces scope.

Historical sessions, benchmark boards, recipes and ADR evidence retain their original meaning. Mark superseded
claims in place when a reader could follow them incorrectly; do not rewrite old measurements as current results.
Redundant documents may be removed after unique content is retained, source IDs are routed and references repaired
(user direction 2026-09-12). Archive records have no execution authority. Do not mistake an old calendar, status
symbol, source count or Next paragraph for a current contract.

A new decision is indexed once. A new module is indexed with its actual public source. A new measured board is saved
at measurement time. A new implementation detail doc must be linked from the relevant master row.

Run `python scripts/check-master-plan.py` after changing the master table or its current entry-point documents.
It checks ordering, IDs, prerequisites, source routing and local references without invoking an engine build.

---

<a id="docs-decisions-readme"></a>
## Source: docs/decisions/README.md

<a id="docs-decisions-readme-architecture-decision-records--index"></a>
### Architecture Decision Records — Index

Each ADR is one file: `NNNN-short-slug.md`.
Status: Proposed / Accepted / Superseded / Deprecated / Reserved.

> When adding a new ADR, give it the next free number, drop it in this
> folder, and add it to BOTH the tag index and the chronological table
> below. Reference it from the relevant row in the one [master table](../ROADMAP.md#master-table).

<a id="docs-decisions-readme-by-tag"></a>
#### By tag

<a id="docs-decisions-readme-arch"></a>
##### `[arch]`
- ADR-0008 — Graphics architecture
- ADR-0009 — RHI v1a scaffold
- ADR-0012 — Config substrate
- ADR-0013 — Asset pipeline
- ADR-0015 — Job system shape
- ADR-0016 — Render path strategy
- ~~ADR-0018 — Physics architecture~~ — **superseded by ADR-0062**
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0021 — Animation architecture
- ADR-0023 — UI architecture
- ADR-0058 — Öbek system
- ADR-0059 — Preset system
- ADR-0060 — Profile system
- ADR-0061 — Async GPU upload contract
- ADR-0062 — **Eylem: Cerid-native physics architecture** (supersedes ADR-0018)
- ADR-0063 — Eylem determinism contract
- ADR-0064 — **`crd-sdf` substrate architecture**
- ADR-0065 — **`crd-hesap` numerical computing substrate (MATLAB-class)**
- ADR-0066 — **`crd-draw` substrate architecture**
- ADR-0079 — **`crd-perf` profiler substrate + `crd-perf-ui` ImGui frontend** (region timing + jobs auto-instrument + GPU timestamps + memory tracking + CPROF v1 capture format + 7-panel ImGui UI)
- ~~ADR-0080 — **`crd-rhi-compute` substrate**~~ — **rhi halves superseded by ADR-0103/0105 (module retired at RET-8); compute surface today = `IComputeContext` on gpu-context**

<a id="docs-decisions-readme-draw"></a>
##### `[draw]`
- ADR-0066 — `crd-draw` substrate architecture (peer module; retained `RenderBuffer` + immediate-mode API; vertex-shader quad-expanded AA lines + sort-by-centroid translucent solids; 3 depth modes; per-component visualizer plug-in registry; ImGui projection day-one text + reserved SDF text; consumed by eylem / sdf / audio / nav / editor / renderer / sandbox)

<a id="docs-decisions-readme-sdf"></a>
##### `[sdf]`
- ADR-0064 — `crd-sdf` substrate architecture (analytic + dense + narrow-band + CSG; mesh→SDF baker via Jacobson 2013 generalised winding number; CPU first, GPU 3D-texture path; consumed by eylem / font / renderer / audio / editor)

<a id="docs-decisions-readme-hesap-math-solvers-autodiff-opt-ode-fft-dsp"></a>
##### `[hesap]` `[math]` `[solvers]` `[autodiff]` `[opt]` `[ode]` `[fft]` `[dsp]`
- ADR-0065 — `crd-hesap` numerical computing substrate (MATLAB-class; dense + sparse + iterative + direct + eig + opt + ODE + FFT + DSP + stats + tensor + autodiff + GPU + REPL; consumed by eylem / audio / robotics / medical / cinematic / DAW / scientific tool)

<a id="docs-decisions-readme-build-lang"></a>
##### `[build]` `[lang]`
- ADR-0001 — Build & language

<a id="docs-decisions-readme-log"></a>
##### `[log]`
- ADR-0002 — Logging

<a id="docs-decisions-readme-memory"></a>
##### `[memory]`
- ADR-0003 — Memory v1
- ADR-0014 — Reference counting split
- ADR-0022 — Streaming pipeline

<a id="docs-decisions-readme-containers"></a>
##### `[containers]`
- ADR-0004 — Containers v1

<a id="docs-decisions-readme-math"></a>
##### `[math]`
- ADR-0005 — Math v1 (`crd-math` lean primitive layer — Vec/Mat/Quat/Transform + SIMD wrappers + deterministic stdlib)
- ADR-0065 — `crd-hesap` numerical computing substrate (heavy LA + solvers + autodiff + DSP + stats; peer module, NOT inside `crd-math`)

<a id="docs-decisions-readme-platform"></a>
##### `[platform]`
- ADR-0006 — Platform v1
- ADR-0041 — `crd-platform` async filesystem I/O

<a id="docs-decisions-readme-app-event"></a>
##### `[app]` `[event]`
- ADR-0007 — `crd-app` shape

<a id="docs-decisions-readme-rhi-vulkan---era-note-the-crd-rhi-stack-was-retired-at-ret-8-2026-07-23-adr-0105-these-remain-the-record-of-how-it-was-built"></a>
##### `[rhi]` `[vulkan]` — ⚠ era note: the crd-rhi stack was RETIRED at RET-8 (2026-07-23, ADR-0105); these remain the record of how it was built
- ADR-0008 — Graphics architecture
- ADR-0009 — RHI v1a scaffold
- ADR-0010 — Vulkan bootstrap
- ADR-0011 — First triangle
- ADR-0061 — Async GPU upload contract (adds `crd::rhi::Fence` + non-waiting `Queue::submit(cmd, fence)`)
- ADR-0080 — **`crd-rhi-compute` substrate** (Phase 3.1.7.6 prerequisite for v9; additive RHI extension: IComputePipeline + IStorageBuffer + dispatch + compute↔graphics sync + opt-in async compute + shaderc compute pipeline)

<a id="docs-decisions-readme-config"></a>
##### `[config]`
- ADR-0012 — Config substrate

<a id="docs-decisions-readme-resources"></a>
##### `[resources]`
- ADR-0013 — Asset pipeline
- ADR-0014 — Reference counting split
- ADR-0022 — Streaming pipeline
- ADR-0036 — `crd-resources` module placement + loader-registry pattern
- ADR-0037 — ResourceId hybrid UUID scheme
- ADR-0038 — Cooked binary container format
- ADR-0039 — `ResourceHandle<T>` semantics
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope

<a id="docs-decisions-readme-cooker"></a>
##### `[cooker]`
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope
- ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked
- ADR-0058 — Öbek system
- ADR-0059 — Preset system
- ADR-0060 — Profile system

<a id="docs-decisions-readme-jobs"></a>
##### `[jobs]`
- ADR-0015 — Job system shape
- ADR-0033 — crd-jobs implementation architecture (fibers, asm switch, Chase-Lev, SBO, ABA-safe counters)

<a id="docs-decisions-readme-scripting"></a>
##### `[scripting]`
- ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (ScriptComponent slot)

<a id="docs-decisions-readme-networking-determinism"></a>
##### `[networking]` `[determinism]`
- ADR-0035 — Networking architecture principles (layered, determinism-first)
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication slot)

<a id="docs-decisions-readme-renderer-render-path---era-note-crd-renderer-was-retired-adr-0105-rendering-today--the-raf-asset-stack-adr-0106-raf-section-below"></a>
##### `[renderer]` `[render-path]` — ⚠ era note: crd-renderer was RETIRED (ADR-0105); rendering today = the RAF asset stack (ADR-0106, `[raf]` section below)
- ADR-0016 — Render path strategy (the IRenderPath plan; render paths are now the post-RAF RPL proof library — no formal superseding ADR yet, flagged 2026-08-07)
- ADR-0032 — Frame graph v1 (runtime-ownership half superseded by ADR-0106, struck in place; its lifetime/aliasing/barrier contracts live on)
- ADR-0042 — Texture cooked format + GPU upload strategy
- ADR-0043 — MeshResource vertex layout + glTF import scope
- ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy
- ADR-0047 — Font rendering system (MTSDF shader, billboard text, Surface domain)
- ADR-0048 — Material system architecture foundation (two-tier Template/Instance, surface function, MATR format, ShaderOptions)
- ADR-0061 — Async GPU upload contract (`UploadHandle` + per-module polling system)

<a id="docs-decisions-readme-culling"></a>
##### `[culling]`
- ADR-0017 — Culling strategy

<a id="docs-decisions-readme-physics-eylem"></a>
##### `[physics]` `[eylem]`
- ~~ADR-0018 — Physics architecture~~ — **superseded by ADR-0062**
- ADR-0062 — Eylem: Cerid-native physics architecture
- ADR-0063 — Eylem determinism contract
- ADR-0064 — `crd-sdf` substrate (eylem consumes for mesh colliders + closest-point; v3 XPBD uses SDF environment colliders)
- ADR-0065 — `crd-hesap` substrate (eylem v7 FEM refactors to consume sparse PCG + sparse Cholesky once `crd-hesap` ships; eylem v9 differentiable refactors to consume reverse-mode autodiff)

<a id="docs-decisions-readme-scene-ecs"></a>
##### `[scene]` `[ecs]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS
- ADR-0049 — Scene/ECS L1: Entity identity & SlotMap
- ADR-0050 — Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid)
- ADR-0051 — Scene/ECS L3: Relations as first-class
- ADR-0052 — Scene/ECS L4: Query · System · Schedule
- ADR-0053 — Scene/ECS L5: Component index slot framework
- ADR-0054 — Scene/ECS: Transform hierarchy update model
- ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked
- ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection)
- ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration)
- ADR-0058 — Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing
- ADR-0059 — Preset system: typed system-config bags with five-layer resolution
- ADR-0060 — Profile system: typed predicate selectors with additive composition

<a id="docs-decisions-readme-obek-prefab"></a>
##### `[obek]` `[prefab]`
- ADR-0058 — Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing

<a id="docs-decisions-readme-preset-profile"></a>
##### `[preset]` `[profile]`
- ADR-0059 — Preset system: typed system-config bags with five-layer resolution
- ADR-0060 — Profile system: typed predicate selectors with additive composition

<a id="docs-decisions-readme-async-upload"></a>
##### `[async]` `[upload]`
- ADR-0014 — Reference counting split (resource handle async-load substrate)
- ADR-0022 — Open-world streaming pipeline (forward-looking)
- ADR-0039 — `ResourceHandle<T>` semantics (CPU-side async load)
- ADR-0053 — Component index slot framework (`AsyncAwareIndex` consumer-facing surface)
- ADR-0061 — Async GPU upload contract (closes the design half of the GPU-side polling protocol)

<a id="docs-decisions-readme-sandbox-build"></a>
##### `[sandbox]` `[build]`
- ADR-0045 — Sandbox executable, asset layout, cook workflow, crd-meshgen

<a id="docs-decisions-readme-meshgen"></a>
##### `[meshgen]`
- ADR-0045 — Sandbox executable, asset layout, cook workflow, crd-meshgen

<a id="docs-decisions-readme-post-fx-rt"></a>
##### `[post-fx]` `[rt]`
- ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy

<a id="docs-decisions-readme-font-text"></a>
##### `[font]` `[text]`
- ADR-0047 — Font rendering system (MTSDF, FreeType+msdfgen, HarfBuzz, offline+dynamic atlas, extruded text)
- ADR-0064 — `crd-sdf` substrate (font consumes for MTSDF baker + sampler patterns)

<a id="docs-decisions-readme-animation"></a>
##### `[animation]`
- ADR-0021 — Animation architecture

<a id="docs-decisions-readme-ui-node-editor"></a>
##### `[ui]` `[node-editor]`
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0023 — UI architecture
- ADR-0047 — Font rendering system (crd-font, crd-ui dependency)
- ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration)

<a id="docs-decisions-readme-imgui-vulkan"></a>
##### `[imgui]` `[vulkan]`
- ADR-0024 — ImGui single-viewport default

<a id="docs-decisions-readme-shader-cache-reflection---era-note-crd-shader-was-retired-adr-0105-shaders-today--ckir-adr-010101030104-below"></a>
##### `[shader]` `[cache]` `[reflection]` — ⚠ era note: crd-shader was RETIRED (ADR-0105); shaders today = CKIR (ADR-0101/0103/0104 below)
- ADR-0025 — Shader mechanism policy
- ADR-0026 — Shader variant key
- ADR-0027 — Shader reflection consumption model
- ADR-0028 — Shader cache hierarchy
- ADR-0029 — Shader hot reload
- ADR-0030 — Shader / PSO boundary
- ADR-0031 — Shader frontend → IR seam
- ADR-0048 — Material system architecture foundation (ShaderOptions, inline functor, ParameterType)

<a id="docs-decisions-readme-geometry-units-multi-domain"></a>
##### `[geometry]` `[units]` `[multi-domain]`
- ADR-0076 — **`crd-geometry` substrate architecture** (11+ sub-modules; §12–§28 amendment trail = the phase ledger)
- ADR-0077 — Multi-domain expansion (CAD/CFD/FEA/CAM/EDA/ML/sciviz/procgen future phases)
- ADR-0078 — **`crd-units` substrate + the two-layer typed architecture** (`Quantity<D, T>`; §5 = the engine-wide layer split)

<a id="docs-decisions-readme-agent-native-cli-scripting"></a>
##### `[agent-native]` `[cli]` `[scripting]`
- ADR-0081 — **Agent-native engine** (CLI/JSON-RPC/MCP as source of truth; C++ hot-reload as the ONLY scripting language; subsumes ADR-0034) — §9's C++-ONLY clause **superseded by ADR-0108 (Accepted 2026-08-07)**; §1-§8 reaffirmed. The in-file §9 strike + PRINCIPLES/AGENTS/README/ROADMAP flip **EXECUTED 2026-08-10** at the first CEIR vertical slice (CEIR-13z, ADR-0108 §7)
- ADR-0108 — **A Cerid-owned executable-program language stack (CEIR/CHIR); C++ is no longer the *only* authorable program** (**Accepted 2026-08-07**; surgically supersedes ADR-0081 §9; cornerstone flip **EXECUTED 2026-08-10** at the first CEIR vertical slice, CEIR-13z)

<a id="docs-decisions-readme-hesap--per-cluster-adrs"></a>
##### `[hesap]` — per-cluster ADRs
- ADR-0082…0097 — one ADR per hesap cluster (v0 dense microkernels → … → 0089 sparse-eig · 0090 opt · 0091 ODE/DAE · 0092 FFT · 0093 DSP/wavelet/comms · 0094/0095 special/interp/quad/diff/motion · 0096 tensors · 0097 autodiff). Exact titles: the chronological table below.

<a id="docs-decisions-readme-ceir-chir--the-execution-ir-stack-d-007-ceir-band"></a>
##### `[ceir]` `[chir]` — the execution-IR stack (D-007 CEIR band)
- ADR-0108 — **A Cerid-owned executable-program language stack** (Accepted; supersedes ADR-0081 §9) — see the `[agent-native]` section
- ADR-0109 — **CEIR/CHIR/CKIR ownership + `crd-ceir` module placement** (Accepted 2026-08-07 — binding for CEIR-1): the one-way layer contract, the dependency-inversion provider seam (`crd-ceir` host-only + `crd-ceir-host`/`crd-ceir-gpu` bridges), the finalized CEIR-1 type names, the semantic-identity model; extends ADR-0101/0103's I1/I2 as I3/I4/I5
- ADR-0110 — **Native-intrinsic schema + the legitimacy rule + the 3 plugin levels** (Accepted 2026-08-07): an intrinsic is an ordinary CEIR-2 op with §100 native-binding metadata + a bridge handler; the IFF legitimacy test (capability = intrinsic, algorithm = program, performance = provider); Level A subgraph / B custom-op+lowering / C native

<a id="docs-decisions-readme-gpu-context-kir-ir--the-gpu-era-north-stars"></a>
##### `[gpu-context]` `[kir]` `[ir]` — the GPU-era north stars
- ADR-0098 — crd-kir + crd-hesap-gpu: the GPU compute COMPILER (CKIR two-level IR, six backends, determinism tiers)
- ADR-0099 — crd-gpu-context: one shared GPU device; compute/rendering are separate concerns on it (§6 struck → ADR-0103)
- ADR-0100 — CKIR is the one GPU compute manager (kernel-source-agnostic dispatch)
- ADR-0101 — **The IR is the single source of truth for EVERY shader; backend languages are OUTPUTS only**
- ADR-0102 — Render-data, lighting & pass architecture (frequency-based sets; material = surface response)
- ADR-0103 — **`crd-gpu-context` owns every GPU program; I1/I2 leak invariants** (supersedes ADR-0099 §6)
- ADR-0105 — **Retire crd-rhi + crd-renderer: gpu-context IS the graphics layer** (supersedes the rhi halves of 0036/0042/0080/0085, struck in place as slices landed)

<a id="docs-decisions-readme-raf-frame-graph-rendering-assets"></a>
##### `[raf]` `[frame-graph]` `[rendering-assets]`
- ADR-0104 — IR-as-crdr: the shader cook + deploy pipeline (content-hash cache, variants, pipeline cache, hot reload)
- ADR-0106 — **Unified frame-graph runtime: `crd-render-graph` is the single live runtime** (closed at RAF-12.3)
- ADR-0107 — Interactive UI + 2D rendering architecture (UiWorld/Canvas/I2D-SPR; **Proposed**, pending review)

<a id="docs-decisions-readme-all-adrs-chronological"></a>
#### All ADRs (chronological)

| ID    | Title                                          | Tags                              | Status   |
| ----- | ---------------------------------------------- | --------------------------------- | -------- |
| 0001  | Build & language                               | build, lang                       | Accepted |
| 0002  | Logging                                        | log                               | Accepted |
| 0003  | Memory v1                                      | memory                            | Accepted |
| 0004  | Containers v1                                  | containers                        | Accepted |
| 0005  | Math v1                                        | math                              | Accepted |
| 0006  | Platform v1                                    | platform                          | Accepted |
| 0007  | `crd-app` shape                                | app, event                        | Accepted |
| 0008  | Graphics architecture                          | rhi, vulkan, arch                 | Accepted |
| 0009  | RHI v1a scaffold                               | rhi, arch                         | Accepted |
| 0010  | Vulkan bootstrap                               | vulkan, rhi                       | Accepted |
| 0011  | First triangle milestone                       | vulkan, rhi, renderer             | Accepted |
| 0012  | Configuration substrate                        | config, arch                      | Accepted |
| 0013  | Asset pipeline                                 | resources, arch                   | Accepted |
| 0014  | Reference counting split                       | memory, resources                 | Accepted |
| 0015  | Job system shape                               | jobs, arch                        | Accepted |
| 0016  | Render path strategy                           | renderer, render-path, arch       | Accepted |
| 0017  | Culling strategy                               | culling, renderer                 | Accepted |
| 0018  | Physics architecture                           | physics, arch                     | **Superseded by 0062** |
| 0019  | (reserved)                                     | —                                 | Reserved |
| 0020  | Scene & ECS hybrid + UI in scene tree          | scene, ecs, ui, arch              | Accepted |
| 0021  | Animation architecture                         | animation, arch                   | Accepted |
| 0022  | Open-world streaming pipeline                  | memory, resources                 | Accepted |
| 0023  | UI architecture                                | ui, node-editor, arch             | Accepted |
| 0024  | ImGui single-viewport default                  | imgui, ui, vulkan                 | Accepted |
| 0025  | Shader mechanism policy                        | shader, renderer, arch            | Accepted    |
| 0026  | Shader variant key                             | shader, cache, arch               | Accepted    |
| 0027  | Shader reflection consumption model            | shader, reflection, rhi           | Accepted    |
| 0028  | Shader cache hierarchy                         | shader, cache, vulkan             | Accepted    |
| 0029  | Shader hot reload                              | shader, hot-reload, runtime       | Accepted    |
| 0030  | Shader / PSO boundary                          | shader, rhi, renderer             | Accepted    |
| 0031  | Shader frontend → IR seam                      | shader, arch, ir                  | Accepted    |
| 0032  | Frame graph v1                                 | renderer, render-path, arch       | Accepted    |
| 0033  | crd-jobs implementation architecture           | jobs, arch, fibers, threading     | Accepted    |
| 0034  | C++ hot-reload DLL scripting                   | scripting, arch, extensibility    | Accepted    |
| 0035  | Networking architecture principles             | networking, arch, determinism     | Accepted    |
| 0036  | `crd-resources` module + loader registry       | resources, arch                   | Accepted    |
| 0037  | ResourceId hybrid UUID scheme                  | resources, arch                   | Accepted    |
| 0038  | Cooked binary container format                 | resources, arch, cooker           | Accepted    |
| 0039  | `ResourceHandle<T>` semantics                  | resources, arch                   | Accepted    |
| 0040  | Cooker CLI + CMake integration                 | resources, cooker, build, arch    | Accepted    |
| 0041  | `crd-platform` async filesystem I/O            | platform, resources, jobs         | Accepted    |
| 0042  | Texture cooked format + GPU upload strategy    | resources, renderer, cooker       | Accepted    |
| 0043  | MeshResource vertex layout + glTF import scope | resources, renderer, cooker       | Accepted    |
| 0044  | Phase ordering: material PSO/variant before scene/ECS | arch, renderer, scene, resources | Accepted |
| 0045  | Sandbox executable, asset layout, cook workflow, crd-meshgen | arch, sandbox, resources, cooker, build | Accepted |
| 0046  | MaterialDomain enum, node-editor future-proofing, RT hybrid strategy | arch, renderer, shader, materials, rt | Accepted |
| 0047  | Font rendering system (MTSDF, FreeType+msdfgen, HarfBuzz, offline+dynamic atlas, extruded text) | arch, font, renderer, ui, text | Accepted |
| 0048  | Material system architecture foundation (two-tier Template/Instance, surface fn, MATR chunks, ShaderOptions, ParameterType) | arch, renderer, shader, materials, resources, cooker | Accepted |
| 0049  | Scene/ECS L1: Entity identity & SlotMap                | scene, ecs, arch, layer-1               | Accepted |
| 0050  | Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid) | scene, ecs, arch, layer-2, performance | Accepted |
| 0051  | Scene/ECS L3: Relations as first-class                 | scene, ecs, arch, layer-3, relations    | Accepted |
| 0052  | Scene/ECS L4: Query · System · Schedule                | scene, ecs, arch, layer-4, query, scheduler | Accepted |
| 0053  | Scene/ECS L5: Component index slot framework           | scene, ecs, arch, layer-5, indexes, extensibility | Accepted |
| 0054  | Scene/ECS: Transform hierarchy update model            | scene, ecs, math, performance           | Accepted |
| 0055  | Scene serialization: TOML authoring + SCEN CRDR cooked | scene, ecs, resources, cooker, arch     | Accepted |
| 0056  | Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection) | scene, ecs, arch, layer-6, layer-7, layer-8, networking, scripting, editor | Accepted |
| 0057  | Scene/ECS: UI nodes in scene tree (boundary declaration) | scene, ecs, ui, arch                  | Accepted |
| 0058  | Öbek system: cooked entity-graph templates with composition, variation, AAAA-tier future-proofing | scene, ecs, cooker, resources, arch, renderer, networking, determinism, obek, prefab | Accepted |
| 0059  | Preset system: typed system-config bags with five-layer resolution | scene, resources, cooker, arch, renderer, audio, physics, input, config, preset | Accepted |
| 0060  | Profile system: typed predicate selectors with additive composition | scene, resources, cooker, arch, config, networking, app, profile | Accepted |
| 0061  | Async GPU upload contract: `UploadHandle` + per-module polling system | arch, renderer, rhi, scene, resources, async | Accepted |
| 0062  | **Eylem: Cerid-native physics architecture** (supersedes 0018) | arch, physics, eylem, ecs, jobs, simd, determinism | Accepted |
| 0063  | Eylem determinism contract                     | arch, physics, eylem, determinism, ci, fp | Accepted |
| 0064  | `crd-sdf` substrate architecture               | arch, sdf, eylem, renderer, font, audio, editor, resources | Accepted |
| 0065  | `crd-hesap` numerical computing substrate (MATLAB-class); §13-§16 v0/v1/v2 locks; §17-§25 v3 dense SVD+eig+lstsq close; **§26 amendment 2026-05-27 — v4 iterative-solvers + preconditioners + AMG cluster CLOSED** (D(iter)-1..10 determinism moat + reorder default-ON + graceful-degrade + O(1) AMD bucket-head + the β=0.3 quantified result) | arch, hesap, math, solvers, autodiff, opt, ode, fft, dsp, scripting | Accepted |
| 0066  | `crd-draw` substrate architecture        | arch, draw, eylem, sdf, audio, renderer, editor, resources | Accepted |
| 0067  | Eylem force-field architecture (three-tier substrate)        | arch, physics, eylem, fields, sdf, draw, ecs, obek, determinism | Accepted |
| 0068  | Eylem body types + collision filtering + callbacks (3 motion types + sensor + 5-tier filter + deferred ECS events + ContactModify) | arch, physics, eylem, collision, filtering, callbacks, ecs, determinism | Accepted |
| 0069  | Eylem materials substrate (friction + restitution + surface velocity + density)  | arch, physics, eylem, materials, friction, restitution                       | Accepted  |
| 0070  | Eylem solver catalog + selection guidance (incl. Nonsmooth Newton)              | arch, physics, eylem, solvers                                                 | Planned   |
| 0071  | Robotics importers (URDF / SDF / MJCF) + actuator catalogue                       | arch, physics, eylem, robotics, importers, actuators, urdf, sdf, mjcf         | Planned   |
| 0072  | Eylem sensor substrate (IMU / LIDAR / proximity / threshold events / diagnostics) | arch, physics, eylem, sensors, robotics                                       | Planned   |
| 0073  | Eylem aerospace substrate (variable mass + aero + atm + propulsion + J2 + sep)    | arch, physics, eylem, aerospace, aero, atmosphere, propulsion, fields         | Planned   |
| 0074  | Eylem cinematic / animation-physics bridge (`crd-eylem-cine` module)              | arch, physics, eylem, cinematic, animation, film                              | Planned   |
| 0075  | Eylem testing rigor + conservation-law CI                                          | arch, physics, eylem, testing, ci, conservation, scientific-computing        | Accepted  |
| 0076  | `crd-geometry` substrate (BVH + GJK/EPA + mesh queries + polygon ops + Delaunay + decomposition + GPU-LBVH + shader-helpers); §19-§21 amendments closed `-mesh` / `-spatial` / `-polygon` clusters 2026-05-16; §22 amendment 2026-05-17 — `-mesh-processing` v7 cluster CLOSED (8 algorithm slices); §23 amendment 2026-05-17 — `-delaunay` v8 cluster CLOSED (11 algorithm slices incl. cospherical Stage D insphere_exact paydown); §24 amendment 2026-05-18 — `-decomposition` v9c cluster CLOSED (V-HACD voxelize + decompose); §25 amendment 2026-05-18 — **`-gpu` v9a LBVH cluster CLOSED** (10 algorithm slices: 30-bit Morton CPU+GPU + 60-bit Morton CPU+GPU + typed wrappers + async-compute pool + CPU radix + GPU radix + scalar+prefetch + parallel-via-jobs + LBVH tree+upsweep elite-combine; locks D132-D164 / 33 decisions); §26 amendment 2026-05-19 — **`-shader-helpers` v9e cluster CLOSED** (4 algorithm slices + close: formula-IR flat 3-array storage + GLSL backend + ULP-conformance GPU dispatch + HLSL backend + dxc → SPIR-V GPU verification + cooker library-API; locks D166-D181 / 16 decisions; substrate-side `crd::shader::compile_hlsl` shipped); §27 amendment 2026-05-19 — **`-curves` v10 cluster CLOSED** (6 slices: substrate + sampling + arc-length + queries + frames-viz-sandbox + typed-boundary; locks D182-D216 / 35 decisions; Wang 2008 RMF + uniform closure-twist; typed `queries_typed.hpp` covers WHOLE v10 surface per ADR-0078 §5 D34); §28 amendment 2026-05-19 — **v11 transform-aware + PHASE 3.1.7 FULLY CLOSED** (TransformedShape composition wrapper with trait-based scalar deduction + 14 3D + 7 2D shape transforms + `transform_*_typed` boundary covering FULL primitive catalog; D217-D233 / 17 decisions; 5 advisor-pinned discriminators); **🎉 Phase 3.1.7 substrate FULLY CLOSED 2026-05-19 — 12 of 11 sub-modules complete** | arch, substrate, computational-geometry, bvh, gjk-epa, mesh-processing, spatial-acceleration, polygon-ops, cdt, decomposition, gpu-lbvh, shader-helpers, sdf-cooker, determinism | Accepted  |
| 0077  | Multi-domain substrate expansion (9 new peer modules + Phase 3.5 prologue + Phase 6 platform expansion)  | arch, strategy, multi-domain, manufacturing, cad, cfd, aerospace, ml, scientific-computing | Accepted  |
| 0078  | `crd-units` substrate (dimensional types + 6-layer conversion system); §2 v0b adoption A; §3 v0c adoption B; §4 v0d adoption C + Phase 3.1.7.5 CLOSE; §5 amendment 2026-05-16 — **two-layer typed architecture** (D32-D36: units at API surface, raw scalars in inner loop; boundary is the API surface and only there; bridges = `.value` / `to_raw_vec` / `from_raw_vec` / strip-compute-retag wrappers) | arch, substrate, units, dimensional-analysis, type-safety, determinism, physics, eylem, geometry, format-parse, ui, architecture-principle | Accepted  |
| 0079  | `crd-perf` profiler substrate + `crd-perf-ui` ImGui frontend (D-003 v0a-v0h)       | arch, substrate, perf, profiler, instrumentation, gpu-timing, capture-format, ui            | Accepted  |
| 0080  | `crd-rhi-compute` substrate (Phase 3.1.7.6 v0a-v0e+close); additive RHI extension for compute pipelines, storage buffers, dispatch, sync, opt-in async compute | arch, substrate, rhi, gpu, compute, async-compute, descriptors, shader-pipeline, prerequisite | Proposed  |
| 0081  | Agent-native engine: CLI + JSON-RPC + Anthropic MCP substrate (`crd-cli` + `crd-rpc` + `crd-script`); CLI is the source of truth (GUI is a visualization layer that emits commands); supersedes ADR-0034 (folded in as the C++ hot-reload sub-aspect); MCP compatibility = instant Claude Code / OpenAI Function Calling / Gemini Function Calling integration; capability-based security + transactional sessions + sandbox isolation + deterministic replay; Phase 4.0 substrate work + per-DoD CLI surface requirement going forward; first concrete consumer = `crd-hesap` v0 (Phase 3.1.6 immediate next slice ships with CLI surface from day 1) | arch, cli, rpc, mcp, agent, scripting, substrate, vision, supersedes-0034 | Proposed |
| 0082  | Hesap GEMM microkernel: intrinsics-via-Vec8f/Vec16f, ASM deferred. Locked hot-swap signature `gemm_microkernel<T>(k, a_packed, b_packed, c_tile, ldc)` + `CRD_HESAP_MICROKERNEL_BACKEND` compile-time switch (Intrinsics default; Asm reserved). Target 80-85% peak via crd-math::simd; final 5-10% gap to MKL deferred. Three-condition revisit gate: GEMM >50% of solve time AND intrinsics <70% peak AND no better alternative (GPU/sparse). Same call Eigen/Faer/Highway/xtensor/Stan-math/Armadillo/mlpack made. | arch, hesap, blas3, microkernel, perf, simd, intrinsics, hot-swap | Accepted |
| 0083  | hesap-dense row-major storage (with per-factor escape hatch). Keep row-major public default (ML/array-ecosystem alignment: NumPy/PyTorch/JAX are row-major; GEMM layout-neutral via packing; GEMV naturally row-major; sparse independent). Accepted bounded cost: small-N (≤256) dense factorizations trail column-major Eigen/LAPACK ~1.4× (column-oriented elimination fits column-major; proven layout-fit gap not kernel quality via 3 experiments). Escape hatch: opaque factor objects may store internal buffer column-major if a hot-loop consumer proves it; batched/fixed-size kernels preferred for tiny-solve hot loops. Revisit only on measured system-level bottleneck. | arch, hesap, dense, storage, layout, perf, rowmajor | Accepted |
| 0084  | Sparse matrices as first-class cooked engine resources (`crd-hesap-resources` bridge module, depends `crd-resources`+`crd-hesap-sparse`; one-way). `'HMTX'` CRDR (MXHD/MXOP/MXII/MXVL chunks) + 40-byte pinned `MatrixFileInfo` (u64 nnz + topology_hash + frame_stamp + format byte; loader asserts-on-hash-mismatch) + append-only `variant` enum (0=f32/1=f64/2=c32/3=c64). Single loader, variant-in-header; type-erased `SparseMatrixResource` payload + `build_csr<T>()`. In-memory cook-time cooker (reuses v1g `read_matrix_market`); no filesystem dep in the module. Corpus delivery reuses `manifest_write`+`mount_manifest`+`load_sync` (no new ResourceManager API). CLI `hesap.matrix.{info,cook,load}` stateless on inline `.mtx`; `fetch` dev-time (no HTTP client). Cooked binary loads 6–7× faster than re-parsing `.mtx` on real SuiteSparse. Solver vs-reference benches through this path land at v4a (ship-at-consumer). | arch, hesap, sparse, resources, cooker, crdr, corpus, agent-native | Accepted |
| 0085  | Virtual-memory + streaming allocator cluster (Phase 2.2, before hesap v5). Six allocators + a `crd::platform::vm` reserve/commit layer, composing with the shipped streaming pipeline (jobs + async I/O + ResourceManager) rather than replacing it: (1) platform VM API; (2) `VirtualMemoryAllocator` — reserve-big / commit-on-demand / decommit, **stable addresses, no relocating CPU heap**; (3) re-parent `GrowableTlsfAllocator` onto VM → **removes malloc-at-the-root**; (4) thread-safe **fence/epoch-gated RingAllocator** (staging); (5) `StreamingAllocator` policy layer + ResourceManager budgets/eviction; (6) `GpuAllocator` VkDeviceMemory suballocation; (7) GPU defrag (handle relocation) + residency, **pluggable `IResidencyPolicy`/`IDefragPolicy`**. GPU built now per explicit eyes-open user override (design-without-consumer risk confined to policy objects). D(mem-stream)-1..7. Extends ADR-0003 Phase B + ADR-0022. | memory, platform, rhi, streaming, resources, virtual-memory, gpu-allocator | Accepted |
| 0086  | Eylem unified motion model (design from a 2026-05-30 session; ratify/reconcile per-slice when eylem resumes after hesap). One solver / one state — animation proposes TARGETS, physics is the sole arbiter of the rendered pose (kills the Unity-style animation-vs-physics fragmentation). Constraints as the universal primitive (collision/joint/IK/animation-motor all one solve). Powered/active ragdoll (animation = per-joint motor targets + a CONTINUOUS gain; no discrete kinematic↔ragdoll switch — smooth hit reactions + death). Reduced-coords Featherstone end-state for characters/robots (FK = forward pass, IK = constraints on the same Jacobians). Pluggable solver over the constraint graph (SI/TGS → XPBD → reduced-coords+hesap → the v9 unify). LOD = a fidelity continuum scaling the whole pipeline per agent (NOT decoupled animation/physics clocks). Authoritative-coarse / cosmetic-fine split = the key LOD + networking enabler (only the cheap authoritative layer is deterministic+networked; cosmetic detail is local). Crowd = detail-follows-attention (thousands cheap/GPU + ~dozens focused full reactions). Networking = deterministic LOCKSTEP for the shared authoritative sim (inputs-not-state ⇒ scales to thousands) + rollback for local players; needs cross-PLATFORM FP determinism (deterministic transcendentals). REFINES ADR-0021 + ADR-0074 (powered ragdoll vs cinematic-kinematic — reconcile), EXTENDS ADR-0035 + ADR-0062 §5 + ADR-0063; builds on ADR-0065 (hesap = the per-step constrained-solve backbone). | arch, physics, eylem, animation, networking, determinism, lod | Proposed |
| 0087  | Large-scale deterministic simulation (worked example: a 1700s naval-battle MMO — sea + colliding ships + thousands of soldiers, ~half real players; rules: incredible physics, deterministic networking, >60fps). Six transferable principles: (1) replace state with DETERMINISTIC FIELDS (ocean/wind/atmosphere = procedural functions, never simulated or networked; physics-field cheap+deterministic vs render-field rich+cosmetic); (2) couple via SURFACE INTEGRAL over a low-poly proxy (Kerner per-face hydrostatic+hydrodynamic → float/righting/wave-ride/drag), NOT point-probe floaters; (3) authoritative-coarse/cosmetic-fine everywhere; (4) detail-follows-attention; (5) SCALE THE NETWORK MODEL to human-player count; (6) solve agents on carriers in the carrier's LOCAL non-inertial frame + balance controller (inverted-pendulum CoM, ankle/hip/step) + powered ragdoll. KEY networking decision (REFINES ADR-0086 D10): few humans + AI crowd → deterministic LOCKSTEP; MANY humans (MMO) → server-authoritative spatially-partitioned DETERMINISTIC regions (server meshing) + interest management + client prediction/rollback (tight via identical deterministic code) + authoritative/cosmetic split; determinism stays the bedrock (replay + anti-cheat + tight prediction), delivery is NOT lockstep. Generalizes ADR-0073 aero (atmosphere = field, aero = surface integral); extends ADR-0035; builds on ADR-0065 (FFT ocean). Honest frontier: server meshing is bleeding-edge (~100-200 humans/region realistic), cross-platform FP determinism is the deep dependency, balance robustness is research-grade. | arch, physics, eylem, networking, determinism, lod, environment, mmo | Proposed |
| 0088  | **GEMM hand-tuned asm — INVESTIGATED → REVERTED (intrinsics vindicated).** Re-opened ADR-0082's asm question, built the whole thing (dual-syntax MASM/GAS f64 6×8 kernel + runtime CPUID dispatch + build integration, all bit-identical to the intrinsic, green on MSVC/clang-cl/gcc), and MEASURED it. **Decisive same-process A/B (asm vs intrinsic, identical clock): asm is ~1–2% SLOWER.** Structural, not tunable — the intrinsic INLINES into the panel loop (no call overhead, ~98% peak) while the asm is a hard function call. The bmwcra premise also failed separately (factor is memory/scaling-bound at 8T, not kernel-bound — the kernel win washes out). Earlier "+10–16%" was a turbo-clock artifact. **All asm code + the v0d-asm-0 direct-to-C framework change REVERTED; tree back to v5a-5. ADR-0082 (intrinsics-first) STANDS, vindicated.** Hot-swap point reserved for a genuine future asm-only-ISA need (AMX etc.), not a generic perf lever. bmwcra's crush = the SCALING lever (within-front parallelism), not the kernel — **EXECUTED in v5a-7 (2026-05-31): bmwcra flipped 0.65→1.04–1.05× WIN, every matrix now beats CHOLMOD.** Record: `docs/phases/hesap-v0d-asm-microkernel-plan.md`. | arch, hesap, math, simd, asm, perf, reverted | **Reverted** |
| 0089  | **crd-hesap-eigen — sparse eigensolvers + module edges (Phase 3.1.6 v6).** Matrix-free over `LinearOp<T>`; Rayleigh-Ritz reuses `crd-hesap-dense`. The crush axis is ALGORITHMIC not kernel: plain Krylov (Lanczos/Arnoldi/IRLBA) = parity + the determinism moat (the honest ceiling, shares ARPACK/PRIMME's BLAS/LAPACK ceilings); the crush is fewer-matvecs via preconditioned methods (shift-invert/LOBPCG/JD/FEAST). **v6-z bench proven (3D Poisson, matched acc): AMG-LOBPCG CRUSHES direct shift-invert ARPACK (9.5× wall + 40× mem, growing) + PARITY vs state-of-art PRIMME at f64 — crush-vs-direct + parity-vs-same-class + the moat is the win, NOT a speed-win over PRIMME+AMG.** Determinism = the differentiator: deterministic counter-RNG start + fixed-order reorthog + pinned sign/coupled-SVD-sign + deterministic dense RR ⇒ eigenpairs bit-identical {1..16} workers (`[moat]` per method); none of ARPACK/PRIMME/SLEPc/PROPACK carries it. Restart SUBSTITUTION (determinism): thick-restart Lanczos≡IRLM, Krylov-Schur≡IRAM, augmented thick-restart≡IRLBA (deterministic, vs the ordering-sensitive implicit bulge-chase). Real-symmetric focus; complex where inherent (Arnoldi/KS emit complex). New acyclic edges hesap-eigen→{hesap-direct (shift-invert/FEAST: v5 LU factor), hesap-iterative (JD: FGMRES correction eqn)} (mirror v5f-c2). Extends ADR-0065 §v6. | arch, hesap, eigen, eigensolver, svd, determinism, module-edges | Accepted |
| 0090  | **crd-hesap-opt — the optimization domain (substrate + contracts + edges, Phase 3.1.6 v7).** Matrix-free over `Objective<T>` (value/n pure + gradient/hessian_vector virtual; `has_gradient()` capability flag agrees with `gradient()->bool`; vtable append-at-end + a RESERVED fused `value_and_gradient` slot for the L-BFGS/LM hot path). v7 ABSORBS the old v8 constrained cluster (user direction — opt is ONE domain); the universal "find the best X" substrate (eylem/control/FEA-CFD/CAD/ML/rendering/games/robotics/DAW). **Determinism moat (the differentiator none carry):** serial optimizer + bit-exact objective eval ⇒ bit-identical TRAJECTORY+result across {1..16} ⇒ certifiable MPC/control (DO-178C/ISO26262) + reproducible training + replay; `[moat]` per slice asserts convergence too (vacuous-maxed-out guard). Contracts pinned up front: line-search g_out cost contract (Wolfe fills it, Armijo recomputes); workspace = caller's `IAllocator*`; stochastic moat SPLITS (minibatch reproducibility needs v12 counter-RNG). Gold standards (honest, crush-where-the-algorithm-allows + parity+moat else; wall-clock-vs-Python-overhead caveat): Ceres/liblbfgs/scipy/NLopt/CMA-ES/PyTorch/OSQP/IPOPT/SCS/HiGHS (probe Ceres v7-e + IPOPT/cyipopt v7-n early). Edges: hesap-opt→hesap-dense/sparse/jobs now; →hesap-direct/iterative (Newton/KKT) + hesap-eig (exact-TR) later; reverse-mode autodiff plugs the same gradient interface. Extends ADR-0065 D14. | arch, hesap, opt, optimization, determinism, module-edges, substrate | Accepted |
| 0091  | **crd-hesap-ode — the ODE/DAE module: two API layers + deterministic controllers + the work-precision contract (Phase 3.1.6 v9).** TWO API LAYERS (ADR-0078 two-layer applied to integration): raw-span allocation-free stepper KERNELS (caller scratch, inlined RHS, in-place safe, fixed per-element FP order — what eylem's fused SoA sweep / animation / DAW inline; never a virtual f per body per substep) + the general DRIVER over `OdeFunction<T>` (the v7 Objective capability contract; vtable LOCKED append-at-end — mass matrix appends at v9-h, sparse Jacobian at v9-j; events = integration options, NOT virtuals). Deterministic `OdeWork` counters (CVODE semantics) = the work-precision scoreboard currency (error-vs-nfev-vs-wall, the domain's native honest format). Step controllers = pure deterministic FP fns, TWO types matching their references exactly (scipy elementary accepts err<1 strictly; Hairer PI accepts err≤1 + facold floor — one parametrization would be dishonest). Dense-output contract pinned day 1 (fixed-width coeff blocks, caller-owned storage; cubic-Hermite fallback shipped); recomputed t (no accumulation). Edges v9-a: core/containers/memory ONLY; hesap-dense at v9-d/e, hesap-direct/iterative at v9-j; NO edge to hesap-opt (acyclic — opt consumes ode for shooting later). Named OUT: LSODA auto-switch, f32 stiff. Gold standards: SUNDIALS 6.4.1 + scipy (trajectory-exact) + Hairer Fortran + odeint. Extends ADR-0065; consumes ADR-0063 (symplectic = eylem replay, v9-g). | arch, hesap, ode, dae, determinism, module-edges, substrate | Accepted |
| 0092  | **crd-hesap-fft — the FFT cluster: deterministic plan-from-factorization + portable-C++ MKL-adjacent + the full transform suite (Phase 3.1.6 v10).** Plan chosen from the size factorization with NO runtime measurement (vs FFTW MEASURE/PATIENT, whose wisdom varies run-to-run + isn't reproducible across builds); one read-only twiddle table/plan ⇒ fixed per-element FP order ⇒ output bit-identical across runs+threads (each thread its own plan; {1..16} nearly free — an FFT has no thread-dependent reduction ⇒ determinism is OFF the usual moat axis, so lead with it + zero-planning-overhead + typed zero-dep integration, NOT a beat-MKL claim). SUITE: complex FFT (`FftPlan`, Stockham radix-2/4/8 + straight-line SIMD codelets N≤32 + Bailey four-step >2¹⁹) · real (`RealFftPlan`) · DCT/DST (`DctPlan`, Makhoul — beats scipy/PocketFFT) · NUFFT (`NufftPlan` — wins FINUFFT small/mid @3× acc) · **Bluestein ANY-size** (`BluesteinPlan`, chirp-z over one pow-2 plan) · **N-D** (`NdFftPlan`, row-column; pow-2 axis→FftPlan else Bluestein; forward-only-per-axis + N-D forward-trick inverse so unnormalized-vs-normalized inverses never clash) · **Sparse FFT** (`SparseFftPlan`, HIKP 2012, END-TO-END SUB-LINEAR + NOISE-ROBUST: multi-scale binary location + voting + median, no O(n) step; coeff-under-noise bounded √(n/B)σ/√R info-theoretically). Lower-layer raw `Complex<T>` (ADR-0078 §5); unnormalized both ways (`ifft_normalized`=1/n). Correctness oracle = brute-force O(N²) DFT (NEVER round-trip — the odeint-d4 trap). **MKL global parity DEFERRED — production-grade portable, not MKL clone:** MKL-adjacent on AVX2 (256K fixed 0.25→0.85×=3.6× default-on, 4M/8M ~0.92-0.95× host-noise-caveated), beats PocketFFT everywhere; residual ~15% = the four-step inter-stage twiddle, measured-exhausted (8 attempts) as the asm-integrated-butterfly gap, OUT OF SCOPE per ADR-0082 (portable-C++/WASM-no-asm). CLI `hesap.fft.*` (forward + sparse). Edges: core/containers/memory + math (simd) + hesap (Complex, CLI). Extends ADR-0065; consumes ADR-0078 + ADR-0082. | arch, hesap, fft, determinism, module-edges, substrate | Accepted |
| 0095  | **crd-hesap-{interp,quadrature,diff,motion} — the v13 Numerical-Analysis + Motion cluster: the 4-module split + the 3 certification moat pillars + the error-tier contract (Phase 3.1.6 v13).** Re-scoped from a ~1.5-wk sketch to a MAJOR certification-grade cluster (for satellites/drones/robots/self-driving/games): **`crd-hesap-interp`** (NEW — 1-D + scattered/gridded N-D interpolation) · **extend `crd-hesap-quadrature`** (the integrate() API + QUADPACK adaptive + cubature, on the v12-c Gauss nodes) · **`crd-hesap-diff`** (NEW — Fornberg/Richardson/**complex-step**/spectral) · **`crd-hesap-motion`** (NEW — SQUAD/clothoid/min-snap/NURBS/Ruckig-OTG; planning = Phase 3.1.11 consumes it). ⭐⭐ **THE 3 MOAT PILLARS** every entry point obeys = (1) determinism-by-construction (crd::math not std::, fixed FP order, {1,4,16} bit-identical → DO-178C/ISO-26262-ASIL-D replay) · (2) allocation-free streaming (caller workspace, adaptive = ITERATIVE bounded-depth NOT recursive, hard limit = WCET knob, status-not-spin → MISRA 21.6/8.2.10) · (3) error-tier-exposing (`{value, error_estimate, status, eval_count}`, TIER-LABELLED: Tier-1 estimate=foolable-not-a-bound / Tier-2 certified `worst_case_error(h, deriv_bound)` / Tier-3 interval enclosure; status-not-exception, builds -fno-exceptions). ⚠ an estimate is NEVER promoted to a bound (the honest-scoreboard scar applied to error reporting). The certification-readiness axis GSL (mallocs) / Boost (throws) / parallel-BLAS (non-reproducible) structurally lose. SANITY-8 reuse (Gauss nodes/eig_sym/hermite_eval/dense/fft/fresnel/quat/Sibson-NNI/opt-QP). Two-layer ADR-0078. Extends ADR-0065; consumes ADR-0078; sibling to ADR-0094. | arch, hesap, interpolation, quadrature, differentiation, motion, determinism, safety-critical, module-edges, substrate | Accepted |
| 0096  | **crd-hesap-tensor — the N-D tensor substrate: templated compute dtypes over stride views + the two-tier deterministic-reduction contract + deterministic stochastic rounding + the certified-inference lane (Phase 3.1.6 v14).** ONE module, internal sub-headers (substrate/elementwise/einsum/sparse/decomp/io/nn — dense-only consumers dead-strip the rest). ⭐ Templated COMPUTE dtypes `Tensor<T>`, T ∈ {f32,f64,c32,c64} (+i64/u8) — NO runtime-dtype VM; the low-precision set (f16/bf16/FP8-e4m3/e5m2/int8/int4-block, ggml-compatible blocks) = STORAGE with explicit convert/quantize ops; the ONE exception = int8/int4 integer inference computes natively (bit-exact across ALL hardware = the strongest cert tier). ⭐ NumPy stride-view semantics on a bounded rank ≤ 8 header (allocation-free metadata, WCET); element strides, signed (flips), stride-0 broadcast, contiguity tracking; Tensor owns via IAllocator, TensorView non-owning (Span discipline, no refcount); DLPack maps at the boundary. ⭐⭐ TWO-TIER reduction contract, named never conflated: Tier D default = fixed-order trees serial≡parallel ({1..16} bit-identical) · Tier R opt-in = ReproBLAS-class binned = partition-INDEPENDENT; + ★deterministic stochastic rounding (Philox keyed by (seed, element index) ⇒ order-independent + reproducible-by-seed — nobody ships it). ⭐ einsum = parser → opt_einsum-class paths → `EinsumPlan` build-once (NumPy re-plans every call) → TTGT over the OWN v0d GEMM + HPTT-class permute; cotengra-class hyper-optimizer w/ dynamic slicing = bounded contraction memory (WCET applied to einsum); GETT only if the profile names the transpose (SANITY-5). v13 pillars verbatim (workspace/no-heap, status-not-exception, bounded iteration). Deterministic-RANDOMIZED decompositions (v12 counter-RNG; same seed bit-identical {1..16}). Per-slice Windows verification from day one (the v13-z scar). Gold standards: NumPy-MKL/PyTorch/xtensor · opt_einsum+cotengra · HPTT · TBLIS/TCL · SPLATT/TACO · TensorLy/MATLAB-TT/ttpy · pagemtimes · ReproBLAS · torch/onnxruntime/llama.cpp+ggml. Extends ADR-0065; consumes ADR-0078 + ADR-0084; sibling to ADR-0094/0095; ADR-0097 (autodiff) at v15 kickoff. | arch, hesap, tensor, einsum, determinism, reproducibility, quantization, ml-inference, safety-critical, module-edges, substrate | Accepted |
| 0097  | **crd-hesap-autodiff — the automatic-differentiation cluster: one module for forward (v15) + reverse (v16), the deterministic no-atomics tape, suite-wide differentiability, differentiable solvers via implicit-diff, tape→C++ codegen (Phase 3.1.6 v15+v16).** ONE module `crd-hesap-autodiff` houses forward+reverse (shared rule math — a VJP is the transpose of a JVP — one 3-oracle gate, one crd::math surface, one set of suite bridges; header-only sub-headers + lean-consumer link isolation). ⭐⭐ THE MOAT = the DETERMINISTIC no-atomics reverse tape: SoA arena tape, fixed-order adjoint accumulation + scatter-inverted-to-gather + fixed-order reduction-tree merge (the v14 Tier-D contract lifted to adjoints), NO float atomics ⇒ **bit-identical {1..16} GRADIENTS = deterministic training** (torch/JAX cannot — atomic scatter-adds); world-first, timely vs the 2025-26 verifiable-training literature. `Dual<T>` MIGRATES from v7-b hesap-opt (opt re-exports, zero-regression gated) ⇒ acyclic edge hesap-opt→hesap-autodiff. Tape operands = OWNING Tensor<T>/arena values (backward runs post-unwind) + revolve checkpointing (Griewank-Walther, O(log T) mem). Autodiff is LOWER than the solvers: a custom_jvp/custom_vjp registration API keeps the edge solver→autodiff always (opt/ode register their own IFT rules — argmin-KKT/Newton/fixed-point implicit-diff with no cycle; differentiate-the-solution-reuse-the-factor, never AD-through-LU). Enzyme named OUT-OF-SCOPE (LLVM-plugin, MSVC-incompatible — the portability cornerstone). tape→.crds.cpp codegen (fusion/CSE/DCE) EMITS+INTERPRETS now; hot-reload execution graceful-gated on ADR-0081. 3-oracle gate on every rule (analytic / v13 complex-step / FD; reconstruct-verify-first). Two-layer (ADR-0078): Dual/Jet/Var = raw lower-layer carriers, typed strip-compute-retag at the driver. Gold standards: forward — Ceres-Jets/autodiff.hpp/CoDiPack/Sacado/Adept/JAX-CPU/ColPack/TaylorDiff-TIDES; reverse — torch-autograd/JAX/Stan-Math/Adept/CoDiPack/dolfin-adjoint-top88/jaxopt(unmaintained⇒own the lane). Showcases: v16-i deterministic-training of the v14-m certified controller · v16-j adjoint topology-opt · v16-k neural-ODE + KAN. Extends ADR-0065; consumes ADR-0078 + ADR-0096 + ADR-0084 + ADR-0081; sibling to ADR-0094/0095/0096. | arch, hesap, autodiff, forward-mode, reverse-mode, determinism, implicit-differentiation, codegen, ml-for-science, safety-critical, module-edges, substrate | Accepted |
| 0098  | **crd-kir + crd-hesap-gpu — the Cerid GPU compute COMPILER: a unified compute+autodiff kernel IR lowering to six backends, vendor-beating kernels, and certified cross-vendor determinism (Phase 3.1.6 v17, MAXIMAL scope).** NOT a Vulkan port — a tensor/kernel COMPILER (Triton/TVM/XLA-class), the substrate every Cerid project stands on (physics/rendering/geometry/ML/research/WASM). **`crd-kir`** = a NEW foundational module (peer of crd-math/crd-rhi, below hesap-gpu): a **TWO-LEVEL IR** — CKIR-Graph (typed tensor op-graph, **extends the v16-h graph_ad IR ⇒ every kernel differentiable by construction**; const-fold/CSE/DCE/fusion) → CKIR-Tile (tile/loop/schedule IR, coop-matrix/tensor-core, **fixed-tree reductions — NO float atomics, IR-enforced**) → codegen; a **CPU reference lowering** = the single oracle + determinism ground truth. ⭐ **SIX backends from one IR:** Vulkan/SPIR-V · CUDA/PTX-NVRTC · WebGPU/WGSL (browser+WASM) · DirectX12/DXIL · Metal/MSL · ROCm/HIP — each = one Tile codegen + one runtime driver over crd-rhi-compute (+ CUDA/HIP driver layers); op library untouched. ⭐ **Scheduler + AUTOTUNER** (schedule search → checked-in tuning DB, deterministic replay, no runtime tuning) = the vendor-beating engine. ⭐⭐ **Determinism baked in:** T1 run-to-run (fixed trees) · T2 cross-arch reproducible (binned/RFA) · T3 cross-VENDOR bit-exact IEEE-only (NoContraction + `float_controls` audit) **+ crd::math transcendentals AS CKIR ops** (one poly def → identical every backend ⇒ cross-vendor deterministic transcendentals, publishable) · **computation CERTIFICATES** (hash-chained IR/dispatch/IO-digest transcripts = verifiable-compute/DO-178C). **`crd-hesap-gpu`** = the op library authored in CKIR (GEMM/FFT/reduce/scan/sort/tensor/sparse/LA/NN/autodiff — each multi-backend + differentiable + deterministic FOR FREE; autodiff-on-GPU nearly free since CKIR-Graph IS the autodiff graph). **Perf bar = BEAT the vendor kernels** (cuBLAS/cuFFT/cuDNN/cuSPARSE/cuSOLVER/rocBLAS) at matched precision, honest per-op gap; portable peers vkFFT/CLBlast/MAGMA/ncnn/llama.cpp-Vulkan too. **Determinism = certified bit-exact core (where `float_controls` audit passes) + reproducible (T2) elsewhere.** DoD §6 every slice: ValidationCapture 0 · bit/ulp vs CPU-ref · gpu_determinism_check ×3 · cross-backend conformance · CRD_PERF_BUDGET_LE. Kickoff: RTX 4070 Ti SUPER + Vulkan 1.4.341 + glslc; **CUDA toolkit to install (v17-a)**; substrate proven via `geometry-bvh-gpu`. Honest size ~63 KLOC / ~1400 tests / ~18–26 wk. Consumes ADR-0080 + 0085 + 0081 + 0096 + 0097§v16-h; sibling to ADR-0097. | arch, kir, gpu, compiler, ir, cuda, vulkan, metal, dx12, rocm, webgpu, determinism, certificates, autodiff, autotuning, substrate | Proposed |
| 0094  | **crd-hesap-special + crd-hesap-stats — the statistics cluster: special-as-leaf + the inverse-incomplete cdf/ppf engine + the counter-RNG determinism moat + the two-axis honest gate (Phase 3.1.6 v12).** MAXIMAL subject = special functions + ~50 distributions + descriptive + the full hypothesis-test suite + resampling + KDE/robust/streaming + MCMC + regression/GLM/multivariate. ⭐ A NEW LEAF `crd-hesap-special` (gamma/beta/erf+incomplete+inverses · Bessel/Airy · orthogonal polys · transcendental tail) — stats/quadrature/dsp consume it, it never references them; the elliptic CANONICAL home (dsp delegates here). ⭐ THE INVERSE INCOMPLETE GAMMA/BETA ARE THE cdf/ppf ENGINE (no per-distribution quantile code; `gammainc_p_inv`/`betainc_inv`+`ndtri`, cached-gln/lbeta amortized ⇒ beat scipy's per-element ufunc). ⭐ DETERMINISM MOAT = counter-RNG (Philox4x32/Threefry4x64 pure fns of (counter,key) ⇒ same seed bit-identical independent of thread count — parallel bootstrap + MCMC/MV streams; DO-178C/ISO26262/FDA replay). ⭐⭐ TWO-AXIS HONEST GATE (the v10/v11 scoreboard scar): deterministic cores BIT-FOR-BIT vs scipy/statsmodels/ArviZ (read the peer's source for subtle gold formulas — the ArviZ R-hat/ESS backend — never guess) · RNG-driven by recover-known+same-seed-bit-identity · perf crushes ALL peers named individually (Bessel 24/24, dists 16/16 vs scipy, NUTS 104× ess/s vs PyMC, regression Ridge 47× vs sklearn); where NO clean gold peer exists (AR-spectral CI = asymptotic-normal) state it + gate analytically, never invent a convention. SANITY-8 reuse (regression rides dense lstsq/pinv/eig_sym; quadrature rides eig_sym; MV rides factor_cholesky). Back-wires the v11 spectral CIs (multitaper χ² vs scipy chi2.ppf + AR Berk-normal) closing the ADR-0093 deferral. CLI `hesap.{stats,special}.*`. Edges: stats→{special,dense,quadrature,jobs}; special = leaf. Extends ADR-0065; consumes ADR-0078. | arch, hesap, statistics, special-functions, rng, determinism, module-edges, substrate | Accepted |
| 0093  | **crd-hesap-dsp (+ -wavelet + -comms) — the DSP cluster: the design/application honest-gate split + SOS-by-default + the two-layer streaming contract (Phase 3.1.6 v11).** MAXIMAL subject (user: "every DSP functionality") = full Signal-Proc-Toolbox + adaptive + wavelets + comms, as 3 ISOLATED modules (a DAW links dsp not comms) + crd-units add (DecibelRatio/DecibelPower nonlinear lenses + NormalizedFrequency). ⭐⭐ THE HONEST GATE (the v10 scoreboard scar): filter DESIGN (ellip/remez/firls = transcendental+iterative — std::sin not bit-identical cross-libm, ULP drift compounds) gates on SPEC-COMPLIANCE (ripple≤Rp·atten≥Rs·alternation-count) + coeffs-to-**10-sig-digits**, NOT bit-match (physically unachievable + meaningless — spec-compliance is what MATLAB validates against, the STRONGER gate); filter APPLICATION (lfilter/sosfilt/biquad = pure mul-add) gates BIT-EXACT + {1..16} determinism MOAT (streaming-only — DAW replay/DO-178C; design is one-time, "deterministic ellip()" is not a differentiator). ⭐ DATA-FLOW RULE (locked v11-a): design in zpk → convert zpk→sos DIRECTLY; tf is OUTPUT-only NEVER a design intermediate (roots-of-tf = Wilkinson-ill-conditioned >order~8, proven by order-12 gate); zpk_freqz = well-conditioned factored eval; zpk→sos = nearest-to-unit-circle pairing (the reason SOS exists); SOS = default high-order IIR. Two-layer (ADR-0078): every filter ships typed whole-array batch + allocation-free stateful streaming kernels (Direct-II-T biquads/FIR/polyphase = the DAW/SDR hot loop). Gold standards: scipy.signal (free primary) + MATLAB-SP-Toolbox-R2026a (industry authority, installed) + Intel-IPP + liquid-dsp + CMSIS-DSP + PyWavelets. Edges: dsp→fft (spectral/conv/Hilbert/CZT) + eigen (DPSS-multitaper/MUSIC-ESPRIT/AR) + math + units + core; wavelet→fft; comms→dsp; NO edge to opt. Extends ADR-0065; consumes ADR-0078 + ADR-0092. | arch, hesap, dsp, filters, determinism, module-edges, substrate | Accepted |
| 0099  | **crd-gpu-context — the shared GPU device foundation; compute/rendering are separate CONCERNS on ONE device.** A deep `IGpuContext` (device/queues/backend) + `GpuContextManager`; `IComputeContext` (compute) and a future `RenderDevice` (rendering) are independent consumers of the same device — NOT a second VkDevice, NOT the rendering RHI reused for compute. Corrects the i-b VulkanComputeDevice shortcut. | arch, gpu-context, compute, rendering, architecture | Accepted |
| 0100  | **CKIR is the one GPU compute manager: a kernel-source-agnostic dispatch surface.** One manager owns context + dispatch runtime + kernel compiler; the dispatch surface dispatches a COMPILED kernel regardless of origin (CKIR-authored OR hand-written .spv/.ptx/.metallib) — lets LBVH + ray tracing live under it without pretending to be dataflow graphs. Amends 0098 + 0099. | kir, gpu-context, compute, dispatch, ray-tracing, abstraction, architecture | Accepted |
| 0101  | **The IR is the single source of truth for EVERY shader (compute + material); backend languages/bytecodes are OUTPUTS only.** Generalizes CKIR to the universal shader IR: front-ends (node editor ↔ IR · our text shader language · C++ builders) → IR (CORE = typed values + structured control flow + full intrinsic library; PROFILES = compute + material w/ PBR surface model) → codegen per backend (GLSL/HLSL/WGSL/MSL/CUDA) → cook to PER-BACKEND bytecode in crdr ('SHDR'); GLSL is never authored or stored. Deep look: Slang (language+IR→all backends, the prior art), MaterialX (material graph→codegen+PBR nodes), Unreal/Unity (graph→HLSL + variant cost + escape-hatch tax). Fix: compute via the rungs (rung 3 = step 1); rendering via the material profile; opaque-import escape hatch (non-portable). Phased A(core hardening)/B(material profile)/C(front-ends)/D(cook+variants). | kir, shader, ir, materials, node-editor, codegen, crdr, architecture, north-star | Accepted |
| 0102  | **Render-data, lighting & pass architecture — how the shader IR feeds a frontier renderer.** Unification, not greenfield: the renderer (frame graph, `IRenderPath` Forward/Forward+/Deferred/VisBuffer, `PerFrameUbo`, `MaterialTemplate`+variants, reflection, cooking) STAYS; CKIR replaces the hand-written GLSL as the shader SOURCE. Decisions: globals (camera/time) live in the renderer's per-frame set 0, NOT the GPU context (upholds 0099); **frequency-based descriptor sets** 0=frame/1=pass-lighting/2=material/3=object (bindless/GPU-driven ready); **material = surface response (OpenPBR params), lighting-agnostic; render path = lighting technique** → one material works Forward+ OR Deferred (HYBRID: deferred/vis-buffer opaque + forward transparent); multi-pass (shadows/CSM/G-buffer) = the frame graph + `variant_for_pass`; skinning = structured-buffer palette (set 3, VS-skin default); uber-shaders = existing `ShaderOption` variants. D-007 DESIGNS+validates the seam (renders); the full Forward+/Deferred pipelines are the post-hesap RENDERING phase. | renderer, shader, ir, materials, lighting, deferred, forward-plus, frame-graph, bindless, architecture, north-star | Accepted |
| 0103  | **`crd-gpu-context` owns every GPU program and pipeline; no module outside a backend names a shading language or a bytecode.** Makes ADR-0101 enforceable. TWO grep-checkable invariants: **I1** no GLSL/HLSL/WGSL/MSL/CUDA source crosses a module boundary (it lives only between our emitter and the vendor compiler, inside one backend); **I2** no SPIR-V/DXIL/PTX bytes appear in a public header — a consumer holds only an opaque `IGpuProgram`. Currency IN = the IR (`KGraph`+`KEntry`), OUT = `IGpuProgram`. `IGpuContext` gains `create_program(graph, entry)` + `create_program(cooked_name)` (ship path + the ADR-0101 §4 non-portable escape hatch) and the three domains `compute()`/`raster()`/`raytracing()`. `ShaderStage` complete day one = the 14 SPIR-V execution models (compute · vertex · tess-control · tess-eval · geometry · fragment · task · mesh · raygen · intersection · any-hit · closest-hit · miss · callable); unimplemented stages REFUSE LOUDLY, never fall back to compute. `crd-shader` loses `compile.hpp` + shaderc + dxc, keeps Effect/reflection/runtime/material. `crd-rhi`'s Device/ShaderModule converge onto `IGpuContext`/`IGpuProgram`; `rhi-vulkan` is absorbed by `gpu-context-vulkan` (one VkDevice, one pipeline cache, one Vulkan-aware module). Edge `crd-gpu-context → crd-kir` verified acyclic. **Supersedes ADR-0099 §6** (crd-shader as the shared GLSL/HLSL compiler — an Accepted decision that contradicted ADR-0101). Rollout = detour D-008 C0–C4, each step independently green. | gpu-context, kir, shader, rhi, renderer, architecture, ir, ray-tracing, mesh-shaders, substrate, north-star | Accepted |
| 0104  | **IR-as-crdr: the shader cook + deploy pipeline (D1–D12).** The IR (ADR-0101) SHIPS: KGraph+KEntry serialize into `.crdr` ('SHDR') → content-hash cook cache → per-backend REAL bytecode (SPIR-V/DXIL/PTX; MSL/WGSL source) → variant containers (VART, content-hash dedup) → zero-compile load + persistent driver pipeline cache (VkPipelineCache / ID3D12PipelineLibrary) → hot-reload (atomic pipeline swap, generation retire) → parallel cook on crd-jobs fibers → async warmup → spec-constant binding at load. Supersedes crd-shader's Effect/PSO path as the deploy currency. | kir, shader-cook, crdr, deploy, variants, pipeline-cache, hot-reload, architecture | Accepted |
| 0105  | **Retire crd-rhi + crd-renderer: crd-gpu-context IS the graphics layer.** Two facades over ONE device (D-008 made rhi ADOPT gpu-context's VkDevice) = a standing double-wiring tax (GEO-3 2b: sRGB wired twice in a day; cooked linear-space mips never reach the real renderer). rhi/renderer FROZEN immediately; the D-007 **RET band** (rows 89-96) absorbs every capability into gpu-context (present/swapchain · cooked-mip+sRGB upload seam · ADR-0085 allocator parity · ImGui · crd-draw port · bvh-gpu Morton compute · shader/meshgen sweep), PORTS ~5k device-level test assertions (coverage parity = deletion PREcondition), then DELETES crd-rhi + crd-rhi-vulkan + crd-renderer (the legacy-GLSL-Effect precedent). CPU-side resource types re-home GPU-free in crd-resources (ADR-0042's loader posture survives; its rhi upload half dies). Sequencing: GEO-3 close → RET complete → GEO-4..7 integrate ONCE on the clean stack. Supersedes the rhi halves of 0036/0042/0080/0085 (struck in place as slices land). | gpu-context, rhi, renderer, retirement, architecture, substrate | Accepted |
| 0106  | **Unified frame-graph runtime: `crd-render-graph` is the single live runtime.** RAF-0…7 built the asset-driven foundation as ADDITIVE leaf modules (render-asset-core · render-program · render-material · render-pass registry · render-graph runtime), gated in isolation — the LIVE renderer was never wired onto them, so two frame-graph execution paths exist over one device (SceneRenderer→`frame_runtime.cpp` `FramePassKind` switch→~57 verbs, vs `crd-render-graph::execute_frame`). §2.1 forbids two at close; corollary 3 (unreachable runtime ≡ missing) means RAF-6/7's live migration silently landed on RAF-8. Decision: **render-graph is THE live runtime** (`FrameGraphTemplate`+`CompiledFrameGraph`); **render-pass**/**render-graph** own the registry+runtime (corrects RAF-0 §3, struck in place — folding into frame-cook would rebuild the giant module §18/§21 forbid); **frame-cook** keeps `FrameGraphDesc`+cooked blobs and gains a Cooked→Template **load bridge** via a new ACYCLIC `frame-cook→render-graph` edge (render-graph depends on neither frame-cook nor scene, so `frame-cook ⊥ crd-scene` holds via the preserved `IFrameGraphHost` seam — the host resolves ECS→pre-resolved `DrawItem`; render-graph never sees a scene type); **SceneRenderer** becomes an ORCHESTRATOR; the `FramePassKind` switch is a migration adapter deleted at RAF-12. Migrate ONE kind at a time — old switch + new executor both resolve; command-parity (mock encoder) + pixel-parity + `crd-sandbox --smoke-test 2` both backends per increment. RAF-8 splits into 8a (wire live runtime) + 8b (orchestration). Supersedes ADR-0032's runtime-ownership (preserves its lifetime/aliasing/barrier/one-submission). **CLOSED at RAF-12.3 (2026-08-06):** the `FramePassKind` adapter is deleted (→ `ExecutorTypeId` + role bits); 12.2 unified the live path via `AuthoredPass`+`run_authored_cb` (Decision #1 refined in place — see the ADR's RAF-12 amendment). | renderer, frame-graph, gpu-context, architecture, render-path | Accepted (closed RAF-12.3) |
| 0107  | **Interactive UI + 2D rendering architecture (I2D-0, post-RAF).** Five distinct concepts (SceneWorld · UiWorld · CanvasCompositor · UiMaterial/UiEffectGraph · FrameGraph), never collapsed. `UiWorld` is a dedicated RETAINED world (`UiNodeId` ≠ gameplay `EntityId`); UI = semantics (layout/focus/nav/a11y/l10n/state/binding), sprites = visuals. `CanvasDisplayList` = a typed backend-neutral compiled paint rep lowering `UiWorld → style/layout/text → paint compile → display list → clip/layer/batch compiler → RAH-hardened canonical GPU commands → RAF executors` (**I2D-1 Canvas BLOCKED on RAH-1 typed attachments + RAH-2 resource-table bindless**; seam consistent with `docs/systems/rah-0-canonical-model-audit.md`). Three authoring paths (documents / builder-reconciliation with stable identity / low-level) → one UiWorld. CKIR-backed UiMaterial + multi-pass UiEffectGraph compiling to the RAF frame graph (no mini scheduler). Reuses `AssetId`/`DiagnosticList`/RAF-11 reloader/`crd-anim`. CR-D007 editor = I2D-9 widgets on the I2D-4 shell (D7E reframed); ImGui kept for debug/recovery. Same L0–L7 maturity (nothing >L5 today). D-007 §UI/2D SUB-PROGRAMME. | ui, 2d, canvas, uiworld, sprite, editor, architecture, post-raf | Proposed |
| 0108  | **A Cerid-owned executable-program language stack (CEIR/CHIR); C++ is no longer the *only* authorable program (CEIR-0b).** Captures the user-directed CEIR pivot (mission §5): algorithms-as-assets need an inspectable/diffable/serializable/hot-reloadable/agent-authorable/lowerable program representation a compiled `.crds.cpp` DLL structurally cannot provide. **Surgical supersession of ADR-0081 §9 ONLY** (the "C++ is the ONLY scripting path" clause); ADR-0081 §1-§8 (agent-native CLI/RPC/MCP, capability security, command schema, replay) REAFFIRMED. Decision (§5): C++ stays first-class native + hot-reload; Cerid gains an OWNED textual+visual language stack (CEIR execution IR now, CHIR high-level layer design-only until CEIR-29); no Lua/Python/JS runtime (the third-party-VM rejection stands); CLI/RPC/MCP stays source-of-truth; program assets are agent-authorable + machine-inspectable. Language non-negotiables pinned now (§98/§99): no mandatory GC in hot paths · deterministic time/RNG · capability security · C++ FFI · structured concurrency · Result/Option · unit-aware. ⛔ The cornerstone flip (PRINCIPLES/AGENTS/README/ROADMAP) + the ADR-0081 §9 in-file strike are DEFERRED to §5's second gate — the first CEIR vertical slice (§7). Refined by CEIR-0c/0d/0e. | scripting, lang, ceir, chir, ir, agent, architecture, substrate, north-star | **Accepted (2026-08-07; cornerstone flip executed 2026-08-10 at CEIR-13z)** |
| 0109  | **CEIR/CHIR/CKIR ownership, the one-way layer contract, and `crd-ceir` module placement (CEIR-0c).** Fixes the layer separation (mission §3, unrevisable): CHIR=source semantics (design-only→CEIR-29) → CEIR=execution/orchestration IR → CKIR=per-invocation kernels (UNCHANGED, referenced by content-hash `KernelRef` identity, never a `KGraph`). Lowering strictly one-way (never sideways/up); provenance flows back as metadata. **Module:** `engine/ceir` (`crd-ceir`) is host-only — deps `crd-core/log/memory/containers/units` ONLY (acyclic by construction; units earned = the ADR-0078 dimension-tag boundary on `Type`, designed at CEIR-3e). GPU/jobs stacks reached via **dependency inversion**: the abstract `IExecutionProvider` (append-at-END, D135) lives in `crd-ceir`; implementations live in bridge modules `crd-ceir-host` (→jobs) + `crd-ceir-gpu` (→gpu-context/render-graph/kir, where the CEIR-0a `record_*`+CKIR-compile STAY). Extends ADR-0103 I1/I2 as **I3** (no shading-language/bytecode name in `crd-ceir`) **I4** (no backend type in a `crd-ceir` public header) **I5** (acyclic edge set, ADR-0096 gate extended). Finalizes the CEIR-1 C++ names (`crd::ceir::{Context,Module,Operation,Value,Block,Region,SymbolTable,Type,Dialect,OpId,SourceLoc,KernelRef,ModuleBuilder}`) + the §10 semantic-identity model (stable ids · source spans · layout-separated-from-semantics). Gates CEIR-1. | ceir, chir, ckir, ir, architecture, module-edges, substrate, north-star | **Accepted (2026-08-07 — binding for CEIR-1)** |
| 0110  | **Native-intrinsic schema, the legitimacy rule, and the three plugin-extension levels (CEIR-0d).** An intrinsic is an ORDINARY CEIR-2 op (printer/verifier/serializer for free) carrying §100 native-binding metadata (id/version/typed-I/O/effects/domain/determinism/thread-safety/lifetime/capabilities/provider/cost/hot-reload/debug) + a bridge-registered handler — NOT a parallel system; `IntrinsicRegistry` in `crd-ceir` (ADR-0109), handlers in bridges. **Legitimacy IFF test:** an intrinsic introduces a capability Cerid didn't previously understand (hardware/OS/device/external/provider primitive); a composable algorithm MUST be a program (§178); ⛔ **composable-but-slow is never Level C — intrinsics = capabilities, providers = performance** (§102/§52/§70). **Plugin levels** (§101, hardest last): A subgraph/func (no native) · B custom op + lowering (plugin compiler, no backend) · C native intrinsic/provider (only when A/B can't express it). Classifies the CEIR-0a atomic set: `present`/codecs/scene-resolvers ✅ legitimate; `submit_overlay`/`draw_overlay` dissolve into `ceir.render` at CEIR-11. Registry lands after CEIR-4; first shipped intrinsic `present` @ CEIR-12. | ceir, intrinsics, plugin, capability, architecture, extensibility, substrate | **Accepted (2026-08-07)** |
| 0111  | **Open-world TYPE model** (CEIR-3 era). | ceir, type, open-world | Accepted (2026-08-09) |
| 0112  | **Open-world ATTRIBUTE model.** | ceir, attribute, open-world | Accepted (2026-08-09) |
| 0113  | **Effect widening + open effect locations.** | ceir, effects | Accepted (2026-08-09) |
| 0114  | **Stable semantic identity** (ids/spans, layout-separated). | ceir, identity | Accepted (2026-08-09) |
| 0115  | **Trait/interface split + region reservation.** | ceir, trait, region | Accepted (2026-08-09) |
| 0116  | **Capabilities/safety split + typed time domains** (the §57 capability model; CEIR-8f). | ceir, capability, safety, time | Accepted (2026-08-09) |
| 0117  | **Compiler-infrastructure skeleton.** | ceir, compiler | Accepted (2026-08-09) |
| 0118  | **Incremental-evaluation unification.** | ceir, incremental | Accepted (2026-08-09) |
| 0119  | **Transactions** (atomic module edit + rollback). | ceir, transactions | Accepted (2026-08-09) |
| 0120  | **Hot-reload + state migration.** | ceir, hot-reload | Accepted (2026-08-10) |
| 0121  | **Execution-plan cache.** | ceir, plan-cache | Accepted (2026-08-10) |
| 0122  | **Reference executor (full host subset).** | ceir, executor | Accepted (2026-08-10) |
| 0123  | **Compiled execution plan.** | ceir, executor, compiled | Accepted (2026-08-10) |
| 0124  | **Memory planner.** | ceir, memory-planner | Accepted (2026-08-10) |
| 0125  | **`ceir-gpu` lowering bridge** (CEIR→CKIR→device). | ceir, gpu-context, lowering | Accepted (2026-08-10) |
| 0126  | **`ceir-gpu` execution seam** (`IExecutionProvider`; the §69 partitioner landed as `partition_ml` @ CEIR-24). | ceir, gpu-context, executor, provider | Accepted (2026-08-10) |
| 0127  | **`ceir.frame` dialect + converter** (CEIR-15). | ceir, frame, dialect | Accepted (2026-08-11) |
| 0128  | **CHIR-0: the Cerid high-level language, binding decisions against the corpus (CEIR-32a).** Binds the decisions CEIR-0e deferred: **scope** = CHIR-0 v1 is the §143 five constructs ONLY (event-handler `func.func`+time-domain, ECS query, `parallel for`→`task.parallel_for` [STATE-FREE body, verifier-enforced], `await`→`async.launch`/`await`, `update state`→`core.state` cell) + diagnostics/spans, everything else → deferral ledger; **ownership** = the 0e §5 COMPOSITE confirmed against the corpus (values + generational handles + arenas + explicit state stores + a LIGHT borrow onto the BUILT `TypeKind::Qualified`/`OwnershipKind::BorrowedView`; full borrow-checker REJECTED — fights visual+agent authorability); **reload survival** = CHIR PINS stable ids from the source declaration (name+scope, not position) before `assign_stable_ids`, so a re-edit → same id → CEIR-10a Migrate not Reject; **text/graph** = two projections of ONE source model, the CR-D007 graph is a SCHEMA (not an editor) lowering through the SAME path (§180 #13), CEIR-32 defines it / CEIR-33 renders it; **module** = new `crd-chir` (engine/chir), host-only, one-way → `crd-ceir` (the ADR-0109 gap; the graph schema lives here, editor→chir→ceir). NO grammar (32c's parser + docs/systems/chir.md own it). | chir, ceir, language, ownership, module-edges, substrate | Accepted (2026-09-06) |

- [0129 — Renderer, UI, CR-D007 and notebook delivery order](../decisions/0129-renderer-ui-editor-delivery-order.md) — Accepted, user decisions 2026-09-12; [roadmap] [rendering] [ui] [editor] [hesap] [platform].

---

<a id="docs-design-readme"></a>
## Source: docs/design/README.md

<a id="docs-design-readme-docsdesign--per-slice-implementation-specs"></a>
### `docs/design/` — per-slice IMPLEMENTATION SPECS

> **What this directory is for.** A design doc here is the **implementation contract for ONE slice**: what
> already exists (so it is reused, not rebuilt), what is genuinely missing, the sequenced increments, the gate
> for each, the named risks, and the explicit non-goals. It sits between the **row** (the one-line contract in
> [master table](../ROADMAP.md#master-table)) and the **session log** (what actually happened).
>
> **How to find the doc for a slice you have been asked to implement:** the slice's master row
> links it BY PATH. Start at `context.md` → ROADMAP → the row → this directory. The index
> below is the reverse lookup.

<a id="docs-design-readme-conventions"></a>
#### Conventions

- **One file per slice**, named `<slice-id>-<slug>.md` (e.g. `ren-3-lighting-shadow-pipeline.md`).
  Shared execution/requirements catalogues define cross-slice contracts; they contain no independent live queue.
- **The row is the pointer.** Whenever a spec is written, the slice's row MUST be edited to link it — a design
  doc that nothing points at is invisible to the next agent, which is the exact failure this index exists to
  prevent (found 2026-07-25: `docs/design/` was not referenced from `docs/README.md` at all, so the canonical
  reading order never reached it).
- **A REUSE AUDIT is mandatory before the increments.** Grep the engine first (SANITY #8) and state, per gap,
  whether it is *wiring* or *new work*, with file:line evidence. The REN-3 spec's sky section is the worked
  example: what read as "new work" turned out to be four LUT kernels already dispatching oracle-green on
  Vulkan, which shrank the increment substantially.
- **Every increment carries its gate**, and the whole slice carries its acceptance criteria + named
  non-goals, so an omission is a decision rather than drift.
- **Specs are living until their slice closes**, then they are historical — the session log supersedes them as
  the record of what shipped. Do not retro-edit a closed spec; write the divergence in the session log.

<a id="docs-design-readme-index"></a>
#### Index

| slice | doc | status |
|---|---|---|
| **CEIR-0a** | [ceir-0a-execution-path-inventory.md](../design/ceir-0a-execution-path-inventory.md) — the from-code inventory of every execution-program representation (executors · frame-graph runtime · scene_renderer orchestration + hand-list · IComputeContext · cookers · .crdr · draw · media), classified composite-vs-atomic. Headline: RAF already did the split — CEIR is a promotion, not a rewrite | ✅ complete 2026-08-07 |
| **CEIR-0e** | [ceir-0e-chir-0-language-design-note.md](../design/ceir-0e-chir-0-language-design-note.md) — the CHIR high-level language design NOTE (design-only, decides nothing; the binding decisions are a CEIR-29 ADR). §98 feature set · ownership-model options weighed (leaning: values + generational handles + arenas + state stores + light borrows, NOT a full borrow checker) · syntax sketches · the text/visual projection model | ✅ accepted (direction) 2026-08-07; binding decisions deferred to CEIR-29 |
| **CEIR-0g** | [ceir-0g-maturity-and-manifest.md](../design/ceir-0g-maturity-and-manifest.md) — reconciles the two maturity models (post-RAF L0–L7 vs CEIR L0–L8) into ONE forward model + a two-axis transition (`raf_level` = today's reality, `ceir_level` = the forward track, both honest); + the §174 manifest field set (adds `providers` + `determinism_tier`) + the registry-migration plan | ✅ accepted 2026-08-08 |
| **CEIR-0h** | [ceir-0h-migration-and-deletion-tables.md](../design/ceir-0h-migration-and-deletion-tables.md) — the migration + DELETION ledger built from CEIR-0a: what promotes vs deletes, every deletion with its parity gate named FIRST (frame-path F1–3 @ CEIR-12 · orchestration E1–5 @ CEIR-13 · residual special-cases R1–2 @ CEIR-11 · §PR-3 supersession @ 0f). CEIR-31 executes it verbatim | ✅ accepted 2026-08-08 |
| **CEIR-0z** | [ceir-0z-close-report-and-sizing.md](../design/ceir-0z-close-report-and-sizing.md) — the CEIR-0 close: the mission §184 fifteen-item report (each answered from 0a–0h evidence) + honest DERIVED sizing (CEIR-1…13 ≈ 34–55 KLOC with in-tree anchors + per-band confidence; ~4–8 mo dark period, very-low confidence) + the 5 unresolved-design-questions docket | ✅ accepted 2026-08-08 |
| **CEIR-10a** | [ceir-10a-hot-reload-and-state-migration.md](../design/ceir-10a-hot-reload-and-state-migration.md) — hot-reload + state-migration slice spec (ADR-0120) | ✅ closed |
| **CEIR-10b** | [ceir-10b-execution-plan-cache.md](../design/ceir-10b-execution-plan-cache.md) — execution-plan cache (ADR-0121) | ✅ closed |
| **CEIR-11a** | [ceir-11a-reference-executor.md](../design/ceir-11a-reference-executor.md) — the reference executor, full host subset (ADR-0122) | ✅ closed |
| **CEIR-11b** | [ceir-11b-compiled-execution-plan.md](../design/ceir-11b-compiled-execution-plan.md) — compiled execution plan (ADR-0123) | ✅ closed |
| **CEIR-13z** | [ceir-13z-execution-proof.md](../design/ceir-13z-execution-proof.md) — the §129 execution proof (the cornerstone flip) | ✅ closed |
| **CEIR-14** | [ceir-14-render-dialect.md](../design/ceir-14-render-dialect.md) — the `ceir.render`/`ceir.frame` dialect (ADR-0127) | ✅ closed |
| **CEIR-14z** | [ceir-14z-render-execution-proof.md](../design/ceir-14z-render-execution-proof.md) — render execution proof | ✅ closed |
| **CEIR-15** | [ceir-15-framegraph-unification.md](../design/ceir-15-framegraph-unification.md) — frame-graph unification | ✅ closed |
| **CEIR-16** | [ceir-16-executor-migration.md](../design/ceir-16-executor-migration.md) — executor migration | ✅ closed |
| **CEIR-17** | [ceir-17-scene-bridge.md](../design/ceir-17-scene-bridge.md) — the scene bridge (`scene_renderer` → authored assets) | ✅ closed |
| RAF-0 | [raf-0-rendering-foundation-design.md](../design/raf-0-rendering-foundation-design.md) — the RAF rendering foundation (ADR-0106) | ✅ closed |
| RAF-12 | [raf12-verb-relocation.md](../design/raf12-verb-relocation.md) — RAF-12 verb relocation | ✅ closed |
| **CEIR-18…25** | _no separate spec files_ — bands 18-25 (`ceir.rt`/`work`/`shape`/`tensor`/`layout`/`linalg`/`quant`/`sparse`/`ml`/`autodiff` + provider partitioning) were design-locked INLINE in the tracker's per-band **BAND-OPEN LOCK** blocks (`docs/detours/D-007-ceir-tracker.md`) + the ADRs, not as standalone specs | 📋 see tracker |
| REN-2 | [ren-2-rtt-and-material-textures.md](../design/ren-2-rtt-and-material-textures.md) — RTT transients + sampled material textures | ✅ closed 2026-07-25 |
| REN-3 | [ren-3-lighting-shadow-pipeline.md](../design/ren-3-lighting-shadow-pipeline.md) — lighting · shadow · procedural sky · full AA, **sandbox-visible** | ◼ superseded — the pre-RAF REN band was superseded by RAF/post-RAF (2026-08-07 trim); the shipped parts (CSM atlas, sky) live in the `forward_csm` frames + session logs |
| REN-3.1 | [ren-3-1-depth-rtt-transients.md](../design/ren-3-1-depth-rtt-transients.md) — RTT **depth** transients + `draw_storage_depth_only` (the shadow-map substrate) | ✅ closed 2026-07-25 (gated both backends + bench) |
| **REN-36** | [ren-36-authorable-frame-graph.md](../design/ren-36-authorable-frame-graph.md) — ⭐ **render passes, pipelines and whole rendering ARCHITECTURES as authorable assets, API-agnostic** (user-declared MUST) | ✅ shipped — the authoring stack landed via REN-36/37/38 (2026-07-25…27) and became the RAF foundation |
| REN band | [ren-band-reuse-audit.md](../design/ren-band-reuse-audit.md) — pass-1 audit of all 35 rows. **Read its method warning**: pass 1 mostly re-derived what the rows already say and was wrong twice; the rows carry their own reuse notes | 📋 reference |
| hesap-fft | [hesap_fft_generated_codelets.md](../design/hesap_fft_generated_codelets.md) — the generated FFT codelet scheme | ✅ shipped |
| REN-37 | [ren-37-material-technique-composition.md](../design/ren-37-material-technique-composition.md) — **MATERIAL x TECHNIQUE composition**: how an authored frame graph reaches into the FRAGMENT SHADER. Three authored layers (material=surface / technique=lighting / frame graph=schedule), the binding contract, ubershader-vs-variants resolved by our IR, and the variant collapse lowering gives for free | ✅ shipped 2026-07-27 (REN-37 — material × technique composition live; `.crdm`/`.crdt` authored programs) |
| **REN-41 Stage 4** | [ren-41-stage4-nanite-cluster-lod.md](../design/ren-41-stage4-nanite-cluster-lod.md) — **Nanite cluster-LOD renderer integration**: wire the CLOSED 40-I cluster-DAG (cook + BVH select + unpack) into the renderer — a GPU `cluster_select` compute kernel, REAL cluster task+mesh `.crdv` programs, packed-buffer upload + a `MeshRenderer` route flag, authored `cluster_select`→`mesh_draw` passes. Reuse audit + 5 de-risked increments (mesh-body spike first) + risks/non-goals | ◧ folded into the post-RAF **VGE** band (40-I cluster-DAG + REN-41 S4-0 shipped; the renderer integration continues as VGE in D-007) |

Current programme references: [execution contract](../design/renderer-ui-execution-contract.md), [inherited renderer/UI requirements](../design/rendering-ui-contracts.md). Live slices/subslices: [ROADMAP](../ROADMAP.md#master-table).

---

<a id="docs-research-readme"></a>
## Source: docs/research/README.md

<a id="docs-research-readme-research"></a>
### Research

One file per research task. The "we googled this and here's what we found"
archive. Plain English. "What was the question, what's the answer, what
should Cerid do."

Read this folder before re-researching something. Read
[`RESEARCH_TEMPLATE.md`](../research/RESEARCH_TEMPLATE.md) to write a new entry.

Filename convention: `YYYY-MM-DD-<kebab-case-topic>.md` (2-4 word slug).

| Date | Topic | Used by |
| ---- | ----- | ------- |
| 2026-05-27 | [Streaming allocators (virtual-memory substrate)](../research/cerid-streaming-allocators.md) | Phase 2.2 S2+ (ADR-0085) |
| 2026-06-14 | [Beating MKL on 1D complex FFT — every number + dead end](../research/fft-mkl-crush.md) | v10 `crd-hesap-fft` (v10 shipped; the banked wins + remaining large-N parity gap are recorded in the dossier's own status) |
| 2026-07-06 | [v15 forward-mode AD: frontier crush levers + reconstruct-verify tables](../research/2026-07-06-v15-forward-ad-crush.md) | v15 `crd-hesap-autodiff` (a–z impl reference) |
| 2026-07-23 | [The offline renderer frontier — the OFF band master survey](../research/2026-07-23-offline-renderer-frontier.md) | D-007 OFF band (rows 46-54); offline render mode |
| 2026-07-23 | [The geometry-resource pipeline — import → decompose → author → cook](../research/2026-07-23-geometry-resource-pipeline.md) | D-007 GEO band (rows 66-70); mesh/scene I/O before the offline renderer |
| 2026-07-23 | [The world-class resource + scene system — GEO band no-gap expansion](../research/2026-07-23-resource-scene-world-class.md) | D-007 GEO band (rows 66-76): ECS integration · asset processor · animation · timeline (film) · audio (DAW) · MCP/CLI agent surface |
| 2026-07-23 | [The owned media-codec platform — images · audio · video · transcode](../research/2026-07-23-media-codec-platform.md) | D-007 MED band (rows 77-88): full codec roster by patent status (H.265/AAC excluded by name, H.264 time-gated 2027-11), decode→intermediate→encode transcode engine |

<a id="docs-research-readme-how-to-add-an-entry"></a>
#### How to add an entry

1. Run `/research <topic>` in OpenCode — the researcher agent writes the file
   for you and updates this index.
2. Or copy `RESEARCH_TEMPLATE.md` manually if you're filling one in by hand.

Keep entries short and opinionated. The point is decision support, not a
literature review.

- [2026-09-12 — Whole-system architecture/documentation audit](../research/2026-09-12-system-audit.md): source findings, primary research, module census and planning consolidation; execution status lives in ROADMAP.

---

