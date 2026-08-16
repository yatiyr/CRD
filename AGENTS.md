# Cerid Engine — Agent Rules

> Rules of engagement for any AI agent (Claude Code, OpenCode, etc.) working on Cerid.
> Concise by design. **Build/test commands + verification + troubleshooting** → `docs/BUILDING.md`. **Module index** → `docs/systems/README.md`. **Live state** → `context.md`. **Master plan** → `docs/ROADMAP.md`. **Memory index** → `MEMORY.md`.

## What is Cerid

Cerid is a **general-purpose C++20 real-time engine substrate**. Games are one consumer; **simulation (incl. robotics), medical visualization, DAW-class creative tools, CAD/CAM, and offline cinematic pipelines** are equal-class consumers. The architecture serves all of them; no domain is privileged.

Shipped modules (~90) — the honest per-module index is **`docs/systems/README.md`** (this file does NOT carry a
module inventory; a second copy of it always rots). The broad strokes: the foundation
(`core/log/vm/memory/containers/math/units/time/platform/app/config/jobs/perf/resources/scene/imgui/profile`), the
GPU platform (`gpu-context{,-vulkan,-dx12,-cuda}` + `kir` + the RAF render-asset stack + `draw` — the former
`rhi/renderer/shader` stack is **retired**, ADR-0105), 13 `geometry-*` sub-modules, and the `crd-hesap` numerical
substrate (v0–v16 shipped: dense → sparse → eig → opt → ODE → FFT → DSP/wavelet/comms → special/stats →
interp/quad/diff/motion → tensors → autodiff). **The current focus lives in `context.md`, never here.** Planned:
`eylem` resume (paused at v1b) / `sdf` / `font` / `audio` / `scripting/language (Cerid-owned CEIR/CHIR + CR-D007 visual + first-class C++ hot-reload, ADR-0108)` / `ui` / editor (CR-D007).

## Engineering Principles (non-negotiable)

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

## Agent Conduct (non-negotiable)

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

## Tech Stack

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

## Build & Test

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

## Project Structure

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
MEMORY.md                    agent memory index (in ~/.claude/projects/.../memory/)
```

## Coding Standards

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

## Architectural Cornerstones

Pinned decisions from `docs/decisions/`. Don't re-litigate; circumstances change → new ADR.

- **The graphics layer IS `crd-gpu-context` (ADR-0103, ADR-0105 — supersedes the ADR-0001/0080 rhi split).** One
  device, one facade, one IR: `crd-gpu-context` owns every GPU program + pipeline; no module outside a backend names
  a shading language or a bytecode (I1/I2 grep gates). The former `crd-rhi`/`crd-rhi-vulkan`/`crd-rhi-compute`/
  `crd-shader`/`crd-renderer` modules were retired at RET-8 (2026-07-23). Vtable-stability discipline unchanged:
  append new virtuals at END (D135).
- **Rendering is asset-driven (ADR-0106; supersedes the ADR-0016/0017 IRenderPath plan).** Every technique is an
  authored frame-graph asset executed by `crd-render-graph`; render "paths" (forward/deferred/vis-buffer) are the
  post-RAF RPL proof library, not C++ path classes. See the ⛔⛔⛔ authored-frame-graphs rule above.
- **Hybrid scene model (ADR-0020 + ADR-0049-0061).** Spatial Hierarchy (scene tree) + SoA component storage. UI nodes coexist with 3D nodes Godot-style. 8-layer slot-shaped ECS substrate (Phase 3.0 ✅).
- **Two-layer typed architecture (ADR-0078).** Upper layer = `Quantity<D, T>` at every API/config/cooker/UI surface. Lower layer = raw `f32`/`f64` in SIMD/math/geometry/numerical kernels + GPU writes. Bridges only at the boundary. Mars Climate Orbiter bug class is a compile error.
- **Physics — Cerid-native (eylem) from day 1 (ADR-0062, ADR-0063).** No third-party wrap; `crd-eylem` substrate IS the interface. Deterministic by construction, ECS-native, fiber-jobified, multi-domain (games + robotics + medical + cinematic + DAW), templated 2D + 3D, GPU-extensible. Phase 3.1 v0–v9 (~30 slices); v0–v1b ✅; v1c+ paused (resumes after detour D-007 + hesap per the locked sequencing).
- **Geometry-before-physics sequencing (ADR-0076 §12).** `crd-geometry` (substrate of 11 sub-modules) ships full BEFORE eylem v1c resumes, so eylem v1c+ consumes geometry from day 1 with no deferred-refactor debt.
- **Numerical substrate (ADR-0065).** `crd-hesap` MATLAB-class numerical substrate, Phase 3.1.6 — **v0–v16 shipped** (dense/sparse/iterative/direct/eig/opt/ode/fft/dsp/wavelet/comms/special/stats/interp/quadrature/diff/motion/tensor/autodiff); v17 GPU compute grew into detour D-007 (hesap-GPU is its last stop); v18 notebook+MCP planned.
- **Authoring vs runtime.** Configs and scenes authored in TOML; scenes cooked to binary for runtime. Configs parsed directly (small, not hot-path).
- **ImGui's role.** Debug-only forever. After `crd-ui` ships, ImGui never grows into editor or game surfaces.
- **Reference counting split.** Generic intrusive ref-counting in `crd-memory`. Resource-facing shared references (eviction, lazy loading, hot-reload ownership) in `crd-resources`.

## Definition of Done

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

## Session Re-entry Prompt

Use when starting the next session and asking the assistant to pick up where the project left off:

```text
You are working on the Cerid Engine. Read context surgically.

