# 14 — Variants, permutation & specialization: the complete guide

> *The deep dive. What a variant is, why permutation explodes, the two ways to author one, the IR primitives that make
> specialization work, the content hash that ties it all together — and the one bug that hid inside `optimize()` for the whole
> life of the pass until a kernel got specialized.*

This is the exhaustive record of the D-007 D3 variant system as it stands after 2026-07-22. Everything here is real code you can
read: `engine/shader-cook/{include/crd/shadercook/variant.hpp, src/variant.cpp}`, `engine/kir/include/crd/kir/ckir.hpp`
(`pin_const`, `optimize`, `specialize_kernel`, `is_resource_leaf`), and the gates
`[gpu-context][vulkan][gpu][variant]` / `[variant][ubergraph]` in `tests/gpu-context-vulkan/test_vulkan_context.cpp`.

---

## 1. The problem: permutation explosion

Real shaders have options. A surface shader might support: normal map on/off, an emissive term on/off, alpha-test on/off, 1–4
dynamic lights, skinned or static geometry. That's `2 × 2 × 2 × 4 × 2 = 64` distinct shaders — from *one* authored intent. A AAA
material system routinely has thousands of these. Two naïve strategies both fail:

- **Write each by hand.** 64 near-identical files that drift out of sync the moment you fix a bug in one.
- **Branch at runtime** (`if (hasNormalMap) …`). The dead branch still costs registers and occupancy; a GPU hates divergent,
  rarely-taken branches, and the compiler can't prove the flag is uniform.

The right answer, used by every serious engine (UE5, Unity), is **static specialization**: author *one* shader that contains
every branch, and at cook time produce a specialized shader per feature combination with the unused branches **compiled away**.
Each specialized shader is a **variant**; the source shader is an **übershader**; the full set is the **permutation matrix**.

The two costs you must then control are **build cost** (don't cook variants nothing uses) and **storage/duplication** (don't ship
two identical variants twice). Cerid solves both with one mechanism — the content hash — which we'll get to.

---

## 2. The vocabulary, precisely

- **Variant** — one concrete configuration of an übershader. Represented as a `key`: a `crd::u32` bitmask where bit *i* means
  "feature *i* is on".
- **Übershader** — one shader authored to cover all feature combinations.
- **Übergraph** — the CKIR graph form of an übershader: one `KGraph` carrying every branch, with `ShaderOption` selector nodes
  that get pinned per variant.
- **Specialization** — turning an übergraph + a key into a minimal specialized graph: pin the options to the key's bits, then
  const-fold the now-constant conditions and DCE the dead branches.
- **Permutation matrix** — the set of variants a shader can produce. Cooking it = cooking each requested key.
- **Dedup** — collapsing variants whose specialized IR is identical to one cooked bundle.

---

## 3. The IR substrate: what a `ShaderOption` actually is

There is no special "option" node type. A `ShaderOption` is just a **`Const` leaf** used as a selector:

```cpp
const int opt0 = g.constant(0.0, sh1, DType::F32);   // a toggle — default 0, pinned per variant
```

You gate behavior on it with the ordinary branchless `select` (both arms are values; the condition picks one):

```cpp
const int cond  = g.binary(KOp::CmpGt, opt0, g.constant(0.5, sh1, DType::F32));
const int scale = g.select(cond, g.constant(2.0, …), g.constant(1.0, …));   // opt0>0.5 ? 2 : 1
```

`select(cond, a, b)` builds a node with `a=a, b=b, c=cond`. That `c` is the fold point: when `cond` becomes a compile-time
constant, `optimize()` replaces the whole `select` with the chosen arm and DCEs the other. So "an option-gated branch" is nothing
more than `select` reading a `Const` you haven't pinned yet.

---

## 4. Style A — the per-key builder

The first (and most general) way to author a variant is a function that builds the kernel *for a given key*, emitting only the
live path. This is exactly how a real material compiler cooks per-permutation.

