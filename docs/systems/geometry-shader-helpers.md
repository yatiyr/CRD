# crd-geometry-shader-helpers

GPU/CPU twin of the C++ analytic SDF library. Walks a `FormulaIr` —
a flat tree of `signed_distance.hpp` primitives + `formulary.hpp`
operators — and emits GLSL 450 / HLSL 6.0 source that produces the
same SDF values the C++ evaluator produces, within mixed ULP+absolute
tolerance. The cooker writes the emitted text to disk so downstream
renderers (DFAO, soft-shadows, font MTSDF, editor preview) consume
cooked shader files directly — no link-time dependency on the cooker.

> Module path: `engine/geometry-shader-helpers/`
> Target: `crd-geometry-shader-helpers`
> Namespace: `crd::geometry::shader_helpers`
> Opened: Phase 3.1.7 v0e (skeleton); v9e-a (substance) 2026-05-18
> Status: ✅ **CLUSTER CLOSED 2026-05-19** — v9e-a formula-IR ✅ +
> v9e-b GLSL backend + ULP-conformance GPU dispatch ✅ + v9e-c HLSL
> backend + dxc → SPIR-V GPU dispatch ✅ + v9e-d cooker ✅ +
> **v9e-close ADR-0076 §26 ✅ Accepted (D166-D181)**. Phase 3.1.7
> sub-module 11 of 11 ✅ (final).

## Public surface

| Header | Purpose |
|---|---|
| `crd/geometry/shader_helpers/formula_ir.hpp`        | `FormulaIr` storage + `IrBuilder` fluent API + `validate(ir)` + kind enums |
| `crd/geometry/shader_helpers/formula_evaluator.hpp` | `evaluate<T>(ir, p) -> T` — C++ ground-truth evaluator (drives backend conformance) |
| `crd/geometry/shader_helpers/golden_manifests.hpp`  | 21 hand-built `FormulaIr` constructors (primitive + operator coverage corpus) |
| `crd/geometry/shader_helpers/glsl_emitter.hpp`      | `glsl_helpers_prelude()` + `emit_glsl_sdf_function` + `emit_glsl_conformance_shader` |
| `crd/geometry/shader_helpers/hlsl_emitter.hpp`      | `hlsl_helpers_prelude()` + `emit_hlsl_sdf_function` + `emit_hlsl_conformance_shader` |
| `crd/geometry/shader_helpers/cooker.hpp`            | `cook_helpers_prelude` + `cook_ir` — write GLSL/HLSL to disk |

## The pipeline

```
                          ┌──────────────────┐
   designer / cooker ─────│   IrBuilder      │
                          │   .sphere(0.5)   │
                          │   .smin_poly(...)│
                          │   .build(root)   │
                          └────────┬─────────┘
                                   │ FormulaIr
              ┌────────────────────┼────────────────────┐
              ▼                    ▼                    ▼
   evaluate<f32>(ir, p)   emit_glsl_sdf_function   emit_hlsl_sdf_function
        (C++ truth)         (GLSL 450 text)         (HLSL 6.0 text)
              │                    │                    │
              │            ┌───────┴───────┐    ┌───────┴───────┐
              │            │ compile_glsl  │    │ compile_hlsl  │
              │            │ (shaderc)     │    │ (dxc)         │
              │            │  → SPIR-V     │    │  → SPIR-V     │
              │            └───────┬───────┘    └───────┬───────┘
              │                    │                    │
              │                    ▼                    ▼
              │              Vulkan dispatch     Vulkan dispatch
              │              (32³ sample grid)   (32³ sample grid)
              │                    │                    │
              └────────────────────┼────────────────────┘
                                   │
                          mixed ULP+abs compare
                          (1 ULP OR 1e-6 absolute)
```

The cooker (v9e-d) is a separate path that writes
`emit_*_sdf_function` output to disk so renderers load the cooked
text at runtime — no cooker library dependency at consumer runtime.

## v9e-a — formula-IR

