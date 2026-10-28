# crd-diag-overhead -- the DIAG.11b measurement harness: one workload (alloc, jobs, offline, io, render, events, zones,
# marks) under one diagnostic mode (off, basic, full) per process, one JSON record (cerid-diag-overhead/1) on stdout.
# scripts/diag-overhead.py runs it interleaved across processes and judges the budgets; the CTests below run every
# workload in every mode for a few frames with --check, which holds the structural half of the modes contract (a
# disabled site evaluates no payload, the off mode acquires nothing, basic filters and full records every event,
# every requested capture is delivered, the lifetime ledger balances, the profiler records every zone and frame mark)
# on every lane. Contract: docs/design/runtime-diagnostics.md (DIAG.11b); ADR-0133 ID-5. Its closure is every
# workload's: the allocators, the jobs pool, the diagnostic host, the CEIR cook, compile and run path, the file readers
# and the scene renderer on a headless device (D3D12 on Windows, Vulkan elsewhere; the render CTests hold the device
# lock).
set(_crd_overhead_libraries
    crd-core
    crd-containers
    crd-math
    crd-memory
    crd-jobs
    crd-perf
    crd-ceir
    crd-ceir-cook
    crd-ceir-host
    crd-platform
    crd-resources
    crd-scene
    crd-scene-render
    crd-gpu-context
)
if(WIN32)
    list(APPEND _crd_overhead_libraries crd-gpu-context-dx12)
else()
    list(APPEND _crd_overhead_libraries crd-gpu-context-vulkan)
endif()
if(CRD_SHIPPING)
    set(_crd_overhead_shipping true)
else()
    set(_crd_overhead_shipping false)
endif()

# The harness's build: its closure, the shared quad fixture (tests/gpu/gpu-shared/quad_scene_fixture.hpp, shared with
# the render tests) and where its authored assets are.
function(_crd_overhead_harness target)
    target_link_libraries(${target} PRIVATE ${_crd_overhead_libraries})
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/tests/gpu/gpu-shared")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        CRD_DIAG_OVERHEAD_ASSETS="${CMAKE_SOURCE_DIR}/assets/diag"
        CRD_DIAG_OVERHEAD_ENGINE_ASSETS="${CMAKE_SOURCE_DIR}/assets"
        CRD_DIAG_OVERHEAD_SHIPPING=${_crd_overhead_shipping}
        CRD_DIAG_OVERHEAD_CONFIG="$<CONFIG>")
endfunction()

add_executable(crd-diag-overhead
    main.cpp
    workloads.cpp
)
_crd_overhead_harness(crd-diag-overhead)

# Every workload in every mode, a few frames each, structurally checked. The scratch directory is the crash and
# capture directory of basic and full; the harness removes the bundles it writes.
set(_crd_overhead_scratch "${CMAKE_CURRENT_BINARY_DIR}/scratch")
file(MAKE_DIRECTORY "${_crd_overhead_scratch}")
set(_crd_overhead_tests "")
foreach(_crd_workload alloc jobs offline io render events zones marks)
    foreach(_crd_mode off basic full)
        set(_crd_name "crd-diag-overhead-${_crd_workload}-${_crd_mode}")
        file(MAKE_DIRECTORY "${_crd_overhead_scratch}/${_crd_workload}-${_crd_mode}")
        add_test(NAME "${_crd_name}"
            COMMAND crd-diag-overhead --workload ${_crd_workload} --mode ${_crd_mode} --iterations 6 --warmup 2
                    --captures 2 --threads 4 --check
                    --scratch "${_crd_overhead_scratch}/${_crd_workload}-${_crd_mode}")
        list(APPEND _crd_overhead_tests "${_crd_name}")
    endforeach()
endforeach()
# The allocator statistics requested (--memory): full registers the heap with the profiler before its first allocation,
# and its counts are complete and balance wherever the build counts them.
file(MAKE_DIRECTORY "${_crd_overhead_scratch}/alloc-full-memory")
add_test(NAME crd-diag-overhead-alloc-full-memory
    COMMAND crd-diag-overhead --workload alloc --mode full --memory --iterations 6 --warmup 2 --captures 2 --check
            --scratch "${_crd_overhead_scratch}/alloc-full-memory")
list(APPEND _crd_overhead_tests crd-diag-overhead-alloc-full-memory)
# The system clock (--clock system, DIAG.11b (q2)): every per-event timestamp from MonotonicClock, as on a CPU whose
# counter may not be mapped; the board's full-system-clock arm. The check holds the clock to the one asked for.
file(MAKE_DIRECTORY "${_crd_overhead_scratch}/jobs-full-system-clock")
add_test(NAME crd-diag-overhead-jobs-full-system-clock
    COMMAND crd-diag-overhead --workload jobs --mode full --clock system --iterations 6 --warmup 2 --captures 2
            --threads 4 --check --scratch "${_crd_overhead_scratch}/jobs-full-system-clock")
