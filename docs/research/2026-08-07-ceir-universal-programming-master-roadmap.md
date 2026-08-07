# D-007 — CEIR UNIVERSAL PROGRAMMING SYSTEM
## Frontier-complete master roadmap for asset-authored execution, visual programming, scripting, GPU/CPU/accelerator orchestration, and CKIR integration

**Status:** proposed architectural priority; supersedes earlier narrower PassIR / CEIR drafts  
**Scope:** Cerid-wide programming substrate, not renderer-only  
**Date context:** 2026-08-07  
**Canonical destination:** integrate into the current *working-tree* D-007 after auditing the repository. Do not overwrite uncommitted D-007 work.

---

# 0. QUEST FOR THE AGENT

Before changing code or D-007:

1. Read the current working tree, not only `main`.
2. Read the current uncommitted `docs/detours/D-007-gpu-program-system.md`.
3. Read `docs/PRINCIPLES.md`, `AGENTS.md`, `docs/ROADMAP.md`, the rendering-foundation system docs, RAF/RAH docs, CKIR docs, asset/cooker docs, hot-reload docs, frame-graph implementation, executor registry, command model, CRD-Hesap GPU-related work, UI/2D planning, media plans, and relevant tests.
4. Inventory every current execution-program representation and every path that directly records or schedules work.
5. Preserve all finished RAF/RAH work.
6. Do not create a parallel architecture.
7. Update D-007 so CEIR becomes the canonical next architectural layer.
8. Do not broadly implement Forward+, Lumen-class GI, Nanite-class geometry, ML, UI effects, or other large feature libraries in old executor form while CEIR migration is incomplete.
9. Finish any currently half-applied migration that would leave the tree unsafe, then pause broad feature expansion and build CEIR.
10. Treat the code as truth. Any stale documentation must be explicitly marked historical or corrected.

This is not a request for a small visual-scripting feature.

This is a request to build Cerid's **universal executable-program substrate**.

---

# 1. THE NON-NEGOTIABLE NORTH STAR

Cerid shall support this statement:

> Every reusable algorithm/program that can be expressed using capabilities Cerid already understands should be representable as a versioned, inspectable, serializable, hot-reloadable Cerid program asset, whether that program performs rendering, GPU compute, ray tracing, ML/AI, scientific computing, media processing, geometry processing, UI composition, physics orchestration, animation evaluation, audio/DSP work, or application logic.

And:

> New native C++ is required only to introduce a genuinely new host capability, operating-system/external integration, device/runtime provider, or hardware primitive that Cerid did not previously understand—not merely because somebody invented a new algorithm using existing capabilities.

And:

> Programmatic construction remains fully supported, but programmatic construction emits the exact same canonical IR, runs through the exact same verifier/compiler/runtime, and can be inspected or serialized. There is no privileged programmatic bypass.

And:

> Visual authoring and textual authoring are two projections of the same typed semantic program model. A visual program is not a toy layer and a textual program is not a separate runtime.

The architectural mantra is:

```text
ALGORITHMS ARE PROGRAM ASSETS.
CAPABILITIES ARE NATIVE PRIMITIVES.
COMPILERS CHOOSE LOWERINGS.
BACKENDS EXECUTE.
```

---

# 2. IMPORTANT CORRECTION TO THE EARLIER "PASS IR" IDEA

Do not build a renderer-specific PassIR.

That would be too narrow.

The correct upper concept is:

# **CEIR — Cerid Execution Intermediate Representation**

CEIR is the common execution/orchestration IR for heterogeneous programs.

CKIR remains distinct:

# **CKIR — Cerid Kernel Intermediate Representation**

CKIR answers:

> What does a GPU invocation / work item / shader invocation compute?

CEIR answers:

> What work exists, what data/resources flow between work units, what controls execution, where can work run, how can work generate more work, how are resources synchronized, and which lower-level programs implement each operation?

A future Cerid scripting language may lower into CEIR, but CEIR should not be confused with the complete high-level source language.

---

# 3. FINAL PROGRAMMING STACK

Use a multi-level architecture.

```text
┌───────────────────────────────────────────────────────┐
│ AUTHORING / SOURCE                                    │
│                                                       │
│ Cerid textual language                               │
│ CR-D007 visual language                              │
│ frame/material/compute/tensor domain frontends       │
│ C++ programmatic builder                             │
│ importers: ONNX/StableHLO/MaterialX/etc.             │
└───────────────────────────┬───────────────────────────┘
                            │
                            ▼
┌───────────────────────────────────────────────────────┐
│ CHIR — Cerid High-level IR (future language layer)    │
│                                                       │
│ modules, generics, ADTs, functions, closures,         │
│ async/state/events, ownership, reflection, language   │
│ semantics not appropriate to bake directly into CEIR │
└───────────────────────────┬───────────────────────────┘
                            │ lowering / partial lowering
                            ▼
┌───────────────────────────────────────────────────────┐
│ CEIR — Cerid Execution IR                             │
│                                                       │
│ typed SSA + graph regions + CFG regions               │
│ resources + effects + capabilities                    │
│ heterogeneous scheduling                              │
│ render / compute / tensor / RT / media / UI / etc.    │
└───────────────┬──────────────────────┬────────────────┘
                │                      │
                │ kernels              │ host/device graph programs
                ▼                      ▼
┌────────────────────────┐   ┌───────────────────────────┐
│ CKIR                   │   │ Execution Providers       │
│ per-invocation compute │   │ native graph/program APIs │
│ shader/kernel IR       │   │ CPU/GPU/NPU/media/etc.    │
└────────────┬───────────┘   └──────────────┬────────────┘
             │                               │
             └──────────────┬────────────────┘
                            ▼
┌───────────────────────────────────────────────────────┐
│ COMPILED EXECUTION PLAN                               │
│                                                       │
│ CPU jobs/native calls                                 │
│ canonical GPU commands                                │
│ persistent graphs                                     │
│ Work Graphs / DGC / ICB                               │
│ native tensor/data-graph programs                     │
│ distributed communication                             │
└───────────────────────────┬───────────────────────────┘
                            ▼
       Vulkan / D3D12 / CUDA / HIP / Metal / WebGPU
       CPU / SIMD / jobs / NPU / codecs / remote nodes
```

The names `CHIR` and exact file extensions may be revisited by ADR, but the **layer separation may not**.

---

# 4. WHY CHIR EXISTS ABOVE CEIR

Do not force every future scripting-language concept into the execution IR.

A source language may need:

- modules/namespaces,
- user-defined types,
- algebraic data types,
- pattern matching,
- generics,
- interfaces/traits,
- closures,
- coroutines,
- events,
- state machines,
- reflection,
- compile-time evaluation,
- attributes,
- package imports,
- high-level ownership rules,
- language-level error handling,
- user-friendly syntax.

Some of these lower naturally into CEIR.

Some are erased or transformed before execution planning.

Therefore:

```text
Cerid Script / Visual Source
        ↓
CHIR
        ↓
CEIR
        ↓
CKIR / host-native / graph providers
```

A CR-D007 graph may also directly author CEIR when the domain is naturally execution-graph-shaped, such as:

- frame graphs,
- compute graphs,
- tensor graphs,
- media graphs,
- UI effect graphs,
- geometry processing graphs,
- node-based renderer programs.

---

# 5. THIS REQUIRES A NEW SCRIPTING ADR

Current Cerid principles state that C++ hot reload is the only scripting language.

The user's direction has changed.

Do not silently contradict the existing principle.

Open a new ADR that explicitly supersedes the old "C++-only scripting" decision.

The ADR must state:

- C++ remains a first-class native extension/programmatic language.
- C++ hot reload remains useful.
- Cerid gains a Cerid-owned textual/visual programmable language stack.
- No Lua/Python/JavaScript runtime dependency is required for the core language.
- CEIR/CHIR are Cerid-owned.
- Application programs may still be written in C++.
- C++ is no longer the *only* authorable executable-program representation.
- CLI/RPC/MCP remains a source-of-truth automation surface.
- Program assets become agent-authorable and machine-inspectable.

Update README/PRINCIPLES only after the ADR is accepted and implementation has a real vertical slice.

---

# 6. OPEN-WORLD COMPLETENESS — THE ONLY WAY TO AVOID FUTURE HOLES

No finite opcode list can guarantee "every future algorithm."

Completeness comes from **open-world extensibility**.

CEIR must support:

1. extensible dialects,
2. extensible operations,
3. extensible types,
4. operation interfaces,
5. custom verifiers,
6. custom canonicalization,
7. custom lowerings,
8. declarative subgraph-defined operations,
9. native intrinsics when genuinely necessary,
10. plugin/provider registration without editing a central enum,
11. unknown-dialect preservation in tools when the defining plugin is absent,
12. capability and version negotiation.

A new algorithm composed from existing operations is just a new asset.

A new reusable abstraction can be a CEIR function/subgraph or a custom high-level op that lowers to existing ops.

Only a genuinely new primitive capability requires native implementation.

---

# 7. DO NOT IMPLEMENT CEIR AS A GIANT ENUM

Forbidden destination:

```cpp
enum CeirOp {
    Draw,
    Dispatch,
    Trace,
    ForwardPlus,
    Deferred,
    Nanite,
    Lumen,
    ...
};
```

That repeats the exact combinatorial mistake RAF removed.

Instead use MLIR-like structural ideas:

- `Operation`
- typed operands/results
- attributes/properties
- regions
- blocks
- symbols
- interfaces/traits
- dialect registry
- verifier callbacks
- parser/printer schemas
- lowering hooks

Compiler analyses operate against semantic interfaces, not giant `switch(op.kind)` statements.

---

# 8. TABLE-DRIVEN OPERATION DEFINITIONS

Cerid should own a schema/definition system analogous in spirit to MLIR ODS/TableGen.

An operation definition should describe:

- canonical name,
- dialect,
- version,
- inputs,
- outputs,
- variadicity,
- attributes,
- properties,
- regions,
- traits,
- effects,
- execution-domain legality,
- capability requirements,
- type inference,
- shape inference,
- verification,
- fold rules,
- serialization schema,
- editor presentation hints,
- documentation,
- deprecation/migration metadata.

From the schema generate as much boilerplate as possible:

- C++ strongly typed wrapper,
- builder,
- verifier scaffolding,
- parser/printer hooks,
- reflection schema,
- CR-D007 node metadata,
- CLI/MCP schema,
- documentation,
- test skeleton.

Avoid stringly-typed compiler code.

---

# 9. VISUAL LANGUAGE IS A FIRST-CLASS SOURCE PROJECTION

CR-D007 must eventually be able to author real programs.

Do not make a separate visual-scripting runtime.

Visual nodes must map to the same semantic operations/functions/types used by textual programs.

Support several visual "lenses" rather than forcing everything into one spaghetti graph:

## Dataflow lens

Best for:

- materials,
- math,
- tensors,
- compute pipelines,
- image processing,
- geometry processing.

## Structured control-flow lens

Best for:

- `if`,
- `switch`,
- loops,
- region bodies,
- tasks,
- error handling.

## Frame/workflow lens

Best for:

- resources,
- passes,
- scheduling,
- queue/domain boundaries.

## State-machine lens

Best for:

- UI state,
- gameplay state,
- animation states,
- protocol/workflow states.

## Reactive/event lens

Best for:

- events,
- signals,
- subscriptions,
- input/UI logic.

## Timeline lens

Best for:

- animation,
- sequencer,
- media,
- audio,
- simulation time.

All lenses project the same semantic program/module when appropriate.

---

# 10. TEXT/VISUAL ROUND TRIP

The roadmap must explicitly solve text/graph coexistence.

Requirements:

- stable semantic operation IDs,
- graph layout metadata separated from semantic meaning,
- deterministic canonical printer,
- source spans,
- comments/documentation retained where practical,
- subgraph/function identities stable across formatting,
- semantic diff independent of node position,
- merge-friendly asset format,
- text edits reparse into the same semantic model,
- graph edits update the same source model,
- no visual-only runtime behavior,
- no hidden textual executor.

Recommended model:

