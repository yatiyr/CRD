# crd-kir-metal tests — Phase 3.1.6 v17-d. Guarded on the backend target existing (APPLE, i.e. Part C on macOS).
if(NOT TARGET crd-kir-metal)
    return()
endif()

add_executable(crd-kir-metal-tests
    test_backend_metal.cpp
)

target_link_libraries(crd-kir-metal-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-metal
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-metal-tests)

catch_discover_tests(crd-kir-metal-tests DISCOVERY_MODE PRE_TEST)
