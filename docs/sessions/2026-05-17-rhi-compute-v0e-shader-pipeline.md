## Session 2026-05-17 — Phase 3.1.7.6 v0e `rhi-compute-shader-pipeline` ✅ SHIPPED

### Goal

Formalize compute shaders through the high-level `crd::shader` Effect /
Variant / Runtime system. The binary-blob compile path already worked
(v0a–v0d tests called `device->create_shader_module(spv_bytes)`
directly). v0e adds reflection support for the compute-specific
metadata — workgroup size + specialization constants — and verifies
the existing hot-reload path covers compute.

### What we shipped

**`engine/shader/include/crd/shader/types.hpp`** —
- `WorkgroupSize { u32 x, y, z }` struct. Default `{1, 1, 1}` for
  consistency; populated for compute modules only.
- `SpecializationConstantReflection { u32 constant_id; String name;
  u32 size_bytes; u32 default_value_u32 }`. 1:1 mirror of the data
  needed at `VkSpecializationInfo` wire-up; default value stored as a
  u32 bit pattern so consumers reinterpret for int / float / bool.

**`engine/shader/include/crd/shader/effect.hpp`** — `Module` interface
gained two accessors:
- `workgroup_size() → std::optional<WorkgroupSize>` — honest sentinel.
  `nullopt` for vertex/fragment modules (genuinely no workgroup size);
  `WorkgroupSize{...}` for compute. Advisor pin: `optional` over
  `{0,0,0}` sentinel because zero could mask a reflection failure.
- `specialization_constants() → ConstSpan<SpecializationConstantReflection>`
  — per-module, not auto-merged across an Effect's modules. Aggregation
  across modules is a consumer-side concern if needed.

**`engine/shader/src/runtime.cpp`** — reflection path extended:
- `ReflectedData` gains `optional<WorkgroupSize>` + `Array<SpecializationConstantReflection>`.
- `reflect_module`: for compute stage, reads `module.entry_points[0].local_size`
  and stores `{x, y, z}`. For all stages, iterates `module.spec_constants[i]`
  and extracts `constant_id` + `name` + first-4-byte default value via
  `memcpy` from spirv-reflect's `void* default_value`.
- `StoredModule` overrides the new accessors.

**NEW** `runtime/examples/shaders/compute_v0e_reflection.comp` — the
discriminating shader. `local_size = (32, 4, 2)` (unique enough that
an X/Y/Z swap fails loudly) + `layout(constant_id = 7) const uint kFoo = 42`
+ `layout(constant_id = 3) const int kBar = -1` + a storage buffer
binding so the compiler keeps the spec consts live. Header comment
makes the "wrong reflection > no reflection" intent explicit.

**`tests/shader/test_runtime.cpp`** — 5 `[v0e]` cases (35 assertions):
1. **Workgroup size reflection** — compute module reports
   `WorkgroupSize{32, 4, 2}` from the shader. The discriminator.
2. **Vertex/Fragment have no workgroup size** — `workgroup_size()`
   returns `nullopt`. Pins the optional sentinel intent.
3. **Spec const reflection by `constant_id + name + size + default`** —
   the *discriminator*. `kFoo` (id=7, default=42) and `kBar`
   (id=3, default=-1 → u32 0xFFFFFFFF) reflected correctly. Wrong
   constant_id is worse than missing reflection.
4. **Vertex/Fragment without spec consts report empty list** — back-compat
   pin for graphics modules.
5. **Compute Effect hot-reload via `reload_effect` succeeds** — the
   existing watch path is stage-agnostic; this is the
   verify-don't-add-code coverage test the advisor recommended.

### Eliteness decisions (advisor-locked at slice start)

1. **Minimal-v0e scope.** Reflection + hot-reload coverage. No
   high-level `Effect → ComputePipelineDesc` accessor; v9-LBVH and
   future consumers build `ComputePipelineDesc` directly from a
   `Module`'s spv bytes + reflected layout (one-line call site).
   Forcing graphics-shaped Effect/VariantRequest onto compute today
   would be the half-baked pattern.
2. **Compute Effects = single-variant rule.** `VariantRequest`'s
   graphics fields (`pass_type` / `alpha_mode` / `render_path` /
   `skinned`) are ignored for compute; `request_variant` returns the
   one variant the compute module has. Verified by test 1 (single
   `modules.size() == 1`).
3. **Spec const reflection is stage-agnostic** but stored per-Module
   (not on Effect, not auto-merged). Advisor pin — Effect is many
   modules; spec consts are intrinsically per-module.
4. **`std::optional<WorkgroupSize>` over `{0,0,0}` sentinel.** Zero
   could mask reflection failure; `optional` is the honest "this
   doesn't apply" signal.
5. **Spec const size pinned at 4 bytes.** 64-bit spec consts are
   extremely rare; pin small and revisit when a real consumer needs 8.
   Pin documented inline.

### Mid-slice fix

The spirv-reflect API I initially wrote against (`specialization_constant_count`
+ typed enum `SPV_REFLECT_SPECIALIZATION_CONSTANT_*` for the constant
kind) doesn't match the Vulkan SDK's bundled version. The actual API:
`module.spec_constant_count` + `module.spec_constants[i]` with
`default_value` as `void*` and explicit `default_value_size` (bytes).
For 32-bit scalars (bool / int / float), the first u32 IS the bit
pattern — `memcpy` preserves int sign bits + float bit representation
alike, no typed dispatch needed. Simpler code than I initially wrote.

Pattern locked: when reaching for spirv-reflect APIs, grep
`/c/VulkanSDK/.../Source/SPIRV-Reflect/spirv_reflect.h` for the actual
struct shape before assuming the documentation API.

### Per-slice 4-config DoD

`scripts/per-slice-check.ps1 -Parallel`, elapsed 00:58:
- win-debug    PASS (build+ctest)
- win-asan     PASS (build+ctest)
- win-shipping PASS (build+ctest)
- win-tidy     PASS (build)

### Stats

- Engine LOC: ~80 (types.hpp: 25; effect.hpp: 12; runtime.cpp: 43).
- Test LOC: ~150 (test_runtime.cpp: 145; shader: 30).
- Total v0e cases / assertions: 5 cases / 35 assertions, all green.
- Estimate at slice start: ~400 engine + ~300 tests, ~3 days. Actual:
  ~80 engine + ~150 tests, ~1 day. Advisor's minimal-scope call was
  the lever — additive reflection inside existing helpers, not new
  paths. Pattern continues from v0a–d.

### Combined Phase 3.1.7.6 stats so far

- **5 of 6 slices SHIPPED** (v0a + v0b + v0c + v0d + v0e).
- Engine LOC: ~1290. Test LOC: ~1200. Cases: 36. Assertions: 456.
- Calendar: 5 days (estimate was ~24 days advisor-tightened).

### Next slice

**v0-close** — Phase 3.1.7.6 CLOSED. ADR-0080 promoted from Proposed
to Accepted with the v0a-v0e amendments folded in (D2 revision, D7
softening, granular `BufferAccess` enum, `compute_barrier` →
`buffer_barrier` rename, `SubmitInfo` shape, pointer-identity contract,
minimal-v0e reflection scope). System doc `docs/systems/rhi-compute.md`.
**18-config full sweep** (`scripts/full-sweep.ps1`). MEMORY / context /
ROADMAP final sync + cluster session log. After v0-close: Phase 3.1.7
v9 unblocked — v9c V-HACD + v9a GPU LBVH + v9b GPU BVH refit + v9e
shader-helpers (16 slices, ~6 weeks per the v9 plan locked 2026-05-17).
