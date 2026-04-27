crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-platform-tests ${TEST_SOURCES})

target_link_libraries(crd-platform-tests PRIVATE
    crd-core
    crd-log
    crd-platform
    Catch2::Catch2WithMain
)

# std::getenv triggers MSVC's C4996 'unsafe' deprecation; we use it
# intentionally for a tiny portable env-var read inside the test runner.
if(MSVC)
    target_compile_definitions(crd-platform-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-platform-tests)

catch_discover_tests(crd-platform-tests)
