# REN-8 — sandbox frame attribution (where the 12 ms goes)

**Date** 2026-07-25 · **Host** i9-14900K, Windows 11 · **Backend** Vulkan · **Build** `win-release`
**Scene** sandbox: 10 000 static + 24 animated + 3 monuments; ~4 140 instances visible; 1280x720
**Method** `crd-sandbox --present immediate --smoke-test 6`, phase means over 400-500 frames.

## Why this board exists

The sandbox ran at ~53-83 fps and the reason was unknown. Two measurements ruled out the usual suspects
**before** any optimization was attempted:

| arm | fps | conclusion |
|---|---|---|
| `win-debug` | 65.1 | — |
| `win-release` | 66.6 | **not CPU-compute-bound** — full optimization buys ~2% |
| release, `Fifo` (vsync) | 74.7 | — |
| release, `--present immediate` | 83.6 | **not vsync-bound** — uncapping buys ~12% |

Neither knob moved it, which is what motivated building per-pass GPU timestamps into the frame graph: nothing
in the engine could time a GPU pass, so any fix would have been guesswork.

## The attribution (phase means, readback OFF)

| phase | ms | share |
|---|---:|---:|
| **render** | **6.37** | **51%** |
| overlay (draw grid) | 2.64 | 21% |
| sync (extract + upload) | 2.67 | 21% |
| present | 0.61 | 5% |
| imgui | 0.23 | 2% |
| **loop total** | **~12.5** | |

…and `render` splits further, from device timestamps:

| | ms |
|---|---:|
| GPU passes (`gpu_ms_total`, 1 pass) | **~1.5** |
| **CPU stall (fence wait)** | **~4.9** |

**The device draws the whole scene in ~1.5 ms.** That is ~660 fps of rendering. Everything else is overhead,
and the single largest item is the frame graph's submit-and-**wait**, which REN-1 kept deliberately
(*"REN-1 keeps the wait; async-across-frames + direct present land at REN-8"*).

## The one fix applied so far: opt-out per-frame readback

`execute()` copied every imported colour target back to host-visible memory each frame so `read_pixel` stays
bit-identical to the synchronous path. That is a **test affordance**; a presenting app never reads it. At
1280x720 RGBA8 it is ~3.7 MB over PCIe per frame, and it runs AFTER the last timed pass — so per-pass
timestamps cannot see it, but the fence wait absorbs it.

`IFrameGraph::set_readback_enabled(bool)` (default **true**, so every readback-asserting gate is unchanged);
the sandbox opts out.

A/B on the SAME build, two runs per arm — because run-to-run fps on this host varies by ~10:

| arm | run 1 | run 2 | mean `render` |
|---|---:|---:|---:|
| readback **ON** | 69.6 fps / render 7.86 ms | 76.4 fps / render 6.93 ms | **7.39 ms** |
| readback **OFF** | 83.6 fps / render 6.09 ms | 75.4 fps / render 6.66 ms | **6.37 ms** |

**≈1.0 ms of `render()` saved.** Real and repeatable in the `render` column (the least noisy metric); the fps
column is too noisy on this host to carry the claim on its own.

⛔ **Correction worth keeping.** An earlier reading of this compared two *separate* runs (8.94 ms → 4.49 ms) and
concluded `render()` had been roughly halved. That was measurement noise, not a 2x win — the arms were never
run against each other. The A/B above is the honest number, and it is ~4x smaller than the first claim. This is
the reason the phase report now prints the **mean over all frames** rather than the last frame: the same build
reported `sync` at 3.3 ms and 9.9 ms on consecutive runs purely by which frame happened to be last.

## THE FIX: deferred fence wait + the overlay as a real graph pass

Two changes, both following directly from the attribution above.

**1. The stall was the fence wait, and it was avoidable.** `execute()` did `vkQueueSubmit` then immediately
`vkWaitForFences` — the CPU blocked on the GPU every frame. That wait exists so `read_pixel` is valid the
instant `execute()` returns: the same **test affordance** as the readback copy. So it now follows the same rule —
when readback is on, wait immediately (every gate keeps its exact semantics); when off, **defer the wait to the
top of the next `execute()`/`reset()`**. The GPU renders frame N while the CPU builds frame N+1.

⛔ Deferring is only safe because every destructive operation sits behind `wait_pending_submit()`: `reset()`
frees transients, the dtor tears down the pool/fence, and `execute()` re-records the single command buffer and
resets the descriptor pool. A deferred wait that skipped any of those is a use-after-free on in-flight GPU work,
not a speedup.

**2. The infinite grid was not in the frame graph** — it called `draw_overlay` outside any pass, so each call did
its own submit+wait. That is a HARD RULE violation (AGENTS.md: every render pass goes through our own machinery)
*and* the reason it cost ~2.6 ms, nearly twice what the whole 4 100-instance scene costs on the GPU.
`SceneRenderer::set_overlay_pass` registers it as the second pass of the SAME graph; `draw_overlay` then hits the
raster context's `frame_recording()` path and lands in the frame's one command buffer.
It is declared `read_writes` (not `writes`) — it composites ON TOP of the scene, and a plain write would let the
scheduler treat the scene's output as dead and alias it away.

