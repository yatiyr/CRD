# crd-frame-cook tests — REN-36.1 (D-007 row 139): the authorable frame graph's schema + cooker.
add_executable(crd-frame-cook-tests
    test_frame_asset.cpp
    test_viewport.cpp   # REN-37.8/37.9: the frame-level graph composer + the budgeted viewport scheduler
)

target_link_libraries(crd-frame-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-frame-cook
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-frame-cook-tests)

catch_discover_tests(crd-frame-cook-tests)
