# 2026-07-24 — REN-1 frame graph: one-submission batching vs the synchronous per-draw substrate

The REN-1 frame graph's core performance claim: recording a frame's N draws into ONE command buffer + ONE
`vkQueueSubmit`/fence-wait replaces the synchronous substrate's N submit+wait+readback stalls. The win is the
collapse of N CPU↔GPU fence round-trips to one, so it GROWS with the number of draws per frame.

## Machine / config

- **CPU**: Intel Core i9-14900K (host build cap; see CLAUDE.md Raptor-Lake note).
- **GPU**: NVIDIA RTX 4070 Ti SUPER (Vulkan 1.4, `VK_EXT_shader_object`).
- **Build**: `win-debug` (MSVC, `/Od`+RTC1). These are DEBUG numbers — the measured quantity is the CPU-side
  submit/fence-stall cost, not GPU compute (the draws are three-vertex triangles into a 256×256 target). The
  batching win is a CPU-scheduling property and is if anything *understated* in a release build where per-draw
  CPU overhead shrinks but the fence round-trips do not.
- **Metric**: CPU wall-clock ms/frame, `std::chrono::steady_clock`, mean of 20 frames per N.

## Harness

`tests/gpu-context-vulkan/test_vulkan_frame_graph.cpp`, hidden case tagged `[.][ren1-bench]`:

```
build/win-debug/tests/gpu-context-vulkan/crd-gpu-context-vulkan-tests.exe "[ren1-bench]" --success
```

A frame = N draws to one color+depth target: the first `draw_storage_depth` (clear), the rest
`draw_storage_depth_load`. **(a) sync** issues them directly (N `vkQueueSubmit` + N fence waits per frame);
**(b) frame graph** records all N into one pass and `execute()`s (ONE submit + ONE wait). Both paths run the
identical CKIR triangle program; the graph asserts `last_submit_count()==1` every frame regardless of N.

## Board — ms/frame, mean of 20 (lower is better)

**Vulkan** (`[ren1-bench]`, `test_vulkan_frame_graph.cpp`):

| N draws/frame | sync (N submits) | frame graph (1 submit) | speedup |
|--------------:|-----------------:|-----------------------:|--------:|
|             1 |          0.348 ms |               0.137 ms |  2.54×  |
|             4 |          0.603 ms |               0.195 ms |  3.10×  |
|            16 |          2.255 ms |               0.462 ms |  4.88×  |
|            64 |          9.578 ms |               1.561 ms |  6.14×  |

**DX12** (`[ren1-bench]`, `test_dx12_frame_graph.cpp`) — same host, same metric:

| N draws/frame | sync (N submits) | frame graph (1 submit) | speedup |
|--------------:|-----------------:|-----------------------:|---------:|
|             1 |          0.146 ms |               0.119 ms |   1.22×  |
|             4 |          0.464 ms |               0.119 ms |   3.90×  |
|            16 |          1.811 ms |               0.128 ms |  14.16×  |
|            64 |          7.293 ms |               0.188 ms |  38.76×  |

The speedup scales monotonically with draw count on BOTH backends — the signature of collapsing N fence stalls
to one. By N=64 (a realistic multi-group scene) the batching is **6.14× on Vulkan** and **38.76× on DX12**
(the DX12 win is larger because D3D12's per-submit cost — command-list Close + ExecuteCommandLists + fence
signal — is heavier than Vulkan's, so removing N-1 of them saves more). DX12's frame-graph frame time is nearly
FLAT in N (0.119 → 0.188 ms) — the recorded draws cost almost nothing next to the eliminated submits.

No peer: this is Cerid's own synchronous substrate (the pre-REN-1 `draw_*` path) vs its frame-graph recording
mode — an internal before/after, not a vendor comparison. No loss: the graph is faster at every N on both
backends; each gate's one hard assertion is a batching NON-regression at N=64 (`graph_ms ≤ sync_ms`).

## End-to-end corroboration (the sandbox)

The live sandbox (foxes + monuments + a 10k-instance field, composed through the SceneRenderer's frame-graph
migration) runs the 4-second smoke at **65.2 fps**, up from ~58 fps on the synchronous substrate — a real
frame-time drop that scales with the number of mesh groups, consistent with the microbenchmark's N-scaling.

## Verdict

> REN-1's one-submission batching beats the synchronous per-draw substrate on the CPU submit path on BOTH
> backends, the win growing with draw count as N fence stalls collapse to one: **Vulkan 2.5× (1 draw) → 6.1×
> (64 draws)**, **DX12 1.2× → 38.8×** (DX12's heavier per-submit cost makes batching pay more; its frame time
> is near-flat in N). The live sandbox rises ~58 → 65.2 fps. Both backends are correctness-gated
> (`test_vulkan_frame_graph.cpp` 33 asserts, `test_dx12_frame_graph.cpp` 35 asserts incl. the per-draw
> descriptor-ring proof).
