# crd-kir-hip tests — Phase 3.1.6 v17-d. Guarded on the backend target existing (HIP SDK present, i.e. Part C on AMD).
if(NOT TARGET crd-kir-hip)
    return()
endif()

add_executable(crd-kir-hip-tests
    test_backend_hip.cpp
)

target_link_libraries(crd-kir-hip-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-hip
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-hip-tests)

catch_discover_tests(crd-kir-hip-tests DISCOVERY_MODE PRE_TEST)
