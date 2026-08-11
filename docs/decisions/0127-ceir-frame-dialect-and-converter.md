# ADR-0127 — the `ceir.frame` dialect + the FrameGraphDesc↔ceir.frame converter placement

**Status: ACCEPTED 2026-08-11 (CEIR-15a).** Born by the CEIR-15 FrameGraph-unification band (§39/§126). Decision
packet: `docs/design/ceir-15-framegraph-unification.md`. Supersedes-in-place target: **ADR-0106** (struck @ CEIR-15f).

## Context

§126 requires all frame graphs to END at a `ceir.frame` dialect: `.frame.toml` and `FrameGraphBuilder` both emit it,
one runtime/compiler remains. REN-36's authorable frame graph already made a pass *common graph metadata + a typed
named-param payload* (the RAF-12.3 §7 fold), so the migration is a PROMOTION, not a rewrite. Two decisions need a
durable record: (1) the `ceir.frame` dialect shape, and (2) where the `FrameGraphDesc ↔ ceir.frame` converter lives.

## Decision 1 — the `ceir.frame` dialect (FOUR ops; resources reuse the resource dialect)

`ceir.frame` owns the frame graph's TOPOLOGY as a CEIR dialect (`engine/ceir/ops/frame.ceirop.toml`):

- **`frame.graph`** — a REGION op (`render.scope` precedent), the whole-frame container.
- **`frame.pass`** — ONE op per mechanic. The MECHANIC is the `executor` SYMBOL attr (the executor NAME; `ExecutorTypeId`
  = fnv1a at the boundary — never one-op-per-executor, which recreates the retired `FramePassKind` enum). Reads/writes
  ride ONE variadic `resources` tail tokened by `access` `{r|w|rw}` (opgen allows only the last operand variadic).
- **`frame.draw_list`** — an ECS-query value (all/any/none/cull/sort/limit).
- **`frame.history`** — the previous-frame READ of a `lifetime=history` resource (the ping-pong/TAA prev-frame case).

⭐ **The frame graph's RESOURCES are `resource.declare`/`resource.import` values — NO `frame.resource` op.** The CEIR-12b/c/d
lifetime/alias/memory planner is built OVER `resource.declare`'s attrs; a parallel op would orphan it. The 11
`FrameResourceKind`s decompose onto EXISTING axes: Transient*→`lifetime=transient`, Persistent→`lifetime=persistent`,
**PingPong→`lifetime=history, history_length=1`**, External{Buffer,Texture}/AccelerationStructure→`resource.import`,
Indirect/Structured/Counter→declare + frame-scoped attrs. Frame SIZING (width/height/scale/samples/mips/depth_buffer/
no_alias/resizable/format) rides as OPEN attrs on the declare (CEIR attrs are an open set), validated by
`Context::find_frame_misuse` when the declare sits inside a `frame.graph` region. Executor params ride as OPEN attrs on
`frame.pass`, validated at cook against the EXECUTOR schema (15c). Consequence: CEIR's `resource_root` + `ops_hazard`
machinery DERIVES the frame's barriers + transient lifetimes — the §159 realization (the hand-rolled lifetime/barrier
pass becomes a CEIR analysis pass). ⭐ The `frame.history` false-RAW fix is STRUCTURAL: `resource_root` chases only
`resource.view` by op kind (the 13d retrofit), so a distinct op result is its own root — read-of-prev vs write-of-curr
are different identities, no hazard special-case.

## Decision 2 — the converter lives IN `crd-frame-cook` (a new acyclic `→ crd-ceir` edge)

The `FrameGraphDesc ↔ ceir.frame` converter needs `FrameGraphDesc` (in `crd-frame-cook`) AND `crd-ceir`. Per ADR-0109
§4.2, `crd-ceir` is HOST-ONLY and depends on none of its providers, so **`crd-frame-cook → crd-ceir` is acyclic by
construction** (frame-cook is a host cook module; no device, no shading language crosses I3/I5). No bridge module is
needed (the 13d `crd-ceir-gpu` bridge exists because a PROVIDER touches the device; a host-to-host cook converter does
not). Home: `crd-frame-cook`'s `frame_ceir.{hpp,cpp}`. Rejected: a `crd-frame-cook-ceir` bridge (F2) — unnecessary
indirection for a host-to-host edge.

## The converter contract (CEIR-15a/15b)

- **Forward** `to_ceir_frame(desc, ctx) → Module` (15a-3a): `func main { frame.graph { <resource.declare/import>*
  <frame.draw_list>* <frame.pass>* } }`. Resources become declares/imports (kind→lifetime/import + sizing/format/usage
  attrs); a NAME→Value map resolves each pass' reads/writes (FrameResourceRef by name) to `resources` operands + `access`
  tokens; the param bag maps to attrs (Float/Int/Bool/U32/Enum→matching AttrKind; Vec4→4 float attrs; String→String, the
  reference params shader/kernel/technique→Symbol). `for_each`/`queue`/`cull`/`sort` → the closed-vocab strings.
- **Backward** `from_ceir_frame(ctx, m, desc)` (15a-3b): the inverse, name-recovered from the declare `name` attrs.
- **Round-trip-identity gate** (15a-3b): `desc → ceir → desc′`, assert `emit_frame_toml(desc) == emit_frame_toml(desc′)`
  (the canonical-text fixpoint — `FrameGraphDesc` has no `operator==`; the emitted TOML is its canonical form, the §121
  render-text precedent). The BACKWARD converter also lets the existing runtime consume `ceir.frame` early
  (`ceir→desc→old runtime`), making 15e incremental, not a cliff.
- **Named-forward (§39 forward capabilities):** subgraphs / includes / anchors / injects (REN-37.6 composition, already
  `flatten_frame_graph`-expanded before build) + multi-window. The converter operates on the CORE (or flattened) desc;
  these are shaped-to-admit (nested `frame.graph`, an `include` attr) but not 15a.

## Consequences

**Positive:** the frame graph's topology becomes an inspectable, analyzable, hot-swappable CEIR dialect; its barriers
derive from CEIR's hazard machinery; `.frame.toml`/builder collapse to one representation; asset IDs preserved (the
`name` attr). **Cost:** a new `crd-frame-cook → crd-ceir` link edge (acyclic, verified). **ADR-0106** ("render-graph is
THE single live runtime") is superseded in place @ 15f — the RUNTIME collapses to one; the `.frame.toml`/`.crdm`/`.crdt`
authoring surfaces SURVIVE as frontends (§39/§125); nothing RAF built is discarded.
