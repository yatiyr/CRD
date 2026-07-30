# REN-40 — the MILLION-INSTANCE baseline (before any REN-40 work)

**This is the "where we start" board.** It is a Cerid-internal scaling curve, not a peer crush: nothing here is
compared against another engine yet. It exists so the REN-40 slices are aimed at a MEASURED wall instead of an
argued one, and so every later claim has a floor to beat.

## Machine / config

| | |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER |
| CPU | Intel i9-14900K |
| Build | `win-release` (MSVC 19.4x, `/O2 /GL /arch:AVX2 /fp:precise`, `CRD_DETERMINISTIC_FP=1`) |
| Backend | Vulkan (`--backend vulkan`); DX12 spot-checked, same shape |
| Present | `--present immediate` (vsync off — a vsynced number cannot answer "how fast is the renderer") |
| Frame | `frame/forward_csm_agx.frame.toml` — 4-cascade CSM (2048² × 4 atlas) + AgX post |
| Scene | `--instances N` grid at FIXED 2.0-unit spacing (the world GROWS with N: 1M ⇒ 2000×2000 units) + 24 skinned foxes |
| Sample | the per-second phase board at shot t = 6.0 s (`--screenshot-at 6.0`) |

Harness: `build/win-release/sandbox/crd-sandbox.exe --backend vulkan --present immediate --instances N
--screenshot <f> --screenshot-at 6.0`, reading the `perf:` line. ⛔ Single-sample per N — the fps-median-of-5
rule applies to any CLOSE claim; this board is an order-of-magnitude map, and the gaps here are 10–30×, far
outside run-to-run noise (~10 fps at the 10k end).

## The curve

| instances | extract | inst upload | palette | **CPU render** | **GPU** | drawn | ≈ fps |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10,000 | 0.32 ms | 0.02 ms | 0.05 ms | **11.8 ms** | 5.2 ms | 4,149 | ~85 |
| 100,000 | 3.65 ms | 0.04 ms | 0.05 ms | **41.4 ms** | 20.7 ms | 29,354 | ~24 |
| 1,000,000 | **171.4 ms** | **32.0 ms** | 0.59 ms | **337.2 ms** | 90.1 ms | 256,225 | **~3** |

Scene arena required: 680 MiB at 1M (a fixed 256 MiB asserted `TlsfAllocator: out of memory` before the first
frame — the arena is now budgeted per instance).

## What the numbers say

**The frame is CPU-bound at every scale, and catastrophically so at 1M** (337 ms CPU against 90 ms GPU). The
cost is not one hot spot but three, all of them per-instance-per-frame CPU work that should not exist:

1. **`extract` 171 ms** — the scene→renderer sync walks the World every frame. It is chunk-grain and
   dirty-aware for UPLOAD, but the walk itself is O(entities).
2. **CPU culling ~130 ms** (the render residual) — camera frustum test over 1M AABBs, then FOUR more passes for
   the cascades (`aabb_in_frustum` × 5M), each building a visible list.
3. **List + instance upload 32 ms** — 256k visible slots × 5 lists, re-uploaded per frame.

**GPU 90 ms for 256k drawn instances** is the second wall, and it is a LOD problem: every instance draws its
full-detail cooked mesh into 4 cascades + forward, so the far half of a 2000-unit field is paying full vertex
cost for sub-pixel coverage.

Target for "blazingly fast" at 1M = **≤16.6 ms** total ⇒ **~20× on CPU, ~6× on GPU**.

## Verdict line (quote this)

> REN-40 baseline, 1M instances + 24 skinned, 4-cascade CSM, Vulkan, 4070 Ti SUPER: **337 ms CPU / 90 ms GPU
> (~3 fps)**, CPU-bound — extract 171 ms, CPU cull ~130 ms, uploads 32 ms; 256k instances drawn at full detail.
> 10k = 11.8 / 5.2 ms. The wall is per-instance-per-frame CPU work and the absence of LOD, not draw submission.

## Open — what this board is asking REN-40 to kill

- Per-frame CPU culling (camera + 4 cascades) → GPU-driven cull producing compacted lists + indirect args.
- The O(entities) extract walk → incremental, dirty-only.
- Per-frame visible-list/instance uploads → persistent GPU residency.
- Full-detail draws for sub-pixel instances → GPU LOD selection (the B4 mesh/amplification path already pulls
  real geometry, F16).


---

# REN-40-A — the DEVICE-SIDE CULL, measured against that start line

Same machine, same scene, same frame asset family. The only change is **who decides what is visible**: the
`forward_csm_gpu` authored graph runs the frustum cull for the camera and all four cascades on the device and the
draws take their counts from device memory, so `set_gpu_cull` skips the CPU cull and its visible-list uploads
entirely. Both arms are ONE build, one flag apart — the readback-A/B rule.

## Harness (differs from the baseline above — read this before comparing)

```
crd-sandbox.exe --backend {vulkan|dx12} --instances N --screenshot <f> --screenshot-at 8.0                 --fixed-dt 16.6667 [--gpu-cull]
```

