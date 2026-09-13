include(CTest)
include(CrdTestDiscovery)
include(CrdWarp) # pinned app-local WARP staging (CRD_WARP_DLL), applied at the end of this file

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME crd-repository-hygiene
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-repository.py")
add_test(NAME crd-master-plan
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-master-plan.py")
add_test(NAME crd-repository-tools
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-repository-tools.py")
add_test(NAME crd-developer-workflow
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-dev-workflow.py")
add_test(NAME crd-project-structure-sync
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-project-sync.py")
add_test(NAME crd-build-profiles
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-native-build-profiles.py")
set_tests_properties(crd-repository-hygiene crd-master-plan crd-repository-tools
    PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-project-structure-sync PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-build-profiles PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-developer-workflow PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONUTF8=1")
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    add_test(NAME crd-project-structure-native
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-project-sync-native.py"
                --generator "${CMAKE_GENERATOR}")
    set_tests_properties(crd-project-structure-native PROPERTIES TIMEOUT 180 ENVIRONMENT "PYTHONUTF8=1")
    add_test(NAME crd-native-build-profiles
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-native-build-profiles.py"
                --generator "${CMAKE_GENERATOR}")
    set_tests_properties(crd-native-build-profiles PROPERTIES TIMEOUT 240 ENVIRONMENT "PYTHONUTF8=1")
endif()

# Header-only test utilities (ULP compare, GPU determinism check) — must
# come BEFORE any test target that links it.
add_subdirectory(support/test_helpers test_helpers)

add_subdirectory(foundation/core core)
add_subdirectory(foundation/vm vm)
add_subdirectory(foundation/units units)
add_subdirectory(execution/ceir ceir)  # D-007 CEIR-1a: Cerid Execution IR core (arena + intrusive def-use)
add_subdirectory(execution/chir chir)  # D-007 CEIR-32b: CHIR source model + CR-D007 graph-schema projection (ADR-0128)
add_subdirectory(execution/ceir-host ceir-host)  # D-007 CEIR-6b: the crd-jobs execution-provider bridge
add_subdirectory(execution/ceir-cook ceir-cook)  # D-007 CEIR-7a: the CEIR asset cook bridge
add_subdirectory(execution/ceir-gpu ceir-gpu)  # D-007 CEIR-13d: the GPU lowering bridge
add_subdirectory(rendering/render-asset-core render-asset-core)  # RAF-1 (D-007 "RAF band"): identity/namespaces/diagnostics/deps Gate 1
add_subdirectory(gpu/gpu-context gpu-context)        # RAF-2a (D-007 "RAF band"): canonical GPU command model (device-free structural gate)
add_subdirectory(rendering/render-program render-program)     # RAF-4 (D-007 "RAF band"): shader + program contract Gate 4
add_subdirectory(rendering/render-material render-material)    # RAF-5 (D-007 "RAF band"): material + technique contract Gate 5
add_subdirectory(rendering/render-pass render-pass)        # RAF-6 (D-007 "RAF band"): pass-executor registry Gate 6
add_subdirectory(rendering/render-graph render-graph)       # RAF-7 (D-007 "RAF band"): unified frame-graph runtime Gate 7
add_subdirectory(foundation/time time)
add_subdirectory(foundation/perf perf)
add_subdirectory(ui/perf-ui perf-ui)
add_subdirectory(foundation/log log)
add_subdirectory(foundation/memory memory)
add_subdirectory(foundation/math math)
add_subdirectory(geometry/geometry-primitives geometry-primitives)
add_subdirectory(geometry/geometry-bvh geometry-bvh)
add_subdirectory(geometry/geometry-convex geometry-convex)
add_subdirectory(geometry/geometry-mesh geometry-mesh)
add_subdirectory(geometry/geometry-spatial geometry-spatial)
add_subdirectory(geometry/geometry-polygon geometry-polygon)
add_subdirectory(geometry/geometry-mesh-processing geometry-mesh-processing)
add_subdirectory(geometry/lod lod) # REN-40-C1: the mesh LOD chain
add_subdirectory(geometry/geometry-delaunay geometry-delaunay)
add_subdirectory(geometry/geometry-decomposition geometry-decomposition)
add_subdirectory(geometry/geometry-bvh-gpu geometry-bvh-gpu)
add_subdirectory(assets/asset-io asset-io)
add_subdirectory(geometry/geometry-shader-helpers geometry-shader-helpers)
add_subdirectory(geometry/geometry-curves geometry-curves)
add_subdirectory(numerics/hesap hesap)
add_subdirectory(numerics/hesap-dense hesap-dense)
add_subdirectory(numerics/hesap-sparse hesap-sparse)
add_subdirectory(numerics/hesap-ordering hesap-ordering)
add_subdirectory(numerics/hesap-resources hesap-resources)
add_subdirectory(numerics/hesap-iterative hesap-iterative)
add_subdirectory(numerics/hesap-amg hesap-amg)
add_subdirectory(numerics/hesap-sched hesap-sched)
add_subdirectory(numerics/hesap-direct hesap-direct)
add_subdirectory(numerics/hesap-eigen hesap-eigen)
add_subdirectory(numerics/hesap-special hesap-special)
add_subdirectory(numerics/hesap-quadrature hesap-quadrature)
add_subdirectory(numerics/hesap-autodiff hesap-autodiff)
add_subdirectory(gpu/kir kir)
add_subdirectory(assets/frame-cook frame-cook)
add_subdirectory(assets/technique-cook technique-cook)  # REN-37 (D-007 row 140): the LIGHTING TECHNIQUE as an authored `.crdt` asset
add_subdirectory(assets/material-cook material-cook)   # REN-38-C (D-007 row 141): the MATERIAL as an authored `.crdm` asset
add_subdirectory(assets/vertex-cook vertex-cook)     # REN-38-D (D-007 row 141): the VERTEX PROGRAM as an authored `.crdv` asset
add_subdirectory(assets/light-cook light-cook)      # REN-38-E (D-007 row 141): the LIGHTING VOCABULARY as an authored asset
add_subdirectory(gpu/gpu-context-vulkan gpu-context-vulkan)  # ADR-0099 v17-i-a
add_subdirectory(gpu/gpu-context-dx12 gpu-context-dx12)    # ADR-0100 v17-i: D3D12 IComputeContext backend
add_subdirectory(gpu/gpu-context-cuda gpu-context-cuda)    # ADR-0100 (2026-08-07): CUDA IComputeContext backend; self-skips without CUDAToolkit
add_subdirectory(gpu/kir-vulkan kir-vulkan)
add_subdirectory(execution/ceir-gpu-vulkan ceir-gpu-vulkan)  # D-007 CEIR-13z-1b: a CEIR compute asset executed on a real Vulkan device (execute_lowered, ADR-0126)
add_subdirectory(gpu/kir-cuda kir-cuda)
add_subdirectory(execution/ceir-gpu-cuda ceir-gpu-cuda)    # D-007 CEIR-13z: the CUDA mirror — a CEIR compute asset on a real CUDA device (required backend)
add_subdirectory(gpu/kir-dx12 kir-dx12)
add_subdirectory(execution/ceir-gpu-dx12 ceir-gpu-dx12)  # D-007 CEIR-13z-1b: the DX12 mirror — a CEIR compute asset on a real D3D12 device (Windows-only)
add_subdirectory(gpu/kir-webgpu kir-webgpu)
add_subdirectory(gpu/kir-hip kir-hip)
add_subdirectory(gpu/kir-metal kir-metal)
add_subdirectory(numerics/hesap-opt hesap-opt)
add_subdirectory(numerics/hesap-stats hesap-stats)
add_subdirectory(numerics/hesap-ode hesap-ode)
add_subdirectory(numerics/hesap-fft hesap-fft)
add_subdirectory(numerics/hesap-dsp hesap-dsp)
add_subdirectory(numerics/hesap-wavelet hesap-wavelet)
add_subdirectory(numerics/hesap-comms hesap-comms)
add_subdirectory(numerics/hesap-interp hesap-interp)
add_subdirectory(numerics/hesap-diff hesap-diff)
add_subdirectory(numerics/hesap-motion hesap-motion)
add_subdirectory(numerics/hesap-tensor hesap-tensor)
add_subdirectory(foundation/containers containers)
add_subdirectory(foundation/platform platform)
add_subdirectory(foundation/app app)
add_subdirectory(foundation/config config)
add_subdirectory(ui/imgui imgui)
# RET-8 (ADR-0105): tests/{renderer,shader,rhi,rhi_vulkan} DELETED with their modules — the parity audit
# (D-007 rows 89-96) records every assertion's green gpu-context home.
add_subdirectory(foundation/jobs jobs)
add_subdirectory(foundation/stress stress)
add_subdirectory(assets/resources resources)
add_subdirectory(geometry/meshgen meshgen)
add_subdirectory(world/scene scene)
add_subdirectory(rendering/scene-render scene-render)  # GEO-7 (D-007 row 72): chunk-grain extract/cull/submit gates
add_subdirectory(world/anim anim)          # GEO-8 (D-007 row 73): SKEL/ANIM resources + THE FOX bit-stable resample gate
add_subdirectory(world/timeline timeline)      # GEO-9 (D-007 row 74): TIML resource + deterministic eval + THE EXR RENDER GATE
add_subdirectory(media/audio audio)         # GEO-10 (D-007 row 75): codecs bit-exact + graph render + MIDI + realtime
add_subdirectory(tools/ceridc ceridc)        # GEO-11 (D-007 row 76): THE AGENT GATE — the full scenario through MCP alone
add_subdirectory(assets/scene_cooker scene_cooker)
add_subdirectory(assets/cooker cooker)
add_subdirectory(assets/preset preset)
add_subdirectory(foundation/profile profile)
add_subdirectory(physics/eylem eylem)
add_subdirectory(physics/eylem-rigid3d eylem-rigid3d)
add_subdirectory(rendering/draw draw)
add_subdirectory(physics/eylem-viz eylem-viz)
add_subdirectory(geometry/geometry-viz geometry-viz)
add_subdirectory(applications/sandbox sandbox)

if(CRD_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

# Pinned WARP (scripts/install-warp.py): copy the verified DLL beside every test executable when CRD_WARP_DLL is
# set. Hosted Windows lanes set it; local builds leave it empty and use the OS WARP or the reversible diagnostic.
crd_stage_warp_dll("${CMAKE_CURRENT_SOURCE_DIR}")