```cpp
namespace crd::geometry::shader_helpers {

enum class IrNodeKind : u8 { Primitive, Operator };

enum class IrPrimKind : u8 {
    Sphere, Box, RoundBox, BoxFrame, Plane,
    Capsule, Cylinder, Cone, Torus, Triangle3D
};

enum class IrOpKind : u8 {
    SminPoly, SminCubic, SminExp, SmaxPoly,
    OpRound, OpOnion,
    DomainRepeat, DomainMirror, DomainElongate,
    DomainTwist, DomainBend
};

struct IrNode {
    IrNodeKind kind;
    /* one of */ IrPrimKind prim_kind;
                 IrOpKind   op_kind;
    u32 params_offset, params_count;
    u32 children_offset, children_count;
};

class FormulaIr {
    Array<IrNode> nodes;
    Array<f32>    params;
    Array<u32>    children;
    u32           root;
};

class IrBuilder {
    // Fluent — every call returns a node index for composition.
    [[nodiscard]] u32 sphere(f32 r);
    [[nodiscard]] u32 box(f32 hx, f32 hy, f32 hz);
    [[nodiscard]] u32 smin_poly(u32 a, u32 b, f32 k);
    [[nodiscard]] u32 domain_twist(u32 child, f32 k);
    // ... 19 more
    [[nodiscard]] FormulaIr build(u32 root) &&;
};

[[nodiscard]] IrValidationResult validate(const FormulaIr&) noexcept;
template <typename T> [[nodiscard]] T evaluate(const FormulaIr&, Vec3<T> p);

} // namespace crd::geometry::shader_helpers
```

### Storage (D167)

Three parallel `Array`s + a root index. NOT pointers, NOT
`std::variant`. Reasons:

1. **Cache-friendly walk** — contiguous nodes; GLSL/HLSL emission
   walks in order.
2. **Bit-exact serialisation for the cooker** — no pointer fix-up on
   load; copy three arrays + root.
3. **O(1) bounds-check validation** — every reference is an array
   index; `validate()` runs a single linear pass.
4. **GPU-portable** — same flat layout transfers to SSBO + GLSL walk
   for future GPU-side IR interp.

### Validation (D169)

`validate(ir) -> IrValidationResult{status, node_index}` enforces:

- `nodes.size() > 0` (else `EmptyIr`)
- `root < nodes.size()`
- For every node: `params_offset + params_count <= params.size()`,
  `children_offset + children_count <= children.size()`
- Per-kind param + child counts match the fixed spec
- Tree is acyclic + every node reachable from `root`
- Every kind enum value is in range

Cooker (v9e-d) pre-validates — bad IR fails as cooker error, not an
assertion deep inside the emitter walker.

### Golden manifests (D171)

`golden_manifests.hpp/cpp` exports 21 `make_*` constructors — the
per-primitive (10) + per-operator (11) conformance corpus:

```
make_sphere_unit / make_box_unit / make_round_box / make_box_frame /
make_plane_y / make_capsule / make_cylinder / make_cone / make_torus /
make_triangle / make_smin_poly_union / make_smin_cubic_union /
make_smin_exp_union / make_smax_poly_intersect / make_op_round_sphere /
make_op_onion_sphere / make_domain_repeat_sphere /
make_domain_mirror_sphere / make_domain_elongate_sphere /
make_domain_twist_cylinder / make_domain_bend_capsule
```

Every backend (GLSL, HLSL, future GPU IR-interp) tests against every
manifest at every conformance run. Closed at v9e-close — adding a
new primitive/operator (D168) requires adding a new golden manifest
in the same slice.

## v9e-b — GLSL backend

```cpp
[[nodiscard]] StringView glsl_helpers_prelude() noexcept;

[[nodiscard]] String emit_glsl_sdf_function(
    const FormulaIr& ir, StringView function_name,
    IAllocator* alloc) noexcept;

[[nodiscard]] String emit_glsl_conformance_shader(
    const FormulaIr& ir, IAllocator* alloc) noexcept;

struct alignas(16) GlslConformancePushConstants {
    float grid_origin[3];
    float pad0;
    float grid_step[3];
    u32   grid_resolution;
};
```

### Emission style: SSA (D175)

Each IR node becomes a `float n_<i>` local. Position-domain operators
(`domain_repeat` / `_mirror` / `_elongate` / `_twist` / `_bend`)
introduce a `vec3 p_<i>` warp local for their child:

```glsl
float sdf(vec3 p) {
    // Position-domain ops introduce p_<i> for their child:
    vec3 p_2 = domain_twist(p, 2.0);    // node 2 (domain op)
    // Primitives + value-domain ops produce n_<i>:
    float n_0 = sd_sphere(p_2, 0.4);    // node 0 (primitive, child of warp)
    float n_1 = sd_sphere(p, 0.25);     // node 1 (primitive)
    float n_3 = crd_smin_poly(n_0, n_1, 0.1);  // node 3 (smin operator)
    return n_3;
}
```

