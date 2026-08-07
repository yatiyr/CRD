# CEIR-0e — CHIR-0: the Cerid high-level language design note (DESIGN-ONLY)

> **This is a DESIGN NOTE, not an ADR — it decides nothing.** It weighs options, sketches directions, and states
> what is deferred. The binding CHIR decisions are made by a future ADR at **CEIR-29**, *after* the CEIR corpus
> exists to correct this design with real evidence. ⛔ **ZERO implementation** follows from this note — CHIR is not
> built until CEIR-29 (mission §4). Its purpose: so CHIR-1 starts from a reviewed design instead of a blank page.
>
> **Band:** D-007 · CEIR-0 · slice 0e. **Tracker row:** `docs/detours/D-007-ceir-tracker.md` → CEIR-0e.
> **Law:** mission `docs/research/2026-08-07-ceir-universal-programming-master-roadmap.md` §4 (why CHIR is above
> CEIR), §98 (language feature set), §19 (ownership), §10 (text/visual projection). **Parents:** ADR-0108 (the owned
> language stack + the non-negotiables), ADR-0109 (CHIR lowers into CEIR; the semantic-identity model).
> **Status:** ACCEPTED as the design DIRECTION (2026-08-07, user-approved at the CEIR-0e gate). ⛔ The binding
> language decisions remain deferred to the CEIR-29 ADR (against the corpus); accepting this note accepts the
> *direction* (the §5 composite ownership leaning, the constraints, the deferral), not a specification.

---

## 1. What CHIR is, and what this note is for

CHIR is the **Cerid High-level IR / language layer** — the source-language semantics that must NOT be baked into
the execution IR (§4): modules, generics, ADTs, closures, traits, ownership, async/state/events, reflection. It
**lowers or erases into CEIR** (ADR-0109 §3, one-way). It is a *real language project* (parser, semantic analysis,
trait solving, ownership checking, diagnostics, tooling) — sequenced at CEIR-29, after CEIR is stable enough that
the proof corpus (renderers, tensor/ML graphs, UI effects, physics, audio) teaches the language what it must make
ergonomic (mission §4). Designing it before that corpus would optimize syntax for programs nobody has written.

This note fixes the **constraints already locked** (ADR-0108/0109), **weighs the load-bearing open question**
(ownership, §5), sketches **direction** (syntax, §6; the text/visual model, §7), and lists **what CEIR-29 decides**
(§8). Nothing here is binding.

## 2. Constraints already locked (not open — inherited)

From ADR-0108 (§3 non-negotiables) + ADR-0109, CHIR **must**:

- have **no mandatory tracing GC in real-time hot paths** (§19 §98) — deterministic lifetimes;
- expose **deterministic time + RNG** (§27 §58) — the ADR-0063 replay contract survives;
- be **capability-secured** (§99) — a program declares capabilities; agent/untrusted programs sandbox;
- reach C++ via **versioned native intrinsics / FFI** (§100 / ADR-0110) — the escape hatch, not arbitrary pointers;
- carry **unit-aware quantities** (§17, ADR-0078 two-layer) at API surfaces;
- be **Cerid-owned** — no Lua/Python/JS/embedded third-party VM (ADR-0108);
- share the **semantic-identity model** with CEIR (ADR-0109 §6: stable ids · source spans · layout-separated);
- lower to CEIR — CHIR owns *language* semantics, CEIR owns *execution*; the split (§4) is not re-openable.

## 3. The §98 feature set — the target surface (not all of CEIR-29 v1)

The language CHIR aims at (mission §98), grouped by how it relates to CEIR:

| Group | Features | Lowering relationship |
|---|---|---|
| **Erased before CEIR** (compile-time / type-level) | modules · packages · imports · namespaces · generics · interfaces/traits · compile-time constants/functions · attributes · reflection metadata · **serialization metadata** (named distinctly from reflection) · **extension methods** (§98 "if desired" — ⚠ DELIBERATELY DEFERRED past CEIR-29 v1 unless the corpus demands them; named here so the omission is a decision, not a silent drop) | resolved/monomorphized/erased by the CHIR→CEIR lowering; CEIR sees concrete typed ops |
| **Lower to CEIR structure** | functions · structs · enums · tagged unions · pattern matching · closures/lambdas · Result/Option · events · state machines · async/await · generators/coroutines · structured concurrency | map onto `ceir.func` (§34), structured regions (§13/§14), `ceir.async`/`ceir.task` (§37/§38), `ceir.event`/`ceir.state` (§66/§67), tagged-union/`Option`/`Result` types (§16) |
| **Runtime contracts** | value semantics · explicit references/handles · deterministic RNG/time · hot reload · native FFI/intrinsics · unit-aware quantities · **no mandatory GC in hot paths** | become CEIR effects/domains/state ops + the ADR-0110 intrinsic seam; the ownership model (§5) governs the memory ones |

