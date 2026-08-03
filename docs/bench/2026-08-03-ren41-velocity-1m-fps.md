# REN-41 per-object velocity — 1M fps board + the DX12 upload-batching fix (median-of-5, both backends)

The REN-41 velocity close measurement: the frontier scene at scale with **per-object motion vectors ON** (the
default in the device-cull frames — a MRT `depth_prepass` writes `velocity`, `taa_resolve` consumes it). Companion
to the velocity CORRECTNESS gate (which proves the buffer's VALUES: static ≈ 0, mover ≈ the expected screen delta).
This board proves the COST — and this session **root-caused and fixed** the DX12 upload gap it first exposed.

## Machine / config

RTX 4070 Ti SUPER / i9-14900K / **win-release**, `--present immediate`. Harness:
`crd-sandbox.exe [--backend dx12] --lod --gpu-cull --instances N --smoke-test 4.0 --present immediate`
— the shipping look (LOD chain + impostors, TAA, PCSS, per-object velocity). Median of 5 soaks per cell.

## The board (median of 5)

| Instances | VK fps | DX12 fps (before fix) | **DX12 fps (after fix)** | 40-J baseline (both) |
|----------:|-------:|----------------------:|-------------------------:|---------------------:|
| 100,000   |  65.6  |                  19.8 |                 **44.5** |                 90.7 |
| 1,000,000 |  20.8  |                   7.5 |                 **13.1** |                 12.4 |

DX12 gained **2.25× at 100k and 1.75× at 1M** from the upload-batching fix below. **0 validation errors** both
backends (GPU-assisted-validation soak, exit 0); no device loss. All 22-pass frames build and render on both.

## ⛔→✅ The DX12 gap was `upload_storage`, and it is now FIXED (not filed for later)

The first measurement showed DX12 ~3× behind Vulkan. Decomposed: the GPU work was comparable (32.9 vs 26.0 ms) —
the gap was **CPU-side `upload_storage`**. DX12's synchronous upload path did, PER CALL, a fresh
`CreateCommittedResource` + `m_cmd_alloc->Reset()` + **`submit_and_wait()`** (a full CPU↔GPU flush). At 1M the
renderer issues dozens of uploads per frame (per dirty run + per-group header/visible/tables), so the frame paid
dozens of serialized GPU round-trips: **~36 ms/frame** of pure upload waits.

Vulkan was ~0.1 ms for the same 1,203 dirty instances because it has an upload BATCH (38-G1): between
`begin_upload_batch()`/`end_upload_batch()` (which the renderer already brackets sync with), uploads become ring
memcpys + recorded copies flushed in ONE submit with no wait. **DX12 simply never implemented the batch** — the
interface methods were no-ops. This session implemented them on DX12 (own double-buffered allocator+list + a
persistent mapped ring; a single upload larger than the ring bypasses to the synchronous path so the ring stays
8 MB and the one-time first-frame bulk upload doesn't balloon it). Result:

**DX12 steady-state `upload`: ~36 ms → 0.11 ms (~300×).** The 1M frame is now GPU-bound (~26 ms) like Vulkan, and
DX12's per-frame CPU (6.4 ms at 100k) dropped below Vulkan's. The residual 1M smoke-avg gap (13.1 vs 20.8) is now
the one-time ~0.7 s first-frame 1M build (extract + the bulk upload) inside the 4 s window, not per-frame cost.

## Per-pass GPU breakdown at 1M (steady-state, velocity passes in **bold**)

| pass | VK ms | DX12 ms | | pass | VK ms | DX12 ms |
|---|---:|---:|---|---|---:|---:|
| **palette_snapshot** | **0.03** | **0.02** | | depth→cull (reset+5 views) | ~4.7 | ~5.9 |
| gpu_skin | 0.09 | 0.12 | | csm_cascade ×4 | ~2.4 | ~2.9 |
| **depth_prepass (MRT vel+depth)** | **8.4** | **10.1** | | forward | 8.7 | 9.9 |
| hzb_build + occlusion | 1.1 | 1.4 | | **taa_resolve (reads velocity)** | **0.22** | **0.03** |
| taa_store / overlay / post | 0.03 | 0.03 | | **GPU total (22 passes)** | **26.0** | **32.9** |

## What the numbers say (honestly)

- **Velocity is cheap.** Its GPU footprint is `palette_snapshot` (0.03 ms), the RG16F write folded into the
  `depth_prepass` (which REPLACED the depth-only prepass TAA already required), and one `taa_resolve` tap (~free).
  The frame is geometry-dominated (`depth_prepass` + `forward` + the 5 cull views), as before.
- **VK 1M improved vs 40-J (12.4 → 20.8)** — Stage 1's LOD/impostor retune (after 40-J) cuts far-field triangles
  enough to outweigh the TAA + velocity passes. **100k slower (90.7 → 65.6)** — TAA's fixed per-frame cost
  dominates when geometry is light; the price of "no aliased pixels" by default. A recorded trade, not a regression.
- **The 1M ≤16.6 ms target is still not met** (VK ~26 ms GPU). Unchanged levers: wave-scalarized compaction (in the
  cull asset) and shadow-map churn (VSM / REN-5).
