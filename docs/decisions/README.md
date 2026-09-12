# Architecture decisions

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Current work/status: [ROADMAP](../ROADMAP.md). This is a reference lookup, never a competing queue.
Search a title/ID, open its document, and respect its Proposed/Accepted/supersession notes.
Older index commentary is [preserved](../archive/2026-09-12-orientation-history.md#docs-decisions-readme).

New decisions get the next unused number and one entry here; the owning ROADMAP row links the ADR.
Existing dated decisions are amended explicitly, not silently rewritten as a different accepted architecture.

| Reference | Purpose |
|---|---|
| [0132-visual-studio-configuration-matrix](0132-visual-studio-configuration-matrix.md) | Canonical presets plus complete native MSVC configurations; user-selected workflow |
| [0131-project-structure-synchronization](0131-project-structure-synchronization.md) | Durable Visual Studio/source/CMake edits, selectable removal and physical module moves |
| [0130-system-qualification-and-agent-driven-products](0130-system-qualification-and-agent-driven-products.md) | Confirmed whole-system directions, combined training/inference and separate authorities; detailed mechanism proposals |
| [0001-build-language](0001-build-language.md) | ADR-0001 — Build & language |
| [0002-logging](0002-logging.md) | ADR-0002 — Logging |
| [0003-memory-v1](0003-memory-v1.md) | ADR-0003 — Memory v1 |
| [0004-containers-v1](0004-containers-v1.md) | ADR-0004 — Containers v1 |
| [0005-math-v1](0005-math-v1.md) | ADR-0005 — Math v1 |
| [0006-platform-v1](0006-platform-v1.md) | ADR-0006 — Platform v1 |
| [0007-app-shape](0007-app-shape.md) | ADR-0007 — `crd-app` shape |
| [0008-graphics-architecture](0008-graphics-architecture.md) | ADR-0008 — Graphics architecture |
| [0009-rhi-v1a-scaffold](0009-rhi-v1a-scaffold.md) | ADR-0009 — RHI v1a scaffold |
| [0010-vulkan-bootstrap](0010-vulkan-bootstrap.md) | ADR-0010 — Vulkan bootstrap |
| [0011-first-triangle](0011-first-triangle.md) | ADR-0011 — First triangle milestone |
| [0012-config-substrate](0012-config-substrate.md) | ADR-0012 — Configuration substrate |
| [0013-asset-pipeline](0013-asset-pipeline.md) | ADR-0013 — Asset pipeline |
| [0014-refcount-split](0014-refcount-split.md) | ADR-0014 — Reference counting split |
| [0015-job-system](0015-job-system.md) | ADR-0015 — Job system shape |
| [0016-render-path](0016-render-path.md) | ADR-0016 — Render path strategy |
| [0017-culling-strategy](0017-culling-strategy.md) | ADR-0017 — Culling strategy |
| [0018-physics-architecture](0018-physics-architecture.md) | ADR-0018 — Physics architecture |
| [0019-reserved](0019-reserved.md) | ADR-0019 — (reserved) |
| [0020-scene-ecs-hybrid](0020-scene-ecs-hybrid.md) | ADR-0020 — Scene & ECS hybrid + UI in scene tree |
| [0021-animation-architecture](0021-animation-architecture.md) | ADR-0021 — Animation architecture |
| [0022-streaming-pipeline](0022-streaming-pipeline.md) | ADR-0022 — Open-world streaming pipeline |
| [0023-ui-architecture](0023-ui-architecture.md) | ADR-0023 — UI architecture |
| [0024-imgui-single-viewport-default](0024-imgui-single-viewport-default.md) | ADR-0024 — ImGui single-viewport default |
| [0025-shader-mechanism-policy](0025-shader-mechanism-policy.md) | ADR-0025 — Shader mechanism policy |
| [0026-shader-variant-key](0026-shader-variant-key.md) | ADR-0026 — Shader variant key |
| [0027-shader-reflection-consumption](0027-shader-reflection-consumption.md) | ADR-0027 — Shader reflection consumption model |
| [0028-shader-cache-hierarchy](0028-shader-cache-hierarchy.md) | ADR-0028 — Shader cache hierarchy |
| [0029-shader-hot-reload](0029-shader-hot-reload.md) | ADR-0029 — Shader hot reload |
| [0030-shader-pso-boundary](0030-shader-pso-boundary.md) | ADR-0030 — Shader / PSO boundary |
| [0031-shader-frontend-ir-seam](0031-shader-frontend-ir-seam.md) | ADR-0031 — Shader frontend → IR seam |
| [0032-frame-graph-v1](0032-frame-graph-v1.md) | ADR-0032 — Frame graph v1 |
| [0033-jobs-implementation-architecture](0033-jobs-implementation-architecture.md) | ADR-0033 — crd-jobs implementation architecture |
| [0034-cpp-hotreload-scripting](0034-cpp-hotreload-scripting.md) | ADR-0034 — C++ hot-reload DLL scripting as primary scripting mechanism |
| [0035-networking-architecture](0035-networking-architecture.md) | ADR-0035 — Networking architecture principles |
| [0036-resources-module-and-loader-registry](0036-resources-module-and-loader-registry.md) | ADR-0036 — `crd-resources` module placement + loader-registry pattern |
| [0037-resource-id-uuid-scheme](0037-resource-id-uuid-scheme.md) | ADR-0037 — ResourceId hybrid UUID scheme |
| [0038-cooked-binary-container-format](0038-cooked-binary-container-format.md) | ADR-0038 — Cooked binary container format |
| [0039-resource-handle-semantics](0039-resource-handle-semantics.md) | ADR-0039 — `ResourceHandle<T>` semantics |
| [0040-cooker-cli-cmake-integration](0040-cooker-cli-cmake-integration.md) | ADR-0040 — Cooker CLI + CMake integration |
| [0041-platform-async-filesystem-io](0041-platform-async-filesystem-io.md) | ADR-0041 — `crd-platform` async filesystem I/O |
| [0042-texture-resource-cooked-format](0042-texture-resource-cooked-format.md) | ADR-0042 — Texture cooked format + GPU upload strategy |
| [0043-mesh-resource-gltf-import](0043-mesh-resource-gltf-import.md) | ADR-0043 — MeshResource vertex layout + glTF import scope |
| [0044-phase-ordering-material-before-scene](0044-phase-ordering-material-before-scene.md) | ADR-0044 — Phase ordering: material PSO/variant completion precedes scene/ECS |
| [0045-sandbox-asset-layout-cook-meshgen](0045-sandbox-asset-layout-cook-meshgen.md) | ADR-0045 — Sandbox executable, asset source layout, cook workflow, crd-meshgen |
| [0046-material-domain-rt-hybrid-strategy](0046-material-domain-rt-hybrid-strategy.md) | ADR-0046 — MaterialDomain enum, node-editor future-proofing, RT hybrid strategy |
| [0047-font-rendering-system](0047-font-rendering-system.md) | ADR-0047 — Font rendering system |
| [0048-material-system-architecture](0048-material-system-architecture.md) | ADR-0048 — Material System Architecture Foundation |
| [0049-scene-entity-identity-slotmap](0049-scene-entity-identity-slotmap.md) | ADR-0049 — Scene/ECS L1: Entity identity & SlotMap |
| [0050-scene-storage-backends](0050-scene-storage-backends.md) | ADR-0050 — Scene/ECS L2: Storage backends (Archetype + SparseSet hybrid) |
| [0051-scene-relations-first-class](0051-scene-relations-first-class.md) | ADR-0051 — Scene/ECS L3: Relations as first-class |
| [0052-scene-query-system-schedule](0052-scene-query-system-schedule.md) | ADR-0052 — Scene/ECS L4: Query · System · Schedule |
| [0053-scene-component-index-framework](0053-scene-component-index-framework.md) | ADR-0053 — Scene/ECS L5: Component index slot framework |
| [0054-scene-transform-hierarchy-update](0054-scene-transform-hierarchy-update.md) | ADR-0054 — Scene/ECS: Transform hierarchy update model |
| [0055-scene-serialization-toml-scen-crdr](0055-scene-serialization-toml-scen-crdr.md) | ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked |
| [0056-scene-reserved-l6-l8-slots](0056-scene-reserved-l6-l8-slots.md) | ADR-0056 — Scene/ECS L6–L8: Reserved API slots (Replication, Scripts, Reflection) |
| [0057-scene-ui-in-tree-boundary](0057-scene-ui-in-tree-boundary.md) | ADR-0057 — Scene/ECS: UI nodes in scene tree (boundary declaration) |
| [0058-obek-system](0058-obek-system.md) | ADR-0058 — Öbek System: cooked entity-graph templates with composition, variation, and AAAA-tier future-proofing |
| [0059-preset-system](0059-preset-system.md) | ADR-0059 — Preset System: typed system-config bags with five-layer resolution and `extends`-chain composability |
| [0060-profile-system](0060-profile-system.md) | ADR-0060 — Profile System: typed predicate selectors with additive composition for cross-domain runtime configuration |
| [0061-async-gpu-upload-contract](0061-async-gpu-upload-contract.md) | ADR-0061 — Async GPU upload contract: `UploadHandle` + per-module polling system |
| [0062-eylem-physics-architecture](0062-eylem-physics-architecture.md) | ADR-0062 — Eylem: Cerid-native physics architecture |
| [0063-eylem-determinism-contract](0063-eylem-determinism-contract.md) | ADR-0063 — Eylem determinism contract |
| [0064-sdf-substrate-architecture](0064-sdf-substrate-architecture.md) | ADR-0064 — `crd-sdf` substrate architecture |
| [0065-hesap-numerical-substrate](0065-hesap-numerical-substrate.md) | ADR-0065 — `crd-hesap` numerical computing substrate |
| [0066-draw-substrate-architecture](0066-draw-substrate-architecture.md) | ADR-0066 — `crd-draw` substrate architecture |
| [0067-eylem-force-field-architecture](0067-eylem-force-field-architecture.md) | ADR-0067 — Eylem force-field architecture |
| [0068-eylem-body-types-collision-filtering-callbacks](0068-eylem-body-types-collision-filtering-callbacks.md) | ADR-0068 — Eylem body types + collision filtering + contact callbacks |
| [0069-eylem-materials-substrate](0069-eylem-materials-substrate.md) | ADR-0069 — Eylem materials substrate |
| [0075-eylem-testing-rigor](0075-eylem-testing-rigor.md) | ADR-0075 — Eylem testing rigor and conservation-law CI |
| [0076-geometry-substrate-architecture](0076-geometry-substrate-architecture.md) | ADR-0076 — `crd-geometry` substrate architecture |
| [0077-multi-domain-expansion-vision](0077-multi-domain-expansion-vision.md) | ADR-0077: Multi-domain expansion — the "absolute beast" vision |
| [0078-units-substrate-architecture](0078-units-substrate-architecture.md) | ADR-0078 — `crd-units` substrate architecture |
| [0079-crd-perf-substrate-architecture](0079-crd-perf-substrate-architecture.md) | ADR-0079 — `crd-perf` profiler substrate + ImGui frontend |
| [0080-crd-rhi-compute](0080-crd-rhi-compute.md) | ADR-0080 — `crd-rhi-compute` substrate |
| [0081-agent-native-engine-cli](0081-agent-native-engine-cli.md) | ADR-0081 — Agent-Native Engine: CLI + RPC + MCP Substrate |
| [0082-hesap-microkernel-intrinsics-strategy](0082-hesap-microkernel-intrinsics-strategy.md) | ADR-0082 — Hesap GEMM microkernel: intrinsics-via-Vec8f/Vec16f, ASM deferred |
| [0083-hesap-dense-rowmajor-storage](0083-hesap-dense-rowmajor-storage.md) | ADR-0083 — hesap-dense row-major storage (with per-factor escape hatch) |
| [0084-hesap-matrix-as-resource](0084-hesap-matrix-as-resource.md) | ADR-0084 — Sparse matrices as first-class cooked engine resources (`crd-hesap-resources`) |
| [0085-streaming-allocator-cluster](0085-streaming-allocator-cluster.md) | ADR-0085 — Virtual-memory + streaming allocator cluster |
| [0086-eylem-physics-animation-unification](0086-eylem-physics-animation-unification.md) | ADR-0086 — Eylem unified motion model: physics-animation as one solve, LOD fidelity continuum, crowd-scale deterministic networking |
| [0087-large-scale-deterministic-simulation](0087-large-scale-deterministic-simulation.md) | ADR-0087 — Large-scale deterministic simulation: environmental fields, surface-integral coupling, moving-frame agents, and player-count-scaled networking |
| [0088-gemm-runtime-dispatch-asm-microkernels](0088-gemm-runtime-dispatch-asm-microkernels.md) | ADR-0088 — GEMM hand-tuned asm: INVESTIGATED → REVERTED (intrinsics vindicated) |
| [0089-hesap-eigen-sparse-eigensolvers](0089-hesap-eigen-sparse-eigensolvers.md) | ADR-0089 — crd-hesap-eigen: sparse eigensolvers + module edges (Phase 3.1.6 v6) |
| [0090-hesap-opt-optimization-domain](0090-hesap-opt-optimization-domain.md) | ADR-0090 — crd-hesap-opt: the optimization domain (substrate + contracts + module edges, Phase 3.1.6 v7) |
| [0091-hesap-ode-module](0091-hesap-ode-module.md) | ADR-0091 — `crd-hesap-ode`: the ODE/DAE module — two API layers, deterministic controllers, the work-precision contract |
| [0092-hesap-fft-cluster](0092-hesap-fft-cluster.md) | ADR-0092 — `crd-hesap-fft`: the FFT cluster — deterministic plan-from-factorization, portable-C++ MKL-adjacent, the full transform suite |
| [0093-hesap-dsp-cluster](0093-hesap-dsp-cluster.md) | ADR-0093 — `crd-hesap-dsp` (+ `-wavelet` + `-comms`): the DSP cluster — the design/application honest-gate split, SOS-by-default, the two-layer streaming contract |
| [0094-hesap-statistics-cluster](0094-hesap-statistics-cluster.md) | ADR-0094 — `crd-hesap-special` + `crd-hesap-stats`: the statistics cluster — special-as-leaf, the inverse-incomplete cdf/ppf engine, the counter-RNG determinism moat, and the tw… |
| [0095-hesap-numerical-analysis-motion-cluster](0095-hesap-numerical-analysis-motion-cluster.md) | ADR-0095 — the v13 Numerical-Analysis + Motion cluster: the 4-module split, the three certification moat pillars, and the error-tier contract |
| [0096-hesap-tensor-cluster](0096-hesap-tensor-cluster.md) | ADR-0096 — crd-hesap-tensor: the N-D tensor substrate — templated compute dtypes over stride views, the two-tier deterministic-reduction contract, deterministic stochastic round… |
| [0097-hesap-autodiff-cluster](0097-hesap-autodiff-cluster.md) | ADR-0097 — crd-hesap-autodiff: the automatic-differentiation cluster — one module for forward + reverse, the deterministic no-atomics tape, suite-wide differentiability, differe… |
| [0098-hesap-gpu-cluster](0098-hesap-gpu-cluster.md) | ADR-0098 — crd-kir + crd-hesap-gpu: the Cerid GPU compute compiler — a unified compute+autodiff kernel IR lowering to six backends, vendor-beating kernels, and certified cross-v… |
| [0099-compute-rendering-separation](0099-compute-rendering-separation.md) | ADR-0099 — `crd-gpu-context`: a backend-agnostic GPU **context manager**; compute and rendering as independent, composable consumers |
| [0100-ckir-one-gpu-compute-manager](0100-ckir-one-gpu-compute-manager.md) | ADR-0100 — CKIR is the one GPU compute manager: a *kernel-source-agnostic* dispatch surface serving both compiler-authored and hand-written kernels |
| [0101-ir-is-source-of-truth-for-all-shaders](0101-ir-is-source-of-truth-for-all-shaders.md) | ADR-0101 — The IR is the single source of truth for every shader (compute AND material); backend languages are outputs only |
| [0102-render-data-lighting-pass-architecture](0102-render-data-lighting-pass-architecture.md) | ADR-0102 — Render-data, lighting & pass architecture: how the shader IR feeds a frontier renderer |
| [0103-gpu-context-owns-every-gpu-program](0103-gpu-context-owns-every-gpu-program.md) | ADR-0103 — `crd-gpu-context` owns every GPU program and pipeline; **no module outside a backend names a shading language or a bytecode** |
| [0104-ir-as-crdr-shader-cook-deploy](0104-ir-as-crdr-shader-cook-deploy.md) | ADR-0104 — IR-as-`crdr`: the shader cook & deploy pipeline (D1–D5) |
| [0105-retire-rhi-renderer-gpu-context-is-the-graphics-layer](0105-retire-rhi-renderer-gpu-context-is-the-graphics-layer.md) | ADR-0105 — Retire crd-rhi + crd-renderer: crd-gpu-context IS the graphics layer |
| [0106-unified-frame-graph-runtime-render-graph](0106-unified-frame-graph-runtime-render-graph.md) | ADR-0106 — Unified frame-graph runtime: `crd-render-graph` is the single live runtime |
| [0107-ui-2d-architecture](0107-ui-2d-architecture.md) | ADR-0107 — Interactive UI + 2D rendering architecture: `UiWorld`, `CanvasCompositor`, and the paint-to-command seam |
| [0108-ceir-owned-language-stack-supersedes-cpp-only-scripting](0108-ceir-owned-language-stack-supersedes-cpp-only-scripting.md) | ADR-0108 — A Cerid-owned executable-program language stack (CEIR/CHIR); C++ is no longer the *only* authorable program |
| [0109-ceir-chir-ckir-ownership-and-module-placement](0109-ceir-chir-ckir-ownership-and-module-placement.md) | ADR-0109 — CEIR / CHIR / CKIR ownership, the one-way layer contract, and `crd-ceir` module placement |
| [0110-native-intrinsic-schema-and-plugin-levels](0110-native-intrinsic-schema-and-plugin-levels.md) | ADR-0110 — Native-intrinsic schema, the legitimacy rule, and the three plugin-extension levels |
| [0111-open-world-type-model](0111-open-world-type-model.md) | ADR-0111 — Open-world type model: dialect-defined type-classes beside the built-in `TypeKind` |
| [0112-open-world-attribute-model](0112-open-world-attribute-model.md) | ADR-0112 — Open-world attribute model: aggregate kinds + dialect-defined attribute-classes |
| [0113-effect-widening-and-open-effect-locations](0113-effect-widening-and-open-effect-locations.md) | ADR-0113 — Effect family widening (u32→u64) + open-world effect-LOCATION model |
| [0114-stable-semantic-identity](0114-stable-semantic-identity.md) | ADR-0114 — Stable semantic identity: content-independent stable ids for ops/functions/state-slots |
| [0115-trait-interface-split-and-region-reservation](0115-trait-interface-split-and-region-reservation.md) | ADR-0115 — Trait/interface split + region-kind reservation |
| [0116-capabilities-safety-split-time-domains](0116-capabilities-safety-split-time-domains.md) | ADR-0116 — Capability contracts + domain/safety split + typed time domains |
| [0117-compiler-infrastructure-skeleton](0117-compiler-infrastructure-skeleton.md) | ADR-0117 — Compiler-infrastructure skeleton: analysis/pass managers, rewrite/conversion, diagnostics |
| [0118-incremental-evaluation-unification](0118-incremental-evaluation-unification.md) | ADR-0118 — Incremental-evaluation unification: one dependency/dirty engine (crd-containers::IncrementalDag) |
| [0119-transactions](0119-transactions.md) | ADR-0119 — Transactions: the atomic authored-mutation surface (commit / rollback) |
| [0120-hot-reload-and-state-migration](0120-hot-reload-and-state-migration.md) | ADR-0120 — Hot-reload lifecycle + state migration (asset lifecycle completion) |
| [0121-execution-plan-cache](0121-execution-plan-cache.md) | ADR-0121 — Execution-plan cache: a validate-on-hit store of live truth |
| [0122-reference-executor-full-host-subset](0122-reference-executor-full-host-subset.md) | ADR-0122 — Reference executor: the full host subset (task vocabulary · sequential parallel reference · step hooks · ON-POOL async) |
| [0123-compiled-execution-plan](0123-compiled-execution-plan.md) | ADR-0123 — The compiled execution plan (CEIR-11b): a dense two-tier, differential-verified against the §118 oracle |
| [0124-memory-planner](0124-memory-planner.md) | ADR-0124 — The memory planner (CEIR-12d): interval-coloring over the §26/§78 lifetime analysis, an inspectable plan |
| [0125-ceir-gpu-lowering-bridge](0125-ceir-gpu-lowering-bridge.md) | ADR-0125 — The `crd-ceir-gpu` lowering bridge (CEIR-13d): CEIR compute/transfer regions → an inspectable command list |
| [0126-ceir-gpu-execution-seam](0126-ceir-gpu-execution-seam.md) | ADR-0126 — the CEIR-13z GPU execution seam (`execute_lowered` on IComputeContext) |
| [0127-ceir-frame-dialect-and-converter](0127-ceir-frame-dialect-and-converter.md) | ADR-0127 — the `ceir.frame` dialect + the FrameGraphDesc↔ceir.frame converter placement |
| [0128-chir-0-language-binding-decisions](0128-chir-0-language-binding-decisions.md) | ADR-0128 — CHIR-0: the Cerid high-level language, binding decisions against the corpus |
| [0129-renderer-ui-editor-delivery-order](0129-renderer-ui-editor-delivery-order.md) | ADR-0129 — Renderer, UI, CR-D007 and notebook delivery order |
