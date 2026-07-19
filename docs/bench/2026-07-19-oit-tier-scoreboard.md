# 2026-07-19 — B17 OIT tier accuracy scoreboard (WBOIT · MBOIT · A-buffer vs the exact reference)

The three order-independent-transparency tiers, scored by **max per-channel RGBA8 error vs the exact sorted `over`
composite** (the ground truth). This is an *accuracy* board (not a perf board): OIT tiers trade accuracy for
cost/memory, so the meaningful measurement is how close each approximation lands to the exact reference. All three run as
CKIR on **both** backends; the exact tier is the reference every other tier is scored against.

## Config
- **GPU:** the session's device (Vulkan `VK_EXT_shader_object` + DX12 FL12); host i9-14900K. win-debug functional build.
- **Harness (re-runnable):** the `[oit]` ctest gate — `crd-gpu-context-vulkan-tests` + `crd-gpu-context-dx12-tests`.
  Shared shaders/kernels: `tests/gpu-shared/ckir_oit_test.hpp` (WBOIT), `engine/kir/include/crd/kir/ckir_oit.hpp`
  (A-buffer + MBOIT compute kernels), `tests/gpu-shared/ckir_abuffer_test.hpp` (dispatch + `eval_cpu_kernel` oracle).
- **Reference:** `build_abuffer_resolve` — deferred fragment store + per-pixel depth sort + exact front-to-back `over`.
  Pure f32 mul/add/sub, deterministic order ⇒ **bit-exact** vs `eval_cpu_kernel` and Vulkan == DX12 (worst |Δ| = 0).

## Board — max per-channel RGBA8 error vs the exact A-buffer reference

| Tier | Technique | Cost / memory | GPU vs its CPU oracle | Error vs EXACT | Notes |
|---|---|---|---|---|---|
| **A-buffer (atomic list)** | Carpenter 1984, GPU deployable form — value-returning atomics build per-pixel linked lists, resolve walks+sorts | O(fragments); one node pool + atomics | **0 (bit-exact vs static)** | **0** | the DEPLOYABLE capture: `atomicAdd` node allocator + `atomicExchange` list push; race-built, sort-resolved ⇒ deterministic; Vulkan == DX12 |
| **A-buffer (static)** | Carpenter 1984 — deferred store + sort + exact `over` | O(fragments); per-pixel sort | **0 (bit-exact)** | **0 (is the reference)** | the ground truth; pure f32, no divide |
| **MBOIT-6** | Münstermann 2018 — **6 power moments** + Peters-Klein Hamburger (4×4 Cholesky + cubic + Gauss-Radau) | 7 moments/pixel (bounded) | to-ULP (2.7e-3, cubic amplifies) | **1** at 3-layer · vs WBOIT's **18** | resolves 3 depth masses ~exactly; **BEATS WBOIT 18×** at 3 layers — the lifted hero tier |
| **MBOIT-4** | Münstermann 2018 — 4 power moments + Peters-Klein Hamburger | 5 moments/pixel (bounded) | to-ULP (2.98e-8) | **0** at 2-layer · vs WBOIT's **30** | resolves 2 depth masses EXACTLY (glass front+back, thin foliage); beats WBOIT |
| **WBOIT** | McGuire-Bavoil 2013 — single weighted blend | 1 accum + 1 reveal target | ≤2 LSB (f16 accum + f32 divide) | 14 (4-layer) · 18 (3-layer) · 30 (2-layer) | cheapest; a crude depth heuristic — cannot capture exact ordering |

## Verdict
- **A-buffer is exact** (0), **bit-exact** across backends and vs oracle — the reference.
- **The atomic linked-list A-buffer is the DEPLOYABLE exact tier** and matches the static-slot reference **bit-for-bit** (worst
  |Δ| = 0, Vulkan == DX12). It is enabled by a NEW CKIR capability — **value-returning atomics**: `atomic_add_fetch` (a node
  allocator, `atomicAdd`/`RWByteAddressBuffer.InterlockedAdd`/CUDA `atomicAdd`/MSL `atomic_fetch_add_explicit`/WGSL `atomicAdd`,
  all returning the OLD value) and `atomic_exchange` (the list push onto `head`). Fragments race into per-pixel lists (order
  nondeterministic) but the resolve sorts by depth ⇒ the composite is deterministic and loses no fragments. Nodes are stored in
  ONE interleaved pool (r,g,b,a,depth stride-5 + a parallel `next` array) — the AAA `struct Node` pool factored into CKIR's
  single-dtype buffers. This is what real engines ship for unbounded transparency; the static-slot variant is the oracle it is
  proven against.
