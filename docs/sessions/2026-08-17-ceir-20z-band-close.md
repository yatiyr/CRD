# CEIR-20z — the CEIR-20 (`ceir.work` / device-generated work) band close (2026-08-17)

The `ceir.work` dialect (device-side work generation) now ships as **one authored semantic program with THREE
device lowerings** — the charter's §135 ("at least one modern device lowering") is not just met but exceeded
(two modern lowerings + the portable fallback). Every algorithm is an authored asset; the C++ is only cooker +
device-mechanism. `scene_renderer.cpp` gained **zero** device-work-generation technique C++ across 20a–20c.

## The band's sub-slices

- **20a** — the `ceir.work` dialect DECLARED (4 native-intrinsic ops `queue_alloc`/`produce`/`consume`/`compact`
  over 2 opaque Extern type-classes `work.queue`/`work.record`; `find_work_misuse`; the 19a mold). `produce` =
  a grid dispatch appending records to a `%queue`; `consume` = an INDIRECT dispatch over the queue's DEVICE
  count (the distinction from `compute.dispatch`); `compact` = stream-compaction. DECLARE-only (crd-ceir never
  links gpu-context). commit ⑭.
- **20b** — the FALLBACK lowering + the AUTHORED consumer: the §134 wavefront's host `while(count>0)` loop is
  re-expressed as an authored `assets/frame/wavefront_work.frame.toml` → `extract_work_desc` → `build_work_ceir`
  → `lower_region` → `execute_work_lowered` + `WorkHooks`, the §134 C++ host loop RETIRED (mandate #1 at the
  execution layer). Device-proven on Win Vulkan (RTX) + lavapipe + Win DX12; decision-hash == the 19c triple32
  oracle + the `[0,1,0,1]` analytic pin. commit ⑮.
