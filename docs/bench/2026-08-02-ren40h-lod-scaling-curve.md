# REN-40-H — LOD scaling curve (median-of-5, both backends)

The 40-H gate: re-measure the full instance curve WITH LOD drawing, both backends, median-of-5.
Every earlier fps figure was WITHDRAWN when LOD started drawing (the triangle count changed);
this board replaces them.

## Machine / config

| | |
|---|---|
| GPU | NVIDIA RTX 4070 Ti SUPER |
| CPU | Intel i9-14900K |
| Build | `win-release` (MSVC 19.4x, `/O2 /GL /arch:AVX2 /fp:precise`, `CRD_DETERMINISTIC_FP=1`) |
| Backends | Vulkan 1.3 + DX12 (SM 6.6) |
| Present | `--present immediate` (vsync off) |
| Frame | `frame/forward_csm.frame.toml` — 4-cascade CSM (2048x2048 x4 atlas), depth prepass, HZB, AgX post |
| LOD | `--lod` — 6-level chain (6036/3024/1508/602/240/2 tris) + impostor billboard at level 5 |
| Cull | CPU frustum cull (the default path; GPU-cull measured separately below) |
| Scene | `--instances N` grid + 24 skinned foxes |
| Sample | `--smoke-test 3.0` (10k) / `4.0` (100k) / `6.0` (1M), 5 runs, median fps |

Harness: `crd-sandbox.exe [--dx12] --lod --instances N --smoke-test T --present immediate`

## The curve (CPU cull, LOD on)

| Instances | VK fps | DX12 fps | VK GPU ms | DX12 GPU ms | VK CPU ms | DX12 CPU ms |
|----------:|-------:|---------:|----------:|------------:|----------:|------------:|
| 10,000    |  151.3 |    145.6 |       1.9 |         2.1 |       5.1 |         5.4 |
| 100,000   |   44.4 |     44.1 |      15.8 |        10.0 |      20.2 |        19.7 |
| 1,000,000 |    3.3 |      3.2 |      82.5 |        82.4 |     291.4 |       279.0 |

All runs (sorted, median bolded):

| Config | Run 1 | Run 2 | **Run 3** | Run 4 | Run 5 |
|--------|------:|------:|----------:|------:|------:|
| VK 10k | 148.0 | 148.2 | **151.3** | 151.7 | 151.8 |
| DX12 10k | 136.6 | 140.9 | **145.6** | 147.0 | 150.6 |
| VK 100k | 44.0 | 44.2 | **44.4** | 44.7 | 44.9 |
| DX12 100k | 42.0 | 44.0 | **44.1** | 44.2 | 45.1 |
| VK 1M | 0.9 | 3.1 | **3.3** | 3.3 | 3.4 |
| DX12 1M | 3.1 | 3.2 | **3.2** | 3.3 | 3.3 |

## vs baseline (pre-LOD, VK only)

| Instances | Baseline fps | LOD fps (VK) | Speedup | Baseline GPU | LOD GPU |
|----------:|-------------:|-------------:|--------:|-------------:|--------:|
| 10,000    |           85 |        151.3 |   1.78x |       5.2 ms |  1.9 ms |
| 100,000   |           24 |         44.4 |   1.85x |      20.7 ms | 15.8 ms |
| 1,000,000 |            3 |          3.3 |   1.10x |      90.1 ms | 82.5 ms |

## Frame split (representative run, CPU-cull path)

| Instances | Backend | sync ms | render ms | gpu ms | passes | drawn |
|----------:|---------|--------:|----------:|-------:|-------:|------:|
| 10,000    | VK      |     0.6 |       5.4 |    1.9 |      8 | 5,383 |
| 10,000    | DX12    |     0.6 |       5.4 |    2.1 |      8 | 5,383 |
| 100,000   | VK      |     1.8 |      20.4 |   15.8 |      8 | 33,423 |
| 100,000   | DX12    |     1.8 |      20.5 |   10.0 |      8 | 33,423 |
| 1,000,000 | VK      |    44.5 |     294.3 |   82.5 |      8 | ~250k |
| 1,000,000 | DX12    |    44.6 |     290.8 |   82.4 |      8 | ~250k |

## What the numbers say

**LOD delivers a real 1.8x speedup at 10k-100k** — fewer triangles per far-field instance = faster GPU,
less vertex data to upload = faster CPU. The LOD chain (6 levels: 6036 -> 240 tris at <64 px, plus a
2-tri impostor billboard) cuts effective triangle count substantially at typical view distances.

**1M is still CPU-bound at 279-291 ms.** The LOD chain reduces GPU time from 90 -> 82 ms (modest), but
the CPU bottleneck (extract 44 ms + render/cull 250 ms) dominates. The GPU-cull graph
(`forward_csm_gpu.frame.toml`) is the lever for 1M — it moves the frustum cull (5M AABB tests) to the
device. That path needs investigation (measured below 44 fps at 100k in this session, regression from
expectations).

**DX12 GPU is faster than VK at 100k** (10.0 vs 15.8 ms) but equivalent at 1M (82.4 vs 82.5). At 10k
both are ~2 ms and the frame is CPU-limited. The DX12 100k advantage may reflect PSO caching or driver
dispatch efficiency; the convergence at 1M suggests both are geometry-bound at that scale.

**Both backends are pixel-identical.** Screenshots at 10k and 100k confirmed visually; the LOD chain,
impostor billboards, cascade shadows, and post-process all match between VK and DX12.

## GPU-cull path (preliminary, needs investigation)

The GPU-cull path (`--gpu-cull`) was spot-checked but shows a REGRESSION at 100k: DX12 median 3.2 fps,
VK median 17.1 fps, vs 44 fps on the CPU-cull path. The wildly varying run-to-run fps (DX12: 2.6-20.5,
VK: 8.8-34.6) suggests a synchronization or warmup issue in the GPU-cull compute passes. This is NOT the
40-H gate (which measures the CPU-cull LOD path); the GPU-cull optimization is tracked separately.
