# crd-diag-overhead -- the DIAG.11b measurement harness: one workload (alloc, jobs, offline, io, events) under one
# diagnostic mode (off, basic, full) per process, one JSON record (cerid-diag-overhead/1) on stdout.
# scripts/diag-overhead.py runs it interleaved across processes and judges the budgets; the CTests below run every
# workload in every mode for a few frames with --check, which holds the structural half of the modes contract (a
# disabled site evaluates no payload, the off mode acquires nothing, basic filters and full records every event,
# every requested capture is delivered, the lifetime ledger balances) on every lane. Contract:
# docs/design/runtime-diagnostics.md (DIAG.11b); ADR-0133 ID-5. Owned by crd-ceir-host, whose dependency closure is
# every workload's but io's file reads (crd-platform): the allocators, the jobs pool, the diagnostic host and the CEIR
# cook, compile and run path.
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
)

target_compile_features(crd-diag-overhead PRIVATE cxx_std_20)
if(CRD_SHIPPING)
    set(_crd_overhead_shipping true)
else()
    set(_crd_overhead_shipping false)
endif()
target_compile_definitions(crd-diag-overhead PRIVATE
    CRD_DIAG_OVERHEAD_ASSETS="${CMAKE_SOURCE_DIR}/assets/diag"
    CRD_DIAG_OVERHEAD_SHIPPING=${_crd_overhead_shipping}
    CRD_DIAG_OVERHEAD_CONFIG="$<CONFIG>")

# Every workload in every mode, a few frames each, structurally checked. The scratch directory is the crash and
# capture directory of basic and full; the harness removes the bundles it writes.
set(_crd_overhead_scratch "${CMAKE_CURRENT_BINARY_DIR}/scratch")
file(MAKE_DIRECTORY "${_crd_overhead_scratch}")
set(_crd_overhead_tests "")
foreach(_crd_workload alloc jobs offline io events)
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
set_tests_properties(${_crd_overhead_tests} PROPERTIES TIMEOUT 120 LABELS "diag;diag-overhead")
foreach(_crd_name IN LISTS _crd_overhead_tests)
    crd_test_target("${_crd_name}" crd-diag-overhead)
endforeach()
