include(CTest)
include(CrdTestDiscovery)
include(CrdFuzz) # REPO.DEV.9: crd_fuzz_target() (replay CTest on every lane; libFuzzer under CRD_ENABLE_FUZZER)
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
add_test(NAME crd-module-selection
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-module-selection.py")
add_test(NAME crd-ci-tiers
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-ci-tiers.py")
add_test(NAME crd-pins
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-pins.py")
add_test(NAME crd-generated-sources # REPO.DEV.10: every generated file named with its generator and bytes
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-generated.py")
add_test(NAME crd-license-manifest # REPO.DEV.10: docs/generated/dependency-licenses.md follows cmake/pins.json
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen_license_manifest.py" --check)
# The document validator walks about a thousand documents; on a native disk that is seconds, on a checkout read
# through a 9p mount (WSL on a Windows drive) it measured 57 to 64 s and the former 60 s budget failed it honestly.
# The budget is a ceiling against hangs, not a performance contract, so it covers the slowest supported instrument.
set_tests_properties(crd-repository-hygiene crd-master-plan crd-repository-tools
    PROPERTIES TIMEOUT 300 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-project-structure-sync PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-build-profiles PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-module-selection PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-ci-tiers crd-pins crd-license-manifest PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-generated-sources PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1") # hashes 14 MB of codelets
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
crd_tests(support/test_helpers test_helpers)
crd_tests(support/fuzz fuzz)  # REPO.DEV.9: the bounded fuzz harness shared by the ingestion fuzz targets

crd_tests(foundation/core core)
crd_tests(foundation/vm vm)
crd_tests(foundation/units units)
crd_tests(execution/ceir ceir)  # D-007 CEIR-1a: Cerid Execution IR core (arena + intrusive def-use)
crd_tests(execution/chir chir)  # D-007 CEIR-32b: CHIR source model + CR-D007 graph-schema projection (ADR-0128)
crd_tests(execution/ceir-host ceir-host)  # D-007 CEIR-6b: the crd-jobs execution-provider bridge
crd_tests(execution/ceir-cook ceir-cook)  # D-007 CEIR-7a: the CEIR asset cook bridge
crd_tests(execution/ceir-gpu ceir-gpu)  # D-007 CEIR-13d: the GPU lowering bridge
crd_tests(rendering/render-asset-core render-asset-core)  # RAF-1 (D-007 "RAF band"): identity/namespaces/diagnostics/deps Gate 1
crd_tests(gpu/gpu-context gpu-context)        # RAF-2a (D-007 "RAF band"): canonical GPU command model (device-free structural gate)
crd_tests(rendering/render-program render-program)     # RAF-4 (D-007 "RAF band"): shader + program contract Gate 4
crd_tests(rendering/render-material render-material)    # RAF-5 (D-007 "RAF band"): material + technique contract Gate 5
crd_tests(rendering/render-pass render-pass)        # RAF-6 (D-007 "RAF band"): pass-executor registry Gate 6
crd_tests(rendering/render-graph render-graph)       # RAF-7 (D-007 "RAF band"): unified frame-graph runtime Gate 7
crd_tests(foundation/time time)
crd_tests(foundation/toml toml)
crd_tests(foundation/perf perf)
crd_tests(ui/perf-ui perf-ui)
crd_tests(foundation/log log)
crd_tests(foundation/memory memory)
crd_tests(foundation/math math)
crd_tests(geometry/geometry-primitives geometry-primitives)
crd_tests(geometry/geometry-bvh geometry-bvh)
crd_tests(geometry/geometry-convex geometry-convex)
crd_tests(geometry/geometry-mesh geometry-mesh)
crd_tests(geometry/geometry-spatial geometry-spatial)
crd_tests(geometry/geometry-polygon geometry-polygon)
crd_tests(geometry/geometry-mesh-processing geometry-mesh-processing)
crd_tests(geometry/lod lod) # REN-40-C1: the mesh LOD chain
crd_tests(geometry/geometry-delaunay geometry-delaunay)
crd_tests(geometry/geometry-decomposition geometry-decomposition)
crd_tests(geometry/geometry-bvh-gpu geometry-bvh-gpu)
crd_tests(assets/asset-io asset-io)
crd_tests(geometry/geometry-shader-helpers geometry-shader-helpers)
crd_tests(geometry/geometry-curves geometry-curves)
crd_tests(numerics/hesap hesap)
crd_tests(numerics/hesap-dense hesap-dense)
crd_tests(numerics/hesap-sparse hesap-sparse)
crd_tests(numerics/hesap-ordering hesap-ordering)
crd_tests(numerics/hesap-resources hesap-resources)
crd_tests(numerics/hesap-iterative hesap-iterative)
crd_tests(numerics/hesap-amg hesap-amg)
crd_tests(numerics/hesap-sched hesap-sched)
crd_tests(numerics/hesap-direct hesap-direct)
crd_tests(numerics/hesap-eigen hesap-eigen)
crd_tests(numerics/hesap-special hesap-special)
crd_tests(numerics/hesap-quadrature hesap-quadrature)
crd_tests(numerics/hesap-autodiff hesap-autodiff)
crd_tests(gpu/kir kir)
crd_tests(assets/frame-cook frame-cook)
crd_tests(assets/technique-cook technique-cook)  # REN-37 (D-007 row 140): the LIGHTING TECHNIQUE as an authored `.crdt` asset
crd_tests(assets/material-cook material-cook)   # REN-38-C (D-007 row 141): the MATERIAL as an authored `.crdm` asset
crd_tests(assets/vertex-cook vertex-cook)     # REN-38-D (D-007 row 141): the VERTEX PROGRAM as an authored `.crdv` asset
crd_tests(assets/light-cook light-cook)      # REN-38-E (D-007 row 141): the LIGHTING VOCABULARY as an authored asset
crd_tests(gpu/gpu-context-vulkan gpu-context-vulkan)  # ADR-0099 v17-i-a
crd_tests(gpu/gpu-context-dx12 gpu-context-dx12)    # ADR-0100 v17-i: D3D12 IComputeContext backend
crd_tests(gpu/gpu-context-cuda gpu-context-cuda)    # ADR-0100 (2026-08-07): CUDA IComputeContext backend; self-skips without CUDAToolkit
crd_tests(gpu/kir-vulkan kir-vulkan)
crd_tests(execution/ceir-gpu-vulkan ceir-gpu-vulkan)  # D-007 CEIR-13z-1b: a CEIR compute asset executed on a real Vulkan device (execute_lowered, ADR-0126)
crd_tests(gpu/kir-cuda kir-cuda)
crd_tests(execution/ceir-gpu-cuda ceir-gpu-cuda)    # D-007 CEIR-13z: the CUDA mirror — a CEIR compute asset on a real CUDA device (required backend)
crd_tests(gpu/kir-dx12 kir-dx12)
crd_tests(execution/ceir-gpu-dx12 ceir-gpu-dx12)  # D-007 CEIR-13z-1b: the DX12 mirror — a CEIR compute asset on a real D3D12 device (Windows-only)
crd_tests(gpu/kir-webgpu kir-webgpu)
crd_tests(gpu/kir-hip kir-hip)
crd_tests(gpu/kir-metal kir-metal)
crd_tests(numerics/hesap-opt hesap-opt)
crd_tests(numerics/hesap-stats hesap-stats)
crd_tests(numerics/hesap-ode hesap-ode)
crd_tests(numerics/hesap-fft hesap-fft)
crd_tests(numerics/hesap-dsp hesap-dsp)
crd_tests(numerics/hesap-wavelet hesap-wavelet)
crd_tests(numerics/hesap-comms hesap-comms)
crd_tests(numerics/hesap-interp hesap-interp)
crd_tests(numerics/hesap-diff hesap-diff)
crd_tests(numerics/hesap-motion hesap-motion)
crd_tests(numerics/hesap-tensor hesap-tensor)
crd_tests(foundation/containers containers)
crd_tests(foundation/platform platform)
crd_tests(foundation/app app)
crd_tests(foundation/config config)
crd_tests(ui/imgui imgui)
# RET-8 (ADR-0105): tests/{renderer,shader,rhi,rhi_vulkan} DELETED with their modules — the parity audit
# (D-007 rows 89-96) records every assertion's green gpu-context home.
crd_tests(foundation/jobs jobs)
crd_tests(foundation/stress stress)
crd_tests(assets/resources resources)
crd_tests(geometry/meshgen meshgen)
crd_tests(world/scene scene)
crd_tests(rendering/scene-render scene-render)  # GEO-7 (D-007 row 72): chunk-grain extract/cull/submit gates
crd_tests(world/anim anim)          # GEO-8 (D-007 row 73): SKEL/ANIM resources + THE FOX bit-stable resample gate
crd_tests(world/timeline timeline)      # GEO-9 (D-007 row 74): TIML resource + deterministic eval + THE EXR RENDER GATE
crd_tests(media/audio audio)         # GEO-10 (D-007 row 75): codecs bit-exact + graph render + MIDI + realtime
crd_tests(tools/ceridc ceridc)        # GEO-11 (D-007 row 76): THE AGENT GATE — the full scenario through MCP alone
crd_tests(assets/scene_cooker scene_cooker)
crd_tests(assets/cooker cooker)
crd_tests(assets/preset preset)
crd_tests(foundation/profile profile)
crd_tests(physics/eylem eylem)
crd_tests(physics/eylem-rigid3d eylem-rigid3d)
crd_tests(rendering/draw draw)
crd_tests(physics/eylem-viz eylem-viz)
crd_tests(geometry/geometry-viz geometry-viz)
crd_tests(applications/sandbox sandbox)

if(CRD_BUILD_BENCHMARKS)
    crd_tests(bench)
endif()

# Pinned WARP (scripts/install-warp.py): copy the verified DLL beside every test executable when CRD_WARP_DLL is
# set. Hosted Windows lanes set it; local builds leave it empty and use the OS WARP or the reversible diagnostic.
crd_stage_warp_dll("${CMAKE_CURRENT_SOURCE_DIR}")
