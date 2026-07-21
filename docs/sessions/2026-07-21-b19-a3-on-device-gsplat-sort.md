# 2026-07-21 — B19-a3: the sort half of 3D Gaussian Splatting runs on-device (CKIR payload radix sort)

**Detour:** D-007 GPU-program-system · **Slice:** B19 (3D Gaussian Splatting) · **Sub-slice:** B19-a3
**Directive:** "let's go, no defers, no debts, full correct, frontier and performant crushing architecture."

## What shipped

Closed **the GPU sort gap** that B19-a2 left open: the tiled 3DGS render sorted splats by depth on the
*host*. 3DGS forward rendering is fundamentally SORT + OIT-composite, so the depth sort must live on the
GPU. It now does, bit-exactly.

### 1. CKIR radix sort → key-VALUE (payload) sort (`engine/kir/include/crd/kir/ckir_sort.hpp`)

`build_sort_scatter(g, epb, threads, radix_bits, shift, nblocks, bool carry_val=false)`. When
`carry_val=true` the scatter also carries a per-key **value** payload: `val_in`=binding 4, `val_out`=
binding 5, an `s_vals` shared array staged in the local reorder and written in the coalesced pass. The
payload rides the *exact* permutation the keys are scattered by — ballot/rank/offset are computed from
the KEYS only — so the sort stays **bit-exact**. `carry_val=false` emits the identical graph, so every
existing caller is untouched (the Vulkan key-only radix gate still passes, 33 assertions).

### 2. Two gsplat pipeline kernels (`engine/kir/include/crd/kir/ckir_gsplat.hpp`)

- `build_gsplat_depthkey_kernel` — per splat → a 24-bit quantised depth key over `[dmin,dmax]` (ascending
  ⇒ nearest-first) + its index as the payload; a culled/invalid splat gets key `0xFFFFFFFF` (sorts last).
- `build_gsplat_gather_kernel` — `sorted[i] = proj[order[i]]`, reordering the projected splats into the
  sorted order using the KV-sort's index payload.

### 3. The full on-device pipeline, gated on real hardware

**project → depthkey → 4-pass KV radix sort → gather**, with NO host depth-sort crutch.

- **CPU oracle** (`tests/kir/test_ckir_gsplat.cpp`): 2048 distinct-depth splats → the pipeline → the
  gathered projected buffer equals a host stable depth-sort splat-for-splat (bit-exact); depths ascending.
- **Real Vulkan** (`tests/gpu-context-vulkan/test_vulkan_gsplat.cpp`, new gate, 43 assertions): the same
  pipeline dispatched end-to-end on the GPU (5 compiled stages + 4×{hist,offset,gbase,scatter}), ping-
  ponging keys AND values across 4 LSD passes; the gathered buffer == the host sort splat-for-splat, the
  index payload is a valid permutation (XOR check), depths ascending.
- **Standalone KV gate** (`tests/kir/test_ckir_sort.cpp`): n=16384, 4-pass ping-pong of keys and values,
  keys ascending + payload points back at its original key + values are a permutation of 0..n-1.

## Numbers

| gate | result |
|---|---|
| `crd-kir-tests [gsplat],[sort]` | 41 assertions / 8 cases — PASS |
| `crd-gpu-context-vulkan-tests [gsplat]` (incl. on-device sort) | 56 assertions / 2 cases — PASS |
| on-device Vulkan sort gate alone | 43 assertions — PASS |
| existing Vulkan key-only radix gate `[kernel][sort]` | 33 assertions — PASS (unbroken by `carry_val`) |
| B19-b Mip energy invariant | ratio 400.0 exact (still holds) |

## Traps hit

- **clang-tidy: function-local `constexpr` is `LocalConstant` → `lower_case` under the pinned 20.1.8 gate.**
  The B19-a/a2/b tests used `constexpr int kW`-style names; the gate FAILS them ("invalid case style for
  local constant"). Making a local `constexpr` does not satisfy `LocalConstexprVariableCase` — the gate
  applies the general `LocalConstant` (lower_case) category. Renamed all local `kX` constants to
  lower_case (`kShC0` at namespace scope stays). Also split every `const int a = x, b = y;`
  (`readability-isolate-declaration`) to match sibling CKIR headers. Recorded in memory.
- **Word-boundary rename collision.** Renaming `kSeg`→`seg` where `seg` was already a loop variable made
  `for (int seg = 0; seg < seg; ++seg)` (self-comparison). Renamed the constant `n_seg`. Likewise
  `kR`→`rad` collided with a later splat-radius local → `srad`. Rebuild caught both (C4456 shadow,
  tautological-compare).
- Heredoc into a `.cpp` via bash choked on an apostrophe in a comment — wrote the block via a scratch
  file + `cat` instead.

## State

B19-a3 DONE. `ckir_gsplat.hpp`, `ckir_sort.hpp`, and all three touched test files are tidy-clean (pinned
LLVM 20.1.8 gate). context.md + `docs/detours/D-007-gpu-program-system.md` updated; the B19 row stays `◧`.

**Next:** B19-a4 (full GPU tile count+scan+scatter+ranges binning — the last host crutch in the tiled
render) · or B19-c (2DGS surfels + mesh, the bridge to B1 materials) · or StopThePop per-pixel resort.
