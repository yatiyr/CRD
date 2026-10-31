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
# ADR-0135: the Cerid CI platform's registry, planner, lanes, executors, dashboard and parity table
add_test(NAME crd-ci-platform
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-ci-platform.py")
# ADR-0135 §12: the Cerid orchestrator's state machines, store, authentication, scheduler, agent and service
add_test(NAME crd-ci-orchestrator
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-ci-orchestrator.py")
# ADR-0135 §12: the coordinator and a real agent process on this host, a trivial pipeline end to end
add_test(NAME crd-ci-orchestrator-e2e
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-ci-orchestrator-e2e.py")
# DIAG.6c: verdict tests of the external sampling, counter-capability and debugger runbooks. They mock wpr, xperf,
# perf and lldb, so they need neither the tools nor elevation and run on every host.
add_test(NAME crd-diag-cpu-sampling-runbook
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-sample-cpu-wpr.py")
add_test(NAME crd-diag-counters-capability
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-counters-capability.py")
add_test(NAME crd-diag-debugger-recipes
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-debugger-recipes.py")
add_test(NAME crd-diag-reverse-debug-recipes # DIAG.9c: every reverse-debugging verdict, without a debugger
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-reverse-debug-recipes.py")
# DIAG.10b: every seeded-failure verdict, the registry against the tree and the suppression policy, without a build
add_test(NAME crd-diag-seeded-faults
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-diag-seeded-faults.py")
# DIAG.10b: every coverage verdict, the coverage registry and the nonreproduced-failure register, without a build
add_test(NAME crd-diag-coverage
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-diag-coverage.py")
# DIAG.11b: every overhead-board statistic and verdict, with fake measurement processes
add_test(NAME crd-diag-overhead-verdicts
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-diag-overhead.py")
# DIAG.11b clause (8): every verdict of the external sampling of the overhead workloads, wpr and xperf mocked
add_test(NAME crd-diag-overhead-sampling-runbook
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-sample-overhead-wpr.py")
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
set_tests_properties(crd-repository-hygiene crd-master-plan crd-repository-tools crd-ci-platform
    crd-ci-orchestrator crd-ci-orchestrator-e2e PROPERTIES TIMEOUT 300 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-project-structure-sync PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-build-profiles PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-module-selection PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-ci-tiers crd-pins crd-license-manifest PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-generated-sources PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1") # hashes 14 MB of codelets
set_tests_properties(crd-developer-workflow PROPERTIES TIMEOUT 90 ENVIRONMENT "PYTHONUTF8=1")
set_tests_properties(crd-diag-cpu-sampling-runbook crd-diag-counters-capability crd-diag-debugger-recipes
    crd-diag-reverse-debug-recipes crd-diag-overhead-verdicts crd-diag-overhead-sampling-runbook
    PROPERTIES TIMEOUT 30 ENVIRONMENT "PYTHONUTF8=1")
# The harness-tree check's teeth configure and build a real CONFIGURE_DEPENDS tree with this tree's own CMake and ninja,
# so under CTest that case always runs (outside CTest it looks for both on PATH).
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(TEST crd-diag-overhead-sampling-runbook APPEND PROPERTY ENVIRONMENT
        "CRD_TEST_CMAKE=${CMAKE_COMMAND}" "CRD_TEST_NINJA=${CMAKE_MAKE_PROGRAM}")
endif()
# It reads the seeded sources through the checkout: 10 s through WSL's 9p mount, under 1 s on a native disk.
set_tests_properties(crd-diag-seeded-faults crd-diag-coverage PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# DIAG.3a: every allocation result in shipped code is checked before it is used (a sub-budget allocator refuses by
# returning nullptr), passed up to a caller that checks it, or a named heap site; it reads every source under engine/,
# tools/, runtime/ and sandbox/ through the checkout.
add_test(NAME crd-check-allocation-results
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-allocation-results.py" "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-allocation-results PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# Every environment variable is read through crd/core/environment.hpp: no std::getenv, _dupenv_s or
# GetEnvironmentVariable outside its reader, in any C-family source under the source-routes roots.
add_test(NAME crd-check-environment-reads
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-environment-reads.py" "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-environment-reads PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# Only frame owners reset the crd::jobs frame arenas: no crd::jobs::frame_reset under engine/ outside the jobs module.
add_test(NAME crd-check-frame-resets
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-frame-resets.py" "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-frame-resets PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# OWN.1 and OWN.2: every engine capability has one owning module (cmake/capability-owners.json); a registered
# capability's signature in an engine source outside its owner fails unless the registry lists the file as pending a
# move or allowed with the reason.
add_test(NAME crd-check-capability-owners
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-capability-owners.py" --root "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-capability-owners PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# ADR-0134 §8: only the board peer libraries (runtime/examples/peers/) include a peer header library (Eigen, Boost,
# pocketfft), so no bench compiles a peer at the baseline ISA.
add_test(NAME crd-check-peer-headers
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-peer-headers.py" "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-peer-headers PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# NET-STD and COMPLY.0: the standards register (cmake/standards-register.json) holds no expired pinned draft, no
# obsoleted pinned RFC and no overdue review; owners, rows, sources and evidence exist (docs/design/registers.md).
add_test(NAME crd-check-registers
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-registers.py" --root "${CMAKE_SOURCE_DIR}")
set_tests_properties(crd-check-registers PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
# DIAG.11b clause (10): the investigation runbooks (docs/design/diagnostic-runbooks.md) against this tree: every
# command, flag, diag command, artifact and example exists, and every test a runbook names (on the rows for this host)
# is one this tree registers (it lists them with ctest --show-only); nothing publishes.
get_property(_crd_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_crd_multi_config)
    set(_crd_runbook_config --config "$<CONFIG>")
else()
    set(_crd_runbook_config "")
endif()
# A module selection registers part of the tests the runbook commands' full presets do, so it resolves none of them.
get_property(_crd_runbook_modules GLOBAL PROPERTY CRD_MODULE_NAMES)
get_property(_crd_runbook_selected GLOBAL PROPERTY CRD_SELECTED_MODULES)
list(REMOVE_ITEM _crd_runbook_modules ${_crd_runbook_selected})
if(_crd_runbook_modules)
    list(JOIN _crd_runbook_modules "," _crd_runbook_omitted)
    list(APPEND _crd_runbook_config --omitted-modules "${_crd_runbook_omitted}")
endif()
add_test(NAME crd-check-diag-runbooks
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-diag-runbooks.py" --build "${CMAKE_BINARY_DIR}"
            ${_crd_runbook_config})
set_tests_properties(crd-check-diag-runbooks PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
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
# Every first-party C-family source is compiled by a target of this tree or named in scripts/source-routes.json with
# the route that builds it, judged against what this tree compiles (its compile_commands.json): a test source that no
# target lists never runs, and nothing else notices. A partial configuration (CRD_MODULES) omits whole modules, so the
# guard runs on full configurations; the generators that write a compile database.
if(CMAKE_EXPORT_COMPILE_COMMANDS AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND NOT CRD_MODULE_SELECTION)
    add_test(NAME crd-check-source-routes
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-source-routes.py" --build "${CMAKE_BINARY_DIR}")
    set_tests_properties(crd-check-source-routes PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()
# One optimization level per cl.exe/clang-cl command: cl.exe reports a second one as D9025, which /WX does not fail.
# The generators that write compile_commands.json; Visual Studio keeps per-file settings in its projects instead.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    add_test(NAME crd-check-compile-commands
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-compile-commands.py"
                "${CMAKE_BINARY_DIR}/compile_commands.json")
    set_tests_properties(crd-check-compile-commands PROPERTIES TIMEOUT 60 ENVIRONMENT "PYTHONUTF8=1")
endif()

# Header-only test utilities (ULP compare, GPU determinism check) — must
# come BEFORE any test target that links it.
crd_tests(support/test_helpers test_helpers)
crd_tests(support/fuzz fuzz)  # REPO.DEV.9: the bounded fuzz harness shared by the ingestion fuzz targets
crd_tests(support/diag diag)  # DIAG.0: the bounded specimen harness (detector controls)

crd_tests(foundation/core core)
crd_tests(foundation/vm vm)
crd_tests(foundation/units units)
crd_tests(execution/ceir ceir)  # D-007 CEIR-1a: Cerid Execution IR core (arena + intrusive def-use)
crd_tests(execution/chir chir)  # D-007 CEIR-32b: CHIR source model + CR-D007 graph-schema projection (ADR-0128)
crd_tests(execution/ceir-host ceir-host)  # D-007 CEIR-6b: the crd-jobs execution-provider bridge
crd_tests(support/diag-overhead diag-overhead)  # DIAG.11b: the diagnostic overhead harness and its structural checks
crd_tests(execution/ceir-cook ceir-cook)  # D-007 CEIR-7a: the CEIR asset cook bridge
crd_tests(execution/ceir-gpu ceir-gpu)  # D-007 CEIR-13d: the GPU lowering bridge
crd_tests(rendering/render-asset-core render-asset-core)  # RAF-1 (D-007 "RAF band"): identity/namespaces/diagnostics/deps Gate 1
crd_tests(gpu/gpu-context gpu-context)        # RAF-2a (D-007 "RAF band"): canonical GPU command model (device-free structural gate)
crd_tests(gpu/perf-gpu-bridge perf-gpu-bridge) # DIAG.6b(i): IFrameGraph timings -> crd-perf GPU spans (fake-verified)
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
crd_tests(foundation/compression compression)  # OWN.1: raw DEFLATE, zlib and the zstd binding
crd_tests(foundation/crypto crypto)  # NET-STD: algorithm identifiers and signed policy generations
crd_tests(foundation/net net)  # NET-STD: version and extension negotiation, GREASE, register identifiers
crd_tests(foundation/comply comply)  # COMPLY.0: reviewed rule generations, the live store and the query
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
crd_tests(applications/sandbox-inspect sandbox-inspect) # DIAG.8b: the sandbox's inspect panel, without a window

if(CRD_BUILD_BENCHMARKS)
    crd_tests(bench)
endif()

# Hot headers stay within their include budget (scripts/include-budget.json): the standard headers the 2026-10-18
# compile-time profiles found costliest never come back into every unit through one include. Each probe borrows a
# test unit's compile command, so the guard runs where those tests are configured and a compile database is written.
if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles" AND TARGET crd-containers-tests AND TARGET crd-jobs-tests
   AND TARGET crd-math-tests)
    add_test(NAME crd-check-include-budget
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-include-budget.py" --build "${CMAKE_BINARY_DIR}")
    set_tests_properties(crd-check-include-budget PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# Pinned WARP (scripts/install-warp.py): copy the verified DLL beside every test executable when CRD_WARP_DLL is
# set. Hosted Windows lanes set it; local builds leave it empty and use the OS WARP or the reversible diagnostic.
crd_stage_warp_dll("${CMAKE_CURRENT_SOURCE_DIR}")