```text
Structured source model
    semantic nodes + stable IDs
    textual source spans
    graph layout metadata
        ↓
CHIR / CEIR
```

The exact persistence format can be decided after a prototype, but the semantic identity model must be designed before the editor.

---

# 11. USER-DEFINED FUNCTIONS AUTOMATICALLY BECOME NODES

A user-written function/subgraph with a valid typed interface should automatically be usable as a visual node.

Example:

```text
func BuildLightGrid(
    depth: image<D32F>,
    lights: buffer<Light>
) -> buffer<LightIndex>
```

CR-D007 can generate:

```text
[Build Light Grid]
depth  ──▶
lights ──▶
         └──▶ light indices
```

Node metadata can come from:

- function signature,
- annotations,
- docstrings,
- type names,
- parameter ranges,
- units,
- categories,
- preview hints.

A reusable graph is therefore a function/program asset, not a hard-coded editor node class.

---

# 12. CORE VALUE MODEL — TYPED SSA

CEIR uses typed SSA where value/dataflow semantics benefit from it.

Example:

```text
%depth = frame.input @depth
%hzb   = compute.call @build_hzb(%depth)
%vis   = compute.call @occlusion(%hzb, %instances)
render.scene %vis
```

Benefits:

- dependency analysis,
- use-def chains,
- constant propagation,
- DCE,
- CSE,
- shape reasoning,
- alias analysis,
- resource versioning,
- provenance,
- debugger inspection,
- source mapping,
- graph visualization.

Do not force mutable variable semantics when SSA is cleaner.

Use explicit state/memory objects where mutation is semantically real.

---

# 13. REGIONS — BOTH GRAPH AND CFG

CEIR must support more than one region semantic.

At minimum:

## Graph regions

Order primarily derived from data/effects.

Useful for:

- frame graphs,
- tensor graphs,
- compute DAGs,
- effect graphs.

## SSACFG regions

Explicit basic blocks/branches.

Useful for lower-level control flow.

## Structured regions

Convenience operations:

```text
if
switch
for
foreach
while
match
scope
try/result handling
async
race
parallel
pipeline
```

These may lower to CFG/dataflow/device graph semantics.

Do not make users manually wire basic-block jumps for ordinary visual programming.

---

# 14. CONTROL FLOW MUST BE COMPLETE

Support:

- `if/else`,
- `switch`,
- pattern matching,
- counted loops,
- range loops,
- while loops,
- foreach over collections/views/work sets,
- break/continue,
- early return,
- function calls,
- recursion where legal,
- bounded recursion attributes,
- tail recursion optimization where useful,
- stateful loops,
- feedback edges only through explicit state/delay constructs,
- device-conditional execution,
- graph conditional execution,
- device-driven loops,
- async loops,
- cancellation.

Every control construct must declare where it can execute.

---

# 15. EVALUATION DOMAINS

A major CEIR feature should be explicit evaluation domain.

Candidate domains:

```text
CompileTime
CookTime
LoadTime
HostFrameTime
HostSimulationTime
HostAudioTime
DeviceTime
OfflineTime
DistributedTime
EitherHostOrDevice
```

Examples:

```text
authored cascade count
    CookTime

ray-tracing capability
    LoadTime

camera transform
    HostFrameTime

visible cluster count after cull
    DeviceTime

offline path-tracing convergence loop
    OfflineTime
```

This enables partial evaluation and specialization.

Do not repeatedly execute work whose inputs are static at a higher domain.

---

# 16. TYPE SYSTEM — GENERAL

CEIR/CHIR type architecture must cover:

## Scalars

- bool,
- signed/unsigned integers,
- f16,
- bf16,
- f32,
- f64,
- future FP8 types,
- index/size types,
- arbitrary/fixed precision where later useful.

## Aggregates

- vectors,
- long vectors,
- matrices,
- quaternions,
- complex,
- arrays,
- tuples,
- structs,
- enums,
- tagged unions / variants,
- option,
- result.

## Generics

- generic functions,
- generic structs,
- generic operation definitions,
- specialization constraints.

## Interfaces/traits

Allow algorithms to depend on capabilities/contracts instead of concrete types.

## Function/callable types

Needed for callbacks, regions, higher-order utilities and future language features.

---

# 17. PHYSICAL QUANTITIES AND UNITS

Cerid's existing upper-layer unit discipline must survive.

High-level CEIR/CHIR APIs may carry:

```text
Length<f32>
Time<f64>
Velocity<f32>
Force<f32>
Angle<f32>
Frequency<f64>
```

Dimensional errors must be caught before lowering.

At the explicit kernel/raw boundary these may lower to canonical SI scalars/vectors according to existing Cerid principles.

Do not leak untagged physical numerics into public CEIR domain APIs.

---

# 18. SEMANTIC DOMAIN TYPES

Permit higher-level domain types where they prevent bugs or enable optimization.

Examples:

- coordinate-space-tagged positions/vectors,
- linear color vs encoded color,
- normals/tangents,
- time domains,
- sample/frame indices,
- tensor shapes/layouts,
- image formats,
- access modes,
- resource states as semantic intents,
- entity/query handles,
- audio sample rates/channel layouts.

Do not over-tag inner hot-loop primitives unnecessarily.

---

# 19. OWNERSHIP, REFERENCES, LIFETIMES

If CEIR participates in future scripting, do not rely on arbitrary raw pointers.

Define explicit categories:

- immutable value,
- mutable value,
- borrowed view,
- owned resource,
- shared handle,
- weak handle,
- runtime state slot,
- external handle,
- transient arena value.

The high-level language should support deterministic lifetime semantics without requiring a mandatory tracing GC for hot-path code.

Possible implementation strategies may include:

- value semantics,
- ownership/borrowing,
- arenas,
- intrusive/shared handles,
- region lifetimes,
- explicit state stores.

The exact source syntax needs its own language ADR, but CEIR must preserve enough lifetime information for safe execution.

---

# 20. STATE IS EXPLICIT

Separate program assets from runtime state.

Program asset:

```text
engine://ceir/taa
```

Runtime state:

```text
history texture
jitter index
exposure history
```

Represent persistent state explicitly:

```text
state<T>
history<T>
delay<T>
persistent_resource<T>
```

Graph cycles must pass through explicit state/delay semantics.

This applies to:

- TAA,
- GI histories,
- simulation,
- RNN/KV cache,
- audio delay lines,
- UI state,
- gameplay state.

---

# 21. SHAPES AND DYNAMIC DIMENSIONS

Tensor/array/image workloads need first-class shape reasoning.

Support:

- static rank/static dimensions,
- static rank/dynamic dimensions,
- symbolic dimensions,
- shape constraints,
- broadcast constraints,
- runtime shape checks,
- shape functions,
- layout constraints,
- specialization from observed shapes.

Never encode shape rules only as ad-hoc runtime checks inside kernels.

---

# 22. DATA LAYOUT

Layout is not merely a tensor detail.

Represent abstract layouts for:

- tensors,
- matrices,
- images,
- structured buffers,
- vertex data,
- AoS/SoA,
- blocked/tiled layouts,
- swizzles,
- packed quantized weights,
- sparse formats.

High-level algorithms should express semantic data.

Compiler/provider chooses physical layout where possible.

Allow explicit layout constraints where interop or algorithms require them.

---

# 23. RESOURCE TYPES

First-class resources:

```text
buffer<T>
buffer_view<T>
raw_buffer
structured_buffer<T>
typed_buffer<T>
image<Dim,Format>
image_view<...>
tensor<Shape,Element,Layout>
sparse_tensor<...>
sampler
comparison_sampler
resource_table<T>
acceleration_structure
video_frame
audio_buffer
external_resource
```

Resource views include:

- byte range,
- element range,
- mip range,
- layer range,
- aspect,
- plane,
- tensor slice,
- sparse tile/page range.

---

# 24. RESOURCE MEMORY DOMAINS

Model intent for:

- host memory,
- pinned host memory,
- device-local,
- host-visible device,
- unified/shared,
- upload,
- readback,
- transient,
- persistent,
- sparse/virtual,
- external/imported,
- peer-visible,
- distributed/remote.

Do not expose Vulkan memory types or D3D heap flags as source semantics.

Providers lower abstract intent to API details.

---

# 25. RESIDENCY AND STREAMING

CEIR resource planning must eventually model:

- residency,
- eviction,
- sparse pages,
- virtual textures,
- virtual geometry,
- streaming priorities,
- budget classes,
- prefetch,
- feedback-driven residency,
- external memory,
- memory aliasing,
- tensor/image aliasing,
- zero-copy interop.

This matters for Nanite-class geometry, virtual texturing, media, huge scientific data, ML weights and multi-GPU.

---

# 26. EFFECT SYSTEM — CENTRAL, NOT OPTIONAL

Every effectful operation declares semantic effects.

Core effect families:

```text
MemoryRead
MemoryWrite
MemoryReadWrite
Allocate
Deallocate
ResourceResidency
GPUCommand
HostStateRead
HostStateWrite
SceneRead
SceneWrite
EcsRead
EcsWrite
PhysicsRead
PhysicsWrite
AudioRead
AudioWrite
FileIO
NetworkIO
DeviceIO
ExternalCall
TimeRead
RandomRead
Nondeterministic
Synchronization
Logging
Debug
```

Effects may carry resource/range identity.

Compiler uses them for:

- reordering legality,
- hazard detection,
- scheduling,
- replay,
- determinism,
- sandboxing,
- caching,
- incremental execution.

---

# 27. DETERMINISM EFFECTS

Determinism is a Cerid product feature.

Each operation/provider should be classifiable:

```text
BitExact
DeterministicWithinTarget
DeterministicWithinBackend
Nondeterministic
ExternalNondeterminism
```

Time, random, input, external services and unordered atomics must be explicit.

Provide deterministic alternatives where feasible.

Record nondeterministic inputs for replay.

Support compiler modes:

```text
certified_deterministic
deterministic
normal
fast
```

Do not allow optimization passes to silently violate an active determinism contract.

---

# 28. NUMERICAL SEMANTICS

Explicitly model where relevant:

- IEEE mode,
- fast math,
- contraction/FMA,
- flush-to-zero,
- denorm behavior,
- rounding,
- overflow behavior,
- integer wrapping/trapping,
- NaN semantics,
- precision promotion,
- mixed precision,
- stochastic rounding.

CRD-Hesap and certification paths require stronger contracts than a visual-effect shader.

---

# 29. ERROR / FAILURE MODEL

Future language/runtime must not hide arbitrary exceptions across engine boundaries.

Support typed failures:

```text
Result<T,E>
Status
Option<T>
```

CEIR operations may declare failure effects.

Different domains may lower failure differently:

- cook-time diagnostic,
- host runtime result,
- device error flag,
- validation trap in debug,
- fallback path.

Do not make GPU kernels depend on language exceptions.

---

# 30. CONCURRENCY MODEL

CEIR must represent concurrency as semantics, not merely implementation detail.

Core concepts:

- task,
- async value,
- token,
- join,
- fork,
- parallel-for,
- pipeline,
- barrier,
- race,
- select,
- cancellation,
- deadline,
- priority,
- task group,
- structured concurrency scope.

The future scripting language should have structured concurrency rather than arbitrary detached-thread spawning as the default.

---

# 31. HOST JOB / FIBER INTEGRATION

Cerid already owns a fiber/job system.

Add a host-task dialect/provider that can lower program regions to:

- inline host execution,
- job-system task,
- parallel-for,
- fiber wait,
- continuation,
- pinned main-thread work,
- I/O worker,
- audio real-time thread where legal.

Do not create a second generic scheduler.

CEIR becomes an authorable front-end to existing Cerid scheduling primitives.

---

# 32. REAL-TIME DOMAINS

Different workloads have different deadlines.

Represent execution classes:

- frame-critical,
- simulation-critical,
- audio-real-time,
- latency-sensitive,
- throughput,
- background,
- offline.

Scheduler/compiler may use this for:

- queue choice,
- CPU worker choice,
- priority,
- preemption boundaries,
- buffering,
- quality tier.

