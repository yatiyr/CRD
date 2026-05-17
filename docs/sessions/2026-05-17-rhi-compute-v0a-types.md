## Session 2026-05-17 — Phase 3.1.7.6 v0a `rhi-compute-types` ✅ SHIPPED

### Goal

Open Phase 3.1.7.6 `crd-rhi-compute` prerequisite sub-phase (locked
2026-05-17 per the v9 eliteness audit Option B). v0a ships the
ComputePipeline type + factory + first-light Vulkan integration; v0b
adds the dispatch/bind path; v0c sync; v0d async compute queue; v0e
shader pipeline; v0-close ADR-0080 lock + 18-config sweep.

### What we shipped

- **`engine/rhi/include/crd/rhi/compute_pipeline.hpp`** (new) —
  `ComputePipeline` interface (sibling of `Pipeline`, not a shared
  base). Concrete justification in the file's header comment:
  graphics + compute bind to *different* Vulkan pipeline bind points,
  which is not polymorphism.
- **`engine/rhi/include/crd/rhi/types.hpp`** — added
  `ComputePipelineDesc { ShaderModule*; PipelineLayout* }`. Two-pointer
  struct, trivially copyable, standard-layout (pinned in tests).
- **`engine/rhi/include/crd/rhi/device.hpp`** — added pure-virtual
  `create_compute_pipeline(const ComputePipelineDesc&)`. Inline
  comment notes "additive; existing graphics surface untouched"
  (ADR-0080 D1).
- **`engine/rhi/include/crd/rhi/rhi.hpp`** — umbrella include.
- **`engine/rhi-vulkan/src/vulkan_backend.cpp`** — `VulkanComputePipeline`
  class + factory impl. Rejects null shader / wrong-stage shader / null
  layout cleanly (returns `nullptr`, no exceptions). Synthesises an
  empty `VkPipelineLayout` for callers passing `nullptr` (matches the
  `create_graphics_pipeline` ergonomic precedent). Owned synthesised
  layout destroyed alongside the pipeline (no leak).
- **`runtime/examples/shaders/compute_v0a.comp`** (new) — minimal
  compute shader (`local_size = 1×1×1`, empty body) for integration
  smoke. Compiled via existing `crd_compile_glsl` CMake helper +
  `glslangValidator`.
- **`tests/rhi/test_rhi.cpp`** — `FakeComputePipeline` + 5 RHI-side
  contract tests: desc surface pin, factory contract, lifecycle
  multi-create/destroy, caller-side `PipelineLayout` composition,
  trivially-copyable layout pin. **23 assertions, 5 cases, PASS.**
- **`tests/rhi_vulkan/test_rhi_vulkan.cpp`** — 6 Vulkan-side
  integration cases: null-shader reject, wrong-stage reject, valid
  compute shader creates successfully, caller-provided
  `PipelineLayout` (storage-buffer descriptor convention), 8-cycle
  multi-create/destroy ASan-clean, `BufferUsage::Storage` 4-KB buffer
  D2-revision pin. **46 assertions, 6 cases, PASS.**

### Decisions locked (carry into ADR-0080 amendment at v0-close)

- **D1** Additive-only RHI extension confirmed. No existing graphics
  surface renamed or removed. New types live alongside.
- **D2 REVISED** Storage buffers **reuse the existing `Buffer`
  interface** with `BufferUsage::Storage`. The original ADR-0080 D2
  draft proposed a separate `IStorageBuffer` type; on orientation we
  discovered the existing `Buffer` (just `desc()` + `map()` + `unmap()`)
  has no per-usage-type divergence — the differences (SSBO vs uniform
  descriptor type, barriers) live at the descriptor-write site and the
  command-buffer barrier API, not at the buffer type. Splitting would
  force consumers holding both kinds to carry two pointer types for
  zero behavioral gain. Tightening pinned in `compute_pipeline.hpp`
  header comment + `test_rhi_vulkan.cpp::"Vulkan create_buffer with
  BufferUsage::Storage works (D2 revision)"`. Consolidated amendment
  text at v0-close per advisor recommendation.