```cpp
using VariantBuildFn = KEntry (*)(KGraph& g, u32 key, void* user);

KEntry build_scale_variant(KGraph& g, u32 key, void*) {
    const int inbuf  = g.buffer_decl(F32, 0, 0, false);
    const int outbuf = g.buffer_decl(F32, 0, 1, true);
    const int lid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const double scale = (key & 1u) ? 2.0 : 1.0;              // bit0 chosen HERE, in C++
    const int mark = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid, g.binary(KOp::Mul, g.buffer_load(inbuf, lid),
                                              g.constant(scale, sh1, F32)));
    KEntry e; e.stage = KStage::Compute; e.local_size[0] = 32;
    e.kernel_body_begin = mark; e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
```

Because the author only emits the live path, two keys that produce the same kernel construct **byte-identical IR**. It's
imperative and fully general — it works for any entry (compute kernel, material, anything), and the specialization logic lives in
plain C++ (`(key & 1u) ? …`). The downside: the branching is scattered through the builder rather than reading like one shader.

---

## 5. Style B — the übergraph + `specialize()`

The second way reads like a normal shader with `#ifdef`s. You build **one** graph with `ShaderOption` toggles and option-gated
selects, then call `specialize()` to pin the options to the key and fold the dead branches away:

```cpp
KEntry build_scale_ubergraph(KGraph& g, u32 key, void*) {
    const int inbuf  = g.buffer_decl(F32, 0, 0, false);
    const int outbuf = g.buffer_decl(F32, 0, 1, true);
    const int lid    = g.builtin(KBuiltin::LocalInvocationIndex);
    const int opt0   = g.constant(0.0, sh1, F32);            // ShaderOption 0 — selects the scale
    const int opt1   = g.constant(0.0, sh1, F32);            // ShaderOption 1 — DECLARED but UNUSED (dead)
    const int cond   = g.binary(KOp::CmpGt, opt0, g.constant(0.5, sh1, F32));
    const int scale  = g.select(cond, g.constant(2.0, …), g.constant(1.0, …));
    const int mark   = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid, g.binary(KOp::Mul, g.buffer_load(inbuf, lid), scale));
    KEntry e; e.stage = KStage::Compute; e.local_size[0] = 32;
    e.kernel_body_begin = mark; e.kernel_body_count = g.stmt_count() - mark;

    const int options[2] = {opt0, opt1};
    crd::shadercook::specialize(g, e, options, key, 2);      // ← pin to key + fold. THIS is the whole trick.
    return e;
}
```

`shadercook::specialize` is a five-line header helper (`variant.hpp`): it turns the bitmask into per-option values and calls the
kir primitive.

```cpp
inline void specialize(KGraph& g, const KEntry& e, const int* options, u32 key, int n) {
    f64 values[32];
    for (int i = 0; i < n; ++i) values[i] = ((key >> i) & 1u) ? 1.0 : 0.0;
    g.specialize_kernel(e, options, values, n);
}
```

**Both styles return a `KEntry`, and both flow through the *same* `cook_variant_matrix`.** That is the unification: Style B is
just Style A whose builder happens to delegate to `specialize()`.

---

## 6. `specialize_kernel` — the primitive, step by step

The load-bearing new primitive is `KGraph::specialize_kernel` (`ckir.hpp`). It's the **compute-kernel analogue of the material
`lower_entry`**: `lower_entry` gathers a material entry's output roots (clip position, `out[k].node`, discard, …), lowers them,
and writes them back; `specialize_kernel` does the same for a kernel's *imperative statement body*.

```cpp
void specialize_kernel(const KEntry& e, const int* options, const f64* values, int n_options) {
    // 1. pin each ShaderOption selector to its per-variant constant
    for (int i = 0; i < n_options; ++i) pin_const(options[i], values[i]);
    if (e.kernel_body_count <= 0) return;

    // 2. gather the ADDRESS of every node-ref field in the body (so we can write back after the renumber)
    Array<i32*> slots(al);
    for (int s = e.kernel_body_begin; s < e.kernel_body_begin + e.kernel_body_count; ++s) {
        KStmt& st = m_stmts[s];                  // MUTABLE — this is why the primitive lives inside KGraph
        if (st.target >= 0) slots.push_back(&st.target);   // BufferDecl / SharedDecl
        if (st.index  >= 0) slots.push_back(&st.index);    // the element index
        if (st.value  >= 0) slots.push_back(&st.value);    // stored value / For count / If cond
        if (st.result >= 0) slots.push_back(&st.result);   // atomic result node
    }

    // 3. optimize the DAG rooted at those refs (const-fold + DCE + CSE), which REWRITES roots[] to new ids
    Array<int> roots(al); roots.resize(slots.size());
    for (usize i = 0; i < slots.size(); ++i) roots[i] = *slots[i];
    optimize(roots.data(), (int)roots.size());

    // 4. write the new ids back into the statements
    for (usize i = 0; i < slots.size(); ++i) *slots[i] = roots[i];
}
```