Never allow a filesystem or blocking network op inside a real-time audio region unless explicitly rejected/bridged.

---

# 33. `ceir.core`

Own:

- constants,
- arithmetic needed at orchestration level,
- comparisons,
- select,
- tuples/struct extraction,
- casts,
- assertions,
- metadata,
- region/control primitives.

Kernel-heavy arithmetic remains CKIR or tensor/linalg dialect where appropriate.

---

# 34. `ceir.func`

Own:

- modules,
- functions,
- calls,
- imports,
- exports,
- visibility,
- generic parameters,
- interface hashes,
- function attributes,
- recursion metadata.

Reusable subgraphs are functions.

---

# 35. `ceir.shape`

Own symbolic and dynamic shape reasoning.

Include:

- shape values,
- rank,
- extents,
- constraints,
- broadcast,
- reshape compatibility,
- shape assertions,
- symbolic expressions.

---

# 36. `ceir.resource`

Own:

- resources,
- views,
- ranges,
- subresources,
- allocation intent,
- alias groups,
- import/export,
- persistent/history state,
- residency hints,
- lifetime classes.

---

# 37. `ceir.async`

Own:

- tokens,
- async values,
- waits,
- groups,
- joins,
- race/select,
- cancellation,
- dependencies.

---

# 38. `ceir.task`

Own host/job execution semantics.

Include:

- spawn,
- parallel_for,
- continuation,
- main-thread,
- worker,
- fiber wait,
- deadline/priority,
- task group.

---

# 39. `ceir.frame`

Existing FrameGraph concept becomes this dialect.

Own:

- logical resources,
- whole-frame/workflow topology,
- pass/work-unit nodes,
- reads/writes,
- history,
- persistent/external/transient resources,
- subgraphs,
- includes,
- injection anchors,
- pass replacement,
- capability variants,
- explicit fallback,
- output/present endpoints,
- multi-view,
- multi-window,
- queue hints,
- compile policies.

`.frame.toml` becomes a frontend to `ceir.frame`.

Programmatic frame builders emit `ceir.frame`.

There must be one runtime/compiler architecture.

---

# 40. `ceir.render`

Own backend-neutral rendering orchestration.

Atomic semantic operations include concepts such as:

- begin/end render scope,
- draw,
- indexed draw,
- indirect draw,
- indirect-count draw,
- mesh dispatch,
- indirect mesh dispatch,
- patch/tessellation draw,
- viewport/scissor dynamic state,
- shading rate,
- multiview/view selection.

Do not encode material/shadow/deferred combinations in op names.

Combinations remain typed data/program composition.

---

# 41. ATTACHMENTS ARE GENERAL

Support ordinary typed N-attachment rendering.

Must cover:

- float,
- signed integer,
- unsigned integer attachments,
- depth,
- stencil,
- separate depth/stencil load/store,
- typed clear,
- resolve,
- MSAA,
- mip/layer/aspect views,
- read-only depth/stencil,
- multiview,
- shading-rate attachment,
- fragment-density/foveation attachments where target supports them.

These are not special canonical concepts:

- G-buffer,
- visibility buffer,
- object ID,
- primitive ID,
- velocity,
- selection buffer,
- material ID.

They are program-defined output contracts over general attachments.

---

# 42. `ceir.compute`

Own ordinary kernel launches:

- direct dispatch,
- indirect dispatch,
- dynamic dispatch dimensions,
- persistent kernels where supported,
- cooperative launches,
- dispatch groups/grids,
- kernel resource binding.

Kernel implementation is CKIR or a higher-level op lowered to CKIR/provider programs.

---

# 43. `ceir.work` — DYNAMIC / DEVICE-GENERATED WORK

This is mandatory for frontier readiness.

Represent semantic work generation independently of vendor APIs.

Concepts:

```text
work_queue<T>
work_record<T>
produce
consume
fanout
fanin
compact
bin
dynamic_foreach
dispatch_graph
device_loop
persistent_worker
```

Lowerings may include:

- compute-generated indirect args,
- multi-draw indirect,
- indirect-count,
- D3D12 ExecuteIndirect,
- D3D12 Work Graphs,
- Vulkan `VK_EXT_device_generated_commands`,
- Vulkan `VK_AMDX_shader_enqueue`,
- Metal indirect command buffers,
- CUDA device graph launch,
- generic queue-buffer fallback.

One authored semantic algorithm can choose different target strategies.

---

# 44. DEVICE-SIDE CONDITIONAL / LOOP GRAPHS

Support semantics for work that remains on device:

- IF,
- SWITCH,
- WHILE,
- recursive/fan-out work where bounded/legal,
- device graph launch,
- tail launch,
- fire-and-forget,
- producer/consumer queues.

This is required for:

- wavefront path tracing,
- adaptive subdivision,
- virtualized geometry traversal,
- GPU scene processing,
- recursive procedural generation,
- ML/control graphs.

---

# 45. `ceir.scene`

Scene operations expose semantics, not opaque renderer algorithms.

Examples:

- get view(s),
- get instance set,
- get material set,
- get lights,
- query components,
- resolve draw candidates,
- resolve geometry/material/technique/program,
- build draw list,
- sort/bin/filter.

Convenience ops such as frustum culling may exist, but must be replaceable/lowerable.

A user must be able to write a completely different culling algorithm as compute/work CEIR + CKIR.

---

# 46. `ceir.ecs`

If CEIR becomes part of scripting, provide typed ECS query semantics.

Concepts:

- query required/optional/excluded components,
- read component,
- write component,
- add/remove component,
- entity creation/destruction,
- stable entity handles,
- chunk/archetype iteration,
- parallel query,
- structural-change barrier.

Effects must identify component reads/writes so jobs can be scheduled safely.

This may initially be host-only.

---

# 47. `ceir.geometry`

Own high-level geometry workflow semantics:

- mesh generation,
- mesh processing,
- skinning,
- morphing,
- deformation,
- subdivision,
- tessellation,
- meshlet/cluster construction,
- simplification,
- LOD,
- culling,
- compaction,
- binning,
- procedural geometry,
- terrain,
- point clouds,
- voxel extraction,
- SDF processing,
- BLAS preparation.

High-level ops can lower to CRD-Geometry CPU algorithms, CKIR compute, mesh shaders, or device work.

---

# 48. `ceir.animation`

Future animation/rig evaluation:

- pose graph,
- blend,
- additive blend,
- constraints,
- IK,
- retargeting,
- motion matching query,
- curve evaluation,
- root motion,
- skeletal deformation,
- morph weights,
- GPU/CPU evaluation,
- previous pose for motion vectors.

Animation assets remain domain assets; CEIR orchestrates evaluation.

---

# 49. `ceir.rt`

Production-grade ray tracing orchestration.

Must represent:

- BLAS build,
- BLAS update/refit,
- BLAS compaction,
- TLAS build/update,
- instance population,
- SBT construction,
- multiple raygen/miss/hit/callable groups,
- triangle/procedural geometry,
- any-hit,
- intersection,
- callable,
- direct/indirect trace,
- inline ray query workflow,
- motion,
- opacity micromaps,
- future displacement/micromap concepts,
- SER/reordering capabilities,
- AS residency/serialization where useful.

Do not define "hybrid reflections" as an atomic primitive.

Hybrid reflections are an asset program built from these capabilities.

---

# 50. `ceir.transfer`

Complete movement semantics:

- buffer copy,
- image copy,
- buffer-image,
- image-buffer,
- tensor copy,
- subresource copies,
- clear,
- blit,
- resolve,
- mip generation,
- upload,
- readback,
- peer copy,
- sparse page mapping/upload,
- compression/decompression steps where supported.

---

# 51. `ceir.tensor`

Preserve tensor semantics above raw dispatch.

Types/ops need:

- static/dynamic shape,
- layout,
- strides,
- element type,
- quantized types,
- sparse/ragged forms,
- views/slices.

Core operations:

- elementwise,
- broadcast,
- reshape,
- transpose,
- gather/scatter,
- reductions,
- scans,
- contractions,
- matmul/batched matmul,
- convolution,
- FFT,
- sort/top-k,
- solve/decomposition hooks.

Do not prematurely lower every tensor op to a standalone kernel.

---

# 52. `ceir.linalg`

Optional distinction from general tensor semantics for numerical algorithms.

Support high-level algorithm identities:

- GEMM/GEMV,
- triangular solve,
- QR,
- LU,
- Cholesky,
- LDLT,
- SVD,
- eigensolvers,
- sparse matmul,
- sparse factorization,
- iterative solver steps.

Compiler may choose:

- CKIR,
- CRD-Hesap native CPU,
- vendor library,
- D3D LinAlg,
- native tensor/data graph provider.

This preserves algorithmic intent for optimization and benchmarking.

---

# 53. `ceir.sparse`

Sparse must be first class.

Represent:

- CSR/CSC/COO,
- block sparse,
- structured sparsity,
- graph adjacency,
- sparse tensors,
- sparsity metadata.

Compiler can specialize loops/storage and select sparse kernels.

Do not hide sparsity behind opaque buffer conventions only.

---

# 54. `ceir.quant`

Represent quantization semantics:

- expressed type,
- storage type,
- scale,
- zero point,
- per-tensor/per-axis,
- packed low-bit formats,
- FP8,
- INT8/4/2 where supported,
- calibration metadata.

Quantization/dequantization may be fused into loads/provider programs.

Important for ML and neural rendering.

---

# 55. `ceir.ml`

High-level ML graph semantics.

Cover inference and eventually training.

Operations/concepts:

- linear,
- convolution,
- embedding,
- activation,
- normalization,
- attention,
- MLP,
- MoE routing,
- sampling,
- recurrent state,
- KV cache,
- quantization,
- sparsity,
- mixture/expert dispatch,
- custom operator subgraphs.

Execution may target:

- CKIR,
- cooperative matrix/vector,
- D3D Linear Algebra,
- DirectX MLIR programs,
- Vulkan tensor/data-graph pipelines,
- GPU libraries,
- NPU execution providers.

---

# 56. TRAINING SUPPORT

Do not architect ML as inference-only.

Long-term CEIR must support:

- forward graph,
- backward graph,
- gradient accumulation,
- optimizers,
- mixed precision,
- loss scaling,
- checkpointing,
- activation recomputation,
- distributed gradients,
- parameter sharding,
- optimizer state,
- training/inference mode.

Implementation can arrive later, but types/effects/state must not block it.

---

# 57. `ceir.autodiff`

Automatic differentiation must be a compiler transformation, not a special ML hack.

Support:

- forward mode,
- reverse mode,
- custom JVP/VJP,
- stop-gradient,
- nondifferentiable ops,
- differentiable types,
- checkpoint/recompute,
- differentiation through control flow where legal,
- differentiation through tensor ops,
- CKIR differentiation,
- differentiable rendering,
- differentiable simulation/physics,
- differentiable geometry.

This aligns future CRD-Hesap AD, neural rendering and optimization workloads.

---

# 58. `ceir.random`

Explicit random streams:

- counter-based deterministic RNG,
- stream/subsequence,
- distributions,
- stochastic sampling,
- blue-noise/sample-sequence handles,
- differentiability policy.

Randomness must be explicit for replay and deterministic modes.

---

# 59. CRD-HESAP INTEGRATION

CRD-Hesap must become a first-class producer/consumer of CEIR.

Examples:

```text
A * B
→ ceir.linalg.matmul

fft(x)
→ ceir.tensor.fft

solve(A,b)
→ ceir.linalg.solve
```

Programs may mix:

```text
CRD-Hesap CPU
→ GPU tensor program
→ custom CKIR
→ geometry generation
→ visualization
```

No separate Hesap GPU scheduler should emerge.

CEIR is the common orchestration substrate.

---

# 60. `ceir.physics` / CRD-EYLEM

Future physics orchestration must fit without redesign.

Represent:

- fixed timestep,
- substeps,
- broadphase,
- narrowphase,
- contact generation,
- solver island building,
- constraint iterations,
- integration,
- sleeping,
- particles/cloth/fluids,
- CPU/GPU hybrid,
- deterministic scheduling,
- sensor/actuator stages,
- differentiable simulation hooks.

