# REN-39 — indexed pull vs classic pull (post-transform vertex reuse)

**Date** 2026-07-29 · **Host** i9-14900K + RTX 4070 Ti SUPER, Windows 11 · **Backend** Vulkan · **Build** `win-release`
**Scene** sandbox: 10 000 static + 24 animated + 3 monuments; ~4 850 instances visible; 1280x720, shadows ON (4 cascades @ 2048)
**Method** `build\win-release\sandbox\crd-sandbox.exe --no-validation --present immediate --smoke-test 6 [--pull-draws]`,
5 runs per arm, MEDIAN reported (the single-run-is-noise rule). Per-pass numbers are each run's LAST GPU board
(t ≈ 6 s of the camera timeline, so the two arms sample the SAME camera phase and are directly comparable;
they are NOT phase-matched against the 38-G1 board, whose run length differed).
**Harness** `sandbox/src/main.cpp` (`--pull-draws` = the A/B baseline arm; both arms are one binary).

## The claim under test

38-G1 measured the frame VERTEX-bound: the pull idiom draws non-indexed (`vertex_count = visible × index_count`),
re-pulling and re-transforming every triangle corner with zero post-transform reuse. REN-39 draws INDEXED against
the same storage buffer (its own u32 index section; `vkCmdDrawIndexedIndirect`), so the hardware's post-transform
cache reuses shared vertices and the per-vertex `indices[]` load + the `vid / index_count` division disappear from
the VS entirely (three storage loads fewer per vertex, gated at cook level).

## fps (median of 5, whole 6 s run incl. warmup)

| arm | runs (fps) | median |
|---|---|---:|
| pull (`--pull-draws`) | 54.4 · 62.5 · 101.0 · 115.8 · 116.0 | **101.0** |
| indexed (default) | 142.0 · 142.3 · 149.6 · 155.5 · 162.3 | **149.6** |

**1.48× whole-frame**, and the indexed arm is far less noisy (spread 20 fps vs 62 — the pull arm's long
GPU frames amplify scheduling jitter). Loop mean 9.66 → 6.19 ms; render phase 8.99 → 5.55 ms.

## Per-pass GPU attribution (median of each run's last board, same camera phase)

| pass | pull ms | indexed ms | cut |
|---|---:|---:|---:|
| csm_cascade 0 | 0.010 | 0.013 | — |
| csm_cascade 1 | 0.26 | 0.17 | 1.5× |
| csm_cascade 2 | 1.38 | 1.61 | ~1× |
| csm_cascade 3 | **3.38** | **3.26** | **1.04×** |
| forward | **14.29** | **1.56** | **9.2×** |
| overlay | 0.036 | 0.037 | — |
| **GPU total** | **~19.4** | **~6.6** | **~2.9×** |

## Verdict

- **The forward pass cut 9.2×** — beyond the 3–6× expectation. The forward VS is the heavy one (world transform,
  four varyings, lighting reads), so post-transform reuse compounds with the three removed per-vertex loads.
- **Cascade 3 did not move (3.38 → 3.26), and that is the finding, not a failure**: its cost is NOT vertex
  shading. With the VS work cut ~9× elsewhere, cascade 3's residual is the depth-only rasterization of the
  coarse cascade itself — ~4.8k instances' triangles swept into one 2048² slice (the earlier map-size probe
  already ruled out fill). **The named next lever: cut what cascade 3 rasterizes, not how — shadow-caster LOD
  and/or per-cascade GPU cull tightening (the authored `scene_cull.crdv` seam exists).** Cascade 3 is now the
  single largest GPU cost in the frame.
- Validation arm (validation ON, indexed): **0 errors**, 141.3 fps.
- Parity: pull and indexed frames are BIT-IDENTICAL on VK + DX12 + llvmpipe with shadows on
  (`REN-39-C1 GATE`s), with the batch-count discriminator proving the switch is real (0 vs ≥5 multi batches).
- Portability: same assets, same verbs on DX12 (`ExecuteIndirect` + `D3D12_DRAW_INDEXED_ARGUMENTS`,
  read-only t0 storage seam) and llvmpipe.

## Repro

```
build\win-release\sandbox\crd-sandbox.exe --no-validation --present immediate --smoke-test 6 --pull-draws  # baseline
build\win-release\sandbox\crd-sandbox.exe --no-validation --present immediate --smoke-test 6               # indexed
build\win-release\sandbox\crd-sandbox.exe --present immediate --smoke-test 6                               # validation arm
```