Four things are worth internalizing:

1. **`pin_const` is destructive.** It overwrites the option node in place with a `Const`. So you must **build or copy the graph
   per variant** — the variant cook does exactly this: each key gets a fresh `KGraph` from its builder. Never specialize a graph
   you intend to reuse.
2. **Why gather field *addresses* (`i32*`), not values.** `optimize` renumbers nodes; the statements still hold the old node ids.
   `lower_entry` solves the same problem with an `int* slots[]` array — we mirror it. `KGraph::stmt(i)` is `const`, but
   `specialize_kernel` is a member, so it reaches `m_stmts[i]` mutably. That mutability is *the reason the primitive has to live
   inside `KGraph`* and not in the variant layer.
3. **Nested `For`/`If` bodies are covered for free.** A control-flow body lives *contiguously* in the statement range
   `[kernel_body_begin, +count)`, so the single loop already visits every nested statement's refs.
4. **Every root must be gathered, or DCE eats it.** `optimize` keeps only what's reachable from the roots you pass. If you forgot
   `st.target`, the `BufferDecl` it points at would be DCE'd and the kernel would reference a dead node. That's why all four
   fields are collected.

---

## 7. Inside `optimize()` — const-fold → DCE → compaction → CSE

`specialize_kernel` leans entirely on `optimize` (`ckir.hpp`). Knowing what it does explains both why specialization works and
where the bug (§9) hid. It runs four passes:

1. **Const-fold, in place.** Walk every node; if all its operands are known constants, evaluate it and overwrite it with a
   `Const` at the *same index*. This is where `select(Const(false), a, b)` becomes `b`, and `CmpGt(Const(0), Const(0.5))` becomes
   `Const(0)`. Certain ops are **skipped** — they have no operands but must never become a compile-time value: `Input`, `Iota`,
   loop constructs, and the *leaves* (`is_stage_leaf`: `StageIn`/`Builtin`/`UniformBlock`, and now `is_resource_leaf`).
2. **Reachability DCE.** Mark everything reachable from `roots[]`; the rest is dead.
3. **Compaction + renumber.** Rebuild the node array keeping only marked nodes, assigning new ids *in ascending order of old id*
   (so **relative order is preserved** — this is why input/binding order survives). Operand fields are rewritten to new ids.
4. **CSE (hash-cons).** As nodes are re-emitted they're interned; two structurally identical nodes merge into one. `node_equal`
   compares the *full* node — op, type, operands, **and** `iidx`/`dset`/`axes`/`cval`/shape/perm/ext — so two `BufferDecl`s at
   different bindings never merge.

The output: a minimal graph where the option is gone, the dead branch is gone, and identical structure is shared. `roots[]` now
holds the new ids of everything you passed in — which is why step 4 of `specialize_kernel` writes them back.

---

## 8. The content hash — one value, three jobs

Specialization gives you *minimal* IR. The **content hash** is what makes permutation cheap. It is:

```cpp
ResourceId id = ResourceId::from_content(serialize_graph(g, e));   // 128-bit hash of the specialized IR
```

The same value does three jobs across the deploy pipeline:

- **Cook cache key (D2)** — the bundle is written to `<id>_<backends>.crdr`; re-cooking the same IR is a lookup.
- **Variant dedup key (D3)** — two keys whose specialized IR hashes equal share one bundle.
- **Hot-reload change detector (D5)** — a shader is rebuilt only when its hash moves.

Because the dedup key and the cache key are *the same hash*, deduplication is **free**: `cook_variant_matrix` computes each key's
hash, and hands the graph to `cook_compute_shader`, whose own content-hash cache turns a duplicate into an instant cache hit — no
second compile. The matrix just counts distinct hashes to report the reduction.