The low-level solver math may be C++/CKIR/CRD-Hesap; CEIR orchestrates pipelines.

---

# 61. `ceir.audio`

Cerid targets DAW-class applications; do not omit audio.

Audio execution requires special semantics:

- sample-accurate time,
- hard real-time thread,
- block size/sample rate,
- audio buffers,
- channels/layout,
- DSP nodes,
- synth/effects,
- convolution/FFT,
- resampling,
- automation,
- event/MIDI streams,
- plugin/external DSP capability,
- graph latency,
- feedback/delay nodes,
- no blocking allocation/I/O on RT thread.

A future visual audio graph can share CEIR infrastructure while using an audio-specific dialect/provider.

---

# 62. `ceir.media`

Media workflow:

- demux,
- decode,
- colorspace conversion,
- resize,
- optical flow,
- filtering,
- denoise,
- interpolation,
- AI enhancement,
- compositing,
- encode,
- mux.

Targets may include:

- CPU codecs,
- Vulkan Video,
- hardware video engines,
- GPU compute,
- native optical-flow/data-graph engines,
- ML providers.

Workflow remains a CEIR asset.

---

# 63. `ceir.ui`

UiWorld remains the semantic UI tree.

CEIR handles UI rendering/effect orchestration:

- Canvas display-list processing,
- vector binning,
- glyph rendering,
- masks/clips/layers,
- backdrop capture,
- blur,
- shadows,
- custom UI materials,
- multi-pass effects,
- compositor.

Do not collapse widget semantics into CEIR rendering ops.

---

# 64. `ceir.io`

For the future scripting/offline workflow layer, model typed capabilities:

- file read/write,
- asset stream,
- archive,
- memory map,
- async I/O,
- watch events.

These require capability permissions and are illegal in restricted domains such as GPU kernels/audio RT.

---

# 65. `ceir.net`

Long-term application scripting may need:

- sockets,
- RPC,
- HTTP/web services,
- replication,
- streams,
- messages.

Do not make unrestricted networking available to every program by default.

Use capability-based access.

Network is a host-domain dialect/provider, not a GPU feature.

---

# 66. `ceir.event` / REACTIVE PROGRAMMING

Support explicit event streams/signals for:

- UI,
- input,
- gameplay,
- editor,
- asset reload,
- async completion,
- media,
- MIDI/audio control.

Concepts:

- event<T>,
- subscribe,
- map/filter,
- merge,
- debounce/throttle where domain-appropriate,
- await event,
- backpressure policy,
- deterministic event ordering.

Avoid hidden global callback webs.

---

# 67. `ceir.state`

State-machine and stateful logic support:

- states,
- transitions,
- guards,
- enter/exit,
- hierarchical states,
- parallel states,
- event-driven transitions,
- history states.

Visual state-machine view can compile into CHIR/CEIR.

Useful for UI, gameplay, animation and workflows.

---

# 68. `ceir.dist` — MULTI-GPU / DISTRIBUTED

Do not assume one device forever.

Represent:

- device mesh,
- logical shards,
- placement,
- send/recv,
- peer copies,
- broadcast,
- reduce,
- all-reduce,
- all-gather,
- reduce-scatter,
- barrier,
- collective groups.

Possible providers:

- multi-adapter local GPU,
- NCCL-like collectives,
- MPI,
- remote workers,
- cluster execution.

High-level tensor sharding annotations should be able to lower to communication automatically.

---

# 69. HETEROGENEOUS EXECUTION PROVIDERS

CEIR should not hardwire "everything becomes CKIR + GPU command."

A provider advertises:

- supported ops/subgraphs,
- supported types/layouts,
- memory domains,
- synchronization model,
- costs,
- determinism,
- capabilities,
- compilation interface.

Graph partitioner can assign regions to:

- CPU,
- Vulkan GPU,
- D3D12 GPU,
- CUDA/HIP,
- NPU,
- native ML graph engine,
- hardware video engine,
- remote worker.

This follows the useful architectural idea of execution providers without adopting ONNX Runtime as the canonical layer.

---

# 70. NATIVE GRAPH / PROGRAM BACKENDS

CEIR must preserve high-level semantics long enough to target new APIs directly.

Potential targets include:

- D3D12 MLIR Programs / DX Compute Graph Compiler,
- Vulkan `VK_ARM_data_graph`,
- Vulkan tensor resources,
- vendor NPU models,
- CUDA Graphs,
- CUDA device graph launch,
- D3D12 Work Graphs,
- Vulkan DGC,
- Vulkan shader enqueue,
- Metal ICB,
- future backend graph systems.

Do not flatten a tensor graph to 100 CKIR dispatches before the compiler has decided whether the device can run it as one native graph program.

---

# 71. `ceir.transform` — PROGRAMMABLE COMPILATION

Frontier requirement: optimization strategy itself should eventually be programmatic.

Inspired by schedule/transform IR ideas.

Allow separate transform programs to say:

- fuse these ops,
- tile this loop,
- unroll this region,
- vectorize,
- map to cooperative matrix,
- choose workgroup shape,
- split this high-level op,
- outline this region,
- assign provider,
- force/forbid fusion,
- set memory policy.

This enables research and autotuning without hard-coding every schedule in compiler C++.

Transform programs are assets too.

---

# 72. `ceir.rewrite` — DECLARATIVE REWRITE RULES

Provide a declarative pattern/rewrite system.

Use for:

- canonicalization,
- algebraic simplification,
- domain lowering,
- peepholes,
- graph fusion patterns,
- import normalization.

Application/plugin dialects can ship rewrite rules without editing central compiler switches.

Long-term optional frontier:

- equality saturation/e-graph optimization,
- cost-based extraction,
- proof-carrying rewrites for critical modes.

---

# 73. PARTIAL EVALUATION

Compiler must evaluate work at the earliest legal domain.

Examples:

- constant graph branch → remove at cook,
- capability branch → select at load,
- known tensor shape → specialize,
- static CSM cascade loop → unroll,
- immutable material option → compile variant,
- dynamic camera input → remain frame-time.

This is essential to make a rich language cheap at runtime.

---

# 74. CORE OPTIMIZATIONS

Implement infrastructure for:

- constant folding,
- SCCP,
- DCE,
- CSE,
- GVN where applicable,
- inlining/outlining,
- loop invariant code motion,
- loop unroll,
- vectorization,
- algebraic simplification,
- dead resource elimination,
- resource lifetime shortening,
- dead pass/work elimination,
- subgraph deduplication,
- specialization.

---

# 75. TENSOR / COMPUTE OPTIMIZATIONS

Support:

- fusion,
- tiling,
- layout assignment,
- bufferization,
- memory planning,
- vectorization,
- cooperative matrix/vector mapping,
- shared-memory staging,
- async copies,
- quantized load/decode fusion,
- sparsification,
- kernel selection,
- library selection,
- persistent kernel selection,
- graph-provider partitioning.

---

# 76. FUSION

Compiler may combine:

```text
A
→ B
→ C
```

into a single CKIR kernel or native graph region when legal and profitable.

Important for:

- ML,
- CRD-Hesap,
- image processing,
- GI denoisers,
- post-processing,
- geometry processing.

Preserve source provenance through fusion.

---

# 77. SPLITTING

A high-level op may lower to multiple executable stages.

Example:

```text
gi.denoise
```

can lower to:

```text
temporal
→ variance
→ atrous x N
→ resolve
```

The user is not forced to author target-specific micro-passes.

---

# 78. MEMORY PLANNING

Global memory planning must consider:

- logical lifetimes,
- alias compatibility,
- concurrency,
- queue overlap,
- device placement,
- residency,
- persistent/history resources,
- tensor layouts,
- memory budgets.

Optimization profiles:

```text
latency
balanced
memory
deterministic
```

Do not optimize memory in isolation if aliasing destroys useful parallelism.

---

# 79. QUEUE / SCHEDULING OPTIMIZATION

Compiler determines or advises:

- graphics queue,
- async compute,
- copy,
- video,
- data graph/NPU,
- CPU tasks.

Inputs:

- dependencies,
- effects,
- device capabilities,
- queue transfer cost,
- historical profiling,
- priority/deadline.

Assets can express policy/hints but should not normally hardcode API queue details.

---

# 80. AUTOTUNING

Support tunable dimensions:

- tile sizes,
- workgroup sizes,
- wave/subgroup preferences,
- cluster dimensions,
- sort radix,
- GEMM configuration,
- FFT radix,
- fusion decisions,
- provider selection,
- indirect vs DGC vs Work Graph,
- CPU/GPU threshold.

Cache tuning by:

- device ID,
- driver,
- compiler version,
- program hash,
- shape/workload class.

Support deterministic "locked configuration" mode.

---

# 81. PROFILE-GUIDED OPTIMIZATION

CR-D007 profiling should eventually feed compiler decisions.

Record:

- op time,
- GPU/CPU occupancy proxies,
- bytes moved,
- work counts,
- resource pressure,
- cache behavior,
- queue stalls,
- graph compile cost.

Allow profile-guided recompile while preserving a deterministic fallback.

---

# 82. COST MODEL

Cost model may begin heuristic and evolve.

Must have extension points for:

- CPU submit cost,
- dispatch/draw cost,
- barrier cost,
- queue-transfer cost,
- memory bandwidth,
- transient VRAM,
- tensor layout conversion,
- work-graph/DGC preprocess,
- network/collective cost,
- NPU transfer cost.

---

# 83. CPU EXECUTION

A universal scripting/execution system cannot assume GPU.

Support host lowering:

- interpreter/debug VM,
- compiled execution plan,
- native C++/LLVM codegen if chosen,
- SIMD/vectorized kernels,
- job system,
- external native functions.

A simple CEIR program must be able to run CPU-only where semantics permit.

---

# 84. TIERED HOST EXECUTION

Possible tiers:

## Debug/reference tier

- easy stepping,
- maximal diagnostics,
- deterministic semantics,
- slower.

## Optimized execution plan

- pre-resolved ops,
- compact arrays,
- no strings,
- no allocations on hot path.

## Optional JIT/AOT

For hot scripting/compute workloads where beneficial.

The existence of an interpreter must not make the interpreter the mandatory shipping hot path.

---

# 85. CKIR INTEGRATION

CKIR remains the source of truth for shader/kernel computation.

CEIR references CKIR programs by stable asset/function identity.

CEIR compiler may generate CKIR during lowering.

Examples:

```text
tensor fusion
→ generated CKIR kernel

scene custom culling
→ user CKIR kernel

material shader
→ material/technique → CKIR

UI vector binning
→ CKIR compute
```

No handwritten backend shading language becomes canonical.

---

# 86. CKIR FRONTIER REQUIREMENTS CEIR MUST PRESERVE

CEIR capability/type system must leave room for CKIR/device features including:

- all graphics stages,
- compute,
- mesh/task,
- tessellation,
- ray tracing stages,
- ray query,
- subgroup/wave operations,
- atomics,
- memory scopes,
- derivatives in compute where supported,
- cooperative matrix,
- cooperative/long vectors,
- specialized matrix decode/quantization,
- shader invocation reordering/SER,
- opacity micromaps,
- VRS,
- bindless/resource tables,
- dynamic resource indexing,
- 16-bit/8-bit types,
- FP64,
- future neural shader primitives.

---

# 87. RENDERER EXPRESSIVENESS CORPUS

CEIR architecture must be able to express, without new native algorithm code when primitives already exist:

- Unlit
- Forward
- Forward+
- Clustered Forward
- Deferred
- Tiled Deferred
- Clustered Deferred
- Hybrid deferred/forward
- Visibility Buffer
- Deferred texturing
- GPU-driven scene renderer
- Mesh-shader renderer
- Tessellation/displacement
- software raster stages
- hybrid software/hardware raster
- virtualized geometry
- Nanite-class alternatives
- CAD hidden-line/technical rendering
- scientific volume rendering
- point clouds
- voxels
- Gaussian splats
- particles
- sprite/2D renderer
- UI compositor.

---