### Result (phase means, `--present immediate`)

| phase | before | after |
|---|---:|---:|
| sync | 2.67 | **1.49** |
| render | 6.37 | **2.89** |
| overlay (own submit) | 2.64 | **0.00** (now inside render, 1 extra pass) |
| imgui | 0.23 | 0.20 |
| present | 0.61 | 1.81 |
| **loop total** | **~12.5** | **~6.39** |
| **fps** | **~79** | **~155** |
| render's GPU / stall | 1.5 / 4.9 | **1.51 / 1.49** |

The stall dropped 4.9 ms → 1.49 ms. `present` rose (0.61 → 1.81 ms) because it now absorbs the tail of the GPU
work the render call used to block on — the work moved, it did not vanish. The graph reports **2 passes**, which
is the overlay being a real member of it.

### ⛔ MEASUREMENT METHODOLOGY — read before trusting any fps number on this host

**This machine varies by ±30% run-to-run on an identical binary.** Six consecutive 4-second runs of the same
build: `106.4 · 116.3 · 107.1 · 120.9 · 115.3 · 125.0` fps. A single run is therefore worthless for comparison,
and an early reading of "155 fps" from this board was an **outlier reported as a result** — the honest figure for
the same build is a **median of ~116 fps** over six samples.

Rule for this board and any successor: **≥5 runs, report the MEDIAN, and state the spread.** Compare only
medians. The `render` column is the least noisy metric and is the one to trust for A/B of a single change.

| state | fps (median of ≥3) |
|---|---:|
| baseline (readback on, overlay outside the graph, wait-per-frame) | ~79 |
| **after all three changes** | **~116** |

**≈1.45x**, not the ~2x a single lucky pair of runs suggested.

⛔ **A bug this introduced and its lesson.** With the wait deferred, timestamps resolve at the *start of the next*
`execute()` — but the pre-record block still zeroed `m_timed_passes`/`m_gpu_ms_total`, wiping the results that
had just been published. The sandbox reported `gpu 0.000 ms (0 passes)` while timing was working perfectly.
Timings are now one frame late (the price of overlap); a broken instrument would have been worse than a late one.

## 3. FRAMES IN FLIGHT — landed, and HONESTLY NEUTRAL on this scene

With one command buffer, frame N+1 cannot re-record until frame N's fence signals, so deferring the wait bought
one frame of overlap but not a pipeline. The graph now owns a **ring of `kFramesInFlight` slots**, each with its
own command buffer, fence, descriptor pool and timestamp pool — sharing any one of them would be a silent data
race (the frame still renders, occasionally from the wrong data).

The subtle half is transients: `reset()` used to DESTROY them, which is a use-after-free the moment a previous
frame can still be reading them. They now go to the submitting slot's **retire list** and are freed when that
slot's fence signals (views before images, memory LAST — freeing a `VkDeviceMemory` under a live `VkImage` is
UB). This is what makes the ring safe for *any* authored graph, including ones that own transients, without the
author knowing it exists.

**A/B, 3 runs each:** `kFramesInFlight = 1` → mean **105.1** fps · `kFramesInFlight = 2` → mean **103.3** fps.
**No measurable difference on this scene**, and that is the expected result once you look at the split: the GPU
does ~1.8 ms of a ~9 ms frame, so there is very little GPU work to overlap CPU work *with*. The ring pays off
when GPU cost rises toward the frame budget — shadow cascades, a real BRDF, post chains, i.e. everything REN-3
adds next. It is kept because it is correct and prerequisite, **not** because it made this scene faster.

## 4. DX12 PARITY — because a Vulkan-only optimization breaks REN-36's promise

REN-36's claim is that ONE authored asset runs on both backends. If only Vulkan gets the readback opt-out and
the deferred wait, that claim survives on pixels but dies on performance: the same graph would be materially
slower on DX12 for reasons the author cannot see or control. So both landed on DX12 too:

- `submit_and_wait()` split into **`submit_no_wait()` + `wait_submitted()`**; the frame graph submits ONE
  `ExecuteCommandLists` and defers the block to the top of the next `execute()`/`reset()` — which is exactly
  where it must go, since `frame_rec_begin` resets the command ALLOCATOR and LIST, both still being consumed by
  the previous submission.
- `set_readback_enabled` with the identical contract (default **true**, so every DX12 gate is unchanged).

