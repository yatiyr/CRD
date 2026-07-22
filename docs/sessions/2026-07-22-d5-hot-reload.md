# Session — 2026-07-22 · D5 hot-reload — the deploy chain closed (+ native `remainder`)

## `remainder` — the last math gap, done correctly

The user asked for `remainder` (deferred in D3 because the naïve `x − y·rint(x/y)` isn't bit-exact). Implemented natively via the
**fdlibm reduction**: `r = fmod(|x|, 2|y|)` then reduce into `[−|y|/2, |y|/2]` — ties-to-even fall out of having reduced mod 2|y|,
and each subtraction is exact by Sterbenz's lemma. Two scars caught by the bit-exact gate:
1. the sign is `signbit(x) ? -r : r`, **NOT** `copysign(r, x)` — remainder is ODD in x (`remainder(5.5,2) = -0.5`, positive input →
   negative result), so forcing x's sign was wrong (155 mismatches);
2. tiny/zero `|y|` needs the `r + r` comparison because `0.5*|y|` underflows to 0 (1 subnormal mismatch).

The special cases (`y==0`, `±inf`, NaN) then resolve **for free** through `fmod` semantics (`fmod(·,0)`/`fmod(±inf,·)`=NaN,
`fmod(·,±inf)`=|x|) — no guards, which also kept it tidy-clean (no `x != x` / `(x-x)` redundant-expression patterns). `[select]`
now **3380 assertions bit-exact vs std**; full math suite 6428/162. `crd::math` covers the common `<cmath>` surface natively.

## D5 — hot-reload

`engine/shader-cook/{reload.hpp, reload.cpp}`. `ReloadableCompute` holds one live compute pipeline and hot-swaps it when its
source graph changes — the last link that turns CKIR into a live edit loop.

- **`reload(g, e, name, backend, create_fn, user) → {ok, changed}`** — serializes the IR and content-hashes it (the D2 cache
  key). An **unchanged** graph re-uses the live pipeline (a cheap no-op, `changed=false`). A **changed** one cooks the graph,
  builds the new pipeline from the bundle's cooked bytecode + IR-reflection binding count, and **atomically swaps** it in.
- **Backend-agnostic** via a caller-supplied `PipelineCreateFn(code, n_bindings, user)` — the Vulkan/DX12 context provides
  `create_pipeline_from_spirv` / `_from_dxil`. No vulkan/dx12 dep leaks into the reloader's public API (only `crd-gpu-context`'s
  `ComputePipeline`).
- **In-flight safety** — the previous pipeline is RETIRED for one generation (kept alive until the next swap) so GPU work still
  referencing it never dangles; a real renderer drains frames-in-flight before the next reload. `generation()` ++ per swap.

**`[d5]` gate:** a `ReloadableCompute` loads kernel v1 (scale ×1) → generation 1, GPU-verified ×1; a re-cook of the SAME graph is
a no-op (`changed=false`, generation stays 1); then the shader is EDITED to scale ×2 → generation 2, and the SAME pipeline slot
now runs the edited kernel (GPU-verified ×2) — all in the same context, no restart.

## Verification

- `[d5]` 14/1; smoke ([d5]/[d4]/[cook]/[variant]) 76/4 — green.
- clang-tidy (LLVM-20.1.8, warnings-as-errors) clean on `reload.{hpp,cpp}`, `select.hpp`, both tests.

## D1–D5 COMPLETE — CKIR is a full shipping pipeline

`IR-as-crdr (D1) → cook (D2: SPIR-V/DXIL/PTX real bytecode + MSL/WGSL source, content-hash cache) → variants (D3: matrix + dedup +
on-demand) → zero-compile runtime load + persistent pipeline cache (D4: VkPipelineCache / ID3D12PipelineLibrary, both backends
warm-restart) → hot-reload (D5)`. The detour's remaining work is the OFF-* offline path tracer.

## Proposed commit (user commits — no AI co-author trailer)

```
feat(d5): shader hot-reload + native crd::math::remainder

Add ReloadableCompute (crd-shader-cook): reload() recooks + content-hashes the
IR; an unchanged graph is a no-op, a changed one builds the new pipeline from the
cooked bytecode and atomically swaps it in (previous retired one generation),
backend-agnostic via a create-callback. Add native remainder to crd::math
(fdlibm reduction over exact fmod; sign is signbit(x)?-r:r; tiny |y| via r+r) —
bit-exact vs std.

[d5]: an IR edit (kernel x1 -> x2) hot-swaps the live pipeline in the same
context; a same-graph re-cook is a no-op. [select]: remainder bit-exact, 3380
assertions. Closes the D1-D5 deploy chain.
```