# 88. SHADOW / LIGHTING CORPUS

Must not preclude:

- CSM
- SDSM
- PCF
- PCSS
- EVSM
- MSM
- point/spot shadows
- area-light shadows
- virtual shadow maps
- cached shadows
- contact shadows
- screen-space shadows
- ray-traced shadows
- stochastic shadows
- LTC lights
- tiled/clustered lights
- ReSTIR DI
- MegaLights-class stochastic direct lighting
- emissive/mesh lights.

These are asset algorithms, not CEIR op names.

---

# 89. GI / REFLECTION CORPUS

Must express:

- SSAO/HBAO/GTAO/CACAO-class AO
- SSR/SSSR
- probes
- planar reflections
- RT reflections
- hybrid reflection selection
- DDGI
- SSGI
- voxel/radiance methods
- radiance caches
- surface caches
- Lumen-class hybrid GI
- ReSTIR GI
- neural radiance caches
- path-traced GI
- reference path tracing.

---

# 90. RAY-TRACING CORPUS

Must express:

- monolithic path tracer,
- wavefront path tracer,
- megakernel,
- ray queues,
- shadow queues,
- compaction,
- material queues,
- SER/reordering-aware paths,
- inline ray queries,
- RT pipeline shaders,
- procedural primitives,
- hair/curve intersections,
- micromap-assisted alpha,
- denoisers,
- temporal/spatial reuse.

---

# 91. VIRTUALIZED GEOMETRY CORPUS

Must express orchestration for:

- offline cluster construction,
- hierarchy,
- error metrics,
- streaming pages,
- residency,
- instance traversal,
- cluster traversal,
- frustum/occlusion/LOD,
- persistent visibility,
- material binning,
- work generation,
- mesh shading,
- software raster,
- visibility output,
- motion vectors,
- shadow representation,
- RT representation.

Some external streaming/asset-system operations may be native host capabilities; the algorithm remains CEIR.

---

# 92. GENERAL GPU COMPUTE CORPUS

Must handle:

- reduction,
- prefix scan,
- compaction,
- radix sort,
- histogram,
- FFT,
- convolution,
- image filters,
- GEMM,
- sparse matrix ops,
- graph algorithms,
- particle simulation,
- fluid kernels,
- PDE/ODE kernels,
- marching cubes,
- procedural generation,
- compression,
- hashing,
- signal processing.

---

# 93. CRD-HESAP CORPUS

Roadmap target includes:

- dense BLAS/LAPACK-class GPU flows,
- sparse,
- orderings,
- iterative solvers,
- direct solvers,
- eigensolvers,
- optimization,
- ODE/DAE,
- FFT,
- DSP,
- wavelets,
- statistics,
- interpolation,
- quadrature,
- differentiation,
- tensors,
- motion/trajectory compute.

Not every algorithm must be GPU-first, but all mixed CPU/GPU workflows should be representable.

---

# 94. ML / AI CORPUS

Must support architecture for:

- MLP,
- CNN,
- transformers,
- attention variants,
- embeddings,
- RNN/stateful models,
- MoE,
- quantized models,
- sparse models,
- neural graphics,
- neural materials,
- neural textures,
- neural compression,
- neural denoisers,
- neural upscalers,
- frame generation components,
- learned GI/radiance caches,
- neural fields,
- NeRF-like workloads,
- training,
- inference.

---

# 95. MEDIA CORPUS

Must support:

- decode/encode,
- color conversion,
- scaling,
- tone mapping,
- optical flow,
- denoise,
- deinterlace,
- interpolation,
- stabilization,
- compositing,
- GPU filters,
- ML enhancement,
- image sequence processing,
- offline transcode.

---

# 96. UI / 2D CORPUS

Must support:

- Canvas display-list compilation,
- vector paths,
- text/glyph rendering,
- clipping,
- layers,
- blur/glass,
- filters,
- custom UI materials,
- sprite rendering,
- tilemaps,
- 2D lighting,
- particles,
- world-space UI,
- editor overlays.

---

# 97. AUDIO / DSP CORPUS

Must support architecture for:

- synth graphs,
- filters,
- EQ,
- dynamics,
- convolution,
- FFT spectral effects,
- spatial audio,
- mixing,
- routing,
- automation,
- sample playback,
- resampling,
- MIDI/event processing,
- plugin nodes,
- offline render.

---

# 98. SCRIPTING-LANGUAGE FEATURE SET

Future Cerid language/CHIR should plan for:

- modules/packages,
- imports,
- namespaces,
- functions,
- structs,
- enums,
- tagged unions,
- pattern matching,
- generics,
- interfaces/traits,
- extension methods if desired,
- value semantics,
- explicit references/handles,
- closures,
- lambdas,
- compile-time constants/functions,
- attributes,
- reflection,
- serialization metadata,
- Result/Option,
- structured concurrency,
- async/await,
- generators/coroutines,
- events,
- state machines,
- deterministic RNG/time APIs,
- hot reload,
- native FFI/intrinsics,
- unit-aware quantities,
- no mandatory GC in real-time hot paths.

This is language roadmap scope, not necessarily CEIR-0 implementation scope.

---

# 99. CAPABILITY-BASED SECURITY

Because agents and applications can author executable assets, programs need permissions.

Examples:

```text
capability.scene.read
capability.scene.write
capability.gpu.compute
capability.gpu.render
capability.file.read
capability.file.write
capability.network.client
capability.audio.rt
capability.external.native
```

An asset declares requested capabilities.

Host/application grants them.

Untrusted programs can be sandboxed.

Do not expose arbitrary pointer/OS access through generic CEIR ops.

---

# 100. NATIVE INTRINSICS

Native intrinsics are the escape hatch.

Each intrinsic declares:

- stable ID,
- version,
- typed inputs/outputs,
- effects,
- domain,
- determinism,
- thread safety,
- lifetime rules,
- capabilities,
- provider,
- cost hints,
- hot-reload compatibility,
- source/debug metadata.

Examples of legitimate native intrinsics:

- acquire swapchain image,
- present,
- OS window interaction,
- read hardware device,
- external codec call,
- external SDK/NPU call,
- custom sensor,
- native plugin callback.

Not legitimate:

- `ForwardPlusExecutor`
- `DeferredExecutor`
- `MyNewGIExecutor`

Those are algorithms and should be CEIR programs.

---

# 101. CUSTOM DIALECT / PLUGIN MODEL

Applications/plugins can add dialects.

Three extension levels:

## Level A — subgraph/function only

No native code.

New node is implemented entirely as existing CEIR.

## Level B — custom high-level op with lowerings

Plugin defines op schema + lowering to existing CEIR/CKIR.

May require plugin compiler code but no backend change.

## Level C — new native capability/provider

Only when lower layers genuinely cannot express it.

This hierarchy must be visible in CR-D007 and documentation.

---

# 102. EXECUTION-PROVIDER PARTITIONING

Compiler can ask providers:

> Which operations/subgraphs can you execute?

Providers may accept maximal subgraphs.

Compiler then inserts transfers/synchronization.

Examples:

```text
preprocess CKIR on GPU
→ neural model on NPU
→ postprocess CKIR on GPU
```

or:

```text
CPU sparse factorization
→ GPU dense update
→ CPU convergence check
```

Provider choice can be automatic, constrained, or explicitly pinned.

---

# 103. MULTI-DEVICE SHARDING

High-level tensors may carry sharding annotations.

Compiler can propagate and materialize:

- partition,
- all-reduce,
- all-gather,
- reduce-scatter,
- reshard,
- peer copies.

This future-proofs large ML/scientific workloads.

---

# 104. COMPILER/TRANSFORM VERSIONING

Track:

- CEIR version,
- dialect versions,
- op versions,
- CHIR version,
- CKIR version,
- compiler version,
- transform-program version,
- target profile,
- optimization profile.

Compiled artifacts are caches, not permanent truth.

Source/program assets get migrations.

---

# 105. ASSET MODEL

All normal reusable programs get canonical asset identity.

Suggested classes:

```text
ProgramSourceAsset
CookedProgramAsset
RuntimeProgram
CompiledExecutionPlan
GeneratedKernelAsset/cache entry
```

Namespaces:

```text
engine://
app://
plugin://
```

Dynamically generated programs may use:

```text
runtime://
session://
```

with the same semantics.

A runtime-generated module can be persisted as a normal asset if desired.

---

# 106. PROGRAM ASSET DEPENDENCIES

Track:

- called functions/subgraphs,
- CKIR kernels,
- materials/techniques,
- resource schemas,
- native intrinsics,
- provider requirements,
- transform schedules,
- imported models,
- external assets.

Dependency graph drives:

- hot reload,
- cache invalidation,
- interface compatibility,
- editor navigation.

---

# 107. INTERFACE HASH

Program/function interface hash includes:

- parameter types,
- result types,
- effects visible to callers,
- capability contract,
- state schema,
- exported symbols,
- resource contract.

Implementation-only changes can hot swap without invalidating callers.

---

# 108. HOT RELOAD

Required pipeline:

```text
detect
→ parse/import
→ semantic analysis
→ verify
→ cook
→ compile affected region
→ validate replacement set
→ atomic install
→ generation bump
→ deferred destruction
```

Preserve last-good generation on failure.

No mixed-generation call graph.

---

# 109. STATE MIGRATION ON HOT RELOAD

Future scripts/programs may have persistent state.

Support:

- compatible state layout reuse,
- explicit migration function,
- versioned state schema,
- reset policy,
- rejected incompatible swap.

Editor shows exactly which state will survive or reset.

---

# 110. INCREMENTAL COMPILATION

Cache at multiple levels:

- parsed source,
- CHIR,
- CEIR function,
- lowered CEIR region,
- CKIR kernel,
- backend program,
- execution plan.

Hash content and interfaces.

Changing one function should not rebuild unrelated modules.

---

# 111. SOURCE MAPS / PROVENANCE

Every transformation preserves provenance.

A backend validation error should map:

```text
Vulkan/DX12 error
→ execution-plan op
→ lowered CEIR op
→ high-level CEIR/CHIR op
→ text span / CR-D007 node
```

Fusion/splitting must preserve source sets.

---

# 112. DEBUGGER

CR-D007 debugger plans:

- breakpoint on host CEIR op,
- step into function/subgraph,
- inspect SSA value,
- inspect resource,
- inspect state,
- view scheduled domain/provider,
- inspect generated GPU commands,
- inspect CKIR,
- view device work queue,
- capture graph inputs,
- deterministic replay.

GPU stepping may be capture-based rather than literal instruction stepping.

---

# 113. PROFILER

Per operation/node:

- CPU time,
- GPU time,
- queue/provider,
- invocation/work count,
- memory bytes,
- resource lifetime,
- transient allocation,
- barrier/sync cost,
- selected variant,
- selected lowering,
- cache/tune hit,
- device-generated work statistics.

Graph heatmap is a first-class research tool.

---

# 114. SEMANTIC CAPTURE / REPLAY

Capture:

- program generations,
- dynamic inputs,
- state snapshots,
- resource bindings,
- capability decisions,
- lowering choices,
- autotune choices,
- external nondeterministic inputs.

Provide CEIR-level replay independent of RenderDoc/PIX/Nsight.

Backend captures remain complementary.

---

# 115. VALIDATION LAYERS

## Syntax/structure

- valid ops,
- valid regions,
- dominance,
- terminators,
- symbols.

## Type

- operand/result types,
- generics,
- units,
- shapes.

## Ownership/lifetime

- dangling views,
- illegal mutation,
- lifetime escape.

## Resource/effects

- undeclared access,
- overlapping illegal writes,
- alias conflicts,
- hazard correctness.

## Domain

- illegal op on audio thread/device/etc.

## Capability

- unsupported feature,
- missing fallback.

## Kernel contract

- resource bindings,
- stage I/O,
- attachment output contracts.

## RT

- SBT/AS/hit-group legality.

## Distributed

- collective consistency.

---

# 116. RACE DETECTION

Provide static and debug-time tools for:

- host data races,
- resource races,
- component races,
- GPU unordered hazards,
- async token misuse.