⛔ **`--fixed-dt` is not optional for this board.** It drives the clock from the presented-frame counter, so both
arms land on the SAME camera pose and the SAME animation phase. Without it the two arms sit at different poses
(their frame rates differ), which makes a pixel A/B measure the camera and moves the phase timings too — the
`sync` column here is therefore NOT comparable with the wall-clock baseline board above, only within this board.

Each cell is the **median of 3 runs**, reading the last steady-state `perf:` line. ⚠ Three, not five: the gaps
below are 2.3× at 1M, far outside the run-to-run spread (±5 ms), and every CLOSE claim still owes five.

## The board — CPU render time, both backends, both arms

| backend | instances | arm | sync | **CPU render** | GPU | render speedup |
|---|---:|---|---:|---:|---:|---:|
| Vulkan | 100,000 | CPU cull | 3.85 ms | **46.23 ms** | 21.21 ms | — |
| Vulkan | 100,000 | **device cull** | 3.85 ms | **31.92 ms** | 21.59 ms | **1.45×** |
| Vulkan | 1,000,000 | CPU cull | 80.20 ms | **281.32 ms** | 85.86 ms | — |
| Vulkan | 1,000,000 | **device cull** | 76.74 ms | **120.82 ms** | 86.82 ms | **2.33×** |
| DX12 | 100,000 | CPU cull | 14.43 ms | **49.90 ms** | 22.44 ms | — |
| DX12 | 100,000 | **device cull** | 13.79 ms | **30.53 ms** | 22.27 ms | **1.63×** |
| DX12 | 1,000,000 | CPU cull | 96.43 ms | **278.94 ms** | 86.26 ms | — |
| DX12 | 1,000,000 | **device cull** | 98.12 ms | **117.24 ms** | 86.42 ms | **2.38×** |

## Correctness, stated before the speed

| check | Vulkan | DX12 |
|---|---|---|
| pixels vs the CPU-cull arm (2000 inst, same pose) | **0 / 921600 differ** | **0 / 921600 differ** |
| per-view survivor counts vs the CPU cull, same frame | camera 1380==1380, cascades 0 / 382 / 1814 / 1963 — all **MATCH** | camera 1377==1377, cascades 0 / 391 / 1813 / 1963 — all **MATCH** |
| device copy of the per-instance world AABBs vs the CPU's | 0 / 646 differ | 0 / 646 differ |
| count-gated indirect draw (depth-only + geometry) | gates green | gates green |

The count comparison runs under `set_gpu_cull_verify()`, which keeps the CPU cull alive purely so both verdicts
exist in one frame. It costs the whole speedup, on purpose: it is a gate mode, never a shipping one.

## What the numbers say

- **The CPU cull is gone, and it was worth 160 ms at 1M.** 281 → 121 ms of render time on Vulkan, 279 → 117 ms on
  DX12. That is the 5M `aabb_in_frustum` calls (camera + 4 cascades) plus the per-frame visible-list uploads,
  moved onto the device and off the frame's critical path.
- **Both backends land within 4 ms of each other** at 1M in the device arm (120.8 vs 117.2). The command layouts
  differ (Vulkan 20 B / args at 0; D3D12 24 B / args at 4 behind a DrawIndex root constant) and each backend uses
  its own mechanism — neither was levelled down, and the result is parity rather than a lowest common denominator.
- **GPU time did not move (~86 ms), and that is expected.** A frustum cull does not reduce what is DRAWN, only who
  decides it; the same ~256k instances still rasterise at full detail into four cascades plus the forward pass.
  **The GPU wall is a LOD problem** and it is 40-C/40-D's, not this slice's.
- **`sync` is now the largest CPU term** (77–98 ms at 1M, almost all of it the O(entities) extract walk). It is
  next: 40-B.

## Verdict line (quote this)

> REN-40-A, 1M instances + 24 skinned, 4-cascade CSM, 4070 Ti SUPER: the device-side cull cuts CPU render time
> **281 → 121 ms on Vulkan (2.33×)** and **279 → 117 ms on DX12 (2.38×)**, with the frame **BIT-IDENTICAL** to the
> CPU-cull arm on both backends (0 of 921600 pixels) and every per-view survivor count asserted EQUAL. GPU time is
> unchanged at ~86 ms — that wall is LOD, not culling. The remaining CPU term is the extract walk (77–98 ms).

## Still open after this slice

- **GPU ~86 ms at 1M** — no LOD. Every instance draws its full-detail mesh into 4 cascades + forward (40-C/40-D).
- **`sync` 77–98 ms** — the O(entities) extract walk (40-B).
- **`count_buf` is passed `nullptr`** today: with one command per group per view (`max_draws = 1`) a count word
  buys nothing. The ability is real, exercised by its own gates on both backends, and becomes load-bearing when
  all groups share one command buffer (40-B).
- **Compaction is per-survivor**, not wave-scalarized — the fast form was implemented first and the parity gate
  rejected it (`~ballot` phantom lanes). Correct first; the gate now guards the optimisation.
