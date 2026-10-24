# crd-diag-overhead -- the DIAG.11b measurement harness: one workload (alloc, jobs, offline, io, render, events) under
# one diagnostic mode (off, basic, full) per process, one JSON record (cerid-diag-overhead/1) on stdout.
# scripts/diag-overhead.py runs it interleaved across processes and judges the budgets; the CTests below run every
# workload in every mode for a few frames with --check, which holds the structural half of the modes contract (a
# disabled site evaluates no payload, the off mode acquires nothing, basic filters and full records every event,
# every requested capture is delivered, the lifetime ledger balances) on every lane. Contract:
# docs/design/runtime-diagnostics.md (DIAG.11b); ADR-0133 ID-5. Its closure is every workload's: the allocators, the
# jobs pool, the diagnostic host, the CEIR cook, compile and run path, the file readers and the scene renderer on a
# headless device (D3D12 on Windows, Vulkan elsewhere; the render CTests hold the device lock).
add_executable(crd-diag-overhead
    main.cpp
    workloads.cpp
)

target_link_libraries(crd-diag-overhead PRIVATE
    crd-core
    crd-containers
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
    target_link_libraries(crd-diag-overhead PRIVATE crd-gpu-context-dx12)
else()
    target_link_libraries(crd-diag-overhead PRIVATE crd-gpu-context-vulkan)
endif()
# The quad mesh and its single-entry pack (tests/gpu/gpu-shared/quad_scene_fixture.hpp), shared with the render tests.
target_include_directories(crd-diag-overhead PRIVATE "${CMAKE_SOURCE_DIR}/tests/gpu/gpu-shared")

target_compile_features(crd-diag-overhead PRIVATE cxx_std_20)
if(CRD_SHIPPING)
    set(_crd_overhead_shipping true)
else()
    set(_crd_overhead_shipping false)
endif()
target_compile_definitions(crd-diag-overhead PRIVATE
    CRD_DIAG_OVERHEAD_ASSETS="${CMAKE_SOURCE_DIR}/assets/diag"
    CRD_DIAG_OVERHEAD_ENGINE_ASSETS="${CMAKE_SOURCE_DIR}/assets"
    CRD_DIAG_OVERHEAD_SHIPPING=${_crd_overhead_shipping}
    CRD_DIAG_OVERHEAD_CONFIG="$<CONFIG>")

# Every workload in every mode, a few frames each, structurally checked. The scratch directory is the crash and
# capture directory of basic and full; the harness removes the bundles it writes.
set(_crd_overhead_scratch "${CMAKE_CURRENT_BINARY_DIR}/scratch")
file(MAKE_DIRECTORY "${_crd_overhead_scratch}")
set(_crd_overhead_tests "")
foreach(_crd_workload alloc jobs offline io render events)
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