Effect/ownership model should make many races statically visible.

---

# 117. FORMAL / CERTIFICATION HOOKS

Long-term critical/scientific modes may use:

- bounded-loop annotations,
- proof assertions,
- range constraints,
- shape constraints,
- determinism certificates,
- checked arithmetic,
- symbolic/SMT-assisted verification,
- compiler transformation audit trail.

Do not make formal verification mandatory for ordinary game rendering.

Keep hooks in architecture.

---

# 118. REFERENCE EXECUTOR

Build a slow, correct semantic/reference executor for a meaningful CEIR subset.

Use for:

- unit testing,
- differential tests,
- headless validation,
- compiler correctness,
- fuzzing,
- deterministic debugging.

Not every GPU feature needs full CPU emulation, but core semantic ops should have reference behavior.

---

# 119. FUZZING / DIFFERENTIAL TESTS

Test compiler with:

- randomly generated valid graphs,
- malformed graphs,
- mutation fuzzing,
- round-trip parser fuzzing,
- optimizer differential comparison,
- Vulkan vs D3D12,
- optimized vs reference executor,
- programmatic builder vs textual frontend,
- graph vs text source.

---

# 120. TRANSFORMATION CORRECTNESS

For high-risk compiler passes:

- semantic differential tests,
- property tests,
- proof/rewrite assertions where possible,
- minimized failure repro generation.

A compiler bug must not silently change numerical/render meaning.

---

# 121. PROGRAMMATIC API

C++ can construct programs fluently.

Example conceptually:

```cpp
CeirModuleBuilder m;
auto depth = m.input_image(...);
auto hzb = m.call("engine://ceir/hzb", depth);
...
```

But builder output is ordinary canonical CEIR.

Required:

- same verifier,
- same compiler,
- same diagnostics,
- same runtime,
- same source/provenance metadata,
- printable textual form,
- optional save-as-asset.

---

# 122. CLI / RPC / MCP

Every CEIR operation needed for authoring is accessible without GUI.

Commands include:

- create module,
- add/remove op,
- connect values,
- set attribute,
- create function,
- validate,
- compile,
- inspect type,
- inspect effects,
- list dialects/ops,
- show capability matrix,
- run,
- profile,
- diff,
- save,
- hot reload.

Agent tooling should operate semantically, not only patch text.

---

# 123. SOURCE CONTROL / DIFF / MERGE

Program assets need:

- deterministic serialization,
- stable semantic IDs,
- layout metadata separated,
- semantic diff,
- conflict diagnostics,
- merge support for independent subgraphs,
- canonical ordering where semantics permit.

Node movement must not create massive semantic diffs.

---

# 124. IMPORT / EXPORT

Potential importers/exporters, never canonical dependencies:

- ONNX,
- StableHLO,
- TOSA,
- MaterialX,
- graph formats,
- external ML models,
- shader/kernel formats as generated/imported compatibility paths.

Imported program becomes Cerid semantic IR and is validated.

Do not make external format semantics the internal architecture.

---

# 125. MATERIAL / TECHNIQUE RELATIONSHIP

Do not collapse material/technique into CEIR.

Material remains surface/domain data.

Technique remains shading algorithm/domain contract.

CEIR orchestration may:

```text
resolve material
resolve technique
resolve program variant
bind
draw
```

CKIR contains actual GPU shader computation.

Material graph frontends may compile into CKIR/CEIR helper programs as appropriate.

---

# 126. FRAMEGRAPH MIGRATION

All current frame graphs must end at `ceir.frame`.

Migration:

1. `.frame.toml` parser frontend emits canonical CEIR.
2. Existing programmatic FrameGraphBuilder emits canonical CEIR.
3. Existing frame validation migrates into CEIR verifiers/interfaces.
4. Existing frame compilation becomes CEIR scheduling/resource planning.
5. Existing runtime executes CEIR compiled plans.
6. Old duplicate frame-runtime structures become adapters.
7. Adapters are deleted.
8. One graph architecture remains.

Preserve asset IDs where possible.

---

# 127. EXECUTOR MIGRATION

Inventory every current executor from code.

Classify:

## Composite algorithm

Must become CEIR asset/program.

## Atomic native capability

May remain native intrinsic/lowering primitive.

Examples of likely composite behavior:

- scene traversal/raster submission,
- fullscreen setup,
- compute setup,
- RT pipeline orchestration,
- transfer workflows.

Do not assume the current executor list from old docs is current; inspect source.

---

# 128. THE DECISIVE PROOF — `scene.raster`

The current concept must eventually be ordinary CEIR.

Concept:

```text
render.begin attachments

foreach draw in draw_list:
    material = scene.resolve_material(draw)
    technique = scene.resolve_technique(material, phase)
    program = scene.resolve_program(technique, draw)
    geometry = scene.resolve_geometry(draw)
    bindings = render.build_bindings(...)
    render.draw(...)

render.end
```

No private C++ algorithm path.

Native helpers may expose truly atomic host data acquisition/resolution capabilities.

---

# 129. COMPUTE PROOF

A program asset:

```text
input A
input B
output C
compute.dispatch @add(A,B,C)
```

must work:

- text,
- graph,
- C++ builder,
- Vulkan,
- D3D12,
- hot reload,
- reference validation.

---

# 130. FORWARD+ PROOF

Asset-only orchestration:

```text
depth
→ light cull CKIR
→ tile/cluster lists
→ forward scene shading
```

No `ForwardPlusExecutor`.

Required diagnostics/profiling.

---

# 131. DEFERRED PROOF

Asset-only:

```text
GBuffer raster
→ lighting
→ transparency
→ post
```

G-buffer is normal attachments, not a canonical special field.

---

# 132. VISIBILITY BUFFER PROOF

Asset-only:

```text
visibility raster
→ material resolve compute
→ lighting
→ composite
```

This validates arbitrary renderer architectures.

---

# 133. HYBRID RT PROOF

Asset-only:

```text
raster primary
→ RT shadows/reflections/AO
→ denoise
→ composite
```

No hybrid renderer class.

---

# 134. WAVEFRONT PATH TRACING PROOF

Use queues/state:

```text
ray queue
→ trace
→ hit/miss queues
→ shade
→ shadow queue
→ next-ray queue
→ compact
→ repeat
```

This is a major dynamic-work/control-flow test.

---

# 135. DEVICE-GENERATED WORK PROOF

One semantic CEIR program supports at least:

```text
fallback:
compute → queue/indirect

D3D12:
Work Graph / ExecuteIndirect where appropriate

Vulkan:
DGC / shader enqueue / indirect

Metal:
ICB where appropriate

CUDA:
Graph/device-launch where appropriate
```

The source program does not encode vendor APIs.

---

# 136. NATIVE DATA-GRAPH PROOF

One tensor/ML CEIR region should be compilable through at least two strategies:

```text
CKIR dispatch sequence
```

and where supported:

```text
native graph/provider program
```

Examples for research:

- DirectX MLIR Programs,
- Vulkan data graph/tensor,
- NPU provider.

This prevents CEIR from being forever tied to command-by-command GPU submission.

---

# 137. CRD-HESAP PROOF

Permanent test workflow:

```text
GEMM
→ FFT
→ reduction
→ visualization prep
```

Requirements:

- CEIR asset,
- mixed high-level tensor + CKIR,
- no mandatory CPU round trip,
- memory plan visible,
- profiling,
- numerical oracle.

---

# 138. ML PROOF

First:

```text
MLP
```

Then:

```text
attention block
```

Test:

- high-level graph,
- fusion,
- quantized variant,
- cooperative matrix/vector path where available,
- generic CKIR fallback,
- native provider path when available.

---

# 139. DIFFERENTIABLE PROGRAM PROOF

Example:

```text
parameterized rendering/compute function
→ loss
→ reverse-mode gradient
```

or a small differentiable physics/optimization example.

Prove custom derivative and checkpointing seam.

---

# 140. MULTI-DEVICE PROOF

Later:

```text
tensor shard
→ per-device compute
→ all-reduce
→ result
```

Prove device placement and transfer are semantic, not hard-coded application glue.

---

# 141. UI EFFECT PROOF

Frosted glass:

```text
backdrop
→ downsample
→ blur
→ tint/noise
→ composite
```

Asset-authored CEIR.

---

# 142. AUDIO PROOF

A small RT-safe DSP graph:

```text
input
→ EQ
→ delay
→ compressor
→ output
```

Prove:

- audio time domain,
- no allocation/blocking,
- graph hot reload at safe boundary.

This may occur after core editor work but must fit architecture.

---

# 143. FUTURE SCRIPT PROOF

A small application behavior authored in both text and visual form:

```text
on event:
    query entities
    parallel update
    await async task
    update state
```

Both lower to the same semantic program and hot reload.

This proves CEIR is part of a language stack rather than only graphics tooling.

---

# 144. COMPILER PIPELINE

Canonical compiler stages:

```text
source/import
↓
parse / structured source model
↓
CHIR semantic analysis (when applicable)
↓
CEIR construction
↓
symbol resolution
↓
type inference/checking
↓
unit/shape checking
↓
ownership/lifetime checking
↓
effect analysis
↓
domain legality
↓
canonicalization
↓
partial evaluation
↓
DCE/CSE/etc.
↓
high-level domain transformations
↓
autodiff if requested
↓
provider partitioning
↓
fusion/splitting
↓
layout assignment
↓
bufferization/resource materialization
↓
memory planning
↓
queue/task scheduling
↓
dynamic-work lowering
↓
CKIR generation / provider compilation
↓
execution-plan generation
↓
backend object creation
```

Keep transformations inspectable.

---

# 145. MULTI-LEVEL IR SNAPSHOTS

CR-D007 should show:

```text
source
CHIR
high-level CEIR
scheduled CEIR
provider-partitioned CEIR
generated CKIR
canonical commands
backend lowering
```

This makes Cerid a research compiler platform.

---

# 146. FRONTIER TRANSFORM SCHEDULES

Allow a researcher to compare:

```text
Schedule A:
fuse → tile 16x16 → async copy

Schedule B:
split → tile 32x8 → cooperative matrix
```

without rewriting the semantic algorithm.

Store transform schedule as asset/profile.

---

# 147. CAPABILITY SYSTEM

Capabilities are stable semantic facts, not API extension names.

Examples:

```text
graphics.mesh_shader
graphics.vrs
graphics.multiview
graphics.shader_reorder

rt.pipeline
rt.inline_query
rt.opacity_micromap

compute.cooperative_matrix
compute.cooperative_vector
compute.long_vector
compute.fp64

work.device_generated_commands
work.dynamic_graph

tensor.native_resource
tensor.native_data_graph

media.video_decode
media.video_encode
media.optical_flow
```

Backend adapters map API-specific features to these capabilities.

---

# 148. REQUIRED / PREFERRED / OPTIONAL

Programs declare:

```text
required
preferred
optional
```

with explicit fallback.

Example:

```text
preferred dynamic device work
fallback compute + indirect
```

No silent semantic degradation.

---

# 149. QUALITY / COST TIERS

Programs can expose variants:

- realtime-low,
- realtime-high,
- cinematic,
- offline/reference,
- mobile,
- memory-constrained.

These are authored program choices/capability profiles, not scattered `if (thumbnail)` C++ branches.

---

# 150. PROVIDER PLUGINS

A provider plugin may provide:

- op support query,
- subgraph support query,
- compile,
- resource import/export,
- execution,
- profiling,
- synchronization,
- cache identity.

No central engine switch must know every provider.

---

# 151. BACKEND TARGETS TO KEEP POSSIBLE

Architecture must not exclude:

- Vulkan,
- D3D12,
- Metal,
- WebGPU,
- CUDA,
- HIP,
- CPU,
- NPU/ML accelerator,
- hardware video,
- remote/distributed provider.

Not all need immediate implementation.

---

# 152. GPU API FRONTIER TO TRACK

Maintain a capability-watch table for:

