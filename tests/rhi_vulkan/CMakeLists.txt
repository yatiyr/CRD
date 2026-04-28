crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-rhi-vulkan-tests ${TEST_SOURCES})

target_link_libraries(crd-rhi-vulkan-tests PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-rhi-vulkan-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-rhi-vulkan-tests)

catch_discover_tests(crd-rhi-vulkan-tests)