# Mandatory reads (always)

Follow the canonical reading order in docs/README.md (the Documentation Map / Start
Here): AGENTS.md -> docs/BUILDING.md -> PRINCIPLES -> SANITY -> context.md -> ROADMAP. The map
also points to every other doc area (ADRs, systems, research, debt, detours).

# Then ONE phase doc

From context.md "Current focus" you'll know the active phase. Open ONLY that
one phase file under docs/phases/. Do NOT read other phase files.

# Lazy-load everything else

- Past decision → docs/decisions/README.md tag index → fetch ONLY the matching ADR.
- Last session detail → the session log linked under "Last shipped milestone".
- Module surgery → docs/systems/<module>.md, then deep-dive only if needed.
- Open follow-ons → docs/debt.md.
- Detour rules → docs/detours/README.md.
- Reusable engineering lessons → MEMORY.md index (one-line entries with file pointers).

# Session start ritual

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

# Session end ritual

When user says session-end:
1. Write docs/sessions/YYYY-MM-DD-<slug>.md
2. Update context.md "Last shipped milestone" (one-paragraph summary + link)
3. Architectural decision → new ADR file under docs/decisions/ + entry in
   docs/decisions/README.md + tag in docs/ROADMAP.md
4. Slice status flip → update phase file slice row + ROADMAP if relevant
5. Phase finished → archive note at top of phase file
6. Surprising engineering lesson → add a memory entry (one-line MEMORY.md
   pointer + a feedback_*.md file with rule + Why + How-to-apply)
7. NEVER run git commit / push. Propose Conventional Commits message in chat;
   user commits themselves.
```

## Detour Queue

Side missions that interrupt the main roadmap. Active detour is named in `context.md`. Detour files in `docs/detours/`.

**Rules:**

- A detour pauses the main roadmap; `context.md` records "Active detour: D-NNN".
- Each detour has: title, why, scope, exit criteria.
- Same DoD applies (per-slice DoD + 18-config sweep at close).
- When done: close the detour file. If it changed architecture → new ADR. The main roadmap then resumes.
- Detours that grow beyond their exit criteria become real phase slices — promote them, don't let them quietly take over.

## Session Expectations

- Keep compile warnings at **zero**.
- For graphics/platform slices, check runtime behavior + validation-layer output, not just compile/test success.
- If a smoke/example reveals a real runtime issue, fix it or document exactly why it is intentionally deferred (and file in `docs/debt.md`).
- Use `crd::gpu::ValidationCapture` to assert validation silence on every GPU test. It's the authoritative oracle for "did I drive Vulkan correctly?" (DX12: the debug-layer counter twin.)

## Documentation Conventions

The full doc-system map — every doc area, its purpose, its index file, and the doc-design
rules (two classes: *living/scannable* get size budgets, *append-only historical* records
don't) — lives in **`docs/README.md`** (the single home; not duplicated here). Quick frame:
`AGENTS.md` + `docs/BUILDING.md` = rules · `PRINCIPLES`/`SANITY` = compass · `context.md` = live
state · `ROADMAP` = hub · then `decisions/` (ADRs, indexed in its `README.md`) · `systems/`
· `phases/` · `sessions/` · `research/` · `debt.md` · `detours/` · `MEMORY.md`.

After a system ships, **prefer adding to its session log over rewriting its overview**.

## Platform

- Windows 11, PowerShell 7 primary.
- Use PowerShell-compatible commands. Avoid `cat`/`grep`/`sed`/`rm -rf`. Use `Get-Content`/`Select-String`/`Remove-Item -Recurse -Force` (cautiously).
- Running executables in PowerShell: use absolute path with `& "..."`. Relative paths sometimes fail in PS invocation contexts.

## Git Policy

- **Agents NEVER run `git commit` or `git push`.** The user commits themselves.
- Agents may freely run `git status`, `git log`, `git diff`, `git show`, `git branch`.
- Conventional Commits: `feat(<module>): ...`, `fix(<module>): ...`, `refactor(<module>): ...`, `docs(<module>): ...`, `test(<module>): ...`, `build: ...`, `ci: ...`.
- When an agent finishes, it proposes a commit message in chat. The user runs commit themselves.
- **NO AI co-author trailers — ever** (user direction 2026-07-02). Proposed commit messages must NOT contain
  `Co-Authored-By: Claude ...` or any AI attribution line; only humans appear in the contributors graph. This
  overrides any harness default that appends such a trailer.
- **Never skip hooks** (`--no-verify`) or bypass signing unless the user explicitly asks. If a hook fails, fix the underlying issue.