- Vulkan Roadmap milestones,
- shader objects,
- dynamic rendering,
- descriptor buffer/descriptor heap,
- mesh shaders,
- ray tracing,
- ray query,
- shader invocation reordering,
- DGC,
- shader enqueue/execution graph,
- cooperative matrix,
- cooperative vector,
- long vectors,
- tensors,
- data graphs,
- optical flow,
- video,
- sparse resources.

DirectX:

- Work Graphs,
- ExecuteIndirect,
- DXR,
- SER,
- Shader Model evolution,
- Linear Algebra,
- MLIR Programs / compute graphs.

Metal:

- ICB,
- mesh/ray features,
- residency,
- future graph/ML APIs.

CUDA/HIP:

- graphs,
- conditional graph nodes,
- device graph launch,
- libraries/collectives.

---

# 153. PERFORMANCE HOT-PATH RULES

No normal hot path may require:

- parsing source,
- string lookup per op,
- heap allocation per op,
- authoring graph traversal,
- generic reflection walk,
- dynamic map lookup for stable bindings,
- virtual dispatch for every trivial primitive if avoidable.

Compiled execution plan uses:

- dense IDs,
- pre-resolved function pointers/provider ops,
- compact arrays,
- stable slots,
- arenas,
- cached bindings,
- persistent graph/backend programs.

---

# 154. RESEARCH/EDITOR MODE MAY BE RICHER

Debug/editor execution may trade speed for:

- op stepping,
- validation,
- instrumentation,
- intermediate resource capture,
- graph mutation,
- source mapping.

Shipping plan can compile away this overhead.

---

# 155. PROGRAM SPECIALIZATION

Specialize by:

- target/device,
- capability,
- static parameters,
- material options,
- tensor shape,
- quality profile,
- numeric policy,
- determinism,
- provider.

Use content/interface hashes to deduplicate variants.

---

# 156. RESOURCE BINDING MODEL

RAH/CEIR should converge on a broad resource vocabulary:

- uniform/constant buffers,
- storage buffers,
- typed/raw/structured views,
- sampled images,
- storage images,
- samplers,
- comparison samplers,
- acceleration structures,
- tensors,
- global resource tables,
- bindless/resource indices,
- external resources.

Do not keep arbitrary fixed small bindless arrays as the long-term model.

---

# 157. GLOBAL RESOURCE TABLES

For GPU-driven rendering/ML/media, support resident resource tables/descriptor heaps as semantic resources.

Programs should pass compact resource indices where appropriate.

Backend picks descriptor-buffer/heap strategy.

---

# 158. COMMAND MODEL RELATIONSHIP

CEIR is above the canonical GPU command model.

Do not stuff compiler/high-level concepts into backend command descriptors.

The canonical command model remains a lower-level execution target.

Some CEIR regions may bypass individual commands by compiling to:

- persistent graph program,
- native data-graph program,
- provider executable.

This is intentional.

---

# 159. FRAME GRAPH RELATIONSHIP

FrameGraph is not merely deprecated syntax.

Its semantics are valuable:

- resource topology,
- lifetime,
- inter-pass dependency,
- history.

It becomes a CEIR dialect/front-end, not a separate runtime universe.

---

# 160. BUILD / OFFLINE WORKFLOWS

Long-term CEIR/CHIR may also orchestrate offline work:

- asset cooking,
- baking,
- lightmap generation,
- geometry processing,
- texture compression,
- ML training,
- cinematic rendering,
- farm tasks.

I/O/network effects and distributed providers make this possible.

Do not make realtime-only assumptions in core CEIR.

---

# 161. AGENT-NATIVE AUTHORING

Agents must be able to:

- discover dialects,
- discover ops,
- query signatures,
- build graphs,
- run verification,
- request compiler explanations,
- inspect lowering,
- benchmark alternatives,
- patch subgraphs,
- save assets.

Node/operation schemas should be machine-readable.

---

# 162. EXPLAINABILITY

Compiler can answer:

- Why did this op run on CPU?
- Why did these nodes fuse?
- Why did this resource allocate 64 MB?
- Why wasn't async compute used?
- Why did capability fallback select this path?
- Why did this graph become DGC instead of Work Graph?
- Why is hot reload incompatible?

Diagnostics should explain decisions, not only report failures.

---

# 163. CR-D007 PROGRAM EDITOR

Required views:

- node graph,
- text,
- hierarchy,
- function list,
- type inspector,
- effect inspector,
- resource lifetime view,
- schedule timeline,
- provider partition view,
- memory plan,
- CKIR view,
- backend view,
- profiler,
- transform schedule editor,
- capability/fallback editor,
- debugger,
- hot-reload inspector.

---

# 164. NODE UX

Support:

- typed pins,
- implicit safe conversions,
- explicit lossy conversions,
- generic type inference,
- unit-aware pins,
- shape-aware tensor pins,
- regions,
- subgraphs,
- reroutes,
- comments/groups,
- search,
- favorites,
- categories,
- documentation preview,
- error highlighting,
- live values,
- execution heatmap,
- diff/merge visualization.

---

# 165. LARGE GRAPH SCALABILITY

Do not assume a program fits one canvas.

Need:

- functions,
- modules,
- nested graphs,
- collapsible regions,
- interfaces,
- node search,
- graph navigation,
- call hierarchy,
- dependency hierarchy,
- semantic bookmarks.

A 10,000-node flat graph is a failure of authoring design.

---

# 166. NOODLE GRAPH IS NOT THE SEMANTIC MODEL

Graph coordinates/edges are UI.

Canonical semantics are operations/values/regions/symbols.

This allows:

- textual editing,
- graph auto-layout,
- compiler transformations,
- semantic diff,
- alternative visual lenses.

---

# 167. TEST MATRIX — IR CORE

Permanent tests:

- parser/printer round-trip,
- binary round-trip,
- stable hash,
- malformed IR,
- dominance,
- region legality,
- unknown dialect behavior,
- dialect versioning,
- op schema generation,
- builder/text equivalence,
- source-map preservation.

---

# 168. TEST MATRIX — COMPILER

- optimizer vs reference executor,
- fusion equivalence,
- split equivalence,
- memory plan correctness,
- async schedule correctness,
- effect hazard tests,
- capability specialization,
- provider partition tests,
- autotune determinism mode.

---

# 169. TEST MATRIX — RENDERING

- scene raster,
- MRT,
- depth/stencil,
- indirect/count,
- mesh,
- tessellation,
- Forward+,
- deferred,
- visibility,
- hybrid RT,
- dynamic work.

Both Vulkan and D3D12 where feature availability permits.

---

# 170. TEST MATRIX — COMPUTE

- add,
- reduction,
- scan,
- sort,
- FFT,
- GEMM,
- dynamic dispatch,
- persistent/device work.

Numerical oracle.

---

# 171. TEST MATRIX — TENSOR/ML

- shape inference,
- dynamic shapes,
- layout,
- quantization,
- fusion,
- MLP,
- attention,
- provider partition,
- cooperative matrix/vector,
- native graph provider where available.

---

# 172. TEST MATRIX — HOT RELOAD

Test changes to:

- function body,
- function signature,
- resource contract,
- state schema,
- CKIR dependency,
- transform schedule,
- provider choice,
- graph topology.

Verify:

- compatible swap,
- incompatible rejection,
- last-good,
- generation safety,
- state migration.

---

# 173. MATURITY MODEL

Use L0–L8.

```text
L0 — listed/research target
L1 — semantics/ADR defined
L2 — IR op/types/verifier landed
L3 — reference or one-provider execution
L4 — optimized execution on primary backend
L5 — Vulkan + D3D12 / relevant multi-provider proof
L6 — shipped engine:// program asset
L7 — CR-D007 authoring + hot reload + diagnostics
L8 — production stress/perf/determinism qualification
```

Do not call an algorithm complete because its CKIR math exists.

---

# 174. MACHINE-READABLE FEATURE MANIFEST

Track:

- CEIR capability,
- maturity,
- assets,
- tests,
- providers,
- fallbacks,
- editor support,
- determinism tier,
- performance board.

Generate docs/status from manifest.

---

# 175. IMPLEMENTATION BANDS

## CEIR-0 — Repository inventory + architecture ADRs

Deliver:

- current execution-path map,
- frame/executor/command/CKIR asset map,
- current C++ scripting conflict,
- proposed language-stack ADR,
- CEIR/CHIR/CKIR ownership ADR,
- native-intrinsic ADR,
- migration table,
- dependency graph.

**Gate:** no ambiguous ownership remains.

---

## CEIR-1 — Core IR substrate

Implement:

- context/module,
- operation,
- value,
- block,
- regions,
- symbols,
- functions,
- attributes/properties,
- source locations,
- dialect registry,
- op interfaces/traits,
- deterministic printer/parser,
- builder,
- bytecode/serialization.

**Gate:** typed hello-world program round-trips text/binary/builder.

---

## CEIR-2 — Schema/ODS-like generator

Implement Cerid-owned op-definition schemas.

Generate:

- wrappers,
- builders,
- verifier metadata,
- reflection,
- docs,
- editor node schemas.

**Gate:** new pure op can be added with no central enum/switch edits.

---

## CEIR-3 — Type/shape/unit/lifetime foundation

Implement:

- core types,
- resource types,
- shape types,
- quantity metadata,
- ownership/view types,
- generic constraints.

**Gate:** invalid unit/shape/view combinations rejected.

---

## CEIR-4 — Effect + determinism model

Implement:

- effect interfaces,
- resource ranges,
- nondeterminism effects,
- domain legality,
- race/hazard foundations.

**Gate:** compiler distinguishes reorderable vs ordered ops correctly.

---

## CEIR-5 — Structured control flow + functions

Implement:

- if/switch/loops,
- regions,
- calls,
- recursion policy,
- state/delay basics.

**Gate:** nontrivial program executes in reference host runtime.

---

## CEIR-6 — Async/task/runtime domains

Implement:

- async tokens,
- task groups,
- host job provider,
- execution domains,
- cancellation/deadline basics.

**Gate:** job-system parallel program is authored as CEIR.

---

## CEIR-7 — Asset/cook/runtime lifecycle

Implement:

- CEIR asset type,
- cook,
- hashes,
- deps,
- RuntimeSlot/Handle,
- hot reload,
- execution-plan cache.

**Gate:** live CEIR program hot swaps safely.

---

## CEIR-8 — Reference executor + compiled host plan

Implement:

- semantic/reference subset,
- compact execution plan,
- pre-resolved ops/intrinsics,
- profiler hooks.

**Gate:** no source parsing/string lookup in shipping execution loop.

---

## CEIR-9 — Resource/memory subsystem

Implement:

- views/ranges/subresources,
- allocations,
- transient/persistent/history,
- alias/lifetime analysis,
- memory planner interface.

**Gate:** resource graph lifetime tests + aliasing.

---

## CEIR-10 — Compute + transfer

Implement:

- compute dialect,
- transfer dialect,
- CKIR integration,
- direct/indirect dispatch.

**Proof:** add/reduction/scan/FFT slice on both backends.

---

## CEIR-11 — Render dialect

Implement:

- generalized attachments,
- render scopes,
- draws,
- indirect/count,
- mesh/tess,
- resource tables.

**Proof:** triangles/MRT/depth/indirect/mesh/tess on both backends.

---

## CEIR-12 — FrameGraph unification

- `.frame.toml` → CEIR frontend,
- programmatic builder → CEIR,
- frame compiler → CEIR scheduling,
- remove duplicate runtime.

**Gate:** every shipped frame asset runs through CEIR.

---

## CEIR-13 — Executor migration

Migrate all composite C++ executors to CEIR assets.

Keep only true atomic intrinsics.

**Gate:** scene raster is a CEIR asset and legacy executor path can be deleted.

---

## CEIR-14 — Scene/ECS/geometry bridge

Implement scene/query/resource resolver semantics.

**Proof:** rigid/skinned/indirect scene rendering and GPU culling.

---

## CEIR-15 — Renderer architecture proof suite

Ship:

- Forward+
- Clustered
- Deferred
- Visibility
- GPU-driven

as CEIR assets.

**Gate:** no new native pass algorithm needed.

---

## CEIR-16 — RT

Implement production RT dialect and AS/SBT semantics.

