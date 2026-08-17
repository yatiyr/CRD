add_executable(crd-ceir-gpu-tests
    test_lower.cpp      # CEIR-13d: lower_region -> the inspectable command list (dispatches + transfers + 4d-derived barriers)
    test_execute.cpp    # CEIR-13z-1: the execution seam (ADR-0126) -- validate_lowered/execute_lowered device-free error paths
    test_work_build.cpp # CEIR-20b: build_work_ceir -> a find_work_misuse-clean ceir.work program (queue_alloc/produce/consume/compact)
)

target_link_libraries(crd-ceir-gpu-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-gpu
    crd-gpu-context   # the LoweredCommand names crd::gpu::DispatchKind (the live bridge edge)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-gpu-tests)
catch_discover_tests(crd-ceir-gpu-tests)
