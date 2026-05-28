crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-vm-tests ${TEST_SOURCES})

target_link_libraries(crd-vm-tests PRIVATE
    crd-core
    crd-log
    crd-vm
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-vm-tests)

catch_discover_tests(crd-vm-tests)