- **D7 SOFTENED** Descriptor-set conventions (set 0 = storage,
  set 1 = uniform) are a **documented consumer guideline**, not a
  type-level enforcement. `PipelineLayoutDesc` already accepts any
  caller-constructed layout (matches graphics flexibility). Will land
  in `docs/systems/rhi-compute.md` § Recommended conventions at
  v0-close, not in the type system.
- **Naming pin** — `Pipeline` kept (graphics-only by class comment);
  `ComputePipeline` standalone. No `PipelineBase` hoist. Bind point
  divergence (`VK_PIPELINE_BIND_POINT_GRAPHICS` vs `_COMPUTE`) makes
  this honest, not polymorphic. Cleanup follow-on filed: "rename
  `Pipeline` → `GraphicsPipeline`" only if a real readability problem
  surfaces in a future slice.

### Mid-slice fix

The new pure-virtual `Device::create_compute_pipeline` triggered C2259
("abstract class can't be instantiated") on three pre-existing
`Fake/Smoke` device classes that didn't override it:
`runtime/examples/smoke_renderer.cpp`, `tests/renderer/test_renderer.cpp`,
`runtime/examples/smoke_rhi_api.cpp`. All three patched to return
`nullptr` (compute not exercised in those smoke / unit-test contexts).
Pattern locked: any new RHI-level pure-virtual requires `grep "public
crd::rhi::Device" engine runtime tools tests` to enumerate impls.

### Pre-existing tidy debt fix

The DoD's `win-tidy` first-attempt caught a pre-existing
`readability-identifier-naming` violation in
`engine/draw-imgui/src/control_panel.cpp:92` — `static const NamedTheme
themes[]` should follow the project `kCamelCase` convention for
function-scope `static const` arrays. Renamed
`themes` → `kThemes` in that file (single-file, 4-line edit). Not a
v0a-introduced violation; my changes triggered a re-tidy of downstream
files that surfaced it. The 2026-05-17 CI relaxation
(`CRD_CLANG_TIDY_WARNINGS_AS_ERRORS=OFF` in CI; local strict) means
the local gate is now the authoritative tidy enforcer, which is doing
its job here.

### Per-slice 4-config DoD

`scripts/per-slice-check.ps1 -Parallel`, elapsed 00:30:
- win-debug    PASS (build+ctest)
- win-asan     PASS (build+ctest)
- win-shipping PASS (build+ctest)
- win-tidy     PASS (build)

### Stats

- Engine LOC: ~360 (rhi: ~120; rhi-vulkan: ~140; shader: 14; Fake/Smoke patches: ~30; tidy fix: 4).
- Test LOC: ~280 (test_rhi.cpp: ~110; test_rhi_vulkan.cpp: ~170).
- Total v0a cases / assertions: 11 cases / 69 assertions, all green.
- Estimate at slice start (advisor-tightened): ~400 engine + ~250 tests, ~2-3 days. Actual: ~360 engine + ~280 tests, ~1 day. Tightening of D2 (reuse Buffer for storage) is what shrank the engine LOC.

### Next slice

**v0b `rhi-compute-dispatch`** — `ICommandBuffer::bind_compute_pipeline`
+ `bind_compute_storage_buffer(set, binding, buffer)` +
`push_compute_constants(offset, size, data)` + `dispatch(x, y, z)` +
`dispatch_indirect(indirect_buffer, offset)`. Specialization constants
threaded through `ComputePipelineDesc`. First-light smoke: bind
pipeline, bind storage buffer initialised with `Array<u32>` zeros,
dispatch one workgroup that writes `gl_GlobalInvocationID.x` to each
element, readback, validate. ADR-0080 D4 + D5 + D6 lock at this slice.
