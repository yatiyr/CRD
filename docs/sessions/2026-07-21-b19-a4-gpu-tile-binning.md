# 2026-07-21 — B19-a4: full GPU tile binning + the real block rasteriser (the last host crutch removed)

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) · **Sub-slice:** B19-a4
**Directive:** "let's go, no defers, no debts, full correct, frontier and performant crushing architecture."

## What shipped

B19-a2's tiled render still built its per-tile buckets on the *host*. B19-a4 moves the entire tile bin on-device and
replaces the fixed-cap bucket render with the real Kerbl-2023 block rasteriser topology. Five new CKIR kernels in
`engine/kir/include/crd/kir/ckir_gsplat.hpp`, plus a reuse of `ckir_scan` and the B19-a3 KV radix sort:

1. **`build_gsplat_tilecount_kernel`** — per depth-sorted splat, the number of screen tiles its 3σ bbox covers
   (half-open clamped rect, Kerbl `getRect`; 0 if culled/off-screen).
2. **reuse `ckir_scan.build_scan`** (exclusive) — prefix-sum the tilecounts → per-splat instance base offset + total T.
3. **`build_gsplat_scatter_instances_kernel`** — GRID-DRIVEN over N·max_cover threads (splat, slot); `slot < tilecount`
   ⇒ decode the slot-th covered tile and write key=tileID, val=splat-index at off[splat]+slot. No CKIR `For` — the
   per-splat variable fan-out is expressed by the grid, so there is no non-uniform loop bound. The write is `If`-guarded.
4. **reuse the KV radix sort BY TILE** (`ckir_sort`, `carry_val`) — LSD radix is stable, and the splats are already
   globally depth-sorted (B19-a3), so a stable sort by **tileID alone** yields tile-major AND depth-order-within-tile.
   No depth bits in the key, no depth-precision loss.
5. **`build_gsplat_tile_ranges_kernel`** — boundary-detect each tile's [start,end) span in the sorted instance list
   (nested `If`; the caller pre-zeroes ranges so empty tiles read [0,0)).
6. **`build_gsplat_block_render_kernel`** — the real 3DGS render: **one workgroup per tile, one thread per pixel,
   looping the tile's [start,end)** — a *variable* `For` bound that is workgroup-uniform (every thread in the workgroup
   shares the tile), which is exactly what the CKIR `For` bound requires (the oracle evaluates it once per workgroup).
   **No fixed bucket cap** — B19-a2's `cap` is gone.

The key realisation that unlocked the variable bound: the CKIR `For` scar is "uniform *within a workgroup*", not "uniform
across the dispatch" — the GLSL emitter emits `for(uint lv=0u; lv<uint(<value>); ...)` with the bound as a value node, and
the CPU oracle evaluates it via `active[0]`. One workgroup = one tile satisfies it exactly.

## Gates

| gate | result |
|---|---|
| CPU oracle: tilecount == host bbox count | mismatches 0 |
| CPU oracle: tilecount→scan→scatter packed instances == host | mismatches 0 |
| CPU oracle: full pipeline (…→ranges→block) == brute pixelwise | **worst 0.000e+00**, ranges partition [0,T) exactly |
| `crd-kir-tests [gsplat]` (8 cases) | 241 assertions — PASS |
| **real Vulkan** `[bin]`: full GPU bin + block == GPU brute | **worst 0.000e+00**, T=290 / 16 tiles, 38 assertions |
| `crd-gpu-context-vulkan-tests [gsplat]` (3 cases) | 94 assertions — PASS |
| existing `[sort]` (a3 KV + radix) unbroken | 76 assertions — PASS |

The block render composites exactly the brute set in depth order ⇒ bit-exact — the same relationship B19-a2's tiled
render had, now with the on-device bin and no cap.

## Traps hit

- **NEVER `stmt_materialize` a Bool value.** The ranges kernel materialised its `is_start`/`is_end` guards (BitOr of
  comparisons) → the emitter declared `int/float tN = (a != b);` → `cannot convert bool to uint` + `boolean expression
  expected`. Bool comparison results must stay inline (consumed directly by the `If` condition), like the tiled render's
  keep-mask. Corollary added to the If-shared-temp memory.
- **If-shared-temp scar (the non-bool half).** `ridx = t*2` is used in *both* inner `If` bodies (the start-store and the
  end-store); the emitter declared it inside the first block, leaving it undeclared in the second (`t31 undeclared`).
  Fix: `stmt_materialize(ridx)` to hoist the decl to the enclosing scope. So both rules compose: materialize the shared
  u32 index bases, do NOT materialize the bool guards.
- **Only the real backend caught both.** The CPU oracle passed the exact same graph bit-exactly; the SPIR-V emit is what
  surfaced the scoping/type errors. Lesson reinforced: emit to a device before trusting an `If`/`For` kernel.
- **Block-render u32 span underflow guard.** Added `span = select(re > rs, re - rs, 0)` so a malformed range can never
  wrap to ~2³² and hang the loop (an earlier long oracle run looked like a hang but was just the 2048-sort + 256-splat
  brute reference being genuinely slow — shrinking the CPU gate to 32×32/64-splats/1024-sort made it seconds).

## State

B19-a4 DONE. `ckir_gsplat.hpp` + both test files tidy-clean (pinned LLVM 20.1.8). context.md + detour updated.

**Next:** B19-c (2DGS surfels + mesh extraction — the bridge into the B1 material pipeline) · or StopThePop per-pixel
resort (view consistency) · or the cross-backend pass (wire all of B19 to DX12/HLSL — the whole B19 line is Vulkan-only
so far, a deliberate build order).
