add_executable(crd-eylem-rigid3d-tests
    test_body_pool.cpp
    test_collider_pool.cpp
)

target_link_libraries(crd-eylem-rigid3d-tests PRIVATE
    crd-core
    crd-memory
    crd-math
    crd-eylem
    crd-eylem-rigid3d
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-eylem-rigid3d-tests)

catch_discover_tests(crd-eylem-rigid3d-tests)
