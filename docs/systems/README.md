# Systems and source map

<!-- doc-role: navigation -->
> Navigation; no independent live queue. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

This index maps the current source tree. **Slice status lives only in [ROADMAP](../ROADMAP.md#master-table).**
An existing module, overview or test directory is not a blanket production-quality claim. The
[system audit](../research/2026-09-12-system-audit.md) records the 2026-09-12 dependency/coverage census.
The [expanded review](../research/2026-09-12-cerid-whole-system-review.md) classifies the whole system and routes
additional qualification gaps to the same master table; its [census](../research/2026-09-12-whole-system-census.json)
records all manifests and sampled source identity without claiming tests ran.

Start with [rendering foundation](rendering-foundation.md), [CEIR](ceir.md), [CHIR](chir.md) and
[CKIR stages](shader-ir-corpus-and-stages.md) for the executable-asset system. The
[execution contract](../design/renderer-ui-execution-contract.md) describes the planned UI/editor boundaries.

## Current module inventory

All 102 registered modules are represented: the 96 engine modules below and the six application and tool
modules in the next table. Existing overview links retain their dated evidence; where no dedicated overview
exists, the public source and CMake manifest are the direct entry points. The documentation validator holds
this map and the `crd_module()` registry in the root CMakeLists to the same set of names.

| Module | Overview / source | Build and dependency contract |
|---|---|---|
| `anim` | [Public source](../../engine/world/anim/include/) | [CMake](../../engine/world/anim/CMakeLists.txt) |
| `app` | [app](app.md) | [CMake](../../engine/foundation/app/CMakeLists.txt) |
| `asset-io` | [Public source](../../engine/assets/asset-io/include/) | [CMake](../../engine/assets/asset-io/CMakeLists.txt) |
| `audio` | [Public source](../../engine/media/audio/include/) | [CMake](../../engine/media/audio/CMakeLists.txt) |
| `ceir` | [ceir](ceir.md) | [CMake](../../engine/execution/ceir/CMakeLists.txt) |
| `ceir-cook` | [Public source](../../engine/execution/ceir-cook/include/) | [CMake](../../engine/execution/ceir-cook/CMakeLists.txt) |
| `ceir-gpu` | [Public source](../../engine/execution/ceir-gpu/include/) | [CMake](../../engine/execution/ceir-gpu/CMakeLists.txt) |
| `ceir-host` | [Public source](../../engine/execution/ceir-host/include/) | [CMake](../../engine/execution/ceir-host/CMakeLists.txt) |
| `chir` | [chir](chir.md) | [CMake](../../engine/execution/chir/CMakeLists.txt) |
| `config` | [config](config.md) | [CMake](../../engine/foundation/config/CMakeLists.txt) |
| `containers` | [containers](containers.md) | [CMake](../../engine/foundation/containers/CMakeLists.txt) |
| `core` | [core](core.md) | [CMake](../../engine/foundation/core/CMakeLists.txt) |
| `draw` | [Public source](../../engine/rendering/draw/include/) | [CMake](../../engine/rendering/draw/CMakeLists.txt) |
| `draw-imgui` | [Public source](../../engine/rendering/draw-imgui/include/) | [CMake](../../engine/rendering/draw-imgui/CMakeLists.txt) |
| `eylem` | [Public source](../../engine/physics/eylem/include/) | [CMake](../../engine/physics/eylem/CMakeLists.txt) |
| `eylem-rigid3d` | [Public source](../../engine/physics/eylem-rigid3d/include/) | [CMake](../../engine/physics/eylem-rigid3d/CMakeLists.txt) |
| `eylem-viz` | [Public source](../../engine/physics/eylem-viz/include/) | [CMake](../../engine/physics/eylem-viz/CMakeLists.txt) |
| `frame-cook` | [Public source](../../engine/assets/frame-cook/include/) | [CMake](../../engine/assets/frame-cook/CMakeLists.txt) |
| `geometry-bvh` | [geometry-bvh](geometry-bvh.md) | [CMake](../../engine/geometry/geometry-bvh/CMakeLists.txt) |
| `geometry-bvh-gpu` | [geometry-bvh-gpu](geometry-bvh-gpu.md) | [CMake](../../engine/geometry/geometry-bvh-gpu/CMakeLists.txt) |
| `geometry-convex` | [geometry-convex](geometry-convex.md) | [CMake](../../engine/geometry/geometry-convex/CMakeLists.txt) |
| `geometry-curves` | [geometry-curves](geometry-curves.md) | [CMake](../../engine/geometry/geometry-curves/CMakeLists.txt) |
| `geometry-decomposition` | [geometry-decomposition](geometry-decomposition.md) | [CMake](../../engine/geometry/geometry-decomposition/CMakeLists.txt) |
| `geometry-delaunay` | [geometry-delaunay](geometry-delaunay.md) | [CMake](../../engine/geometry/geometry-delaunay/CMakeLists.txt) |
| `geometry-mesh` | [geometry-mesh](geometry-mesh.md) | [CMake](../../engine/geometry/geometry-mesh/CMakeLists.txt) |
| `geometry-mesh-processing` | [geometry-mesh-processing](geometry-mesh-processing.md) | [CMake](../../engine/geometry/geometry-mesh-processing/CMakeLists.txt) |
| `geometry-polygon` | [geometry-polygon](geometry-polygon.md) | [CMake](../../engine/geometry/geometry-polygon/CMakeLists.txt) |
| `geometry-primitives` | [geometry-primitives](geometry-primitives.md) | [CMake](../../engine/geometry/geometry-primitives/CMakeLists.txt) |
| `geometry-shader-helpers` | [geometry-shader-helpers](geometry-shader-helpers.md) | [CMake](../../engine/geometry/geometry-shader-helpers/CMakeLists.txt) |
| `geometry-spatial` | [geometry-spatial](geometry-spatial.md) | [CMake](../../engine/geometry/geometry-spatial/CMakeLists.txt) |
| `geometry-viz` | [geometry-viz](geometry-viz.md) | [CMake](../../engine/geometry/geometry-viz/CMakeLists.txt) |
| `gpu-context` | [Public source](../../engine/gpu/gpu-context/include/) | [CMake](../../engine/gpu/gpu-context/CMakeLists.txt) |
| `gpu-context-cuda` | [Public source](../../engine/gpu/gpu-context-cuda/include/) | [CMake](../../engine/gpu/gpu-context-cuda/CMakeLists.txt) |
| `gpu-context-dx12` | [Public source](../../engine/gpu/gpu-context-dx12/include/) | [CMake](../../engine/gpu/gpu-context-dx12/CMakeLists.txt) |
| `gpu-context-vulkan` | [Public source](../../engine/gpu/gpu-context-vulkan/include/) | [CMake](../../engine/gpu/gpu-context-vulkan/CMakeLists.txt) |
| `hesap` | [Public source](../../engine/numerics/hesap/include/) | [CMake](../../engine/numerics/hesap/CMakeLists.txt) |
| `hesap-amg` | [Public source](../../engine/numerics/hesap-amg/include/) | [CMake](../../engine/numerics/hesap-amg/CMakeLists.txt) |
| `hesap-autodiff` | [hesap-autodiff](hesap-autodiff.md) | [CMake](../../engine/numerics/hesap-autodiff/CMakeLists.txt) |
| `hesap-comms` | [hesap-comms](hesap-comms.md) | [CMake](../../engine/numerics/hesap-comms/CMakeLists.txt) |
| `hesap-dense` | [hesap-dense](hesap-dense.md) | [CMake](../../engine/numerics/hesap-dense/CMakeLists.txt) |
| `hesap-diff` | [hesap-diff](hesap-diff.md) | [CMake](../../engine/numerics/hesap-diff/CMakeLists.txt) |
| `hesap-direct` | [Public source](../../engine/numerics/hesap-direct/include/) | [CMake](../../engine/numerics/hesap-direct/CMakeLists.txt) |
| `hesap-dsp` | [hesap-dsp](hesap-dsp.md) | [CMake](../../engine/numerics/hesap-dsp/CMakeLists.txt) |
| `hesap-eigen` | [hesap-eigen](hesap-eigen.md) | [CMake](../../engine/numerics/hesap-eigen/CMakeLists.txt) |
| `hesap-fft` | [hesap-fft](hesap-fft.md) | [CMake](../../engine/numerics/hesap-fft/CMakeLists.txt) |
| `hesap-interp` | [hesap-interp](hesap-interp.md) | [CMake](../../engine/numerics/hesap-interp/CMakeLists.txt) |
| `hesap-iterative` | [Public source](../../engine/numerics/hesap-iterative/include/) | [CMake](../../engine/numerics/hesap-iterative/CMakeLists.txt) |
| `hesap-motion` | [hesap-motion](hesap-motion.md) | [CMake](../../engine/numerics/hesap-motion/CMakeLists.txt) |
| `hesap-ode` | [hesap-ode](hesap-ode.md) | [CMake](../../engine/numerics/hesap-ode/CMakeLists.txt) |
| `hesap-opt` | [hesap-opt](hesap-opt.md) | [CMake](../../engine/numerics/hesap-opt/CMakeLists.txt) |
| `hesap-ordering` | [hesap-ordering](hesap-ordering.md) | [CMake](../../engine/numerics/hesap-ordering/CMakeLists.txt) |
| `hesap-preconditioners` | [Public source](../../engine/numerics/hesap-preconditioners/include/) | [CMake](../../engine/numerics/hesap-preconditioners/CMakeLists.txt) |
| `hesap-quadrature` | [hesap-quadrature](hesap-quadrature.md) | [CMake](../../engine/numerics/hesap-quadrature/CMakeLists.txt) |
| `hesap-resources` | [hesap-resources](hesap-resources.md) | [CMake](../../engine/numerics/hesap-resources/CMakeLists.txt) |
| `hesap-sched` | [Public source](../../engine/numerics/hesap-sched/include/) | [CMake](../../engine/numerics/hesap-sched/CMakeLists.txt) |
| `hesap-sparse` | [hesap-sparse](hesap-sparse.md) | [CMake](../../engine/numerics/hesap-sparse/CMakeLists.txt) |
| `hesap-special` | [hesap-special](hesap-special.md) | [CMake](../../engine/numerics/hesap-special/CMakeLists.txt) |
| `hesap-stats` | [hesap-stats](hesap-stats.md) | [CMake](../../engine/numerics/hesap-stats/CMakeLists.txt) |
| `hesap-tensor` | [hesap-tensor](hesap-tensor.md) | [CMake](../../engine/numerics/hesap-tensor/CMakeLists.txt) |
| `hesap-wavelet` | [hesap-wavelet](hesap-wavelet.md) | [CMake](../../engine/numerics/hesap-wavelet/CMakeLists.txt) |
| `imgui` | [imgui](imgui.md) | [CMake](../../engine/ui/imgui/CMakeLists.txt) |
| `jobs` | [jobs](jobs.md) | [CMake](../../engine/foundation/jobs/CMakeLists.txt) |
| `kir` | [Public source](../../engine/gpu/kir/include/) | [CMake](../../engine/gpu/kir/CMakeLists.txt) |
| `kir-cuda` | [Public source](../../engine/gpu/kir-cuda/include/) | [CMake](../../engine/gpu/kir-cuda/CMakeLists.txt) |
| `kir-dx12` | [Public source](../../engine/gpu/kir-dx12/include/) | [CMake](../../engine/gpu/kir-dx12/CMakeLists.txt) |
| `kir-hip` | [Public source](../../engine/gpu/kir-hip/include/) | [CMake](../../engine/gpu/kir-hip/CMakeLists.txt) |
| `kir-metal` | [Public source](../../engine/gpu/kir-metal/include/) | [CMake](../../engine/gpu/kir-metal/CMakeLists.txt) |
| `kir-vulkan` | [Public source](../../engine/gpu/kir-vulkan/include/) | [CMake](../../engine/gpu/kir-vulkan/CMakeLists.txt) |
| `kir-webgpu` | [Public source](../../engine/gpu/kir-webgpu/include/) | [CMake](../../engine/gpu/kir-webgpu/CMakeLists.txt) |
| `light-cook` | [Public source](../../engine/assets/light-cook/include/) | [CMake](../../engine/assets/light-cook/CMakeLists.txt) |
| `lod` | [Public source](../../engine/geometry/lod/include/) | [CMake](../../engine/geometry/lod/CMakeLists.txt) |
| `log` | [log](log.md) | [CMake](../../engine/foundation/log/CMakeLists.txt) |
| `material-cook` | [Public source](../../engine/assets/material-cook/include/) | [CMake](../../engine/assets/material-cook/CMakeLists.txt) |
| `math` | [math](math.md) | [CMake](../../engine/foundation/math/CMakeLists.txt) |
| `memory` | [memory](memory.md) | [CMake](../../engine/foundation/memory/CMakeLists.txt) |
| `meshgen` | [meshgen](meshgen.md) | [CMake](../../engine/geometry/meshgen/CMakeLists.txt) |
| `perf` | [perf](perf.md) | [CMake](../../engine/foundation/perf/CMakeLists.txt) |
| `perf-ui` | [Public source](../../engine/ui/perf-ui/include/) | [CMake](../../engine/ui/perf-ui/CMakeLists.txt) |
| `platform` | [platform](platform.md) | [CMake](../../engine/foundation/platform/CMakeLists.txt) |
| `preset` | [Public source](../../engine/assets/preset/include/) | [CMake](../../engine/assets/preset/CMakeLists.txt) |
| `profile` | [Public source](../../engine/foundation/profile/include/) | [CMake](../../engine/foundation/profile/CMakeLists.txt) |
| `render-asset-core` | [Public source](../../engine/rendering/render-asset-core/include/) | [CMake](../../engine/rendering/render-asset-core/CMakeLists.txt) |
| `render-graph` | [Public source](../../engine/rendering/render-graph/include/) | [CMake](../../engine/rendering/render-graph/CMakeLists.txt) |
| `render-material` | [Public source](../../engine/rendering/render-material/include/) | [CMake](../../engine/rendering/render-material/CMakeLists.txt) |
| `render-pass` | [Public source](../../engine/rendering/render-pass/include/) | [CMake](../../engine/rendering/render-pass/CMakeLists.txt) |
| `render-program` | [Public source](../../engine/rendering/render-program/include/) | [CMake](../../engine/rendering/render-program/CMakeLists.txt) |
| `resources` | [resources](resources.md) | [CMake](../../engine/assets/resources/CMakeLists.txt) |
| `scene` | [scene](scene.md) | [CMake](../../engine/world/scene/CMakeLists.txt) |
| `scene-render` | [Public source](../../engine/rendering/scene-render/include/) | [CMake](../../engine/rendering/scene-render/CMakeLists.txt) |
| `shader-cook` | [Public source](../../engine/assets/shader-cook/include/) | [CMake](../../engine/assets/shader-cook/CMakeLists.txt) |
| `technique-cook` | [Public source](../../engine/assets/technique-cook/include/) | [CMake](../../engine/assets/technique-cook/CMakeLists.txt) |
| `time` | [Public source](../../engine/foundation/time/include/) | [CMake](../../engine/foundation/time/CMakeLists.txt) |
| `timeline` | [Public source](../../engine/world/timeline/include/) | [CMake](../../engine/world/timeline/CMakeLists.txt) |
| `toml` | [Public source](../../engine/foundation/toml/include/) | [CMake](../../engine/foundation/toml/CMakeLists.txt) |
| `units` | [units](units.md) | [CMake](../../engine/foundation/units/CMakeLists.txt) |
| `vertex-cook` | [Public source](../../engine/assets/vertex-cook/include/) | [CMake](../../engine/assets/vertex-cook/CMakeLists.txt) |
| `vm` | [vm](vm.md) | [CMake](../../engine/foundation/vm/CMakeLists.txt) |

## Registered applications and tools

The registry owns these non-engine modules under the same contract (their names are the `NAME` arguments of
their rows where the directory name is not a plain identifier). Their review route is the same: the owning
ROADMAP row, then the single maintainer.

| Module | Overview / source | Build and dependency contract |
|---|---|---|
| `asset_cooker` | [Source](../../tools/asset_cooker/) | [CMake](../../tools/asset_cooker/CMakeLists.txt) |
| `ceridc` | [Source](../../tools/ceridc/) | [CMake](../../tools/ceridc/CMakeLists.txt) |
| `kir_autotune` | [Source](../../tools/kir-autotune/) | [CMake](../../tools/kir-autotune/CMakeLists.txt) |
| `shader_cook` | [Source](../../tools/shader-cook/) | [CMake](../../tools/shader-cook/CMakeLists.txt) |
| `runtime` | [Source](../../runtime/) | [CMake](../../runtime/CMakeLists.txt) |
| `sandbox` | [Source](../../sandbox/) | [CMake](../../sandbox/CMakeLists.txt) |

## Planned product modules

`crd-ui`, Canvas, owned font/shaping, vector rendering and reflection are not existing module directories at this
baseline. Their final boundaries are specified by ADR-0107 and the execution contract; implementation closes the
I2D/REFLECT rows. CR-D007 is an application assembled from public modules, and the notebook is another consumer.

## Retired graphics stack

`rhi`, `rhi-vulkan`, `rhi-compute`, `renderer` and `shader` were retired under
[ADR-0105](../decisions/0105-retire-rhi-renderer-gpu-context-is-the-graphics-layer.md).
Their successor is gpu-context plus CEIR/CKIR and the authored render-asset stack. Historical source mentions in
session records do not authorize rebuilding the old interfaces. Geometry/eylem/numerical implementation is outside
the current audit; use the master table and existing phase contracts when those programmes resume.