CEIR-29 v1 need not ship all of this — but the ownership/state/error/concurrency **foundations** (§5, and the
CEIR bands that already build `ceir.async`/`ceir.state`/`Result`) must not be precluded.

## 4. Why the corpus must come first (the sequencing, restated)

The best language decisions — which sugar matters, what the ownership story should *feel* like, which generic
constraints are actually needed — come from pain felt while authoring **real programs**. The CEIR proof suite
(§128–§143: scene.raster, Forward+, wavefront PT, GEMM→FFT chains, MLP/attention, UI effects, DSP graphs) IS that
corpus. So CHIR-1 (CEIR-29) starts from *this note + corpus evidence*, and the CEIR-29 ADR is where syntax/ownership
crystallize against what the corpus proved painful. This note is deliberately a **direction**, not a specification.

## 5. The load-bearing open question — the ownership model (§19)

CHIR needs deterministic lifetimes without a mandatory GC. The options, weighed against Cerid's two hard
constraints — **(a)** no GC in hot paths, **(b)** the language must be **visually authorable AND agent-authorable**
(a model humans-at-a-graph and LLM agents can both satisfy):

| Option | What it is | For | Against |
|---|---|---|---|
| **Value semantics** (copy/move) | data is values; big data moves | simple · deterministic · no GC · trivial to author/visualize | large/shared/cyclic data needs references |
| **Full borrow-checker** (Rust-style) | compile-time ownership + lifetimes | maximal safety, no GC | ⛔ steep; **hard to express visually**; **hard for agents to satisfy** (lifetime errors are the #1 Rust friction) — fights constraint (b) |
| **Generational handles** | `Handle<T>` into a slot map; generation check on deref | **Cerid already lives on these** (resources, scene, jobs); safe vs use-after-free; shareable; long-lived | a deref indirection; not zero-cost for tight inner loops |
| **Arenas / region lifetimes** | bulk-allocate in an arena; free the region | **Cerid already has frame arenas**; ideal for transient bulk (per-frame, per-pass) | region discipline must be expressible; not for long-lived objects |
| **Explicit state stores** (§20) | persistent state is a named slot, separate from the program asset | matches CEIR's explicit-state rule (§20 — TAA history, GI, sim, audio delay); clean hot-reload state migration (§109) | only covers *persistent* state, not general memory |

**Leaning direction (to be confirmed by CEIR-29 against the corpus): a COMPOSITE model native to Cerid, not a
single mechanism.**
- **Value semantics by default** for ordinary data (simple, visual, deterministic).
- **Generational handles** for shared / long-lived objects (Cerid's existing idiom — humans and agents already
  reason about handles, and the generation check makes dangling a runtime-caught error, not UB).
- **Arenas / region lifetimes** for transient bulk (the frame-arena idiom, first-class).
- **Explicit state stores** for persistence (already required by CEIR §20).
- **A LIGHT borrow notion, NOT a full checker:** a *borrowed view* that the verifier forbids from outliving its
  region — the ownership/view qualifiers **CEIR-3f is specified to encode** (⬜ not yet built). CHIR surfaces borrows as this bounded
  form (roughly: no stored borrows, no borrow-returning-functions without a region parameter), which catches the
  common dangling bug without the full-lifetime-inference tax that fights visual + agent authoring.

Rationale: this composite is what the engine *already is* (values + handles + arenas + non-owning views), so it is
the model Cerid programmers, agents trained on Cerid, and the visual editor already understand — constraint (b) is
the tiebreaker that rejects the full borrow-checker despite its safety, because a language nobody can author
visually or an agent can't satisfy fails CEIR's whole agent-native/visual premise.

## 6. Syntax direction — ILLUSTRATIVE sketches (not a grammar)

Not a specification — a feel, to make the §5 ownership model and the §98 features concrete. CEIR-29 owns the real
grammar.

```
// a function; unit-aware params (ADR-0078); Result error model (§29); value + handle mix
fn build_light_grid(depth: Handle<Image<D32F>>, lights: &[Light], near: Length<f32>) -> Result<Handle<Buffer<LightIndex>>, GfxError> {
    let grid = arena.alloc_buffer::<LightIndex>(tile_count());   // arena-scoped transient
    for tile in tiles() {                                        // structured loop → ceir.core region
        grid[tile] = cull_tile(depth, lights, tile)?;           // ? propagates the Result
    }
    Ok(grid.into_persistent())                                  // hand off ownership as a handle
}

// generics + trait bound (erased before CEIR); a compute kernel referenced by identity
fn reduce<T: Numeric>(xs: Tensor<[N], T>) -> T { ceir.compute(@sum_kernel, xs) }

// explicit persistent state (§20) — a state store, not a global; survives hot reload per §109
state taa_history: Image<RGBA16F>;

// structured concurrency (§30) — a scope that joins; no detached threads by default
parallel for chunk in scene.chunks() { cull(chunk) }
```

The visual projection of the same program is a graph over the same semantic nodes (§7) — `build_light_grid` becomes
a node with typed pins `depth`/`lights`/`near` → `light indices`, exactly as mission §11 shows.

## 7. The text/visual projection model (§10) — half-specified by ADR-0109 (Proposed)

CHIR does not re-invent this — ADR-0109 §6 (Proposed, pending review) specifies the semantic-identity model (stable
ids · source spans · layout separated from semantics); if that identity model changes at review, this section
follows. CHIR adds only that **text and visual are two lenses on ONE structured source model** (§10):

- one canonical semantic model (nodes + stable ids); text and the CR-D007 graph are **projections**, not two
  runtimes (§166 — a noodle graph is UI, not semantics);
- a text edit reparses into the same semantic model; a graph edit updates the same model; **semantic diff is
  position-independent** (node movement ≠ semantic change);
- comments/docs retained where practical; layout (coordinates, grouping) in a side table keyed by semantic id.

This is why the semantic-identity model is scheduled to land at **CEIR-1c (with the core, ⬜ not yet built)**, not at
CEIR-29 — the editor and the language share it, so it must exist before either.

## 8. What CEIR-29 decides (deferred — this note does NOT)

- the **concrete grammar** + keyword set;
- the **final ownership model** (confirm/adjust §5's composite against the corpus) + the exact borrow surface;
- the **trait/generics** solver design + monomorphization-vs-dictionary lowering;
- the **error model** surface (Result/Option ergonomics, `?` semantics, panic vs typed-failure boundaries);
- the **async/coroutine** surface + how it lowers to `ceir.async`/`ceir.task`;
- the **module/package** system + versioning;
- the **reflection + serialization-metadata** surface;
- the **capability-declaration surface** — where a program declares `capability.gpu.compute` / `capability.file.write`
  (§99) in CHIR syntax (a constraint §2 locks but leaves un-sited);
- **hot-reload state migration** ergonomics (§109);
- the **standard library** boundary (what is language vs `ceir.*` dialect vs intrinsic).

## 9. Open questions to carry to CEIR-29

1. Does the §5 light-borrow surface catch enough of the real dangling bugs the corpus produces, or does a subset of
   the corpus (device work graphs? wavefront PT queues?) demand more lifetime expressiveness?
2. Monomorphization (fast, code-bloat) vs dictionary-passing (small, indirection) for generics — decide against
   measured corpus code size.
3. How much of §98's async/generator surface does the corpus actually need at CEIR-29 v1 vs defer?
4. Visual authoring of generics + traits — is there a lens (§9) that makes type-level programming visual, or do
   those stay text-only in v1?
5. **Generic functions referencing CKIR kernels** (the §6 sketch's `reduce<T: Numeric>` over `@sum_kernel`) — how
   does CHIR monomorphization interact with ADR-0104's kernel variant selection (VART)? A generic instantiated at
   `T = f32` and `T = f64` needs the matching CKIR variant; the CHIR→CEIR→`KernelRef` lowering + the VART cache must
   agree. A genuine design landmine the corpus will hit; also folds in the `into_persistent()` arena-escape
   semantics (copy cost / alias-freedom) the §6 sketch quietly assumes.
