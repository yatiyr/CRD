# 2026-08-11 — CEIR-14z: the render DEVICE proof band closes (BAND-14 COMPLETE)

Autonomous-loop arc closing **CEIR-14 (band-14)** — the render dialect's device proof. The §169 shape
list is now proven device-side on both backends through the gold-standard frame-recording executor.

## What shipped (the §169 shapes, all pixel-asserted both backends)

- **14z-3 — triangle.** The first render pixels from CEIR: a shared `render.scope{render.draw}` renders
  the CKIR triangle RED on a blue clear, Vulkan (real GPU) + DX12 (real GPU) + llvmpipe, validation-silent.
- **14z-4 — MRT + per-target typed clears.** 4a the frame-recording drive (`execute_render_frame`, one
  frame-graph pass per scope) + 2-scope; 4b per-attachment `AttachmentClear`; 4c c1 the CEIR binding
  resolver + first CEIR MRT, c2 uint MRT, c3 heterogeneous uint@0+float@1 MRT.
- **14z-5 — depth-only.** By depth-test OCCLUSION (attachment-only, no sampling): a genuine depth-only
  pass writes depth 0.5 under the triangle; a later fullscreen tests 0.75 against the loaded depth →
  centre BLUE (fails) / corner RED (passes). The depth-only-≠-forward scar honored by construction.
- **14z-6 — indexed-indirect(-count).** c1 the REN-40 DrawIndex-PUSH proof (a DrawIndex-reading VS +
  two identical sub-draw commands → draw 0 left / draw 1 right; right-RED proves SV_DrawIndex=1 pushed).
  c2 the count discriminator (count=2 both / count=1 right-BLUE → the device count gates execution).
- **14z-7 — mesh dispatch.** A procedural mesh shader (`build_triangle_mesh`, one meshlet) via
  `create_mesh_program` → `draw_mesh`: centre RED, corner BLUE. Ran on llvmpipe (both linux configs).

## The composition claim (why this is one machine, not five)

Every shape rides ONE `execute_render_frame` + one materializer + one lowering. Each cross-cutting path
was FORCED by a later shape: the N-scope closure array by 4c's 2-scope test, the per-op program-map by
14z-5 (two scopes, two programs), the operand-keyed buffer-map by 14z-6 (args + vbuf from distinct
operands). §169 proves a single executor.

## The empirical vindication (the user's no-name-forward verdict)

The original recommendation was Option-A-defer (name-forward MRT + indexed-indirect to 15/16). The user
overruled it: drive frame-recording mode now, fix engine bugs immediately, no shortcuts. That pull-forward
found + fixed **five two-backend engine bugs** the synchronous path would never have surfaced:

1. DX12 uint-clear must VALUE-CONVERT, not bit-reinterpret (14z-4c c2).
2. DX12 `pso_for` must thread PER-ATTACHMENT RTV formats through the content-keyed cache (c3).
3. `record_plain` (frame-recording draw/draw_depth) hardcoded the depth loadOp to CLEAR, ignoring
   `set_next_draw_load_depth` (14z-5).
4. `free_transients` deleted the transient IMAGE wrappers but leaked the transient BUFFER wrapper —
   24 bytes/rebuild, both backends (LeakSanitizer named it) (14z-5).
5. `mesh_dispatch` is 3D but the Meshlet verb is 1D — a y/z>1 grid would silently draw (gx);
   now refused LOUDLY at the lowering layer (14z-7).

## What band-14 does NOT claim

The proofs drive TEST-AUTHORED CEIR programs through identity/map sentinel resolvers. Resolver
production, cook integration, and authored-graph unification are exactly **CEIR-15 (FrameGraph
unification)** / **CEIR-16 (Executor migration)**. Validation-silence is Vulkan-only (the DX12
info-queue capture stays the 13z named-forward).

## Named-forwards (documented, all with a home)

- `first_draw_index` (the DrawIndex BASE, runtime scene state) → 15/16.
- `render.mesh_dispatch_indirect` (MeshletIndirect device consumer) → §169 "mesh" = the direct op.
- mesh y/z 3D verb widen (an API-step-down) → the user's call.
- `build_mesh_pso` heterogeneous mesh-MRT format → first mesh-MRT consumer / 15-16.
- CEIR-bound-resource fg-tracking + GPU-written indirect args → 15/16.

## Gate

All slices gated 4 configs (win-debug/asan + linux-gcc-debug/asan), LLVM-20 tidy clean, LeakSanitizer
clean on linux-asan. Final band tallies: win ceir 554/554; linux ceir 545/545. The whole CEIR-14z
render-device band sits uncommitted for the user's batch.

## Next

**CEIR-15 (FrameGraph unification)** — an architecture band (§126's eight steps; `FrameGraphDesc` ↔
`ceir.frame`). It opens with a decision packet, not code.