Why SSA (vs nested-expression): warps compose. Nested form like
`sd_sphere(domain_twist(p, 2.0), 0.1)` collapses when warps chain —
the inner positions become unreadable + impossible at depth. SSA
emission is uniform regardless of depth + debuggable (you can
breakpoint any `n_<i>`).

### Fixed-text prelude (D172)

`glsl_helpers_prelude()` returns a stable string with:

- All 10 `sd_*` primitive functions (sphere/box/round_box/box_frame/
  plane/capsule/cylinder/cone/torus/triangle).
- All 11 operator functions (crd_smin_poly/cubic/exp + crd_smax_poly
  + op_round/onion + domain_repeat/mirror/elongate/twist/bend).
- The **deterministic Cephes-poly port** (D174): `crd_det_sin/cos/
  exp/exp2/log/log2` — polynomial approximations matching
  `crd::math::deterministic::*` bit-for-bit on their approximation
  range. Used by `smin_exp`, `domain_twist`, `domain_bend`.
- Helper macros + utilities: `crd_sign_bit` (returns mask 0 /
  0x80000000 — NOT 0/1; D176-i), `crd_max3`, etc.

### ULP-conformance test path

`emit_glsl_conformance_shader(ir, alloc)` returns a complete compute
shader: prelude + `float sdf(vec3 p)` + a `main()` that reads
`{grid_origin, grid_step, grid_resolution}` from push constants and
writes one `float` per voxel into a storage buffer at
`(z * res + y) * res + x`.

The conformance test (`test_glsl_emit.cpp`) compiles this via
`compile_glsl` (shaderc dynamic-load) → SPIR-V → Vulkan dispatch via
`crd-rhi-compute`, reads back the storage buffer, and `ulp_compare`s
each sample against `evaluate<float>(ir, p)` with mixed ULP+abs
tolerance (D173):

```
21 manifests × 32³ samples = 688 128 per-pixel comparisons, all pass.
```

### Critical pins (D176, catalogued because they ate hours)

| Bug | Fix | Symptom |
|---|---|---|
| `crd_sign_bit` returned 0/1 | Return mask (0 or 0x80000000) | `domain_bend` at 4.6 M ULP → 14 ULP |
| `smin_poly` used `k` directly | Use `kk = k * 4.0` (matches C++) | 747 448 ULP → 2 ULP |
| `smin_cubic` used `k` directly | Use `kk = k * 6.0` (matches C++) | Same class |
| `max3` collides with `GL_NV_shader_extension` | Renamed `crd_max3` | Compile error in GLSL |
| `flat` is reserved | Renamed `flat_idx` | Compile error in GLSL |
| GPU-native transcendentals diverge from CPU | Cephes-poly port `crd_det_*` (D174) | `smin_exp`/`twist`/`bend` ULP-bounded |

## v9e-c — HLSL backend

Mirror of GLSL backend. Same IR walker, same emission style, same
helpers — only language-level syntax differs:

| GLSL | HLSL |
|---|---|
| `vec3` | `float3` |
| `vec2` | `float2` |
| `uintBitsToFloat` | `asfloat` |
| `floatBitsToUint` | `asuint` |
| `layout(std430, binding=0) buffer Out { float[] o; }` | `RWStructuredBuffer<float> o : register(u0);` |
| `layout(push_constant) uniform Push { … }` | `[[vk::push_constant]] cbuffer Push { … }` |
| `layout(local_size_x=4, local_size_y=4, local_size_z=4) in;` | `[numthreads(4, 4, 4)]` |
| `void main()` | `void cs_main(uint3 gid : SV_DispatchThreadID)` |

The math is **line-for-line identical** to the GLSL emitter — same
Cephes-poly port, same `crd_sign_bit` mask discipline, same
`kk = k * 4.0` / `kk = k * 6.0` C++-parity fixes, same SSA walker
structure.

```cpp
[[nodiscard]] StringView hlsl_helpers_prelude() noexcept;
[[nodiscard]] String     emit_hlsl_sdf_function(const FormulaIr&, StringView, IAllocator*);
[[nodiscard]] String     emit_hlsl_conformance_shader(const FormulaIr&, IAllocator*);
```

### Why HLSL too?