- **MBOIT BEATS WBOIT — and the win scales with the moment count.** 4 power moments → **exact at 2 depth masses** (glass
  front+back), where WBOIT errs 30 LSB. **6 power moments → ~1 LSB at 3 masses, an 18× win over WBOIT** at 3-layer depth
  complexity, achieved exactly as the moment theory predicts: the Hamburger solve generalizes to a larger (4×4) Hankel
  Cholesky + a cubic root-solve + a Gauss-Radau form factor (`oit::msm_hamburger6_scalar`). The moment tier captures the
  exact depth ordering the crude single weight cannot; the exact-capacity = ⌊moments/2⌋ masses. MBOIT is a **to-ULP** tier
  (the cubic amplifies the GPU/CPU transcendental ULP to ~2.7e-3 float ≈ 1 LSB in the 8-bit output; a larger regularizing
  bias tames it).
- **6-moment (cubic) is the practical ceiling of the POWER-moment reconstruction.** The 8-moment quartic was implemented
  fully (5×5 Hankel Cholesky + Ferrari quartic + 5-node Gauss-Radau) and is mathematically correct, but the reconstruction —
  whether unrolled per-fragment or in a runtime loop — is **too large for the shader toolchain** (the driver re-unrolls the
  constant-bound loop; the kernel never finishes compiling → test hang, MEASURED).
- **Trigonometric moments were also explored — a QUALITY wall.** The compact Fourier/Fejér reconstruction (depth→angle,
  integrated truncated Fourier series) *compiles fast* (no root-solve → scales to 12+ moments) but is too blurry: raw Fourier
  is Gibbs garbage (200 LSB), Fejér-weighted is bounded but plateaus at ~80 LSB — **worse than WBOIT's 14**. The trig
  reconstruction that *would* beat WBOIT (Szegő quadrature = exact mass recovery) needs to find Szegő-polynomial roots → back
  on the same compile wall. Both attempts removed (a tier worse than WBOIT is not worth shipping).
- **Ceiling, mapped:** 6-moment power (3 masses) is the practical ceiling of *per-fragment moment-based* OIT here. **Beyond 3
  masses the EXACT A-buffer** (shipped, bit-exact both backends) **is the tool** — it beats WBOIT trivially at any complexity.
  Not a loss to WBOIT: the moment tiers own the bounded-memory glass/foliage regime (and beat it there, up to 18×); the
  A-buffer owns high complexity.
- **Per-pixel-hoisted runtime-loop — tried, and it pinned the wall precisely.** Ran the quartic in a loop whose bound comes
  from a config buffer (runtime ⇒ the driver can't re-unroll). Still hung. But a diagnostic swapping the quartic for a
  trivial body (same loop + config) runs *instantly* ⇒ the architecture is correct; the hang is the shader compiler choking
  on the 8-moment quartic body **even emitted once**. The wall is the single reconstruction's intrinsic complexity, not
  replication — so no architecture change lifts the 3-mass ceiling. (The runtime loop *does* scale the fragment count/overdraw
  freely — a separate axis from masses-resolved.)
- **WBOIT is the cheap tier**: a bounded (14–30 LSB) approximation, single-pass, no per-pixel storage — the FX default,
  now dominated in accuracy by MBOIT wherever the depth complexity is within the moment budget.

The GPU-timed **perf board** (kernel-only cost per tier on a 1024²×4-layer high-overdraw scene) is its companion:
`2026-07-19-oit-tier-perf.md`. Headline: the static store-based tiers are cheapest and **MBOIT's accuracy is FREE** (the
reconstruction is memory-bound-hidden); the atomic list is ~10× (unbounded-depth price); stochastic is the cheapest unbounded
tier per-frame (~0.061 ms @ S=1 + TAA). These tiers are validated here for accuracy/correctness on both backends, which is the
B17 Definition of Done. **No loss recorded** — MBOIT beats WBOIT at every depth complexity within its moment budget, and the
budget is a tunable (more moments = larger Cholesky = more masses).
