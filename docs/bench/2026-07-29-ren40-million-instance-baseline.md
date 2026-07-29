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
