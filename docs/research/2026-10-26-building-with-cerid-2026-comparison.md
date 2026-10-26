# Building with Cerid: languages, tooling, agents and practice against the 2026 state of the art

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists only in
> [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-10-26. **Status:** Recorded research, part 3 of 3. Every proposed row below is a proposal for the user,
not an accepted change. Nothing in [ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

How do agents and people build, debug, test, profile and ship with Cerid, how does that compare with the best 2026
languages, compilers, tools, agent protocols and engineering practice, and which missing pieces should become roadmap
rows? The bar is the user's standing order: the proper, full-feature, gold-standard implementation, never the "simple"
or "modern only" option, and an honest statement of what is missing.

- **Parts 1 and 2 are not repeated.** [Part 1](2026-10-26-state-of-the-art-2026-comparison.md) covers GPU, rendering,
  engine architecture, diagnostics tools, physics, ML, assets, UI, platforms and a first pass at agent tooling;
  [Part 2](2026-10-26-beyond-games-2026-comparison.md) covers audio, media, simulation, CAD, scientific computing, web,
  networking and robotics. This part links their findings (Slang as a shader peer, the MCP security pin, HarfBuzz,
  Wasm 3.0, WASI 0.3, the Jupyter gap, crash-reporting service, `KHR_gaussian_splatting`) and reuses their proposed IDs
  as dependencies, marked "(P1)" and "(P2)". It does not anchor-link a proposed ID, because no such row exists yet.
- **Cerid side.** Read, not run: [ROADMAP](../ROADMAP.md#master-table) rows for LANG, REFLECT, CMD, SCRIPT, DOC, EXT,
  AGENT, NATIVE, I2D, D7E, EDITOR, CHIR-SCI, NOTEBOOK, WEB, OPS, COLLAB, INTEROP, OWN, QUAL and DIAG; the
  [CEIR](../systems/ceir.md) and [CHIR](../systems/chir.md) references; ADR-0065 (sections 8 and 9), ADR-0081, ADR-0108,
  ADR-0111 to ADR-0118, ADR-0120, ADR-0121, ADR-0123, ADR-0128, ADR-0130 and ADR-0133; the
  [test instruments](../design/test-instruments.md), [public consumption](../design/public-consumption.md) and
  [CI tiers](../design/ci-tiers.md) contracts; `tools/ceridc` (2,291 lines), `engine/execution`, `assets/ceir`,
  `assets/chir`, `cmake/pins.json`, `.github/workflows`; AGENTS.md, START_HERE.md, CONTRIBUTING.md, MEMORY.md; the
  [user decisions session](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md). A grep hit or a miss
  is a source fact on 2026-10-26, not a runtime or quality claim. No build, test, benchmark or CI ran for this document.
- **Outside side.** Web pages read on 2026-10-26 (search results or fetched pages). Sources are listed at the end as
  C-numbers (Part 1 uses W, Part 2 uses B). **(K)** marks a reference given from prior knowledge and not re-read today;
  its current version or status is uncertain. "Uncertain" means the sources disagree or were not conclusive.
- **No downloads** were made. Items that can only be judged with a download are in [Needs a download](#needs-a-download).

## The principles every area is judged by

Taken from the user's standing order and the 2026-10-26 decisions; each area ends with how they apply.

- **(a) Runtime changeability.** Everything is authored in CEIR/CHIR/CKIR, edited live and swapped without restart or
  engine rebuild, under the staged commit, last-good generation and completion-driven retirement of
  [RAH-7](../ROADMAP.md#slice-rah-7) and the CEIR `ReloadSet` (ADR-0120). For tooling this means the language server,
  debugger, REPL and agent all act on the live generation, not on a separate offline copy.
- **(b) crd-hesap is the computational base**, with one owning module per capability inside `engine/`
  ([OWN.1](../ROADMAP.md#slice-own.1), [OWN.2](../ROADMAP.md#slice-own.2)); mods, plugins, user projects and scripts may
  implement anything. Tooling owns no numerics: a REPL's `A\b` is a hesap call, a perceptual image metric is one owned
  implementation used by every test.
- **(c) CHIR is the Cerid language** (MATLAB-class through [CHIR-SCI](../ROADMAP.md#slice-chir-sci)); commands,
  scripts, cheats, mods and every agent action go through it and one shared command, reflection and transaction model
  with MCP access ([CMD](../ROADMAP.md#slice-cmd), [REFLECT](../ROADMAP.md#slice-reflect), ADR-0081, ADR-0108).
- **(d) An interactive REPL follows the UI system.** ADR-0065 section 9 names `crd-hesap-repl` (inline REPL, notebook,
  plot routing, variable inspector); no roadmap row owns it. It connects to NOTEBOOK, CHIR-SCI and MCP.
- **(e) Wide hardware.** SSE2 x64 floor with runtime dispatch ([SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch)),
  older-GPU fallbacks; tooling and agents run on the same floor, and any code generator targets every dispatch level.
- **(f) Budgets from peers.** Beat the best peer; parity only when no lever remains, with that evidence recorded.
  Peers here include rust-analyzer and clangd (editor latency), LuaJIT and V8 (execution tiers), MATLAB, Julia and
  IPython (REPL latency), the Unreal/Unity/Blender MCP servers (agent task success), and Unity/Godot (time to first
  frame for a new user).

## Highest-impact gaps first

Ranked by how much later work each gap blocks, how far Cerid is from the 2026 reference, and principles (a) to (f).
Details and placement are in [Gaps and proposed roadmap changes](#gaps-and-proposed-roadmap-changes).

| Rank | Gap | Cerid today | Proposed owner |
|---:|---|---|---|
| 1 | No pinned CHIR language specification: type system, generics, modules, error model, editions and a conformance suite | CHIR-0 is five constructs (ADR-0128 D1); LANG.1-LANG.5 are one-line rows; ADR-0128's deferral ledger defers generics, ADTs, modules and errors to "first consumer" | New LANG.0; amend LANG.1-LANG.5 |
| 2 | No language tooling: no query-based semantic service, LSP server, DAP adapter, formatter, linter, refactoring, tree-sitter grammar or editor extension | Missing (no roadmap hit for LSP, DAP, tree-sitter, formatter); I2D-9.d plans an in-editor code editor only; DIAG.8b inspection and DIAG.9 replay exist as engines a DAP adapter can use | New LANGTOOL.1-LANGTOOL.6; amend I2D-9.d |
| 3 | MCP surface is four revisions behind and tools-only | `ceridc` answers `protocolVersion` `2024-11-05` with capabilities `{tools}` only (`tools/ceridc/src/mcp.cpp`); current revision is `2026-07-28` [C01](#c01) | New MCP-ADAPT.0 (now) and MCP-ADAPT; amend CMD.4, AGENT.1-AGENT.3 |
| 4 | No REPL row | ADR-0065 section 9 only; NOTEBOOK.1 and CHIR-SCI.4 own kernel, interrupt and debugger pieces | New REPL.0-REPL.4; amend NOTEBOOK.1, CHIR-SCI |
| 5 | No agent-facing documentation or example corpus: no "how to write CEIR/CHIR/CKIR" guide, no llms.txt, no skills, no MCP prompts/resources | Repository AGENTS.md serves contributors; 27 generated dialect references (`*.ops.md`) exist but are not served to agents | New AGENT-DOCS |
| 6 | No agent task benchmark and no measured agent safety suite | AGENT.3 names a hostile-input corpus; no task suite; 2026 peers publish Godot-based agent benchmarks [C14](#c14) | New AGENT-BENCH; amend AGENT.3, AGENT |
| 7 | Host CEIR has only an interpreter and a dense compiled plan; no native baseline or optimizing tier, no AOT for JIT-forbidden targets | ADR-0122/0123 two tiers; GPU through CKIR emitters; no host machine-code generation in source | New CEIR-JIT.0-CEIR-JIT.3 |
| 8 | No random program generators for differential testing of CEIR, CKIR and CHIR compilers | Corpus differentials (ADR-0123), bounded loader fuzzing (REPO.DEV.9, DIAG.10a); no Csmith/YARPGen-class generator or reducer | New IR-FUZZ |
| 9 | No perceptual image oracle for golden tests | No FLIP, SSIM or ColorVideoVDP in source (grep); renderer rows require "golden" without a metric | New IMG-ORACLE |
| 10 | No SBOM, provenance attestation, code signing, notarization or reproducible-build check; EU CRA reporting duties started 2026-09-11 [C25](#c25) | `pins.json` holds version, SHA-256 and licence per input; actions pinned by commit; OPS.2 (Later, Order 858) names "reproducible supply-chain artifacts" | New SUPPLY.1-SUPPLY.4, SEC.1; amend OPS.2 |
| 11 | No API/ABI stability, semver, deprecation or language-edition policy | SCRIPT.2 and NATIVE.1 name a versioned native ABI; CEIR ops carry a version field; no policy row | New API-STAB.0 |
| 12 | No user-facing package format, registry, dependency resolver or marketplace | EXT.2 names install/remove; no manifest, lockfile, signing or registry | New EXT.4, EXT.5 |
| 13 | No user-project system, templates, samples or tutorial track; no "first hour" acceptance | EDITOR.6/EDITOR.7 export; assets live inside the engine repository | New DX.1-DX.4 |
| 14 | No version-control integration for large content and no semantic diff/merge of CEIR/CHIR/scene files | D7E-0 names diff/merge for graphs; no VCS adapter, locking or LFS/P4 support | New VCS.1, VCS.2; amend DOC.2 |
| 15 | Localization and game accessibility beyond widgets: CLDR data, MessageFormat 2, XLIFF exchange, Xbox Accessibility Guidelines-class game features | I2D-6.d (formats, plurals, pseudo-locales, RTL) and I2D-8.e (UI Automation, AT-SPI) planned | New I18N.1, A11Y-GAME; amend I2D-8.e |

Next in impact, with rows below: CKIR modules, generics, autodiff and a tile-level dialect (CKIR-LANG.1-CKIR-LANG.3);
property-based and mutation testing (TEST-METH); TLA+ models of transactional protocols (FORMAL.1); DCC live links
(INTEROP.3); native C++ patching peers (NATIVE.2).

Structural note, not ranked: Cerid's agent contract is ahead of every peer in design. ADR-0081 makes the typed command
surface the source of truth, DIAG.8c already separates `read`, `record`, `inject` and `execute` authority granted at
process start, and every `ceridc` verb validates completely before its first side effect. The two shipped peer
servers found today are an experimental HTTP server with no authentication that runs serially on the game thread
(Unreal 5.8, per a secondary guide [C08](#c08)) and an add-on that runs model-generated Python "without any guards"
(Blender Lab, official page [C07](#c07)). Cerid's gap is breadth and currency of the surface, not its safety model.

## 1. Languages, compilers and IR

### State of the art 2026

- **IR design.** MLIR's dialect system (K) is the reference for extensible IR; Cerid's CEIR already follows it
  (ADR-0111 open-world types, ADR-0112 attributes, ADR-0113 effects, ADR-0115 interfaces, ADR-0117 pass manager,
  rewrite/conversion and a diagnostic engine with stable codes and fix-its). LLVM 23.1.0 is the current LLVM line with
  ORC as its JIT [C18](#c18). Cranelift (Wasmtime's optimizing backend) and Winch (its baseline compiler) are the
  reference for small, fast, verifiable code generators (K). GraalVM Truffle derives compilers from interpreters by
  partial evaluation (K). The WebAssembly component model with WASI 0.3 native async was ratified on 2026-06-11
  ([P2 B39](2026-10-26-beyond-games-2026-comparison.md#b39)).
- **Tiered execution.** V8 runs four tiers (Ignition, Sparkplug, Maglev, TurboFan) (K); .NET uses tiered compilation
  with dynamic PGO (K); LuaJIT a trace JIT (K). CPython's copy-and-patch JIT (LLVM-built templates stitched at run time)
  reached 8-13% geometric-mean gains in 3.15 alphas, but the Steering Council required a standards-track PEP; PEP 836
  is a draft targeting 3.16 [C17](#c17). Copy-and-patch (Xu and Kjolstad, OOPSLA 2021) (K) is the cheapest route to a
  baseline native tier from an existing interpreter. Platforms that forbid writable-executable memory (consoles, iOS,
  some browsers' CSP) need AOT (K).
- **Incremental, query-based compilers.** rustc's query system and salsa (used by rust-analyzer) and Roslyn's
  immutable syntax trees with incremental semantic models (K) let one frontend serve compiler, IDE and refactoring with
  the same answers. Cerid's ADR-0118 `IncrementalDag` is the same class of engine (content-addressed memoization with
  dependency-driven dirty propagation) but serves cook and analysis, not a language frontend.
- **Hot reload and live coding.** Unity is removing domain reload: 6.6 defaults new projects to scene-only reload on
  play, and its June 2026 CoreCLR update says traditional domain reloads go away entirely with the CoreCLR editor; the
  Unite Seoul 2026 keynote moves this to Unity 7 (naming conflicts between sources) [C19](#c19). Live++ and Unreal Live
  Coding patch native functions in place (K); .NET Hot Reload applies metadata deltas (K); Erlang/OTP swaps modules with
  two live versions and explicit code-change callbacks (K); Smalltalk/Pharo edit the running image (K).
- **Type systems for scripting.** Gradual typing (TypeScript, Python typing, Luau's type solver) (K); algebraic effects
  and handlers (OCaml 5, Koka) (K); ownership without a full borrow checker (Swift noncopyable types, Hylo mutable value
  semantics, Mojo origins) (K). Mojo reached 1.0 on 2026-08-11 and its compiler was open-sourced on 2026-08-18
  [C16](#c16).
- **Shader and GPU kernel languages.** Slang (Khronos since Nov 2024): modules, generics and interfaces, automatic
  differentiation, many targets ([P1 W21](2026-10-26-state-of-the-art-2026-comparison.md#w21)). CUDA 13.1 (Dec 2025)
  introduced CUDA Tile: a tile virtual ISA (CUDA Tile IR) and the cuTile Python DSL where the compiler maps tiles to
  threads [C15](#c15). Triton's block-level programming (K) and Mojo's kernel library [C16](#c16) are the other tile
  references.

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Extensible typed IR with regions, effects, interfaces | MLIR (K) | Has: CEIR (ADR-0109, ADR-0111-ADR-0117); 28 dialect definitions in `engine/execution/ceir/ops` |
| Verifier, pass manager, rewrite, structured diagnostics with fix-its | MLIR, LLVM (K) | Has: ADR-0117 (`diagnostic.hpp` declares `FixIt`); consumers limited to the CEIR bands |
| Reference interpreter plus compiled tier, differential-proven | V8/.NET tiering (K) | Has: interpreter (ADR-0122) and dense compiled plan (ADR-0123) proven byte-identical on a corpus |
| Baseline native JIT, optimizing JIT with profiles, OSR and deoptimization | V8, .NET, LuaJIT (K); CPython copy-and-patch [C17](#c17) | Missing for host CEIR (no machine-code emitter in source) |
| AOT for JIT-forbidden targets | Unity IL2CPP, .NET Native AOT (K) | Missing; LANG.5 says "cook/package" without a native-code form |
| GPU lowering | Slang, Triton, CUDA Tile [C15](#c15) | Has: CKIR emitters for GLSL, HLSL, MSL, WGSL, CUDA, HIP (Part 1) |
| Query-based incremental frontend shared with the IDE | salsa, Roslyn (K) | Partial: `IncrementalDag` (ADR-0118) for cook and analyses; CHIR frontend is not query-based |
| Hot reload with state migration | Erlang, Live++, Unity CoreCLR plan [C19](#c19) | Has for CEIR: `ReloadSet` with atomic install, last-good and migration (ADR-0120); native reload planned in SCRIPT.3, NATIVE.2; renderer granularity in RAH-7 |
| Language: generics, ADTs/pattern match, modules, closures, async, error model | Rust, Swift, Mojo [C16](#c16) | Planned and deferred: ADR-0128 deferral ledger; [LANG.1](../ROADMAP.md#slice-lang.1)-[LANG.5](../ROADMAP.md#slice-lang.5) |
| Ownership model | Swift, Hylo, Mojo (K) | Decided: ADR-0128 D4 composite (values, generational handles, arenas, state stores, light borrow); full borrow checker rejected |
| Effects in the type system | Koka, OCaml 5 (K) | Partial: CEIR effect families and open effect locations (ADR-0113); no CHIR effect syntax |
| Gradual or dynamic boundary for interactive use | TypeScript, Luau, Julia (K) | Missing as a decision; a REPL and a MATLAB-class notebook need one |
| Units of measure in types | F# (K) | Has in CEIR (`!qty<!i32,L1>` in `assets/ceir/inspect_demo.ceir`; ADR-0078) |
| Capability-based security for programs | (Cerid-specific) | Decided: ADR-0108 section 3, ADR-0116; enforcement named forward |
| Shader modules, generics, interfaces | Slang (K) | Missing in CKIR (no source evidence; uncertain) |
| Shader automatic differentiation | Slang autodiff (K) | Missing in CKIR; hesap-autodiff is CPU (ADR-0097) |
| Tile-level kernel dialect | CUDA Tile IR, cuTile, Triton [C15](#c15) | Missing; v17 tensor-core GEMM is hand-written CKIR (Part 1) |
| Language specification and conformance suite | Rust reference, WebAssembly spec tests (K) | Missing: ADR-0128 and the CEIR-0e note are design records, not a reference |

### Gaps in area 1

- **The language is the largest missing piece.** CHIR-0 proves lowering, identity and reload for five constructs. Every
  later surface in this document (LSP, DAP, REPL, agent guides, benchmarks) needs a pinned language: grammar, static
  semantics, a type system with local inference, generics whose instantiation strategy survives hot reload (a
  monomorphized instance must be retired and re-instantiated under the ReloadSet; dictionary passing for interactive
  tiers), ADTs and pattern matching over the reserved `core.match`, modules and packages, closures, structured async and
  cancellation, the Result/Option error model, CHIR effect syntax mapped onto ADR-0113 effect families, editions for
  breaking changes, and a numbered conformance suite. ADR-0128's "trigger = first consumer" deferral fits a prototype;
  a full-feature language needs the whole design accepted before LANG.1 code, so that each LANG row implements a fixed
  target rather than growing one.
- **A gradual boundary for interactive tiers.** MATLAB and Julia users type untyped code at a prompt. CHIR should keep
  static types for assets and allow an explicit dynamic type (checked at run time, with the check visible to the
  optimizer) in the REPL, notebook and cheat console, never in shipping hot paths.
- **No native execution tier.** The interpreter and compiled plan are correct and measured against hand dispatch on GPU
  work (CEIR-35 boards), but CPU behaviour code, notebook loops and MATLAB-class scalar code need native code to meet
  peer budgets (LuaJIT, Julia, MATLAB's execution engine). ADR-0108 forbids an external IR as internal semantics; it
  does not forbid an external code generator below CEIR. The decision must still lead with the owned option:
  an owned copy-and-patch baseline over the existing plan, then an owned optimizing tier on ADR-0117's pass manager,
  with per-ISA code for every SIMD-DISPATCH level; Cranelift and LLVM ORC are measured peers and fallbacks for the
  decision, not assumed. The web target uses Part 2's WEB-CEIR (P2) Wasm emission; AOT object files serve consoles,
  iOS and locked-down deployments.
- **CKIR lags Slang** in modules, generics, interfaces and autodiff, and has no tile-level dialect while CUDA Tile,
  Triton and Mojo make tiles the 2026 abstraction for tensor-core kernels.
- **Principles.** (a) Every tier must accept a new generation between calls: compiled code is keyed by program generation
  and retired after in-flight frames complete, exactly like GPU programs. (b) No numerics in the compiler; constant
  folding of math calls uses hesap. (c) CHIR stays the one language; CEIR text stays directly authorable. (e) The JIT
  emits SSE2 to AVX-512/AVX10 and NEON/SVE code through SIMD-DISPATCH's levels and has an interpreter fallback.
  (f) Peers: LuaJIT and Luau (behaviour scripts), Julia and MATLAB (numeric loops), V8 (startup and warm-up), and the
  native C++ equivalent of each benchmark.

## 2. Language tooling

### State of the art 2026

- **LSP 3.18** is the current release; 3.19 is upcoming. New in 3.18: inline completions, snippet text edits, markup in
  diagnostics, relative patterns, workspace edit metadata, command tooltips [C20](#c20). Earlier versions brought
  semantic tokens, inlay hints, type hierarchy, pull diagnostics and notebook documents (K).
- **DAP** 1.71 adds integer formats and console modes; 1.68 added `locations`; the protocol has supported reverse
  execution (`stepBack`, `reverseContinue`), data breakpoints, `readMemory` and `disassemble` for years [C21](#c21).
  rr and WinDbg TTD give record-replay debugging (K; Cerid's DIAG.9c qualified them where hosts allow).
- **REPLs and interactive environments.** The MATLAB command window and Live Editor (R2026a,
  [P2 B34](2026-10-26-beyond-games-2026-comparison.md#b34)); Julia's REPL with Revise.jl live code tracking (K);
  IPython and the Jupyter kernel messaging protocol, which any language can implement (K); Mathematica notebooks (K);
  Pharo's live image with inspectors and a debugger that edits code while stopped (K).
- **Formatters, linters, refactoring and docs.** gofmt/rustfmt/clang-format as canonical formatters; clippy and
  clang-tidy as linters with machine-applicable fixes; rename and extract refactorings served through LSP (K).
- **Grammars and editors.** Tree-sitter incremental grammars power highlighting and structural navigation in many
  editors (K); VS Code, Visual Studio and JetBrains IDEs consume LSP and DAP (K).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Semantic service (parse, resolve, type, diagnostics) shared by compiler, IDE, agent | rust-analyzer, Roslyn (K) | Missing as a module; CHIR parser and lowering exist (`engine/execution/chir`) |
| LSP server for CHIR, CEIR, CKIR | LSP 3.18 [C20](#c20) | Missing (no roadmap hit) |
| Diagnostics with authored source positions | Any modern compiler | Has: DIAG.8a provenance (CHIR line:col through CSE and binary round trip); ADR-0117 stable codes and fix-its |
| Breakpoints, stepping, typed values | DAP [C21](#c21) | Has as an engine: `crd/ceir/inspect.hpp`, `ceridc inspect`, `program.inspect` (DIAG.8b); no DAP adapter |
| Reverse stepping and replay | DAP `stepBack`; rr/TTD (K) | Partial: run records and cross-process replay with first-divergence reports (DIAG.9a); no reverse stepping over them |
| GPU shader source-level debugging | PIX, RenderDoc with shader debug info (K) | Missing for CKIR source (debug names and markers exist, Part 1) |
| Interactive REPL | MATLAB, Julia, IPython (K) | Missing (no row; ADR-0065 section 9 only) |
| Notebook | Jupyter, Pluto, MATLAB Live Editor | Planned: [NOTEBOOK.1](../ROADMAP.md#slice-notebook.1)-[NOTEBOOK.5](../ROADMAP.md#slice-notebook.5); no Jupyter protocol (Part 2) |
| Formatter | gofmt, rustfmt (K) | Partial: CHIR and CEIR printers round-trip text (canonical print), not a style-preserving formatter |
| Linter with fixes | clippy, clang-tidy (K) | Partial: verifier and `FixIt` support; no lint rule set |
| Refactoring | rename, extract (K) | Missing; stable semantic identity (ADR-0114) is a strong base for rename |
| Generated reference docs | rustdoc, Doxygen (K) | Partial: 27 generated dialect references (`*.ops.md`, drift-tested); no CHIR library docs |
| Tree-sitter grammars, editor extensions | (K) | Missing |
| In-editor code editor | VS Code, JetBrains | Planned: [I2D-9.d](../ROADMAP.md#slice-i2d-9.d) (diagnostics, source mapping, completion, live preview) |
| Graph editors over the same source | Blueprints, Houdini networks (K) | Planned: [D7E-0](../ROADMAP.md#slice-d7e-0) and [I2D-9.a](../ROADMAP.md#slice-i2d-9.a); CHIR text and graph are one model (ADR-0128 D5) |

### Gaps in area 2

- **One semantic service, many clients.** I2D-9.d, a VS Code extension, an MCP tool and the REPL must ask one
  query-based service (built on `IncrementalDag`) for diagnostics, hover, completion and references, so they never
  disagree. Without it, I2D-9.d would grow a private analyzer (a (d) violation).
- **DAP over existing engines.** DIAG.8b and DIAG.9 already provide breakpoints, stepping, typed values, recorded inputs
  and first-divergence replay. A DAP adapter makes them usable from VS Code, Visual Studio and CR-D007, and reverse
  stepping can be implemented by deterministic re-execution of a run record to step N-1 (the replay contract already
  guarantees identical traces under observation).
- **CKIR source on the GPU.** Emit SPIR-V `NonSemantic.Shader.DebugInfo.100` and DXIL debug information that map back to
  CKIR source lines, so the PIX and RenderDoc shader debuggers step CKIR, not generated GLSL/HLSL (K for the formats).
- **REPL.** Covered in area-specific proposals below; it must be one kernel shared with the notebook and MCP.
- **Formatter, linter, refactoring, docs and grammars** for all three languages, each a client of the semantic service.
- **Principles.** (a) Language server and debugger attach to the running process's live generation; an edit applied from
  the editor goes through the same ReloadSet as a file save. (c) The same service answers human and agent queries.
  (d) The REPL rows follow the UI rows (Order 240-363). (f) Peers: rust-analyzer and clangd for edit-to-diagnostic
  latency and memory on large workspaces; MATLAB, Julia and IPython for REPL round trip.

## 3. Agent-native engine design

### State of the art 2026

- **MCP revisions.** Current revision `2026-07-28` [C01](#c01): stateless protocol (no `initialize` handshake; version,
  client capabilities and identity in every request's `_meta`), mandatory `server/discover`, no protocol sessions,
  `subscriptions/listen` for change notifications, multi round-trip requests (`input_required` results replace
  server-initiated elicitation, sampling and roots requests), `resultType` on every result, tasks moved to the official
  extension `io.modelcontextprotocol/tasks` (polling with `tasks/get`, `tasks/update`), cacheable list results
  (`ttlMs`, `cacheScope`), deterministic `tools/list` order recommended, OpenTelemetry trace context in `_meta`, full
  JSON Schema 2020-12 for `inputSchema`/`outputSchema`; Roots, Sampling and Logging deprecated; Client ID Metadata
  Documents preferred over dynamic client registration; a twelve-month deprecation policy [C02](#c02).
  `2025-11-25` added icons, URL-mode elicitation, incremental OAuth scope consent, OpenID Connect discovery, experimental
  tasks, tool-name guidance, and returning input validation errors as tool execution errors so models can self-correct
  [C03](#c03). `2025-06-18` added structured tool output (`structuredContent`, `outputSchema`), elicitation, resource
  links, servers as OAuth resource servers with RFC 8707 resource indicators, and removed JSON-RPC batching
  [C04](#c04). `2025-03-26` introduced OAuth 2.1 authorization, Streamable HTTP and tool annotations (K).
- **MCP Apps** (SEP-1865) is the first official extension: `ui://` HTML resources linked to tools with two-way
  messaging, stable since early 2026 (date from a secondary source) [C06](#c06).
- **MCP Registry** is live at registry.modelcontextprotocol.io with `server.json` entries under verified reverse-DNS
  namespaces; its API was frozen at v0.1 on 2025-10-24 and was still a preview in mid-2026 (secondary sources, server
  counts disagree) [C05](#c05).
- **Engines and DCCs.** Unreal 5.8 ships an experimental MCP server plugin: a toolset registry generates schemas,
  meta-tools (`list_toolsets`, `describe_toolset`, `call_tool`), Streamable HTTP on loopback with no authentication,
  serial execution on the game thread, no built-in dry run, and tool calls not wrapped in editor transactions (secondary
  guide, July 2026) [C08](#c08), with Part 1's [W29](2026-10-26-state-of-the-art-2026-comparison.md#w29). Unity ships MCP
  in the Unity AI beta ([P1 W32](2026-10-26-state-of-the-art-2026-comparison.md#w32)). Blender Lab's official MCP server
  (add-on 1.0.3, Blender 5.1+) turns requests into Python that runs "without any guards", and the page recommends a VM
  [C07](#c07). Godot and Houdini have only community servers (one Houdini server lists 131 tools; a SideFX forum
  question about APEX Script MCP support is unanswered) [C09](#c09).
- **Agent instructions and docs.** AGENTS.md became an open format under the Linux Foundation's Agentic AI Foundation
  (founded 2025-12-09 with MCP and goose) [C10](#c10); Agent Skills define `SKILL.md` folders with progressive
  disclosure [C11](#c11); llms.txt [C12](#c12) is published by the MCP documentation site itself (observed in fetched pages
  [C01](#c01)).
- **Agent evaluation.** GameDevBench (arXiv 2602.11103): 333 Godot tasks from 88 tutorials, about three times the code
  changes of SWE-bench tasks, checked by engine tests [C14](#c14); GameCraft-Bench (140 Godot 4 tasks, best agent about
  41%) and SWE-Game (247 Godot tasks, executable checks agreed with human labels more than a video judge) are reported
  by secondary sources [C14](#c14).
- **Agent safety.** OWASP Top 10 for Agentic Applications 2026 (published 2025-12-09) lists tool misuse, memory and
  context poisoning among ten risks (secondary sources) [C13](#c13); MCP security best practices
  ([P1 W63](2026-10-26-state-of-the-art-2026-comparison.md#w63)).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Typed command registry as source of truth for GUI, CLI, MCP | ADR-0081; Unreal toolset registry [C08](#c08) | Planned: [CMD.1](../ROADMAP.md#slice-cmd.1)-[CMD.4](../ROADMAP.md#slice-cmd.4), [AGENT.1](../ROADMAP.md#slice-agent.1); today 9 hand-written `ceridc` verbs |
| MCP protocol revision | `2026-07-28` [C01](#c01) | Behind: `2024-11-05`, initialize handshake, `ping` (`mcp.cpp`) |
| Structured output with `outputSchema` | `2025-06-18` [C04](#c04) | Missing: verbs return JSON text reports, not `structuredContent` |
| Resources (scene, asset, program, capture by URI) | MCP resources | Missing (ADR-0081 section 6.4 requires them) |
| Prompts (authored workflows) | MCP prompts | Missing (ADR-0081 section 6.4 requires them) |
| Long-running work with progress and cancel | Tasks extension [C02](#c02) | Partial: bounded stops, step budgets and cancel inside verbs; `notifications/cancelled` owned by AGENT.2 |
| Confirmation and user input mid-call | MRTR `input_required` [C02](#c02) | Missing; dry run exists per verb (`instantiate --dry-run`) |
| Authorization for remote use | OAuth 2.1, RFC 8707/9728, CIMD [C02](#c02) [C03](#c03) | Planned: [AGENT.3](../ROADMAP.md#slice-agent.3) (principal, token audience); stdio only today |
| Authority classes separated (inspect, record, inject, execute) | Rare among peers | Has: DIAG.8c, granted by the host at start, never by a request |
| Transactional edits and dry run | Unreal: none built in [C08](#c08) | Has per verb (validate fully before first side effect); full transactions in [CMD.3](../ROADMAP.md#slice-cmd.3) |
| Deterministic replay of agent actions | (Cerid-specific) | Has for programs (DIAG.9a); for commands planned in CMD.4 |
| Introspection and reflection | Unreal schema generation [C08](#c08) | Planned: [REFLECT.1](../ROADMAP.md#slice-reflect.1)-[REFLECT.3](../ROADMAP.md#slice-reflect.3), [AGENT.1](../ROADMAP.md#slice-agent.1) |
| Sandboxed workspace, checkpoints, provenance | Unreal guide recommends a disposable branch [C08](#c08) | Planned: [AGENT.2](../ROADMAP.md#slice-agent.2) |
| Budgets and cancellation | Tasks extension | Partial: `max_stops`, step budgets; resource budgets in AGENT.3 |
| Agent docs: llms.txt, skills, language guides | [C10](#c10) [C11](#c11) | Missing for users; contributor AGENTS.md exists |
| UI returned to the agent host | MCP Apps [C06](#c06) | Missing |
| Registry listing | MCP Registry [C05](#c05) | Missing |
| Agent task benchmark | GameDevBench [C14](#c14) | Missing |
| Prompt-injection and tool-poisoning defences | OWASP agentic list [C13](#c13) | Planned in part: AGENT.3 hostile-input corpus; content provenance and description integrity not named |
| Agent edits a real asset and restores state | (Cerid-specific) | Planned: [AGENT.4](../ROADMAP.md#slice-agent.4), [AGENT.5](../ROADMAP.md#slice-agent.5) |

### Gaps in area 3

- **Currency.** The shipped MCP loop is four revisions behind. Clients still accept handshake-based servers through the
  backward-compatibility rules [C01](#c01), but structured output, resources, prompts, tasks and MRTR are the features
  agents now rely on. The diagnostic host is a shipped consumer, so bringing it to `2026-07-28` (with `2025-11-25`
  handshake compatibility) should not wait for CMD.4.
- **Generated, not hand-written.** Tool schemas, `outputSchema`, resources and prompts must be generated from the
  REFLECT/CMD schemas (ADR-0130 item 1), so a new command appears on every surface with one declaration. Tool names
  follow the 2025-11-25 guidance; `tools/list` order is deterministic.
- **Authority mapped onto MCP.** DIAG.8c's grants become per-tool authority classes; side effects above a threshold
  return `input_required` and need a human principal's confirmation; an agent never raises its own grant. Remote
  transports use the spec's OAuth profile with audience-bound tokens (AGENT.3).
- **Content is data.** Project content (asset names, CHIR comments, plugin descriptions, logs) reaches the model inside
  tool results. Results must tag untrusted spans with provenance, tool descriptions must come only from reviewed,
  signed schemas (no free text from a plugin at run time), and plugin tools live in their own namespace with their own
  grant. This is the engine's defence against prompt injection and tool-description poisoning.
- **Docs for agents.** A "how to write CEIR, CHIR and CKIR" guide with CI-verified examples, generated llms.txt, skills
  for the common workflows and MCP prompts/resources serving them. Today an agent learns CEIR only from repository
  assets and the dialect references.
- **Evaluation.** No suite measures whether an agent can build, debug, profile and ship with Cerid. Peers now publish
  Godot-based suites; Cerid needs its own across its domains, with safety tasks.
- **Principles.** (a) An agent's edit installs live under the ReloadSet and RAH-7 and is undone through production
  commands (AGENT.4). (c) Every agent action is a CHIR-reachable command; the REPL tool and MCP tools are the same
  commands. (e) The MCP server runs on the SSE2 floor and on headless hosts without a GPU (capabilities report absence).
  (f) Peers: the Unreal, Unity and Blender servers on the same tasks, scored by task success, wall time, tool calls and
  tokens.

## 4. Developer experience

### State of the art 2026

- **Onboarding, templates and samples.** Unity Hub templates and learn courses, Unreal templates and sample projects,
  Godot's demo projects (K); first-run time to a running sample is the comparison point.
- **Package managers and marketplaces.** vcpkg manifests with versioning and binary caching; Conan 2 lockfiles (K);
  Cargo/crates.io with semver resolution and the PubGrub-class solver used by uv (K); Unity Package Manager with scoped
  registries (K); the Godot Foundation's official Asset Store is live, with the old Asset Library being deprecated and
  paid assets planned (coverage to May 2026) [C22](#c22); Epic's Fab (K).
- **API/ABI stability.** Semantic versioning, Rust editions, protobuf editions (Edition 2026 planned for the 36.x line)
  [C23](#c23), C ABI shims for plugins (K); MCP's own twelve-month deprecation policy [C02](#c02).
- **User-project builds.** Unreal Build Tool and project files, Unity's assembly definitions, Godot export templates (K).
- **Crash reporting for users.** Sentry and Crashpad (Part 1 [W55](2026-10-26-state-of-the-art-2026-comparison.md#w55);
  Part 1 proposes an OPS.1 amendment).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| New-project generator and templates | Unity, Unreal, Godot (K) | Missing |
| Sample projects as CI-tested downstream consumers | (K) | Partial: `tests/support/package` consumer test and the sandbox; no domain samples |
| Tutorials with executed snippets | Rust book, doc tests (K) | Missing |
| Package format, manifest, lockfile, signing | vcpkg, Cargo, UPM (K) | Missing; [EXT.2](../ROADMAP.md#slice-ext.2) names registry, install and remove |
| Public registry and marketplace | Godot Asset Store [C22](#c22), Fab (K) | Missing |
| Semver and deprecation policy | [C02](#c02) [C23](#c23) | Missing as a policy; CEIR ops carry a `Version` field (generated docs) |
| Native plugin ABI | C ABI shims (K) | Planned: [SCRIPT.2](../ROADMAP.md#slice-script.2), [NATIVE.1](../ROADMAP.md#slice-native.1) |
| Relocatable SDK package | CMake packages (K) | Has: REPO.DEV.8 installable, relocatable package with a consumer test |
| User-project build and export | UBT, Godot export (K) | Planned: [EDITOR.6](../ROADMAP.md#slice-editor.6), [EDITOR.7](../ROADMAP.md#slice-editor.7) |
| Crash reporting for users | Sentry (P1) | Local bundles (DIAG.5); service in [OPS.1](../ROADMAP.md#slice-ops.1) (Later; Part 1 proposes moving it before QUAL.2) |

### Gaps in area 4

- **No user project.** Everything authored today lives in the engine repository (`assets/ceir`, `app://` overrides in the
  sandbox). A project manifest with module selection, dependencies, lockfile and per-target settings, and
  `ceridc new/build/run/test/package` verbs (the same commands in CR-D007 and over MCP), are missing.
- **No package ecosystem.** Plugins, CHIR libraries, CKIR kernels and asset packs need one signed package format with
  semver, a resolver, lockfiles, a local and a hosted registry, and later a marketplace with reviews and paid listings.
- **No stability policy.** Without semver per module, CEIR dialect/op versioning rules, CHIR editions and a deprecation
  window, no third party can depend on Cerid.
- **Learning path.** Templates per domain (game, engineering viewer, DAW plugin, notebook, web app, command-line tool),
  samples that CI builds as downstream projects, and a tutorial track whose every snippet runs in CI.
- **Principles.** (a) Packages install, update and roll back live through EXT.2 generation-safe deregistration.
  (b) Packages may implement anything; the engine's own packages obey OWN. (c) Package and project commands are CHIR
  commands. (e) Package metadata declares ISA and GPU tiers with fallbacks. (f) Peers: Unity, Godot and Unreal for time
  from install to first running template; Cargo and vcpkg for resolve and install time.

## 5. Data, interchange and versioning

### State of the art 2026

- **Serialization and schema evolution.** Protobuf editions (2023, 2024; Edition 2026 planned in Q3 2026 with stricter
  defaults and enforced limits) [C23](#c23); FlatBuffers and Cap'n Proto zero-copy formats with field-addition rules
  (K); Bebop (K). Unknown-field preservation and N-1 readers are the common rules (K).
- **Large-content version control.** Perforce P4 for large binaries with locking; Git with LFS; Unity Version Control
  (formerly Plastic SCM); cloud-native newcomers such as Diversion; Git front ends for artists such as Anchorpoint with
  file locking (vendor sources) [C24](#c24). No primary source was found for a tool named "Lore" (uncertain).
- **DCC interchange.** OpenUSD with AOUSD Core Specification 1.0 ([P1 W58](2026-10-26-state-of-the-art-2026-comparison.md#w58)),
  glTF 2.0 and its extensions, FBX through Autodesk's closed SDK or the MIT `ufbx` reader, Alembic for caches (K);
  live links (Unreal Live Link, Omniverse connectors) (K).
- **Asset databases and dependency graphs.** Unreal's derived data cache and asset registry, Unity's asset database
  with GUIDs (K).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Stable asset identity | Unity GUIDs (K) | Has: UUID scheme (ADR-0037), stable semantic identity (ADR-0114) |
| Cooked container with versions | (K) | Has: CRDR (ADR-0038); RAH-0 accepted re-cooking under new format versions |
| Incremental dependency graph | Unreal DDC (K) | Has: `IncrementalDag` (ADR-0118); dependency browser in [EDITOR.1](../ROADMAP.md#slice-editor.1) |
| Document identity, dirty state, autosave, migration | (K) | Planned: [DOC.1](../ROADMAP.md#slice-doc.1)-[DOC.3](../ROADMAP.md#slice-doc.3) |
| One schema-evolution contract across TOML, CRDR, CEIR binary, bundles | protobuf editions [C23](#c23) | Partial: per-format versions; N-1 bundle reader in [OPS.2](../ROADMAP.md#slice-ops.2); no shared rule set |
| Version control adapters and locking | P4, LFS, Unity VC [C24](#c24) | Missing (no `.gitattributes` LFS rules, no adapter) |
| Semantic diff and merge of CEIR/CHIR/scene | (rare in peers) | Planned for graphs in [D7E-0](../ROADMAP.md#slice-d7e-0); text formats line-based today |
| Shared cook cache | Unreal shared DDC (K) | Partial: local cook cache; distributed cook in [OPS.3](../ROADMAP.md#slice-ops.3) (Later) |
| USD, glTF, FBX, Alembic interchange | (K) | glTF import (ADR-0043); rest Later in [INTEROP.1](../ROADMAP.md#slice-interop.1) |
| DCC live link | Live Link, Omniverse (K) | Missing |

### Gaps in area 5

- **One evolution rule set** for every persisted format: tagged fields, unknown-field preservation, required N-1 and
  N-2 readers, editions for breaking changes, and migration functions registered in REFLECT (state migration already
  exists for CEIR in ADR-0120).
- **Version control.** A VCS-neutral adapter (Git with LFS, P4, Unity Version Control, Diversion), file locking for
  unmergeable binaries, and semantic diff/merge drivers for CEIR, CHIR and scene text built on stable identity (ADR-0114)
  so a merge is resolved by node, not by line.
- **Interchange.** FBX needs an explicit decision (own reader with `ufbx` as oracle, versus the closed SDK); live links to
  Blender, Maya and Houdini are absent.
- **Principles.** (a) A pulled change or a merge installs live as a new generation. (c) VCS operations are commands. (f)
  Peers: P4 and Git LFS for large-repository operations; Unreal DDC for shared cook hit rates.

## 6. Security and supply chain

### State of the art 2026

- **Plugin isolation.** WASI 0.3 components ([P2 B39](2026-10-26-beyond-games-2026-comparison.md#b39)); process
  isolation with AppContainer on Windows and seccomp/Landlock on Linux (K); capability-based handles (K).
- **SBOM.** CycloneDX 1.7 (October 2025; ECMA-424 second edition, December 2025) [C27](#c27); SPDX 3.0 (K).
- **Provenance.** SLSA 1.2 (November 2025) adds a Source track to the unchanged Build track levels 0-3 [C26](#c26);
  Sigstore signing and GitHub artifact attestations (K).
- **Signing.** Authenticode with a cloud or HSM key, Apple notarization, Linux package signatures (K).
- **Reproducible builds.** reproducible-builds.org practice, `SOURCE_DATE_EPOCH`, MSVC `/Brepro`, path-prefix maps (K).
- **Regulation.** The EU Cyber Resilience Act requires manufacturers to report actively exploited vulnerabilities and
  severe incidents from 2026-09-11 (24-hour early warning, 72-hour notification); SBOM and the remaining obligations
  apply from 2027-12-11 (secondary sources; verify against Regulation (EU) 2024/2847) [C25](#c25).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Pinned, hashed, licensed external inputs | SLSA source/build inputs [C26](#c26) | Has: `cmake/pins.json` (10 packages, 19 tools, 5 actions) with SHA-256 and licence (REPO.DEV.6); actions pinned by commit |
| SBOM per build and per export | CycloneDX 1.7, SPDX 3 [C27](#c27) | Missing |
| Build provenance attestation | SLSA Build L3 [C26](#c26) | Missing |
| Code signing and notarization | Authenticode, notarization (K) | Missing |
| Reproducible-build verification | (K) | Missing; OPS.2 (Later) names "reproducible supply-chain artifacts" |
| Trusted versus isolated plugins | WASI 0.3, process isolation | Planned: [EXT.1](../ROADMAP.md#slice-ext.1) (Review), [EXT.3](../ROADMAP.md#slice-ext.3); Part 2 proposes evaluating WASI 0.3 |
| Program capabilities | ADR-0108, ADR-0116 | Decided; grant enforcement named forward |
| Hostile input to loaders | OSS-Fuzz practice | Has: bounded fuzz targets for CEIR/CKIR loaders and bundles (REPO.DEV.9, DIAG.10a) |
| Secrets handling | (K) | Partial: secret-safe logs in [OPS.1](../ROADMAP.md#slice-ops.1) (Later); bundles are private and never uploaded (DIAG.5) |
| Vulnerability disclosure and CRA reporting readiness | CRA [C25](#c25) | Missing |

### Gaps in area 6

- **Supply chain is half done.** Inputs are pinned and hashed; outputs are not described (SBOM), attested (SLSA
  provenance), signed or checked for reproducibility. Every exported game, tool and plugin package needs all four, and
  plugin installs must verify signatures and provenance before load.
- **Isolation tiers made concrete.** EXT.3's isolated tier needs two realizations: WASI 0.3 components for portable,
  in-process isolation, and an OS process with a restricted token or seccomp/Landlock profile for native code, both
  reached through capability handles, never ambient authority.
- **Security practice.** Threat models per attack surface (loaders, plugins, MCP, network, notebooks that run code from
  documents), a disclosure policy, an advisory process and CRA reporting readiness for any product placed on the EU
  market.
- **Principles.** (a) Updating a signed plugin is a live generation swap; a failed verification leaves the last-good
  generation. (b) Hashes and signatures use maintained cryptographic implementations behind owned interfaces (ADR-0130
  item 3). (e) Isolation has a fallback when a tier is unavailable (no WASI runtime: refuse untrusted code, never run it
  trusted). (f) Peers: Chrome and VS Code extension isolation for plugin overhead.

## 7. Testing methodology

### State of the art 2026

- **Perceptual image comparison.** NVIDIA FLIP and HDR-FLIP target rendering differences (fireflies, viewing
  conditions); ColorVideoVDP (SIGGRAPH 2024) models spatial, temporal and colour vision with display parameters and is
  reported to predict colour suprathreshold contrast better than HDR-FLIP; a learned variant appeared in 2025
  [C30](#c30). LPIPS and SSIM remain common (K).
- **Fuzzing.** libFuzzer's original authors "have stopped active work on it" and moved to Centipede; libFuzzer gets
  important bug fixes only [C28](#c28). Centipede is merged into FuzzTest and targets large, slow targets
  [C29](#c29). OSS-Fuzz-gen generates fuzz harnesses with LLMs; results vary by project and model [C29](#c29). AFL++
  and LibAFL are the other maintained engines (K).
- **Property-based and mutation testing.** Hypothesis and RapidCheck with shrinking and stateful model testing; Mull
  (LLVM) for C++ mutation testing (K).
- **Formal methods.** TLA+ with TLC and Apalache for protocols; CBMC/ESBMC for bounded C and C++ checks; Kani for Rust
  (not applicable to C++); Gappa and FPTaylor for floating-point error bounds; Lean for proofs (K). Cerid already runs
  GenMC weak-memory models in CI (DIAG.4b, [CI tiers](../design/ci-tiers.md)).
- **Compiler differential testing.** Csmith and YARPGen generate UB-free programs; C-Reduce minimizes; GraphicsFuzz-style
  metamorphic testing found many GPU compiler bugs (K).
- **Flakes.** Industry practice quarantines flaky tests (K); Cerid forbids quarantine or retry as proof (DIAG.10b).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Sanitizers, race detection, weak-memory models | ASan/TSan, GenMC | Has: tiered CI (DIAG.1b, DIAG.3, DIAG.4b, DIAG.11b) |
| Seeded detector failures, no retry-as-proof | (rare) | Has: DIAG.10b register and seeded faults; ahead of common practice |
| Coverage-guided fuzzing | libFuzzer, Centipede, AFL++ [C28](#c28) [C29](#c29) | Has on libFuzzer only (bounded targets, corpus replay on every lane) |
| LLM-generated fuzz harnesses | OSS-Fuzz-gen [C29](#c29) | Missing |
| Random program generation and reduction for the compilers | Csmith, YARPGen (K) | Missing; corpus differential exists (ADR-0123) |
| Perceptual golden-image oracle | FLIP, ColorVideoVDP [C30](#c30) | Missing (no metric in source); golden comparisons are named in renderer contracts |
| Property-based testing with shrinking | Hypothesis, RapidCheck (K) | Missing as a framework (DIAG.10a stateful fuzzing covers allocators and commands) |
| Mutation testing | Mull (K) | Partial: OWN.2 planted copies, DIAG.10b seeded faults; no mutation score |
| Protocol model checking | TLA+ (K) | Missing; GenMC covers memory-model level only |
| Numerical error bounds | Gappa, FPTaylor (K) | Missing; numerical oracles and ULP envelopes exist (device replay `ulp:N`) |
| Device replay across adapters | (rare) | Has: schema-5 device records with `exact` and `ulp:N` envelopes |

### Gaps in area 7

- **Compiler testing needs generators.** CEIR, CKIR (six emitters) and CHIR need typed, UB-free random program
  generators, cross-executor and cross-backend differentials (interpreter, compiled plan, future JIT tiers, every GPU
  emitter and provider), metamorphic transformations for GPU compilers, and an automatic reducer.
- **Golden images need a metric.** One owned implementation of HDR-FLIP and a ColorVideoVDP-class metric with declared
  viewing conditions, per-asset thresholds, per-backend and per-driver baselines, an approval workflow and
  content-addressed golden storage.
- **Fuzzing engine diversity.** libFuzzer is in maintenance; add Centipede/FuzzTest for large targets and AFL++ as a
  second engine, and LLM-generated harnesses reviewed like any code.
- **Property, mutation and protocol checks.** Stateful property tests for command transactions and undo/redo (CMD.3),
  mutation scores on selected modules, TLA+ models for RAH-7's staged commit, CMD.3's journal, the flight recorder's
  global sequence and COLLAB's host.
- **Principles.** (a) Live replacement is itself tested by generated edit sequences against the last-good invariant. (b)
  Image metrics and error-bound tools live in one owned module. (e) Differentials run per SIMD-DISPATCH level and per GPU
  tier. (f) Fuzzing and test throughput are measured, not assumed.

## 8. Accessibility and internationalization

### State of the art 2026

- **Unicode and locale data.** ICU 79.1 (2026-10-22) is the current ICU; its C++ and Java MessageFormat 2.0
  implementations follow CLDR 49 with some API still in technology preview [C31](#c31). ICU 78 (2025-10-30) moved to
  Unicode 17 and CLDR 48 [C31](#c31). HarfBuzz for shaping (Part 1 proposes it as a peer).
- **Message formats and exchange.** Unicode MessageFormat 2 (K for the specification), Mozilla Fluent (K), XLIFF 2.2
  Committee Specification (2025-03-13) [C33](#c33).
- **Screen readers.** UI Automation, AT-SPI and NSAccessibility; AccessKit as a cross-platform layer, with a 2026 iOS
  adapter and GTK using it on Windows and macOS (secondary) [C34](#c34); Godot 4.5 screen readers through AccessKit
  ([P1 W35](2026-10-26-state-of-the-art-2026-comparison.md#w35)).
- **Guidelines.** WCAG 2.2 is the referenced standard; WCAG 3 is an incomplete working draft (March 2026), not expected
  to be complete for years [C32](#c32). Xbox Accessibility Guidelines and the Game Accessibility Guidelines (K).

### Cerid comparison

| Capability | 2026 reference | Cerid status |
|---|---|---|
| Pinned Unicode segmentation, BiDi, line break | ICU 79 [C31](#c31) | Planned: [I2D-6.a](../ROADMAP.md#slice-i2d-6.a) (owned) |
| Complex-script shaping and fallback | HarfBuzz (K) | Planned: [I2D-6.b](../ROADMAP.md#slice-i2d-6.b) (owned) |
| IME composition | (K) | Planned: [I2D-6.c](../ROADMAP.md#slice-i2d-6.c), [I2D-6.d](../ROADMAP.md#slice-i2d-6.d) |
| Plurals, formats, pseudo-locales, RTL mirroring | ICU, CLDR [C31](#c31) | Planned: I2D-6.d; CLDR data pipeline not named |
| MessageFormat 2 or Fluent message syntax | [C31](#c31) (K) | Missing as a decision |
| XLIFF exchange and translator workflow | XLIFF 2.2 [C33](#c33) | Missing |
| Screen-reader adapters | UIA, AT-SPI, AccessKit [C34](#c34) | Planned: [I2D-8.e](../ROADMAP.md#slice-i2d-8.e) (Windows, Linux); macOS and web not named there |
| Semantic accessibility tree | (K) | Planned: [I2D-3.d](../ROADMAP.md#slice-i2d-3.d) |
| High contrast, reduced motion | WCAG 2.2 [C32](#c32) | Planned: [I2D-5.d](../ROADMAP.md#slice-i2d-5.d) |
| Game accessibility (remapping, captions, colour-vision, narration, assists) | Xbox Accessibility Guidelines (K) | Missing as a row |

### Gaps in area 8

- **Locale data, not only algorithms.** Owned Unicode algorithms need a CLDR-driven data pipeline (plural rules, number,
  date, unit and list formats, collation) cooked as assets, with ICU 79 as the conformance oracle and peer.
- **One message syntax** (MessageFormat 2, with Fluent as the measured alternative) with XLIFF 2.2 import/export for
  translators and live reload of string tables.
- **Games need more than widgets.** Input remapping, subtitle and caption styling, colour-vision modes, screen narration
  through platform text to speech, visual and haptic cues for audio, difficulty and assist settings, all as authored
  assets with tests against an Xbox Accessibility Guidelines-class checklist.
- **Platforms.** NSAccessibility (with MAC.1) and ARIA projection for the web (WEB.2) belong beside UIA and AT-SPI.
- **Principles.** (a) Locale, strings and accessibility settings reload live. (c) Narration and caption cues are CHIR
  behaviour. (e) Text rendering keeps a CPU path for older GPUs. (f) Peers: ICU and HarfBuzz for conformance and speed;
  NVDA, Narrator, Orca and VoiceOver for real screen-reader behaviour.

## End-to-end walkthrough: building, debugging, profiling and shipping a feature

The feature: a parameterized "energy shield" for a game and an engineering viewer. A CHIR behaviour drives its state
from gameplay events; a CKIR kernel shades it; a material and a frame pass composite it; a designer tunes it; it ships
in Windows and Linux exports. An agent and a person work together. "Today" is what the source supports on 2026-10-26;
"Gold standard" is the target; **Missing** names each step with no owner.

| Step | Today | Gold standard | Missing step and proposed owner |
|---|---|---|---|
| 1. Agent connects | `ceridc mcp`: `2024-11-05`, 9 verbs plus a `diag` tool with host-granted authority | `2026-07-28` server with `server/discover`, generated tools with `outputSchema`, resources, prompts, tasks | **Protocol currency** (MCP-ADAPT.0), **generated surface** (MCP-ADAPT, AGENT.1) |
| 2. Agent learns the languages | Reads repository assets and generated dialect references | llms.txt, skills, language guides and graded examples served as MCP resources and prompts | **Agent docs** (AGENT-DOCS) |
| 3. Create a project | No user project; assets live in the engine tree | `ceridc new shield --template game`, manifest and lockfile, the same command in CR-D007 | **Project system and templates** (DX.1, DX.2) |
| 4. Write the CHIR behaviour | Five CHIR-0 constructs; errors carry CHIR line:col (DIAG.8a) | Full language, LSP diagnostics with fix-its, completion from REFLECT, formatter | **Language** (LANG.0, LANG rows), **semantic service and LSP** (LANGTOOL.1, LANGTOOL.2, LANGTOOL.4) |
| 5. Write the CKIR kernel | Text CKIR cooked to six targets | Modules, generics and interfaces, LSP, CKIR-source GPU debugging | **CKIR language** (CKIR-LANG.1), **GPU debug info** (LANGTOOL.3) |
| 6. Try it live | Frame and program reload in the sandbox (coarse); `ReloadSet` for CEIR | Granular, no-hitch replacement; REPL attached to the running game to set parameters and query state | **RAH-7.e/f (P1)**, **REPL attach** (REPL.1, REPL.2) |
| 7. Tune parameters | No inspector for user projects yet | CR-D007 inspector and REPL share REFLECT properties; every change is a command with undo | Planned (I2D-4.b, CMD.3); REPL.2 |
| 8. Test | In-repository Catch2 tests; no user test runner; no image metric | `ceridc test` in the project, perceptual goldens with declared tolerances, property tests on the behaviour | **Project tests** (DX.1), **image oracle** (IMG-ORACLE), **property tests** (TEST-METH) |
| 9. Debug a wrong state | `ceridc inspect` (JSON stops, typed values), run records and replay with first divergence | DAP in VS Code/Visual Studio/CR-D007, reverse step by replay, same session over MCP | **DAP adapter** (LANGTOOL.3) |
| 10. Profile a spike | Flight recorder, CPROF, Perfetto export, `capture.start/stop` commands | Traces mapped to CHIR/CKIR source lines, live-edit budget board, traces as MCP resources | **Source-mapped profiles** (LANGTOOL.3 amendment of D7E-10), RAH-7.f (P1), MCP-ADAPT resources |
| 11. Make it fast | Interpreter or compiled plan for CPU behaviour | Baseline and optimizing native tiers, per-ISA code | **Native tiers** (CEIR-JIT.0-CEIR-JIT.2) |
| 12. Review and merge | Git line diffs | Semantic diff/merge by stable node identity, locks for binaries | **VCS** (VCS.1, VCS.2) |
| 13. Localize and make accessible | Planned widget-level work | Strings in MessageFormat 2, XLIFF to translators, captions and narration cues | **I18N.1, A11Y-GAME** |
| 14. Package and ship | EDITOR.6/EDITOR.7 Open | Signed, notarized exports with SBOM and provenance, reproducible, AOT where JIT is forbidden | **SUPPLY.1-SUPPLY.4**, **CEIR-JIT.3**, crash service (OPS.1 amendment, P1) |
| 15. Publish a reusable shield package | No package format | Signed package with semver and lockfile in a registry | **EXT.4**, **API-STAB.0** |
| 16. Measure the agent | Nothing | The same task in the Cerid agent benchmark, with safety tasks (an injected instruction hidden in an asset name) | **AGENT-BENCH**, AGENT.3 amendment |

Steps 4, 9 and 14 block a real third-party user today; steps 1, 2 and 16 block a credible agent claim.

<a id="gaps-and-proposed-roadmap-changes"></a>
## Gaps and proposed roadmap changes

Proposals only; the user approves before any [ROADMAP](../ROADMAP.md) edit. "After" names the existing row and its
Order number on 2026-10-26; the validator's order rule means a row's Requires must sit earlier in the table. IDs from
Part 1 and Part 2 that are themselves proposals are marked "(P1)" and "(P2)". Highest impact first, matching the
ranking above. No retained ID is removed.

| # | Proposed ID | Kind | Placement | Requires | Content and acceptance |
|---:|---|---|---|---|---|
| 1 | LANG.0 | New | After [I2D-0](../ROADMAP.md#slice-i2d-0) (240), before LANG.1 | I2D-0 | ADR and language reference for CHIR 1: grammar, static semantics, local type inference, generics with a hot-reload-safe instantiation strategy, ADTs and pattern matching over `core.match`, modules/packages, closures, structured async and cancellation, Result/Option error model, effect syntax on ADR-0113 families, the ADR-0128 D4 ownership composite, units, an explicit dynamic type for interactive tiers only, editions; numbered conformance suite. No CHIR code change before acceptance |
| 1 | LANG.1 to LANG.5 | Amend | In place (241-245) | LANG.0 | Each row implements a named part of the LANG.0 reference and passes its conformance sections; LANG.5 adds the language's package form and source maps used by LANGTOOL.3 |
| 2 | LANGTOOL.1 | New | After [REFLECT](../ROADMAP.md#slice-reflect) (250) | LANG, REFLECT.2 | One query-based semantic service on `IncrementalDag` for CHIR, CEIR and CKIR: parse, resolve, types, diagnostics with fix-its, references, rename by stable identity; serves compiler, I2D-9.d, LSP, REPL and MCP with identical answers; latency and memory board against rust-analyzer and clangd on a large workspace |
| 2 | LANGTOOL.2 | New | After LANGTOOL.1 | LANGTOOL.1 | LSP 3.18 server: diagnostics (pull), hover, completion and inline completion, semantic tokens, inlay hints, go to definition, references, rename, code actions, notebook documents; attaches to a running process's live generation |
| 2 | LANGTOOL.3 | New | After LANGTOOL.2 | LANGTOOL.1, DIAG | DAP adapter over DIAG.8b inspection and DIAG.9 replay: breakpoints, stepping, typed variables, reverse step and reverse continue by deterministic re-execution of a run record; CKIR source debug info (SPIR-V `NonSemantic.Shader.DebugInfo.100`, DXIL debug info) so PIX and RenderDoc step CKIR lines; profiles and captures mapped to CHIR/CKIR lines (amends D7E-8, D7E-10) |
| 2 | LANGTOOL.4 | New | After LANGTOOL.3 | LANGTOOL.1 | Style-preserving formatter for CHIR, CEIR and CKIR text; lint rule set with machine-applicable fixes; refactorings (rename, extract function, inline, move to module) as transactional commands |
| 2 | LANGTOOL.5 | New | After LANGTOOL.4 | LANGTOOL.2, LANGTOOL.3 | Tree-sitter grammars; VS Code, Visual Studio and JetBrains extensions using LSP and DAP; grammars tested against the conformance corpus |
| 2 | LANGTOOL.6 | New | After LANGTOOL.5 | LANGTOOL.1, REFLECT | Generated reference documentation for CHIR libraries, CEIR dialects (extends the 27 `*.ops.md`) and CKIR intrinsics, with every example compiled and run in CI |
| 2 | I2D-9.d | Amend | In place (340) | LANGTOOL.1 | The CR-D007 code editor is a client of LANGTOOL.1/LANGTOOL.2/LANGTOOL.3; no private analyzer |
| 3 | MCP-ADAPT.0 | New | After [DIAG](../ROADMAP.md#slice-diag) (097) | DIAG.8c | Bring the shipped `ceridc mcp` loop to `2026-07-28` with `2025-11-25` handshake compatibility: `server/discover`, `_meta` version negotiation, `resultType`, deterministic `tools/list`, `outputSchema` and `structuredContent` for every verb and the `diag` tool, input validation errors as tool errors; conformance tests against the published schema |
| 3 | MCP-ADAPT | New | After [CMD.4](../ROADMAP.md#slice-cmd.4) (257) | CMD.4, MCP-ADAPT.0 | One MCP adapter module over the command service, tracking the current revision with N-1 support: generated tools, resources (scenes, assets, programs, captures, docs by URI with `ttlMs`/`cacheScope`), prompts from authored workflow assets, tasks extension for cook/render/test/experiments, MRTR `input_required` confirmations bound to the authority model, Streamable HTTP and stdio, `subscriptions/listen`, OpenTelemetry context into DIAG traces, MCP Apps views for viewport and inspector, registry `server.json` |
| 3 | CMD.4, AGENT.1, AGENT.2, AGENT.3 | Amend | In place (257, 272-274) | MCP-ADAPT | CMD.4 consumes MCP-ADAPT; AGENT.1 generates `outputSchema` and tool names per the 2025-11-25 guidance; AGENT.2 maps workspaces and checkpoints onto tasks; AGENT.3 adopts the spec's OAuth profile (resource indicators, protected resource metadata, Client ID Metadata Documents) for remote use (extends Part 1's pin) |
| 4 | REPL.0 | New | After [CHIR-SCI.3](../ROADMAP.md#slice-chir-sci.3) (518) | CHIR-SCI.1, SCRIPT | Decision: one interactive kernel shared by REPL, notebook, game console and MCP; module name and placement (recommended: a CHIR-level `crd-repl` in `engine/execution`, with crd-hesap supplying display formats and variable inspection, superseding the `crd-hesap-repl` name in ADR-0065 section 9); session protocol, authority and persistence. Placement is after the UI system (Orders 240-363) as decided |
| 4 | REPL.1 | New | After [CHIR-SCI.4](../ROADMAP.md#slice-chir-sci.4) (519) | CHIR-SCI.4, REPL.0, MCP-ADAPT | Headless REPL: each line compiles incrementally into the session's live module generation; workspace variables persist; MATLAB-class display of matrices, units and structures; completion and help from LANGTOOL.1 and REFLECT; history; interrupt; `whos`/`clear`/`save`/`load`; `ceridc repl` and an MCP `repl.eval` tool with server-minted session handles; a session is recordable and replayable as a DIAG.9 run record |
| 4 | REPL.2 | New | After REPL.1 | REPL.1, I2D-9.d, SCRIPT.4 | CR-D007 command window and in-game console on the same kernel; attach to a running game, editor or headless process under host-granted authority; console and cheat commands of SCRIPT.4 are REPL commands with shipping allowlists |
| 4 | CHIR-SCI | Amend | In place (520) | REPL.2 | Scientific language readiness includes the command window |
| 4 | NOTEBOOK.1 | Amend | In place (521) | REPL.1 | The notebook kernel API is the REPL.1 session protocol plus documents and cells; no second evaluator |
| 4 | REPL.3 | New | After [NOTEBOOK.1](../ROADMAP.md#slice-notebook.1) (521) | NOTEBOOK.1, REPL.1 | Jupyter kernel messaging adapter and `.ipynb` import/export (closes Part 2's Jupyter gap); rich display (plots via SCIVIZ-PLOT (P2), tables, images) through the same display formats |
| 4 | REPL.4 | New | After [NOTEBOOK.5](../ROADMAP.md#slice-notebook.5) (525) | REPL.3, NOTEBOOK.5 | Qualification: line round-trip latency and first-line startup against MATLAB, Julia and IPython; crash isolation of user code; interrupt during GPU work; Windows/Linux tuples |
| 5 | AGENT-DOCS | New | After [AGENT.1](../ROADMAP.md#slice-agent.1) (272) | AGENT.1, LANGTOOL.6 | "How to write CEIR, CHIR and CKIR" guides with CI-verified examples; generated `llms.txt` and `llms-full.txt`; Agent Skills for build, debug, profile, author shader, author behaviour, run experiment; a graded example corpus; all served as MCP resources and prompts; user-project AGENTS.md template |
| 6 | AGENT.3 | Amend | In place (274) | AGENT.2 | Content provenance tags on untrusted spans in tool results; tool descriptions only from reviewed, signed schemas; per-plugin tool namespaces and grants; egress and file-root limits; audit log; hostile corpus includes prompt injection through asset names, comments, logs and plugin metadata, and tool-description poisoning |
| 6 | AGENT-BENCH | New | After [AGENT.5](../ROADMAP.md#slice-agent.5) (365) | AGENT.5 | Cerid agent benchmark: tasks across game, rendering (CKIR), UI (CHIR), notebook, audio, debugging, profiling, packaging; hidden executable checks (engine tests, image oracle, replay); success, wall time, tool calls and tokens; safety tasks; the same tasks run against Unreal, Unity and Blender servers where comparable; published board |
| 6 | AGENT | Amend | In place (366) | AGENT-BENCH | Agent control qualifies only with a recorded AGENT-BENCH board |
| 7 | CEIR-JIT.0 | New | After [SCRIPT](../ROADMAP.md#slice-script) (263) | LANG, SIMD-DISPATCH | Decision: owned baseline (copy-and-patch over the compiled plan) and owned optimizing tier (ADR-0117 pass manager, register allocation, profiles, OSR, deoptimization) versus Cranelift or LLVM ORC as the code generator; generation-keyed code retirement under the ReloadSet; per-ISA code for every SIMD-DISPATCH level; relation to WEB-CEIR (P2) |
| 7 | CEIR-JIT.1 | New | After [NATIVE](../ROADMAP.md#slice-native) (369) | CEIR-JIT.0, SCRIPT.3 | Baseline native tier, x64 and ARM64, interpreter fallback, byte-identical to the reference oracle on the corpus and on IR-FUZZ programs |
| 7 | CEIR-JIT.2 | New | After CEIR-JIT.1 | CEIR-JIT.1 | Optimizing tier with profile feedback; peer boards against LuaJIT, Luau, Julia and native C++ |
| 7 | CEIR-JIT.3 | New | After CEIR-JIT.2 | CEIR-JIT.2, EDITOR.7 | AOT object generation for exports and JIT-forbidden targets; QUAL.2 (500) and the console/mobile rows require it |
| 8 | IR-FUZZ | New | After [OWN](../ROADMAP.md#slice-own) (104) | DIAG.10a | Typed UB-free random program generators for CEIR and CKIR (CHIR added at LANG.5), differentials across interpreter, compiled plan, every GPU emitter and provider, metamorphic GPU transformations, an automatic reducer; Centipede/FuzzTest and AFL++ beside libFuzzer; reviewed LLM-generated harnesses |
| 9 | IMG-ORACLE | New | After GPU-TIER.0 (P1), itself after [RAH-0](../ROADMAP.md#slice-rah-0) (112) | RAH-0, OWN.1 | One owned HDR-FLIP and ColorVideoVDP-class metric (owner registered in OWN.1) with declared viewing conditions; per-asset thresholds; per-backend, per-tier and per-driver baselines; approval workflow; content-addressed golden store; agent-readable difference maps; validated against the reference implementations |
| 10 | SUPPLY.1 | New | After [OWN](../ROADMAP.md#slice-own) (104) | REPO.DEV.6 | SBOM in SPDX 3 and CycloneDX 1.7 for every build and export, generated from `pins.json` and the module registry, with VEX statements |
| 10 | SUPPLY.4 | New | After SUPPLY.1 | SUPPLY.1 | Reproducible builds: deterministic flags per toolchain, path maps, `SOURCE_DATE_EPOCH`, a double-build comparison lane in CI |
| 10 | SUPPLY.2 | New | After SUPPLY.4 | SUPPLY.1 | SLSA 1.2 Build L3 provenance from hosted CI with signed attestations and verification on install |
| 10 | SUPPLY.3 | New | After [EXT.3](../ROADMAP.md#slice-ext.3) (270) | SUPPLY.2 | Code signing (Authenticode with an HSM-held key, Linux signatures, Apple notarization with MAC.1) and plugin package signing with a transparency log |
| 10 | SEC.1 | New | After SUPPLY.4 | - | Threat models per surface (loaders, plugins, MCP, network, notebooks), disclosure policy, advisory process, CRA reporting readiness |
| 10 | OPS.2 | Amend | In place (858) | SUPPLY.2 | Reproducible supply-chain artifacts move to SUPPLY rows; OPS.2 keeps product matrices |
| 11 | API-STAB.0 | New | After [CMD.2](../ROADMAP.md#slice-cmd.2) (255) | CMD.2, REFLECT | Semver per module, CEIR dialect and op version rules, CHIR editions, command-schema versions, a twelve-month deprecation window, C ABI shim rules for native plugins, an ABI/API checker in CI |
| 12 | EXT.4 | New | After [EXT.3](../ROADMAP.md#slice-ext.3) (270), after SUPPLY.3 | EXT.3, SUPPLY.3, API-STAB.0 | Package format for plugins, CHIR libraries, CKIR kernels and asset packs: manifest, semver resolution, lockfile, local and hosted registry, signature and provenance verification, live install and rollback |
| 12 | EXT.5 | New | After EXT.4 (Later) | EXT.4 | Public registry and marketplace: reviews, publisher analytics, paid listings, licence metadata; peers Godot Asset Store, Fab, UPM |
| 13 | DX.1 | New | After [NATIVE](../ROADMAP.md#slice-native) (369) | EDITOR.6, NATIVE.2, EXT.4 | User project model and `ceridc new/build/run/test/package` (same commands in CR-D007 and MCP); CHIR-only projects build without a C++ toolchain |
| 13 | DX.2 | New | After DX.1 | DX.1 | Templates and samples per domain (game, engineering viewer, DAW plugin, notebook, web app, command-line tool), each built and run by CI as a downstream project |
| 13 | DX.3 | New | After DX.2 | DX.2, LANGTOOL.6, AGENT-DOCS | Tutorial track with every snippet executed in CI; the same material serves people and agents |
| 13 | DX.4 | New | After DX.3 | DX.3 | Onboarding qualification: clean-machine install to first running template, measured against Unity, Godot and Unreal |
| 14 | VCS.1 | New | After [D7E-0](../ROADMAP.md#slice-d7e-0) (326) | DOC, D7E-0 | Semantic diff and merge drivers for CEIR, CHIR, CKIR and scene text by stable identity; conflict presentation through D7E-0 |
| 14 | VCS.2 | New | After [EDITOR.1](../ROADMAP.md#slice-editor.1) (356) | VCS.1, EDITOR.1 | VCS-neutral adapter (Git with LFS, P4, Unity Version Control, Diversion), file locking, shared cook-cache keys |
| 14 | DOC.2, REFLECT.2 | Amend | In place (265, 248) | - | One schema-evolution contract for every persisted format: tagged fields, unknown-field preservation, N-1 and N-2 readers, editions, registered migrations |
| 14 | INTEROP.1, INTEROP.3 | Amend; New | In place (839); INTEROP.3 after INTEROP.1 | INTEROP.1 | INTEROP.1 records the FBX decision (owned reader with `ufbx` as oracle) and Alembic; INTEROP.3: live links to Blender, Maya and Houdini |
| - | TEST-METH | New | After IR-FUZZ | IR-FUZZ | Owned property-based testing with shrinking and stateful model tests (CMD.3 undo/redo, DOC.2 recovery); mutation testing with scores on selected modules |
| - | FORMAL.1 | New | After IMG-ORACLE | RAH-0 | TLA+ models checked in CI for RAH-7 staged commit, CMD.3 journal, the flight recorder's global sequence and the COLLAB host; FPTaylor/Gappa error bounds for selected hesap kernels |
| 15 | I18N.1 | New | After [I2D-6](../ROADMAP.md#slice-i2d-6) (313) | I2D-6.d | CLDR-driven locale data cooked as assets with ICU 79 as conformance oracle; MessageFormat 2 runtime (Fluent as measured alternative); XLIFF 2.2 exchange; live string reload |
| 15 | A11Y-GAME | New | After [I2D-8](../ROADMAP.md#slice-i2d-8) (325) | I2D-8.e | Game accessibility kit as authored assets: remapping, captions and subtitles, colour-vision modes, narration through platform TTS, audio cues, assists; Xbox Accessibility Guidelines-class checklist tests; WCAG 2.2 AA for engineering apps |
| 15 | I2D-8.e | Amend | In place (323) | - | AccessKit as behaviour reference; NSAccessibility in MAC.1 and ARIA in WEB.2 named as the other adapters |
| - | CKIR-LANG.1 | New | After [RAH-7](../ROADMAP.md#slice-rah-7) (145) | RAH-0 | CKIR modules, generics and interfaces with variant-cache agreement (ADR-0128 Q5 landmine); Slang import route and Slang as measured peer (Part 1) |
| - | CKIR-LANG.3 | New | After [HGP-1](../ROADMAP.md#slice-hgp-1) (511) | HGP-1 | Tile-level dialect above CKIR lowering to cooperative matrix, DX LinAlg and CUDA Tile IR; peers Triton, cuTile, CUTLASS; MLR-1 consumes it |
| - | CKIR-LANG.2 | New | After [HGP-4](../ROADMAP.md#slice-hgp-4) (514) | HGP-4 | Forward and reverse autodiff of CKIR kernels sharing hesap-autodiff's graph rules; differentiable rendering consumers |
| - | NATIVE.2 | Amend | In place (368) | - | Name Live++ and Unreal Live Coding as peers; decide in-place function patching versus module reload with quiescence |

<a id="needs-a-download"></a>
## Needs a download

Not downloaded. Sizes were not checked (that would need at least a HEAD request); each item needs separate user
approval before it is fetched.

| Item | Why | URL | Size |
|---|---|---|---|
| MCP specification schema (`schema.json`, revision 2026-07-28) and the MCP Inspector | MCP-ADAPT.0 conformance tests | https://github.com/modelcontextprotocol/modelcontextprotocol ; https://github.com/modelcontextprotocol/inspector | not checked |
| Unreal Engine 5.8 (MCP plugin), Unity 6 with the Unity AI MCP beta, Blender 5.1 with the Blender Lab MCP add-on | AGENT-BENCH peer runs | https://www.unrealengine.com ; https://unity.com ; https://www.blender.org/lab/mcp-server/ | not checked |
| GameDevBench task set | AGENT-BENCH method reference | https://arxiv.org/abs/2602.11103 | not checked |
| LuaJIT, Luau, Julia 1.12 | CEIR-JIT peers | https://luajit.org (K) ; https://luau.org (K) ; https://julialang.org (K) | not checked |
| Cranelift/Wasmtime, LLVM 23.1 | CEIR-JIT.0 decision peers | https://github.com/bytecodealliance/wasmtime ; https://releases.llvm.org | not checked |
| rust-analyzer, clangd | LANGTOOL.1 latency peers | https://rust-analyzer.github.io (K) ; https://clangd.llvm.org (K) | not checked |
| tree-sitter CLI | LANGTOOL.5 | https://github.com/tree-sitter/tree-sitter | not checked |
| VS Code extension SDK samples (LSP, DAP) | LANGTOOL.5 | https://github.com/microsoft/vscode-extension-samples | not checked |
| Jupyter (JupyterLab 4, `jupyter_client`), IPython | REPL.3 protocol tests and peer | https://jupyter.org (K) | not checked |
| MATLAB (licensed) | REPL.4 peer (already named in Part 2) | https://www.mathworks.com | not checked |
| NVIDIA FLIP, ColorVideoVDP reference code | IMG-ORACLE validation | https://github.com/NVlabs/flip (K) ; https://github.com/gfxdisp/ColorVideoVDP | not checked |
| Centipede/FuzzTest, AFL++ | IR-FUZZ engines | https://github.com/google/fuzztest ; https://github.com/AFLplusplus/AFLplusplus (K) | not checked |
| Csmith, YARPGen, C-Reduce | IR-FUZZ references | https://github.com/csmith-project/csmith (K) ; https://github.com/intel/yarpgen (K) | not checked |
| Mull | TEST-METH mutation testing | https://github.com/mull-project/mull (K) | not checked |
| RapidCheck | TEST-METH reference | https://github.com/emil-e/rapidcheck (K) | not checked |
| TLA+ tools (TLC), Apalache; FPTaylor, Gappa | FORMAL.1 | https://github.com/tlaplus/tlaplus (K) ; https://apalache-mc.org (K) | not checked |
| Syft or other SBOM generators, CycloneDX and SPDX validators | SUPPLY.1 cross-check | https://cyclonedx.org/tool-center/ (K) ; https://spdx.dev (K) | not checked |
| Sigstore cosign, slsa-verifier | SUPPLY.2 | https://github.com/sigstore/cosign (K) ; https://github.com/slsa-framework/slsa-verifier (K) | not checked |
| ICU 79.1 and CLDR 49 data | I18N.1 oracle | https://icu.unicode.org | not checked |
| Fluent reference implementation | I18N.1 alternative | https://projectfluent.org (K) | not checked |
| XLIFF 2.2 schemas | I18N.1 | https://docs.oasis-open.org/xliff/xliff-core/v2.2/ | not checked |
| AccessKit; NVDA; Orca | A11Y and I2D-8.e peers | https://accesskit.dev (K) ; https://www.nvaccess.org (K) | not checked |
| ufbx | INTEROP.1 FBX oracle | https://github.com/ufbx/ufbx (K) | not checked |
| Slang, Triton, cuTile (CUDA 13.1) | CKIR-LANG peers | https://github.com/shader-slang/slang ; https://github.com/triton-lang/triton (K) ; https://developer.nvidia.com/cuda-toolkit | not checked |

<a id="sources"></a>
## Sources

Read 2026-10-26 unless marked (K). Dates are publication dates as shown by the source or its coverage.

<a id="c01"></a>C01. MCP, [Versioning](https://modelcontextprotocol.io/specification/versioning): current revision 2026-07-28, per-request negotiation, `server/discover` (fetched).

<a id="c02"></a>C02. MCP, [Key changes in 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28/changelog) (fetched).

<a id="c03"></a>C03. MCP, [Key changes in 2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25/changelog) (fetched).

<a id="c04"></a>C04. MCP, [Key changes in 2025-06-18](https://modelcontextprotocol.io/specification/2025-06-18/changelog) (fetched).

<a id="c05"></a>C05. Towards AI, [MCP Registries in Mid-2026](https://pub.towardsai.net/mcp-registries-in-mid-2026-one-upstream-won-2ce541b92036), 2026 (secondary; v0.1 API freeze 2025-10-24); heise, [MCP Registry launched](https://heise.de/-10637436), 2025.

<a id="c06"></a>C06. MCP, [SEP-1865: MCP Apps](https://modelcontextprotocol.io/seps/1865-mcp-apps-interactive-user-interfaces-for-mcp); MCP blog, [MCP Apps](https://blog.modelcontextprotocol.io/posts/2025-11-21-mcp-apps/), 2025-11-21; stable date 2026-01-26 from secondary coverage.

<a id="c07"></a>C07. Blender, [Blender Lab MCP server](https://www.blender.org/lab/mcp-server/) (fetched; add-on 1.0.3, Blender 5.1+); release dates from [StraySpark](https://www.strayspark.studio/blog/official-blender-mcp-server-comparison-2026) (secondary, commercial).

<a id="c08"></a>C08. DEV Community, [Using Unreal MCP in UE 5.8 from Codex](https://dev.to/gamedevtoollab/using-unreal-mcp-in-ue-58-from-codex-setup-toolsets-and-safe-editor-automation-4pon), July 2026 (fetched; secondary).

<a id="c09"></a>C09. Summer Engine, [The Best Godot MCP Server in 2026](https://www.summerengine.com/blog/best-godot-mcp-server) (secondary); SideFX forum, [APEX Script MCP question](https://www.sidefx.com/forum/post/463522/); [fxhoudinimcp](https://pepy.tech/projects/fxhoudinimcp).

<a id="c10"></a>C10. OpenAI, [Agentic AI Foundation](https://openai.com/index/agentic-ai-foundation/), 2025-12-09.

<a id="c11"></a>C11. Agent Skills, [Specification](https://agentskills.io/specification).

<a id="c12"></a>C12. J. Howard, [llms.txt proposal](https://llmstxt.org) (K), 2024.

<a id="c13"></a>C13. BleepingComputer, [The real-world attacks behind OWASP Agentic AI Top 10](https://bleepingcomputer.com/news/security/the-real-world-attacks-behind-owasp-agentic-ai-top-10) (secondary); OWASP list published 2025-12-09.

<a id="c14"></a>C14. [GameDevBench](https://arxiv.org/abs/2602.11103), arXiv 2602.11103, Feb 2026; [GameCraft-Bench](https://hyper.ai/en/papers/2606.17861) (secondary listing), Jun 2026; [SWE-Game](https://aiweekly.co/alerts/swe-game-opus5-leads-247-godot-agent-tasks-still-below-60) (secondary).

<a id="c15"></a>C15. NVIDIA, [CUDA 13.1 with CUDA Tile](https://developer.nvidia.com/blog/nvidia-cuda-13-1-powers-next-gen-gpu-programming-with-nvidia-cuda-tile-and-performance-gains/), Dec 2025; [cuTile Python](https://developer.nvidia.com/blog/simplify-gpu-programming-with-nvidia-cuda-tile-in-python).

<a id="c16"></a>C16. Modular, [Mojo 1.0 is here](https://modular.com/blog/modular-26-5-mojo-1-0-is-here), 2026-08-11; [Mojo open source](https://modular.com/blog/mojo-open-source), 2026-08-18.

<a id="c17"></a>C17. Python, [PEP 836](https://peps.python.org/pep-0836/) (Draft, created 2026-07-02); [What is CPython's JIT compiler](https://pydevtools.com/handbook/explanation/what-is-cpythons-jit-compiler/) (secondary, speed figures).

<a id="c18"></a>C18. LLVM, [LLVM 23.1.0 release notes](https://releases.llvm.org/23.1.0/docs/ReleaseNotes.html).

<a id="c19"></a>C19. Unity, [CoreCLR, Scripting, and Serialization Update, June 2026](https://discussions.unity.com/t/coreclr-scripting-and-serialization-update-june-2026/1723299); [Unite Seoul 2026 keynote recap](https://unity.com/blog/unite-seoul-keynote-2026-recap).

<a id="c20"></a>C20. Microsoft, [Language Server Protocol 3.18 specification](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.18/specification/) (fetched; release date not shown).

<a id="c21"></a>C21. Microsoft, [Debug Adapter Protocol changelog](https://microsoft.github.io/debug-adapter-protocol/changelog) (fetched; dates not shown).

<a id="c22"></a>C22. GameDev.net, [Introducing the Godot Asset Store](https://gamedev.net/news/3514-introducing-the-godot-asset-store/), 2026.

<a id="c23"></a>C23. Protocol Buffers, [Changes announced on July 13, 2026](https://protobuf.dev/news/2026-07-13).

<a id="c24"></a>C24. Diversion, [Perforce alternatives for game development, 2026](https://www.diversion.dev/knowledge-center/perforce-alternatives) (vendor); 80.lv, [Game studios are moving beyond legacy version control](https://80.lv/articles/game-studios-are-moving-beyond-legacy-version-control-and-here-s-why/).

<a id="c25"></a>C25. Advisori, [CRA September 2026: vulnerability reporting starts](https://www.advisori.de/en/blog/cra-september-2026-vulnerability-reporting-manufacturers-check); Docker, [EU Cyber Resilience Act overview](https://www.docker.com/blog/eu-cyber-resilience-act-overview/) (secondary; Regulation (EU) 2024/2847).

<a id="c26"></a>C26. SLSA, [Announcing SLSA v1.2](https://slsa.dev/blog/2025/11/announce-slsa-v1.2), Nov 2025; [What's new](https://slsa.dev/spec/v1.2/whats-new).

<a id="c27"></a>C27. CycloneDX, [v1.7 released](https://cyclonedx.org/news/cyclonedx-v1.7-released), Oct 2025; Ecma, [ECMA-424](https://ecma-international.org/publications-and-standards/standards/ecma-424/), 2nd edition, Dec 2025.

<a id="c28"></a>C28. LLVM, [libFuzzer status](https://llvm.org/docs/LibFuzzer.html) (fetched).

<a id="c29"></a>C29. Google, [Centipede README in FuzzTest](https://chromium.googlesource.com/external/github.com/google/fuzztest/+/6e19297/centipede/README.md); OSS-Fuzz, [Fuzz target generation using LLMs](https://google.github.io/oss-fuzz/research/llms/target_generation/).

<a id="c30"></a>C30. R. Mantiuk et al., [ColorVideoVDP](https://arxiv.org/abs/2401.11485), SIGGRAPH 2024; P. Andersson et al., [FLIP](https://research.nvidia.com/sites/default/files/node/3260/FLIP_Paper.pdf), 2020 (K for the date); [ColorVideoVDP-ML](https://www.cl.cam.ac.uk/~rkm38/pdfs/hammou2025_ICME_GC_ColorVideoVDP_ML.pdf), ICME 2025.

<a id="c31"></a>C31. Unicode, [ICU home](https://icu.unicode.org/) (fetched; ICU 79.1 dated 2026-10-22); [ICU 78 released](https://blog.unicode.org/2025/10/icu-78-released.html), 2025-10-30.

<a id="c32"></a>C32. W3C, [WCAG 3 Working Draft, March 2026](https://www.w3.org/WAI/news/2026-03-03/wcag3); [WCAG 3 Introduction](https://www.w3.org/WAI/WCAG3).

<a id="c33"></a>C33. OASIS, [XLIFF Version 2.2 Part 1: Core](https://docs.oasis-open.org/xliff/xliff-core/v2.2/xliff-core-v2.2-part1.html), Committee Specification, 2025-03-13.

<a id="c34"></a>C34. [AccessKit repository mirror](https://codeberg.org/WaylandNewton/accesskit) (2026 commits; secondary); LWN, [GTK hackfest report](https://lwn.net/Articles/1058024), early 2026.

Prior-knowledge (K) references not re-read today: MLIR; Cranelift and Winch; GraalVM Truffle; V8's tiers; .NET tiered
compilation and Hot Reload; LuaJIT; copy-and-patch (Xu and Kjolstad, OOPSLA 2021); salsa, rust-analyzer and Roslyn;
Live++ and Unreal Live Coding; Erlang/OTP code loading; Smalltalk and Pharo; TypeScript, Luau, OCaml 5 effects, Koka,
Hylo, Swift noncopyable types; Triton; IL2CPP and .NET Native AOT; LSP features before 3.18; rr and WinDbg TTD; SPIR-V
`NonSemantic.Shader.DebugInfo.100` and DXIL debug information; Julia REPL and Revise.jl; IPython and the Jupyter
messaging protocol; Mathematica; gofmt, rustfmt, clippy, clang-tidy; tree-sitter; vcpkg, Conan 2, Cargo, PubGrub, UPM,
Fab; FlatBuffers, Cap'n Proto, Bebop; Git LFS, P4, Unity Version Control; glTF, FBX SDK, ufbx, Alembic, Live Link,
Omniverse connectors; SPDX 3; Sigstore and artifact attestations; Authenticode, Apple notarization;
reproducible-builds.org and `SOURCE_DATE_EPOCH`; AppContainer, seccomp, Landlock; LPIPS, SSIM; AFL++, LibAFL;
Hypothesis, RapidCheck, Mull; TLA+, Apalache, CBMC, ESBMC, Kani, Gappa, FPTaylor, Lean; Csmith, YARPGen, C-Reduce,
GraphicsFuzz; MessageFormat 2 specification; Fluent; Xbox Accessibility Guidelines; Game Accessibility Guidelines;
llms.txt. Re-pin each before use.
