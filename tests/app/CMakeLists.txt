crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-app-tests ${TEST_SOURCES})

target_link_libraries(crd-app-tests PRIVATE
    crd-core
    crd-app
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-app-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-app-tests)

catch_discover_tests(crd-app-tests)