D3D12 / DirectX renderers consume HLSL natively. Vulkan-only engines
(Cerid today) consume GLSL. A multi-backend renderer wants both, and
the cooker (v9e-d) emits both from the same IR. When Cerid gains a
D3D12 backend (Phase 3.5+), the cooked HLSL files drop in directly —
no shader re-authoring step. HLSL 6.0 is the floor (D172).

### Full GPU verification via `compile_hlsl`

The `v9e-c-dxc-spirv-dispatch` follow-on (shipped same day as v9e-c,
before v9e-d) added a new sibling to `compile_glsl` in `crd-shader`:

```cpp
namespace crd::shader {
[[nodiscard]] CompileResult compile_hlsl(
    Stage                       stage,
    crd::containers::StringView source,
    crd::containers::StringView name,
    crd::memory::IAllocator*    a = crd::memory::default_allocator());
}
```

- Dynamically loads `dxcompiler.dll` from `VULKAN_SDK/Bin` (Vulkan SDK
  ships dxc next to shaderc).
- COM-style API (`IDxcCompiler3` / `IDxcUtils` / `IDxcResult` /
  `IDxcBlob`) with explicit `QueryInterface` / `Release` refcount
  management.
- Args fixed to `-T cs_6_0 -E cs_main -spirv -fspv-target-env=vulkan1.3`
  for compute. Vertex/Fragment use `-T vs_6_0 / ps_6_0` + entry `main`.

`test_hlsl_conformance.cpp` mirrors the GLSL conformance test path
exactly with the emit/compile pair swapped: 21 manifests × 32³
samples = 688 128 per-pixel comparisons, all pass.

### dxcapi.h Windows COM dependency

`dxcapi.h` expects the consumer to have pulled in the Windows COM
types (`UINT32`, `LPCWSTR`, `IUnknown` with its `AddRef`/`Release`/
`QueryInterface` slots) before the `#include`. On a vanilla TU it
errors out with undeclared identifiers. Fix in
`engine/shader/src/compile_hlsl.cpp`:

```cpp
#if CRD_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // UINT32, LPCWSTR, …
#include <unknwn.h>    // IUnknown full definition
#endif
#include <dxc/dxcapi.h>
```

## v9e-d — cooker

```cpp
struct CookResult {
    bool                ok = false;
    String              error_message;
    Array<String>       emitted_paths;

    explicit CookResult(IAllocator* a) noexcept;
};

[[nodiscard]] CookResult cook_helpers_prelude(
    StringView output_dir, IAllocator* alloc) noexcept;

[[nodiscard]] CookResult cook_ir(
    const FormulaIr& ir, StringView name,
    StringView output_dir, IAllocator* alloc) noexcept;
```

### Output layout

```
<output_dir>/
  sdf_helpers.glsl       ← shared GLSL prelude (every sd_* + smin/op/domain)
  sdf_helpers.hlsl       ← shared HLSL prelude
  <name>.glsl            ← `float <name>(vec3 p)`   — emitted SDF function
  <name>.hlsl            ← `float <name>(float3 p)` — same in HLSL
```

### Library, not CMake-target (D177)

The original §26-planned "CMake-target vs runtime-cooker"
decision-fork resolved toward **library-first**. A library composes
into:

- Build-time CMake targets (when `crd_cook_sdf_manifest()` ships as
  `v9e-d-cmake`).
- Editor-time tools (a designer authoring a scene in the editor calls
  the cooker on save).
- Test fixtures (the cooker's own conformance test cooks then
  re-validates the on-disk text).
- One-shot CLI cookers.

A CMake-target-only design forecloses three of those.

### Per-SDF file contains ONLY the function (D178)

The prelude is the heavy text (~5 KB; every `sd_*` + every smin/op/
domain helper + the Cephes-poly port). Per-SDF functions are
100-300 LOC. If every cooked SDF embedded its own prelude, a library
of 50 cooked SDFs would carry the prelude 50 times. Instead:

- Prelude written once per output dir (`sdf_helpers.{glsl,hlsl}`).
- Each per-SDF file contains ONLY `float <name>(vec3/float3 p)`.
- Consumers `#include "sdf_helpers.glsl"` alongside the per-SDF file.

Mirrors Inigo Quilez `iqlibs` / iquilezles.org reference pattern.

### Idempotent overwrite (D179)

`fs::write_file_text` always overwrites. Build-time cookers re-running
after a source change must produce new content — skip-if-exists would
silently mask source updates.

### Failure model (D180)

