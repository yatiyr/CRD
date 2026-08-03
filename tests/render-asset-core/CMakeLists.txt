add_executable(crd-render-asset-core-tests
    test_render_asset_core.cpp  # RAF-1 Gate 1
    test_cooked_forms.cpp       # RAF-3 Gate 3 (cooked envelope + generational runtime handle)
)

target_link_libraries(crd-render-asset-core-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-render-asset-core
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-render-asset-core-tests)

catch_discover_tests(crd-render-asset-core-tests)