list(APPEND _crd_overhead_tests crd-diag-overhead-jobs-full-system-clock)
# The render workload under device validation (D3D12 has no synchronization validation): the requested modes active on
# the device it draws with and no validation message over the run (GPU-assisted validation's notices of its own cost
# are reported, not failed).
if(WIN32)
    set(_crd_overhead_validations core gpu)
else()
    set(_crd_overhead_validations core sync gpu)
endif()
set(_crd_overhead_device_tests crd-diag-overhead-render-off crd-diag-overhead-render-basic crd-diag-overhead-render-full)
foreach(_crd_validation IN LISTS _crd_overhead_validations)
    set(_crd_name "crd-diag-overhead-render-validation-${_crd_validation}")
    file(MAKE_DIRECTORY "${_crd_overhead_scratch}/render-validation-${_crd_validation}")
    add_test(NAME "${_crd_name}"
        COMMAND crd-diag-overhead --workload render --mode off --validation ${_crd_validation} --iterations 6
                --warmup 2 --check --scratch "${_crd_overhead_scratch}/render-validation-${_crd_validation}")
    list(APPEND _crd_overhead_tests "${_crd_name}")
    list(APPEND _crd_overhead_device_tests "${_crd_name}")
endforeach()
set_tests_properties(${_crd_overhead_tests} PROPERTIES TIMEOUT 120 LABELS "diag;diag-overhead")
set_tests_properties(${_crd_overhead_device_tests} PROPERTIES RESOURCE_LOCK crd_gpu_device)
foreach(_crd_name IN LISTS _crd_overhead_tests)
    crd_test_target("${_crd_name}" crd-diag-overhead)
endforeach()

# DIAG.11b clause (8), the peer on comparable features: the same harness with Tracy Profiler at the same sites
# (CRD_DIAG_PEER_TRACY; the root CMakeLists.txt builds tracy-client from the pinned archive). One binary holds the Cerid
# modes and the tracy mode, so the comparison is free of a second binary's layout. Its CTests run every workload in the
# tracy mode with no client connected: Tracy started, no Cerid diagnostic, no payload formatted, every callback zone
# closed where it began. A connected run needs a capture client and is the driver's (scripts/diag-overhead.py
# --tracy-tools), which holds it to the zones, frames and messages the capture received.
if(CRD_DIAG_PEER_TRACY)
    add_executable(crd-diag-overhead-tracy
        main.cpp
        workloads.cpp
        tracy_peer.cpp
    )
    _crd_overhead_harness(crd-diag-overhead-tracy)
    target_link_libraries(crd-diag-overhead-tracy PRIVATE tracy-client)
    target_compile_definitions(crd-diag-overhead-tracy PRIVATE CRD_DIAG_OVERHEAD_TRACY=1)
    # The discriminator: Tracy timed by the operating system's clock, the one Cerid's zones read (tracy-client-osclock).
    add_executable(crd-diag-overhead-tracy-osclock
        main.cpp
        workloads.cpp
        tracy_peer.cpp
    )
    _crd_overhead_harness(crd-diag-overhead-tracy-osclock)
    target_link_libraries(crd-diag-overhead-tracy-osclock PRIVATE tracy-client-osclock)
    target_compile_definitions(crd-diag-overhead-tracy-osclock PRIVATE CRD_DIAG_OVERHEAD_TRACY=1)
    set(_crd_peer_tests "")
    foreach(_crd_workload alloc jobs offline io render events zones marks)
        set(_crd_name "crd-diag-overhead-tracy-${_crd_workload}")
        file(MAKE_DIRECTORY "${_crd_overhead_scratch}/tracy-${_crd_workload}")
        add_test(NAME "${_crd_name}"
            COMMAND crd-diag-overhead-tracy --workload ${_crd_workload} --mode tracy --iterations 6 --warmup 2
                    --threads 4 --check --scratch "${_crd_overhead_scratch}/tracy-${_crd_workload}")
        list(APPEND _crd_peer_tests "${_crd_name}")
    endforeach()
    file(MAKE_DIRECTORY "${_crd_overhead_scratch}/tracy-alloc-memory")
    add_test(NAME crd-diag-overhead-tracy-alloc-memory
        COMMAND crd-diag-overhead-tracy --workload alloc --mode tracy --memory --iterations 6 --warmup 2 --check
                --scratch "${_crd_overhead_scratch}/tracy-alloc-memory")
    list(APPEND _crd_peer_tests crd-diag-overhead-tracy-alloc-memory)
    set_tests_properties(${_crd_peer_tests} PROPERTIES TIMEOUT 120 LABELS "diag;diag-overhead;diag-peer")
    set_tests_properties(crd-diag-overhead-tracy-render PROPERTIES RESOURCE_LOCK crd_gpu_device)
    foreach(_crd_name IN LISTS _crd_peer_tests)
        crd_test_target("${_crd_name}" crd-diag-overhead-tracy)
    endforeach()
endif()