**Proof:** hybrid RT + wavefront path tracer.

---

## CEIR-17 — Dynamic/device work

Implement semantic work dialect.

First lowering:

- compute queues + indirect.

Then:

- D3D12 Work Graph,
- Vulkan DGC,
- Vulkan shader enqueue,
- Metal ICB,
- CUDA graph/device launch where available.

**Gate:** same semantic program uses multiple lowerings.

---

## CEIR-18 — Tensor/shape/layout

Implement high-level tensor IR.

**Proof:** chained tensor workflow without premature raw dispatch lowering.

---

## CEIR-19 — CRD-Hesap integration

Route GPU/mixed Hesap workflows through CEIR.

**Proof:** GEMM → FFT → reduction → visualization prep.

---

## CEIR-20 — Sparse + quantization

Implement sparse/quant/layout semantics.

**Proof:** sparse op + quantized MLP path.

---

## CEIR-21 — ML provider architecture

Implement ML dialect/provider partitioning.

**Proof:** MLP + attention; CKIR and native-provider strategies.

---

## CEIR-22 — Autodiff

Implement transform-level differentiation.

**Proof:** differentiable compute/render example.

---

## CEIR-23 — Optimizer phase 1

- canonicalization,
- DCE,
- CSE,
- partial evaluation,
- specialization,
- inlining,
- basic fusion,
- memory planning,
- scheduling.

---

## CEIR-24 — Transform/rewrite programs

Implement authorable transform schedules and declarative rewrite infrastructure.

**Proof:** two different schedules optimize same semantic program.

---

## CEIR-25 — Autotuning / PGO / cost model

Implement profiling-fed strategy selection.

**Proof:** target-specific configuration cache.

---

## CEIR-26 — Heterogeneous/native graph providers

Investigate/implement:

- D3D MLIR Program,
- Vulkan tensor/data graph,
- NPU/provider interface,
- graph partitioning.

**Proof:** one graph partitioned across two provider classes.

---

## CEIR-27 — Multi-device/distributed

Implement device mesh, communication and sharding foundation.

**Proof:** multi-device reduction/training slice.

---

## CEIR-28 — Media/UI/audio bridges

Route media, UiEffectGraph and audio/DSP workflows through CEIR rather than inventing new schedulers.

---

## CEIR-29 — CHIR / Cerid language prototype

After CEIR is stable enough:

- textual language,
- semantic source model,
- visual projection,
- modules,
- user types,
- functions,
- generics,
- events/tasks,
- hot reload.

**Proof:** same small application program authored visually and textually.

---

## CEIR-30 — CR-D007 universal program editor

Build all program-authoring/debugging views.

---

## CEIR-31 — Legacy deletion

Delete:

- composite executor architecture,
- duplicate frame runtimes,
- privileged programmatic paths,
- stale hand-maintained program lists,
- old scripting assumptions superseded by ADR,
- hidden renderer algorithms.

**Gate:** one execution-program architecture.

---

## CEIR-32 — Production qualification

- cross-backend,
- validation layers,
- ASan,
- fuzzing,
- deterministic cook,
- hot-reload stress,
- large graph stress,
- numerical correctness,
- performance boards,
- documentation,
- examples.

---

# 176. PAUSE POLICY

Before CEIR-13 completes:

Do not expand the renderer/default-program library dramatically through old C++ executor behavior.

Allowed:

- bug fixes,
- unsafe half-migration completion,
- foundational RAH work required by CEIR,
- CKIR fixes,
- tests,
- documentation,
- CEIR vertical slices.

After executor migration:

resume broad renderer/compute/ML/UI feature development as CEIR program assets.

---

# 177. WHAT MAY REMAIN NATIVE FOREVER

Examples:

- backend API calls,
- OS/window APIs,
- filesystem/network implementation,
- device enumeration,
- swapchain acquire/present,
- external library invocation,
- vendor compiler invocation,
- hardware codec/NPU provider,
- native sensor/driver integration,
- job-system implementation,
- compiler/runtime implementation itself.

The **algorithmic use** of these capabilities should still be CEIR where possible.

---

# 178. WHAT MUST NOT REMAIN NATIVE-ONLY

If expressible with existing semantics, these should not require bespoke executor code:

- renderer architectures,
- culling algorithms,
- lighting algorithms,
- post-processing,
- GI workflows,
- RT workflows,
- compute chains,
- numerical workflows,
- tensor graphs,
- ML graphs,
- media processing chains,
- UI effect graphs,
- geometry pipelines.

---

# 179. DEFINITION OF DONE — UNIVERSALITY

CEIR is not "done" because it runs a triangle.

Before calling the architecture universal:

1. typed SSA works,
2. graph + CFG + structured regions work,
3. extensible dialects work,
4. op interfaces work,
5. program assets work,
6. programmatic builder parity works,
7. resource/effect model works,
8. determinism model exists,
9. shape/layout exists,
10. host tasks work,
11. compute works,
12. raster works,
13. frame graph is unified,
14. composite executors are migrated,
15. scene raster is asset-authored,
16. RT works,
17. dynamic device work has a semantic representation,
18. tensor graph exists,
19. CRD-Hesap routes through it,
20. ML graph/provider seam exists,
21. hot reload works,
22. source mapping works,
23. CR-D007 can inspect it,
24. custom app algorithms require no engine edits,
25. custom native capabilities can be registered without central enums.

---

# 180. DEFINITION OF DONE — VISUAL LANGUAGE READINESS

Before calling it a visual language:

1. stable node IDs,
2. typed pins,
3. functions/subgraphs,
4. structured control-flow regions,
5. generics/type inference,
6. source mapping,
7. graph/text semantic parity,
8. node generation from op/function schemas,
9. layout separated from semantics,
10. semantic diff,
11. hot reload,
12. debugger/profiler hooks,
13. no visual-only runtime.

---

# 181. DEFINITION OF DONE — FUTURE SCRIPTING READINESS

Before claiming CEIR can underlie Cerid scripting:

1. language ADR accepted,
2. CHIR/source layer defined,
3. ownership/state/error model defined,
4. structured concurrency defined,
5. events/state-machine story defined,
6. ECS bridge defined,
7. capability security defined,
8. deterministic time/RNG defined,
9. host compiled execution path exists,
10. native FFI/intrinsic versioning exists,
11. state migration exists,
12. text/visual source share semantics.

---

# 182. DEFINITION OF DONE — FRONTIER GPU READINESS

1. DGC/Work Graph/device graph semantics are not API-specific.
2. At least one indirect fallback works.
3. At least one modern device-generated lowering works.
4. tensor high-level semantics survive until provider selection.
5. cooperative matrix/vector lowering is possible.
6. native data-graph/MLIR program target is architecturally supported.
7. RT/SER/micromap capability model exists.
8. sparse residency/resource table model does not block virtualized geometry.
9. profiling and autotune hooks exist.
10. CR-D007 displays selected lowering.

---

# 183. RESEARCH REFERENCES TO STUDY DURING DESIGN

These are architectural references, not dependencies to clone blindly.

## MLIR

- Language reference — operations, SSA values, blocks, graph/CFG regions:
  https://mlir.llvm.org/docs/LangRef/
- Interfaces:
  https://mlir.llvm.org/docs/Interfaces/
- Defining dialects / dynamic extensibility:
  https://mlir.llvm.org/docs/DefiningDialects/
- ODS:
  https://mlir.llvm.org/docs/DefiningDialects/Operations/
- Transform dialect:
  https://mlir.llvm.org/docs/Dialects/Transform/
- GPU:
  https://mlir.llvm.org/docs/Dialects/GPU/
- Tensor/Linalg/Sparse/Quant/Shape/MPI dialects:
  https://mlir.llvm.org/docs/Dialects/

## IREE

- Flow/Stream/HAL model:
  https://iree.dev/reference/mlir-dialects/
  https://iree.dev/reference/mlir-dialects/Stream/

## OpenXLA

- XLA GPU compiler architecture:
  https://openxla.org/xla/gpu_architecture
- StableHLO:
  https://openxla.org/stablehlo/spec
- Shardy:
  https://openxla.org/shardy/overview

## Triton

- GPU kernel DSL/compiler:
  https://triton-lang.org/main/
- Gluon lower-level model:
  https://triton-lang.org/main/getting-started/tutorials/gluon/intro.html

## DirectX 2026

- MLIR Programs / Compute Graphs:
  https://microsoft.github.io/DirectX-Specs/d3d/MlirPrograms.html
- Work Graphs:
  https://microsoft.github.io/DirectX-Specs/d3d/WorkGraphs.html
- Shader Model 6.9:
  https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_9.html
- D3D Linear Algebra:
  https://devblogs.microsoft.com/directx/d3d12-linalg-preview/

## Vulkan 2026

- Roadmap 2026:
  https://github.khronos.org/Vulkan-Site/spec/latest/appendices/roadmap.html
- Device Generated Commands:
  https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_EXT_device_generated_commands.html
- Shader Enqueue:
  https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_AMDX_shader_enqueue.html
- Cooperative Matrix:
  https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_KHR_cooperative_matrix.html
- Cooperative Vector:
  https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_NV_cooperative_vector.html
- Tensors:
  https://github.khronos.org/Vulkan-Site/features/latest/features/proposals/VK_ARM_tensors.html
- Data Graph:
  https://github.khronos.org/Vulkan-Site/spec/latest/chapters/VK_ARM_data_graph/graphs.html

## CUDA

- CUDA Graphs, conditional graph nodes, memory nodes, device launch:
  https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html

## Metal

- Indirect Command Buffers:
  https://developer.apple.com/documentation/metal/indirect-command-encoding

## ONNX Runtime

- heterogeneous execution-provider partitioning:
  https://onnxruntime.ai/docs/reference/high-level-design.html

## Shader / material language references

- Slang generics/interfaces/autodiff:
  https://shader-slang.org/
- MaterialX:
  https://materialx.org/
- NVIDIA MDL:
  https://developer.nvidia.com/rendering-technologies/mdl-sdk

## Node/visual-program references

- Houdini VOP/VEX/compiled blocks:
  https://www.sidefx.com/docs/houdini/
- Unity visual scripting:
  https://docs.unity3d.com/Manual/com.unity.visualscripting.html

Do not make Cerid semantically dependent on any one vendor or external IR.

---

# 184. FINAL AGENT REPORT

After integrating this roadmap into D-007, report:

1. current working-tree architecture discovered,
2. stale/conflicting statements found,
3. new ADRs proposed,
4. D-007 sections changed,
5. final CEIR/CHIR/CKIR ownership map,
6. exact executor migration inventory,
7. exact FrameGraph migration inventory,
8. exact current program asset inventory,
9. current CKIR capabilities relevant to CEIR,
10. module dependency plan,
11. CEIR band order,
12. first implementation slice,
13. tests that will guard it,
14. explicit deletion list,
15. any design question that truly cannot be resolved from code/research.

Do not silently reduce scope.

Do not reinterpret "universal" as "renderer universal."

Do not treat ML, CRD-Hesap, media, UI, audio, physics, distributed compute or future scripting as somebody else's scheduler problem.

The shared compiler/execution substrate must be designed now so these domains can converge on it.

---

# 185. FINAL FINISH LINE

The system is successful when a Cerid user can open CR-D007 and author, save, inspect, hot reload, profile and reuse programs such as:

```text
Forward+ renderer
clustered renderer
visibility-buffer renderer
new experimental GI algorithm
wavefront path tracer
Nanite-class alternative traversal
GPU geometry pipeline
CRD-Hesap compute workflow
tensor solver chain
ML inference/training graph
neural-rendering algorithm
media processing pipeline
UI compositor effect
2D rendering pipeline
physics simulation pipeline
audio/DSP graph
application behavior/state workflow
```

and these programs can be expressed visually or textually, composed from libraries, lowered to the best legal CPU/GPU/NPU/provider strategy, and executed without writing bespoke C++ algorithm/executor code.

C++ remains available for native extension and programmatic authoring.

But C++ is no longer where ordinary new algorithm composition has to live.

That is the destination.
