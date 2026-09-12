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

All 96 engine module CMake files are represented below. Existing overview links retain their dated evidence;
where no dedicated overview exists, the public source and CMake manifest are the direct entry points.

| Module | Overview / source | Build and dependency contract |
|---|---|---|
| `anim` | [Public source](../../engine/anim/include/) | [CMake](../../engine/anim/CMakeLists.txt) |
| `app` | [app](app.md) | [CMake](../../engine/app/CMakeLists.txt) |
| `asset-io` | [Public source](../../engine/asset-io/include/) | [CMake](../../engine/asset-io/CMakeLists.txt) |
| `audio` | [Public source](../../engine/audio/include/) | [CMake](../../engine/audio/CMakeLists.txt) |
| `ceir` | [ceir](ceir.md) | [CMake](../../engine/ceir/CMakeLists.txt) |
| `ceir-cook` | [Public source](../../engine/ceir-cook/include/) | [CMake](../../engine/ceir-cook/CMakeLists.txt) |
| `ceir-gpu` | [Public source](../../engine/ceir-gpu/include/) | [CMake](../../engine/ceir-gpu/CMakeLists.txt) |
| `ceir-host` | [Public source](../../engine/ceir-host/include/) | [CMake](../../engine/ceir-host/CMakeLists.txt) |
| `chir` | [chir](chir.md) | [CMake](../../engine/chir/CMakeLists.txt) |
| `config` | [config](config.md) | [CMake](../../engine/config/CMakeLists.txt) |
| `containers` | [containers](containers.md) | [CMake](../../engine/containers/CMakeLists.txt) |
| `core` | [core](core.md) | [CMake](../../engine/core/CMakeLists.txt) |
| `draw` | [Public source](../../engine/draw/include/) | [CMake](../../engine/draw/CMakeLists.txt) |
| `draw-imgui` | [Public source](../../engine/draw-imgui/include/) | [CMake](../../engine/draw-imgui/CMakeLists.txt) |
| `eylem` | [Public source](../../engine/eylem/include/) | [CMake](../../engine/eylem/CMakeLists.txt) |
| `eylem-rigid3d` | [Public source](../../engine/eylem-rigid3d/include/) | [CMake](../../engine/eylem-rigid3d/CMakeLists.txt) |
| `eylem-viz` | [Public source](../../engine/eylem-viz/include/) | [CMake](../../engine/eylem-viz/CMakeLists.txt) |
| `frame-cook` | [Public source](../../engine/frame-cook/include/) | [CMake](../../engine/frame-cook/CMakeLists.txt) |
| `geometry-bvh` | [geometry-bvh](geometry-bvh.md) | [CMake](../../engine/geometry-bvh/CMakeLists.txt) |
| `geometry-bvh-gpu` | [geometry-bvh-gpu](geometry-bvh-gpu.md) | [CMake](../../engine/geometry-bvh-gpu/CMakeLists.txt) |
| `geometry-convex` | [geometry-convex](geometry-convex.md) | [CMake](../../engine/geometry-convex/CMakeLists.txt) |
| `geometry-curves` | [geometry-curves](geometry-curves.md) | [CMake](../../engine/geometry-curves/CMakeLists.txt) |
| `geometry-decomposition` | [geometry-decomposition](geometry-decomposition.md) | [CMake](../../engine/geometry-decomposition/CMakeLists.txt) |
| `geometry-delaunay` | [geometry-delaunay](geometry-delaunay.md) | [CMake](../../engine/geometry-delaunay/CMakeLists.txt) |
| `geometry-mesh` | [geometry-mesh](geometry-mesh.md) | [CMake](../../engine/geometry-mesh/CMakeLists.txt) |
| `geometry-mesh-processing` | [geometry-mesh-processing](geometry-mesh-processing.md) | [CMake](../../engine/geometry-mesh-processing/CMakeLists.txt) |
| `geometry-polygon` | [geometry-polygon](geometry-polygon.md) | [CMake](../../engine/geometry-polygon/CMakeLists.txt) |
| `geometry-primitives` | [geometry-primitives](geometry-primitives.md) | [CMake](../../engine/geometry-primitives/CMakeLists.txt) |
| `geometry-shader-helpers` | [geometry-shader-helpers](geometry-shader-helpers.md) | [CMake](../../engine/geometry-shader-helpers/CMakeLists.txt) |
| `geometry-spatial` | [geometry-spatial](geometry-spatial.md) | [CMake](../../engine/geometry-spatial/CMakeLists.txt) |
| `geometry-viz` | [geometry-viz](geometry-viz.md) | [CMake](../../engine/geometry-viz/CMakeLists.txt) |
| `gpu-context` | [Public source](../../engine/gpu-context/include/) | [CMake](../../engine/gpu-context/CMakeLists.txt) |
| `gpu-context-cuda` | [Public source](../../engine/gpu-context-cuda/include/) | [CMake](../../engine/gpu-context-cuda/CMakeLists.txt) |
| `gpu-context-dx12` | [Public source](../../engine/gpu-context-dx12/include/) | [CMake](../../engine/gpu-context-dx12/CMakeLists.txt) |
| `gpu-context-vulkan` | [Public source](../../engine/gpu-context-vulkan/include/) | [CMake](../../engine/gpu-context-vulkan/CMakeLists.txt) |
| `hesap` | [Public source](../../engine/hesap/include/) | [CMake](../../engine/hesap/CMakeLists.txt) |
| `hesap-amg` | [Public source](../../engine/hesap-amg/include/) | [CMake](../../engine/hesap-amg/CMakeLists.txt) |
| `hesap-autodiff` | [hesap-autodiff](hesap-autodiff.md) | [CMake](../../engine/hesap-autodiff/CMakeLists.txt) |
| `hesap-comms` | [hesap-comms](hesap-comms.md) | [CMake](../../engine/hesap-comms/CMakeLists.txt) |
| `hesap-dense` | [hesap-dense](hesap-dense.md) | [CMake](../../engine/hesap-dense/CMakeLists.txt) |
| `hesap-diff` | [hesap-diff](hesap-diff.md) | [CMake](../../engine/hesap-diff/CMakeLists.txt) |
| `hesap-direct` | [Public source](../../engine/hesap-direct/include/) | [CMake](../../engine/hesap-direct/CMakeLists.txt) |
| `hesap-dsp` | [hesap-dsp](hesap-dsp.md) | [CMake](../../engine/hesap-dsp/CMakeLists.txt) |
| `hesap-eigen` | [hesap-eigen](hesap-eigen.md) | [CMake](../../engine/hesap-eigen/CMakeLists.txt) |
| `hesap-fft` | [hesap-fft](hesap-fft.md) | [CMake](../../engine/hesap-fft/CMakeLists.txt) |
| `hesap-interp` | [hesap-interp](hesap-interp.md) | [CMake](../../engine/hesap-interp/CMakeLists.txt) |
| `hesap-iterative` | [Public source](../../engine/hesap-iterative/include/) | [CMake](../../engine/hesap-iterative/CMakeLists.txt) |
| `hesap-motion` | [hesap-motion](hesap-motion.md) | [CMake](../../engine/hesap-motion/CMakeLists.txt) |
| `hesap-ode` | [hesap-ode](hesap-ode.md) | [CMake](../../engine/hesap-ode/CMakeLists.txt) |
| `hesap-opt` | [hesap-opt](hesap-opt.md) | [CMake](../../engine/hesap-opt/CMakeLists.txt) |
| `hesap-ordering` | [hesap-ordering](hesap-ordering.md) | [CMake](../../engine/hesap-ordering/CMakeLists.txt) |
| `hesap-preconditioners` | [Public source](../../engine/hesap-preconditioners/include/) | [CMake](../../engine/hesap-preconditioners/CMakeLists.txt) |
| `hesap-quadrature` | [hesap-quadrature](hesap-quadrature.md) | [CMake](../../engine/hesap-quadrature/CMakeLists.txt) |
| `hesap-resources` | [hesap-resources](hesap-resources.md) | [CMake](../../engine/hesap-resources/CMakeLists.txt) |
| `hesap-sched` | [Public source](../../engine/hesap-sched/include/) | [CMake](../../engine/hesap-sched/CMakeLists.txt) |
| `hesap-sparse` | [hesap-sparse](hesap-sparse.md) | [CMake](../../engine/hesap-sparse/CMakeLists.txt) |
| `hesap-special` | [hesap-special](hesap-special.md) | [CMake](../../engine/hesap-special/CMakeLists.txt) |
| `hesap-stats` | [hesap-stats](hesap-stats.md) | [CMake](../../engine/hesap-stats/CMakeLists.txt) |
| `hesap-tensor` | [hesap-tensor](hesap-tensor.md) | [CMake](../../engine/hesap-tensor/CMakeLists.txt) |
| `hesap-wavelet` | [hesap-wavelet](hesap-wavelet.md) | [CMake](../../engine/hesap-wavelet/CMakeLists.txt) |
| `imgui` | [imgui](imgui.md) | [CMake](../../engine/imgui/CMakeLists.txt) |
| `jobs` | [jobs](jobs.md) | [CMake](../../engine/jobs/CMakeLists.txt) |
| `kir` | [Public source](../../engine/kir/include/) | [CMake](../../engine/kir/CMakeLists.txt) |
| `kir-cuda` | [Public source](../../engine/kir-cuda/include/) | [CMake](../../engine/kir-cuda/CMakeLists.txt) |
| `kir-dx12` | [Public source](../../engine/kir-dx12/include/) | [CMake](../../engine/kir-dx12/CMakeLists.txt) |
| `kir-hip` | [Public source](../../engine/kir-hip/include/) | [CMake](../../engine/kir-hip/CMakeLists.txt) |
| `kir-metal` | [Public source](../../engine/kir-metal/include/) | [CMake](../../engine/kir-metal/CMakeLists.txt) |
| `kir-vulkan` | [Public source](../../engine/kir-vulkan/include/) | [CMake](../../engine/kir-vulkan/CMakeLists.txt) |
| `kir-webgpu` | [Public source](../../engine/kir-webgpu/include/) | [CMake](../../engine/kir-webgpu/CMakeLists.txt) |
| `light-cook` | [Public source](../../engine/light-cook/include/) | [CMake](../../engine/light-cook/CMakeLists.txt) |
| `lod` | [Public source](../../engine/lod/include/) | [CMake](../../engine/lod/CMakeLists.txt) |
| `log` | [log](log.md) | [CMake](../../engine/log/CMakeLists.txt) |
| `material-cook` | [Public source](../../engine/material-cook/include/) | [CMake](../../engine/material-cook/CMakeLists.txt) |
| `math` | [math](math.md) | [CMake](../../engine/math/CMakeLists.txt) |
| `memory` | [memory](memory.md) | [CMake](../../engine/memory/CMakeLists.txt) |
| `meshgen` | [meshgen](meshgen.md) | [CMake](../../engine/meshgen/CMakeLists.txt) |
| `perf` | [perf](perf.md) | [CMake](../../engine/perf/CMakeLists.txt) |
| `perf-ui` | [Public source](../../engine/perf-ui/include/) | [CMake](../../engine/perf-ui/CMakeLists.txt) |
| `platform` | [platform](platform.md) | [CMake](../../engine/platform/CMakeLists.txt) |
| `preset` | [Public source](../../engine/preset/include/) | [CMake](../../engine/preset/CMakeLists.txt) |
| `profile` | [Public source](../../engine/profile/include/) | [CMake](../../engine/profile/CMakeLists.txt) |
| `render-asset-core` | [Public source](../../engine/render-asset-core/include/) | [CMake](../../engine/render-asset-core/CMakeLists.txt) |
| `render-graph` | [Public source](../../engine/render-graph/include/) | [CMake](../../engine/render-graph/CMakeLists.txt) |
| `render-material` | [Public source](../../engine/render-material/include/) | [CMake](../../engine/render-material/CMakeLists.txt) |
| `render-pass` | [Public source](../../engine/render-pass/include/) | [CMake](../../engine/render-pass/CMakeLists.txt) |
| `render-program` | [Public source](../../engine/render-program/include/) | [CMake](../../engine/render-program/CMakeLists.txt) |
| `resources` | [resources](resources.md) | [CMake](../../engine/resources/CMakeLists.txt) |
| `scene` | [scene](scene.md) | [CMake](../../engine/scene/CMakeLists.txt) |
| `scene-render` | [Public source](../../engine/scene-render/include/) | [CMake](../../engine/scene-render/CMakeLists.txt) |
| `shader-cook` | [Public source](../../engine/shader-cook/include/) | [CMake](../../engine/shader-cook/CMakeLists.txt) |
| `technique-cook` | [Public source](../../engine/technique-cook/include/) | [CMake](../../engine/technique-cook/CMakeLists.txt) |
| `time` | [Public source](../../engine/time/include/) | [CMake](../../engine/time/CMakeLists.txt) |
| `timeline` | [Public source](../../engine/timeline/include/) | [CMake](../../engine/timeline/CMakeLists.txt) |
| `units` | [units](units.md) | [CMake](../../engine/units/CMakeLists.txt) |
| `vertex-cook` | [Public source](../../engine/vertex-cook/include/) | [CMake](../../engine/vertex-cook/CMakeLists.txt) |
| `vm` | [vm](vm.md) | [CMake](../../engine/vm/CMakeLists.txt) |

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