- **20c-1** — the **D3D12 Work Graphs** (SM 6.8) lowering: `emit_work_graph_{node,library}_hlsl` (cook: .ckir →
  node HLSL, union-deduped decls) → `compile_work_graph_library_to_dxil` (lib_6_8) → `Dx12WorkGraphContext`
  state-object + WORK_GRAPH subobject + `DispatchGraph` → out[0]==count==5 on the RTX 4070 Ti. The producer
  writes the (count,1,1) header + emits a grid-launch RECORD; the GPU SELF-SCHEDULES the consumer sized by it
  (removes 20b's host submit boundary). MODULE-DRIVEN via `build_work_graph_plan`. commit ⑯.
- **20c-2** — the **VK_EXT_device_generated_commands** (cross-vendor) lowering: `VulkanGpuContext::device_generated_commands_ext()`
  (EXT + maintenance5 + BDA + COMPUTE-generation cap) + `VulkanDgcContext` rig (lone DISPATCH-token indirect
  commands layout, NULL execution set + pipeline via `VkGeneratedCommandsPipelineInfoEXT` pNext, `VkBufferUsageFlags2`
  preprocess, legacy COMMAND_PREPROCESS barrier, `vkCmdExecuteGeneratedCommandsEXT`) + the AUTHORED
  `work_smoke_produce_dgc.ckir` (five (1,1,1) payloads + count=5; REUSE `work_smoke_consume.ckir` verbatim) →
  out[0]==5 (FIVE device-generated dispatch commands) on the 4070 Ti. MODULE-DRIVEN via `build_work_graph_plan`.
  commit ⑰.
- **20d** — §44 device-side control flow (IF/SWITCH/WHILE/producer-consumer): PRE-AUTHORIZED DEFERRAL (ledger
  L1) — 20c did not make it cheap enough to fold in; homed to a future band.

## ⛔ THE HONEST SCOREBOARD — one `ceir.work` program, three device lowerings

| Lowering | What the DEVICE does | What the HOST does | Proof |
|---|---|---|---|
| **20b** fallback | supplies the dispatch GRID (reads the produce-written count) | RECORDS the dispatch (`vkCmdDispatchIndirect` / `ExecuteIndirect`) | out[0]==count on Win Vk + lavapipe + DX12 |
| **20c-1** D3D12 Work Graphs | SELF-SCHEDULES the consumer via a grid-launch RECORD (no host submit boundary) | records ONE `DispatchGraph` | out[0]==count on the 4070 Ti |
| **20c-2** VK_EXT_DGC | AUTHORS the command STREAM — the sequence COUNT **and** every dispatch PAYLOAD | records ONLY `ExecuteGeneratedCommands` | out[0]==5 (five distinct device-generated commands) — ran the full 14-assertion path on **TWO independent Vulkan drivers**: NVIDIA RTX 4070 Ti + Mesa lavapipe (software) — a genuine cross-vendor proof |

⛔ Carried verbatim (never overclaimed): 20c-2's five distinct device-generated commands is the thing a single
dispatch-indirect literally cannot express (it host-records ONE dispatch). The 20b wavefront's RT stages remain
HOST-SPAN `trace_dispatch` (device-indirect was proven on the COMPUTE surface, not the RT wavefront — ledger L4).

## MATRIX (band-scope × legs — re-run at close, never inherited)

Band-scope (the CEIR-20 tests only; whole-repo × all-legs = CI's job, per the standing user directive). Each
cell is a run executed at close time (the GATE-reverify discipline).

| Sub-slice (tests) | win-debug | linux-gcc-debug (lavapipe) | win-asan (bounded: memcpy targets) |
|---|---|---|---|
| **20a** `[work]` dialect (2) | 2/2 ✅ | 2/2 ✅ (host) | — (bounded pass) |
| **20b** fallback+wavefront (14 win / 12 linux) | 14/14 ✅ | 12/12 ✅ (host + Vulkan device #4404 wavefront + #4467 executor; DX12 gates Windows-only) | — (bounded pass) |
| **20c-1** D3D12 Work Graphs (3 win / 2 linux) | 3/3 ✅ | 2/2 ✅ (host #749 plan + #3818 emit; #4566 DX12 device Windows-only) | **device gate ✅** (#4527, asan-clean) |
| **20c-2** VK_EXT_DGC (1) | 1/1 ✅ (4070 Ti) | 1/1 ✅ **RAN on Mesa lavapipe** — #4468, 14 assertions (full path, not skipped) | **device gate ✅** (#4582, asan-clean) |
| **blast radius** — NV DGC (#4152/#4153 linux, #4154/#4155 win; shared device-init) | 2/2 ✅ | 2/2 ✅ (skip on lavapipe — no VK_NV; confirms EXT edits didn't break the NV path) | — |

win-debug: **20/20 band + 2/2 NV blast-radius, 0 failed.** linux-gcc-debug: whole tree compiled under **gcc `-Werror`**; the CEIR-20 band + blast-radius all green; the only 3 repo reds are the documented **`dsp spectral` llvmpipe multi-thread SEGFAULT flake** (#4974–4976, hesap-dsp — NOT CEIR-20, NOT this band's code, identical to the pre-work baseline). win-asan (a BOUNDED pass on the 2 device gates that do manual buffer/readback memcpy into caller stack arrays — the `VulkanDgcContext` + `Dx12WorkGraphContext` new code): **2/2 clean, ZERO AddressSanitizer errors** (whole-band × asan = CI's job, per the standing user directive — this bounds the pass to the new memory-handling code, honoring the 19z asan discipline without the full-repo cost).

## builder AUDIT — mandate #1 (author the asset; C++ only as cooker/mechanism)

`grep` of `engine/scene-render/src/scene_renderer.cpp` for any `ceir.work` / `queue_alloc` / work-graph builder
= **ZERO** (the device-work orchestration lives in authored assets + generic ceir-gpu machinery, never in the
renderer).

| Symbol(s) | File | Class | Note |
|---|---|---|---|
| `build_work_ceir` | engine/ceir-gpu/src/work_build.cpp | **cooker (generic)** | the `build_fullscreen_ceir` mold — materializes a ceir.work module from an authored `WorkBuildDesc`/`.frame.toml`; the ALGORITHM is the asset, not this function |
| `extract_work_desc` | engine/frame-cook/src/frame_work.cpp | **cooker (generic)** | walks the parsed frame → `WorkBuildDesc`; no algorithm baked in |
| `emit_work_graph_{node,library}_hlsl` | engine/kir/include/crd/kir/ckir_hlsl.hpp | **cooker** | node-ABI wrapper emitted AROUND the ckir-emitted kernel body (a node-emission mode beside `emit_compute_kernel_hlsl`); never a hand-written .hlsl |
| `build_work_graph_plan` | engine/ceir-gpu/include/crd/ceir/gpu/work_graph.hpp | **cooker (generic)** | derives the produce→consume topology from the authored `WorkBuildDesc` (both 20c lowerings consume it) |
| `Dx12WorkGraphContext`, `VulkanDgcContext` | engine/gpu-context-{dx12,vulkan} | **device mechanism** | RHI verbs (state object + DispatchGraph / indirect commands layout + ExecuteGeneratedCommands); the class the mandate exempts (device binding/dispatch, not a render technique) |
| authored `.ckir` / `.frame.toml` | assets/ckir/work_smoke_{produce,consume,produce_dgc}.ckir, assets/frame/wavefront_work.frame.toml | **AUTHORED ASSET (SOLE source)** | each has a DECOUPLED load/roundtrip or device gate; no committed test couples an asset to a builder |

## ROW-PER-CLAIM table (capability → gate → re-run)

| Claim | Gate (test #) | Re-run result |
|---|---|---|
| **20a `queue_alloc`** — abstract queue PROVISIONING (no device op; the resolver provisions the buffer at execute) | `[work]` #603 (well-formed chain) + #604 (type-chain rejects) | win-debug ✅ + lavapipe ✅ (host) |
| **`produce`** — a grid dispatch appending to a `%queue` (writes the DEVICE count) | #4467/#4620 (executor Vk), #4682 (DX12), #4404 (wavefront Vk), #4565 (wavefront DX12) | win-debug ✅ + lavapipe ✅ (Vk; DX12 Windows-only) |
| **`consume`** — an INDIRECT dispatch DEVICE-count-sized (out[0]==count) | #4467/#4620/#4682 (executor); all THREE lowerings exercise consume | win-debug ✅ + lavapipe ✅ |
| **`compact`** — declared + lowered + HOST-execute-tested; NO device consumer | #750/#753 (build + lower + route) | win-debug ✅ + lavapipe ✅ (host) — device consumer ⛔ LEDGERED L3 |
| **20c-1 D3D12 Work Graphs** — .ckir→node HLSL → DispatchGraph, out[0]==count | #3818 (emit), #749 (plan), #4566 (device 4070 Ti) | win-debug ✅ + win-asan #4527 ✅ (Windows-only) |
| **20c-2 VK_EXT_DGC** — device-generated command STREAM, out[0]==5 (five commands) | #4621 (4070 Ti), #4468 (lavapipe, 14 assertions) | win-debug ✅ + lavapipe RAN ✅ + win-asan #4582 ✅ |
| **module-driven topology** — `build_work_graph_plan` derives produce→consume from the SAME `WorkBuildDesc`; both 20c gates SELECT their SPIR-V/nodes FROM the plan (not hardcoded) | #749 (plan); the 20c-1/20c-2 device gates drive THROUGH it | win-debug ✅ + lavapipe ✅ |
| **wavefront-drives-asset** — the authored `wavefront_work.frame.toml` drives the device wavefront == the 19c oracle | #4404 (Vk), #4565 (DX12), #4094–4096 (headless parse+extract) | win-debug ✅ + lavapipe ✅ (Vk) |
| **.ckir ASSET INVENTORY** — `work_smoke_{produce,consume,produce_dgc}.ckir` each has a DEVICE-FREE load/round-trip/emit gate | **#3961 (NEW** — the 3 assets parse + round-trip byte-exact + emit GLSL, no device) | win-debug ✅ + lavapipe ✅ — ⛔ the new gate closes the "format-validation rode the cap-skipping device tests" hole the advisor flagged |
| **mandate #1** — the algorithm is the AUTHORED asset; C++ = cooker + device mechanism ONLY | the builder audit above (grep scene_renderer = zero) | win-debug ✅ (audit clean) |

## DEFERRAL LEDGER (filed forward, each with a home)

| # | Deferred | Home |
|---|---|---|
| L1 | **20d §44 device-side control flow** (IF/SWITCH/WHILE/producer-consumer) — pre-authorized deferral | a future CEIR band (device control-flow) |
| L2 | **`work.record` payload flow** — the type-class is declared-forward-unused (no op takes a `%record`); a future `produce_one`/`consume_one` + the Work-Graphs record-PAYLOAD path (20c-1 proved grid-launch RECORDS only, not payload data flow) | a future work-record band |
| L3 | **`work.compact` DEVICE consumer** — declared + lowered + host-execute-tested, but NO device consumer shipped. ⛔ RE-HOMED: the 20b ledger homed this to "the 20c parallel-compaction path," but **20c did NOT discharge it** (both 20c smokes are produce+consume only). The wavefront's serial compact is a const-grid `Dispatch`, not a device compaction. | a future parallel-compaction band (NOT 20c) |
| L4 | **RT device-resident / indirect trace** — the 20b wavefront's RT stages (trace/shade) keep the 19c HOST-SPAN `trace_dispatch`; device-indirect sizing was proven on the COMPUTE surface only | a future RT-execution band (next to `rt.trace` lowering, ex-19z L1) |
| L5 | **live-frame work-pass executor** — a frame-level executor running compute+work passes uniformly inside a LIVE frame does not exist; 20b's gate is parse→cook→`execute_work_lowered`, the trace run by the harness at the asset-declared position | a future frame-runtime band |
| L6 | **`build_work_graph_plan` single-producer restriction** — the plan rejects multi-entry / compact-fed graphs (`!=1` entry); the smokes are single-producer | a future band when a multi-producer asset exists |
| L7 | **`work_graph.hpp` naming** — the header derives a generic work-topology plan consumed by BOTH Work Graphs and DGC, so the `work_graph` name is narrower than its use. DECISION: NOT renamed at close (cosmetic; a rename during band close is churn-for-no-function). Homed as a deferred cleanup. | a future cleanup pass |

## Verdict

✅✅✅ **CEIR-20 (`ceir.work` / device-generated work) BAND CLOSED.** One authored semantic program, **three
device lowerings** — the §135 charter ("at least one modern lowering") exceeded:

- **win-debug**: 20/20 band + 2/2 NV blast-radius, 0 failed.
- **linux-gcc-debug (lavapipe)**: whole tree compiled under gcc `-Werror`; the CEIR-20 band + blast-radius all
  green; the sole 3 reds are the pre-existing `dsp spectral` llvmpipe flake (unrelated). ⭐ the VK_EXT_DGC gate
  **RAN the full 14-assertion path on Mesa lavapipe** — a genuine second-vendor device proof.
- **win-asan**: the 2 manual-readback-memcpy device gates (DGC + WG) clean, zero AddressSanitizer errors.

Every leg re-run at close time (never inherited — the GATE-reverify discipline). The zero-builder audit is
clean (no ceir.work builder in the renderer; C++ is cooker + device mechanism only). Seven items filed forward in
the ledger, each with a home; the expired `work.compact`→"20c" home was RE-HOMED (20c did not discharge it).
The honest scoreboard holds: 20b's RT wavefront stages remain host-span; 20c-2's cross-vendor claim is now
backed by two independent drivers actually running the path.

Sub-slice + ledger detail is AUTHORITATIVE in this log + the tracker CEIR-20 row. Charter §135 proof: two
modern lowerings (Work Graphs + DGC) + the portable fallback, all device-gated.