```cpp
VariantMatrixResult cook_variant_matrix(build, user, keys, n, opts, a) {
    Array<ResourceId> seen;
    for each key:
        KGraph g; KEntry e = build(g, key, user);            // Style A or B — same call
        ResourceId id = from_content(serialize_graph(g, e)); // the dedup key
        if (id not in seen) seen.push_back(id);
        cook_compute_shader(g, e, name, opts_with_cache, a);  // duplicate ⇒ cache hit, no re-cook
        entries.push_back({key, id});                         // manifest: key → bundle hash
    result.requested = n; result.unique = seen.size();        // telemetry
}
```

`cook_one_variant(build, user, key, …)` is the on-demand single: cook exactly one key. Nothing forces you to enumerate the whole
matrix — you cook only what the scene asks for.

---

## 9. The scar: `optimize()` folded resource declarations into garbage

This is the debugging story worth keeping, because the bug was invisible for the *entire life of the pass* — it only surfaced the
first time a **compute kernel** (as opposed to a material expression) was specialized.

**Symptom.** The übergraph gate cooked (dedup even worked: `requested=4 unique=2`), but `create_pipeline_from_spirv` returned
`nullptr`, and `spirv_bytes == 0`. Dumping the emitted GLSL showed:

```glsl
void main() {
  precise float t4 = (buf0[gl_LocalInvocationIndex] * 1.0);
  buf0[gl_LocalInvocationIndex] = t4;      // BOTH read and write use buf0 — the two buffers collapsed
}
// ...and NO `buffer B0 {...}` / `buffer B1 {...}` declarations at all; n_inputs == 0
```

**Diagnosis.** Two false leads first: (a) "CSE merged the two buffers" — ruled out, `node_equal` compares `iidx`; (b) "a builtin
got folded" — ruled out, `is_stage_leaf` already covers `Builtin`. The real cause: `KOp::BufferDecl` has **no operands**
(`a=b=c=-1`), so in the const-fold pass all-operands-constant is trivially true, and `BufferDecl` was **not in the skip list**. So
`optimize` "folded" each buffer declaration into a garbage `Const` via `apply_unary(BufferDecl, 0)`. The emitter then found no
`BufferDecl` nodes to declare (`n_inputs == 0`), and the store/load referenced folded nodes that rendered as `buf0`.

**Why it never showed before.** `optimize` is used by material *lowering* (`lower_entry`), and materials declare resources with
`UniformBlock`/`StageIn`/`Texture` — `UniformBlock`/`StageIn` were already in `is_stage_leaf`. Plain storage-buffer kernels never
ran through `optimize` until `specialize_kernel` existed. The latent bug had simply never been reachable.

**Fix.** A sibling guard to `is_stage_leaf`:

```cpp
[[nodiscard]] inline bool is_resource_leaf(KOp op) noexcept {
    return op == KOp::BufferDecl || op == KOp::SharedDecl || op == KOp::Texture
        || op == KOp::Sampler || op == KOp::AccelStructDecl || op == KOp::RayPayloadDecl;
}
// in optimize's const-fold loop, next to the is_stage_leaf skip:
if (is_resource_leaf(g.op)) continue;   // resource decls NAME storage — never fold to a value
```

Full kir regression after the fix: **52420 assertions / 230 cases**, materials unaffected.

**The lesson, generalized.** `optimize()` will const-fold *any* operand-less node that isn't a `Const` and isn't explicitly
skipped. Every time you add a new leaf op to CKIR — a new resource kind, a new builtin, a new stage input — it must join a skip
list, or it becomes a landmine that only detonates when someone runs `optimize` on a graph containing it. Encode the invariant
where it's enforced (the two `is_*_leaf` predicates), not in a comment.

---

## 10. Gotchas & invariants

- **Specialization is destructive → copy per variant.** `pin_const` overwrites the option node; `optimize` renumbers everything.
  The variant cook gives each key a fresh graph. If you ever specialize a shared graph, you corrupt it for the next key.
