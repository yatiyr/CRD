# CKIR snapshots and bounded compute emission

<!-- doc-role: reference -->
> Technique reference. Live owner: [REPO.3c.6](../ROADMAP.md#slice-repo.3c.6).

## Parameters and limits

| Input | Meaning / units | Default | Contract used here |
|---|---|---|---|
| `AbufferConfig.width`, `height` | Image dimensions, pixels | 32 each | Positive dimensions; allocation/index products must fit the host/provider contract |
| `layers` | Captured fragments per pixel | 4 | This fixed resolve supports 1–8; source regression exercises 1, 4, 8; device oracle uses 4 |
| `local_size` | Threads per workgroup | 64 | Match entry and dispatch dimensions and the provider limit |
| `bg[3]` | Linear RGB background, dimensionless | 0.10, 0.10, 0.12 | Tests copy their scene background into the configuration |
| `samples` | Stochastic sub-samples | 256 | Unused by the atomic resolve; belongs to the separate stochastic tier |
| Empty link | No fragment/end-of-list identifier | `0xffffffff` | Initialize every head before capture; never use the sentinel as a pool index |
| Pool layout | Five f32 values per node | r, g, b, alpha, depth | Head/next are u32; resolve has four bindings, not eight separate channel buffers |
| `Materialize` | Evaluate a value at this statement | Explicit IR operation | Preserve statement order and lexical scope; do not replace every live load with a snapshot |
| Source bound | Regression budget, bytes per graph node | 128 | Each of five emitted sources must be smaller than `graph.size() * 128` for each sampled layer count |
| Exact arithmetic | f32 composite operation sequence | Existing deterministic path | Preserve mul/add/sub order and disabled contraction where required; no tolerance expansion |

These bounds describe this existing test/bootstrap workload, not a general unbounded production transparency API.
The authored-asset requirement remains unchanged: a shipping algorithm must be editable canonical IR on disk.

## What failed and why

A graph can share subexpressions while an emitted expression tree duplicates them. Linked-list gathering feeds a
compare-exchange sorting network. Inlining both sides of every select recursively expands the shared predecessors.
Four layers produced a 413,859-byte HLSL resolve with 8,966 `.Load` occurrences and a 33,010-character longest line.
The local WARP test overflowed its stack during resolve pipeline creation, before dispatch. Stage instrumentation
distinguished this from a queue, synchronization or readback failure; the static A-buffer baseline passed separately.

An explicit snapshot gives the shared value one evaluated name. This is an IR semantic choice, not a backend-specific
shader-string rewrite. It must also survive declaration placement: hoisting a consumer before its snapshot recreates
the expanded expression or reads before a producer. GLSL/HLSL already had dependency analysis; CUDA/MSL/WGSL now use
that same analysis. Local Boolean values also need Boolean types rather than their physical buffer representation.
For example, [WGSL's specification](https://www.w3.org/TR/2026/CRD-WGSL-20260817/) defines function-local immutable
`let` values and Boolean typing; an emitter must respect both scope and type independently of buffer layout.

## Composite and assembly

The existing front-to-back composite starts with `C = 0`, `T = 1`. In ascending depth order, for each fragment:
`C = C + (T * alpha) * rgb`, then `T = T * (1 - alpha)`. Finish with `C + T * background`.
The repair preserves this expression order, depth comparison, payload layout and capture/resolve synchronization.

1. Capture allocates/appends pool nodes through the existing value-returning atomics. Complete capture before resolve.
2. Inside the pixel-range guard, materialize the initial head and every next-link result. Clamp a speculative pool
   load to valid storage while retaining the separate sentinel/validity selection.
3. Materialize each gathered r/g/b/alpha/depth value. Empty entries contribute alpha zero and sort behind real entries.
4. For each compare-exchange, materialize the Boolean comparison and both selected results for every channel.
   Later comparisons use these names, not recursively rebuilt prior comparisons.
5. Emit the unchanged composite from these values and write interleaved RGB.
6. The shared `KernelEmissionOrder` marks explicit snapshots, value-returning atomic/ray statement results and written
   resources. Memoized dependency traversal includes ordinary and extended operands. Hoist only independent values;
   defer dependent declarations to ordered statement emission and suppress duplicate declarations.
7. A materialized Boolean is a `bool` local (GLSL uses the full value type). Buffer element representations remain
   separate. Merely containing emitted text is not a successful compile or runtime result.

## Traps and qualification

- Do not solve expression growth by freezing all loads: a live load after a store/barrier has different semantics.
- A small graph can emit an enormous source tree. Keep a source-growth regression before invoking a driver compiler;
  fail immediately on the first violated bound rather than continuing into a larger allocation explosion.
- A compiler failure followed by a null pipeline crash is not evidence of a GPU dispatch defect. Check compilation,
  pipeline creation, dispatch, completion and readback independently.
- Adapter classification can explain which existing oracle applies; it cannot explain an unrelated pipeline crash.
  [Controlled WARP selection](2026-09-12-dx12-adapter-classification.md#controlled-warp-workload) must preserve user settings.
- Emission on all five languages is not Metal/WebGPU device qualification. The available CUDA/Vulkan/DX12 paths have
  actual exact-output tests; unavailable tuples retain their roadmap gates. One exact-output run is not a repeated
  determinism or performance qualification.

After explicit snapshots, the diagnostic four-layer HLSL was 7,859 bytes with 24 loads. These are compiler-artifact
counts, not timing/throughput claims; no performance benchmark is claimed. The
[session](../sessions/2026-09-12-atomic-abuffer-emitter-repair.md) records intermediate failures, final source/device
tests, provider versions and immutable local evidence envelopes.

## Source and reproductions

- [Canonical builder](../../engine/gpu/kir/include/crd/kir/ckir_oit.hpp) and
  [shared order analysis](../../engine/gpu/kir/include/crd/kir/ckir_kernel_order.hpp).
- Compute emitters beside that helper: `ckir_glsl.hpp`, `ckir_hlsl.hpp`, `ckir_cuda.hpp`, `ckir_msl.hpp`, `ckir_wgsl.hpp`.
- [Source-size regression](../../tests/gpu/kir/test_ckir_kernel_emit.cpp),
  [CUDA runtime regression](../../tests/gpu/gpu-context-cuda/test_cuda_compute.cpp) and
  [shared capture/resolve/oracle](../../tests/gpu/gpu-shared/ckir_abuffer_test.hpp).
- Rebuild affected test executables via [BUILDING](../BUILDING.md), then use scoped CTest regex
  `^Atomic A-buffer resolve|^CUDA atomic A-buffer|^D-007 B17-c: scalable atomic linked-list A-buffer` with timeout 180.
  Reconcile selected/executed names, inspect validation/compiler output and retain the exact source/build identity.
