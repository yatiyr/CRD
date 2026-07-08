# crd-kir-dx12 tests — Phase 3.1.6 v17-d. Guarded: only built if the D3D12 backend target exists (Windows).
if(NOT TARGET crd-kir-dx12)
    return()
endif()

add_executable(crd-kir-dx12-tests
    test_backend_dx12.cpp
)

target_link_libraries(crd-kir-dx12-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-dx12
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-dx12-tests)

catch_discover_tests(crd-kir-dx12-tests DISCOVERY_MODE PRE_TEST)