`CookResult{ok, error_message, emitted_paths}` reports the first
failure path + message and short-circuits. Multi-error reporting adds
complexity for no benefit; if a write fails, the filesystem is the
problem and all subsequent writes will fail too. Pre-validates IR via
`validate(ir)` so a bad input fails as a cooker error (not an
assertion).

## Consumer wire-up (D181)

The first consumer (renderer DFAO sampler, Phase 3.5+) wires via
**cooked-file consumption**:

1. Designer / build-time tool authors an `IrBuilder` chain in C++
   (or future TOML manifest).
2. `cook_helpers_prelude(out, alloc)` writes `sdf_helpers.{glsl,hlsl}`
   to the output dir.
3. `cook_ir(ir, "my_sdf", out, alloc)` writes `my_sdf.{glsl,hlsl}`.
4. Renderer reads `sdf_helpers.glsl + my_sdf.glsl` from disk at
   load-time.
5. `crd::shader::compile_glsl(...)` → SPIR-V → Vulkan dispatch.

**No direct C++ runtime dependency on `crd-geometry-shader-helpers`
in the consumer.** The cooker is a cooker, not a hot-path library.
This means a shipping game with no editor tooling carries zero
shader-helpers code at runtime — only the cooked text files in the
asset bundle.

## Test layout

| File | Coverage |
|---|---|
| `test_formula_ir.cpp` | Builder API + `validate` edge cases + per-manifest well-formedness + `evaluate<float>` ground-truth at sample points |
| `test_glsl_emit.cpp` | GLSL emitter shape + structural invariants + 21-manifest × 32³-sample GPU conformance dispatch via `compile_glsl` |
| `test_hlsl_emit.cpp` | HLSL emitter shape + structural parity with GLSL emitter (every manifest, same node count + same operator sequence + syntax swap only) |
| `test_hlsl_conformance.cpp` | Full HLSL GPU verification: 21-manifest × 32³-sample GPU conformance dispatch via `compile_hlsl` (dxc) |
| `test_cooker.cpp` | Prelude byte-identity + 21-manifest cooker cross-check + empty-name error + invalid-IR error + idempotent re-cook byte-identity |

Module total: **21 test cases / 910 assertions PASS** across 4-config
DoD (debug + asan + shipping + tidy).

## Filed follow-ons (consumer-pull)

- **v9e-d-toml** — TOML manifest format + parser so designers can
  author SDFs as text files instead of C++ code. Ships when a
  designer-driven consumer arrives (likely Phase 3.5+ editor).
- **v9e-d-cmake** — CMake `crd_cook_sdf_manifest()` helper that
  registers a manifest as a build-time dependency + runs the cooker.
  Ships when the renderer DFAO pipeline lands.
- **v9e-d-crdr-pack** — pack cooked files into a CRDR asset bundle
  for runtime load. Ships when the resources loader needs it.
- **v9e-glsl-versions** — `#version 460` support + SPIR-V 1.6 target
  if a future consumer needs newer features (subgroup ops, bindless).
- **v9e-d3d12-native** — direct D3D12 backend HLSL consumption (no
  dxc → SPIR-V → Vulkan detour). Ships when Cerid gains a D3D12
  backend. Current cooked HLSL drops in directly — D172 already pins
  HLSL 6.0.
- **GPU-side IR interpreter** — flat 3-array storage (D167)
  transfers directly to SSBO + GLSL walk function. Useful when a
  consumer needs runtime SDF specialisation without re-cooking.

## Cross-references

- ADR-0076 §26 — cluster decisions D166-D181.
- `docs/sessions/2026-05-18-geometry-v9e-a-formula-ir.md` — v9e-a session log.
- `docs/sessions/2026-05-18-geometry-v9e-b-glsl-emitter.md` — v9e-b session log.
- `docs/sessions/2026-05-19-geometry-v9e-c-hlsl-emitter-and-dxc-spirv-dispatch.md` — v9e-c + follow-on log.
- `docs/sessions/2026-05-19-geometry-v9e-d-cooker.md` — v9e-d session log.
- `docs/systems/geometry-primitives.md` — `signed_distance.hpp` + `formulary.hpp` (the C++ side of the library this module mirrors to GPU).
- The compute substrate the conformance test path consumes was `crd-rhi-compute` (ADR-0080) at ship time; since
  RET-7 (2026-07-23, ADR-0105) the dispatches run on gpu-context's `IComputeContext`.