- **`stmt()` is const on purpose.** Statements are POD facts about the kernel; general code shouldn't rewrite node-refs. Only the
  specialization primitive, which lives inside `KGraph`, may — via `m_stmts[i]` and the slot-address trick.
- **Input order must survive.** `optimize`'s compaction preserves relative order, so `BufferDecl`s keep their emit/binding order.
  If a future `optimize` change reordered kept nodes, kernel bindings would silently permute. (The emitter also names buffers by
  `iidx`, not position, which is a second line of defense.)
- **Dedup requires DCE, and DCE requires specialization.** Without folding the dead branches away, two keys that differ only in a
  *dead* option produce different IR (the dead option's pinned constant differs) and would **not** dedup. Style B gets dedup only
  because `specialize()` runs `optimize`. Style A gets it because the author emits only the live path in the first place.
- **`specialize_kernel` is compute-only today.** It walks the *kernel body statements*. Material expression graphs specialize
  through `lower_entry` instead (same `optimize`, different roots). A material übergraph is the natural next extension.

---

## 11. Worked example: 4 keys → 2 bundles, both styles

The gate uses two toggles: bit 0 selects the scale (×1 / ×2); bit 1 is **declared but unused** (dead). Four requested keys:

| key  | bit0 | bit1 | specialized kernel | hash    | outcome        |
|------|------|------|--------------------|---------|----------------|
| 0b00 | 0    | 0    | `out = in × 1`     | hash A  | cooked         |
| 0b01 | 1    | 0    | `out = in × 2`     | hash B  | cooked         |
| 0b10 | 0    | 1    | `out = in × 1`     | hash A  | **dedup → A**  |
| 0b11 | 1    | 1    | `out = in × 2`     | hash B  | **dedup → B**  |

**`requested=4 unique=2` — a 50% cut.** In Style A the dedup happens because `(key & 1u)` ignores bit 1, so keys 0/2 build
identical IR. In Style B it happens because `opt1` is unreferenced, so `optimize` DCEs it away and keys 0/2 fold to the same
graph. Both are verified to run GPU-correct (×1 and ×2) from their cooked SPIR-V, and both report the identical `4 → 2`.

---

## 12. Where this connects — materials, lighting, the node editor

- **Materials.** A material *is* an übergraph: normal-map on/off, emissive on/off, alpha-test on/off are `ShaderOption`s gating
  `select`s over the surface outputs. Material lowering (`lower_entry`) already specializes the expression DAG; the same content
  hash dedups material variants exactly like kernel variants. The surface-only material contract (material = surface, lighting =
  transport) means a material's permutations are independent of the lighting model.
- **Lighting.** Light-count variants (1–4 lights), shadow on/off, GI on/off are the same mechanism applied to the lighting
  kernels. Each lighting pass is a CKIR graph; its permutations cook and dedup through the identical path.
- **The node editor (future).** A visual graph editor is just a second front-end that emits the same `KGraph`. Toggles in the
  editor become `ShaderOption` nodes; the specialization, cook, dedup and hot-reload downstream neither know nor care that a
  human dragged nodes instead of calling C++ builders.

---

## The one-paragraph version

A variant is a `key`; an übershader is a graph with `ShaderOption` toggles; specialization pins those toggles and lets
`optimize()` const-fold the conditions and DCE the dead branches; the specialized IR's **content hash** is simultaneously the
cook cache key, the dedup key, and the hot-reload change detector, so cooking only what the scene asks for and never cooking a
duplicate both fall out for free. Two authoring styles — imperative per-key builder and declarative übergraph+`specialize()` —
converge on the same `cook_variant_matrix`. The only real hazard is that `optimize()` folds any operand-less non-`Const` node, so
every new leaf op must join a skip list — a rule now enforced by `is_stage_leaf` and `is_resource_leaf`.

---

*Companion lessons: [12 — the CKIR deploy pipeline](12-ckir-deploy-pipeline.md) (how a graph becomes shippable bytecode),
[13 — shaders, pipelines, materials & lighting](13-shaders-pipelines-materials-lighting.md) (the concepts), and
[11 — the shader-stage frontier](11-the-shader-stage-frontier.md) (the 14 stages + node editor). Reference: ADR-0104.*