- **Per-pass timestamps**, same public contract (`pass_count` / `pass_name` / `pass_gpu_ms` / `gpu_ms_total` /
  `gpu_timing_available`): a `D3D12_QUERY_HEAP_TYPE_TIMESTAMP` heap, two `EndQuery` calls per pass (D3D12 has no
  begin/end pair for timestamps — a pass is two singles and the delta is its cost), `ResolveQueryData` recorded
  into a READBACK buffer *before the list closes* (it is a command, not a CPU call), and ticks → ms via
  `GetTimestampFrequency`. Gate `[ren8][dx12]` mirrors the Vulkan one assertion-for-assertion and reports
  `shadow_depth 0.0031 ms | shade 0.0020 ms | span 0.0051 ms` — against Vulkan's `0.0031 / 0.0031 / 0.0061` on
  the same scene.

Verified: full DX12 suite **1141 assertions / 115 cases** green with the wait deferred and timing live.

- **Frames-in-flight ring**, closing the last asymmetry. The obstacle was that DX12's command allocator and list
  are shared with the standalone synchronous draw paths, which Reset-then-submit-and-wait — sharing a ring with
  them would let a Reset land on an allocator the deferred graph submission is still consuming. So the graph gets
  its **own** allocator+list ring, swapped in by `frame_rec_begin` and handed back by `frame_rec_end`; the
  dedicated pair the standalone paths use is never touched.
  ⛔ Each slot remembers the **fence value of its own submission** and waits for THAT when reused. Waiting on the
  latest value instead would mean waiting for the frame just submitted — no pipelining at all, which is the exact
  bug the ring exists to remove. Ring allocation failure is non-fatal: the graph falls back to the dedicated pair
  and behaves as before.

**Both backends now have identical machinery**: readback opt-out · deferred wait · per-pass timestamps ·
dependency-sorted execution with cycle rejection · frames-in-flight.

⚠ Honest note on value: the ring measured as **worth nothing on this scene** on Vulkan (105.1 vs 103.3 fps, 3
runs each), and there is no reason to expect different on DX12 — the GPU does ~1.5 ms of a ~9 ms frame, so there
is little to overlap. It is in because it is correct, symmetric, and prerequisite for the REN-3 work that will
actually push GPU cost toward the frame budget — **not** because it made anything faster today.

## 5. ⛔ THE DOCS PROMISED A TOPO-SORT; THE CODE DID NOT DO ONE

`frame_graph.hpp` has always said `build()` *"topo-sorts passes by declared read/write dependencies… returns
false on a dependency CYCLE."* Both backends actually walked `m_passes` in **declaration order** and never
detected a cycle. So a correctly-declared graph whose passes were added out of order rendered garbage —
**silently**, with no validation error, on both APIs.

Found by asking a question the code could not answer: *can a pass be inserted BETWEEN two existing passes?*
The documented answer was yes; the real answer was no.

Now implemented on both backends, identically: edges are **writer → reader per resource**, two writers of the
same resource keep declaration order (the author's stated intent — reordering two writes would silently change
the result), and Kahn's algorithm always takes the lowest ready index so the schedule is **deterministic** and a
dependency-free graph keeps exactly the order the author wrote. Transient lifetimes are then computed over
positions in the SORTED order, since "disjoint lifetime" — the precondition for memory aliasing — is only
meaningful in execution order.

**Gates (both backends):** a graph declaring the shadow-depth PRODUCER *second*, after the pass that samples it,
schedules the producer FIRST (`pass_name(0) == "shadow_depth"`) and produces **pixel-identical** output to the
correctly-ordered REN-3.1 gate; and a true cycle (A reads what B writes, B reads what A writes) makes `build()`
return **false** rather than scheduling partially.

This is what makes graphs composable: a pass lands where its DATA says it belongs, so REN-36 authors — and the
built-in pack — can insert a pass anywhere without renumbering anything.

## What this says to do next, in order

1. ~~The fence stall~~ — **DONE**, 4.9 ms → 1.49 ms (deferred wait).
2. ~~overlay outside the graph~~ — **DONE**, now a pass; 2.64 ms → 0.
3. **present (~1.81 ms, now the second-largest)** — it blits the canvas into the backbuffer and presents.
   Direct-to-backbuffer rendering removes the blit entirely; it also still contains the tail of the GPU wait,
   so a deeper pipeline (2-3 frames in flight, one fence/command-buffer/descriptor-pool per slot) is what
   removes the remaining 1.49 ms of stall.
4. **sync (~1.49 ms)** — CPU extraction/upload of the visible list.
5. **DX12 parity** — timestamps + the deferred wait exist on Vulkan only so far, so this board is single-backend.

Note `1 passes`: the whole scene is ONE frame-graph pass, so REN-1's batching win (6.14x VK / 38.76x DX12 at
64 draws, `2026-07-24-ren1-frame-graph-batching.md`) has nothing to bite on in this scene — the draws are
already inside a single pass.

## Reproduce

```
scripts/build-target.bat build\win-release crd-sandbox
build\win-release\sandbox\crd-sandbox.exe --present immediate --smoke-test 6            # readback off
build\win-release\sandbox\crd-sandbox.exe --present immediate --readback --smoke-test 6 # readback on
```
Gate for the timing machinery itself: `crd-gpu-context-vulkan-tests "[ren8]"`.
