crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-memory-tests ${TEST_SOURCES})

target_link_libraries(crd-memory-tests PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-jobs   # RingAllocator concurrency test drives producers through the fiber job system (ADR-0085 S4)
    crd-diag-harness # DIAG.3a: runs the allocator fatal-path specimen as a bounded child
    Catch2::Catch2WithMain
)

# DIAG.3a fatal-path control: a reallocate no pool can satisfy must reach the out-of-memory fatal. The specimen
# is built and its path handed to the test, which demands the handler's exit code -- never a silent pass.
add_dependencies(crd-memory-tests crd-diag-alloc-fatal-specimen)
target_compile_definitions(crd-memory-tests PRIVATE
    CRD_DIAG_ALLOC_FATAL_SPECIMEN="$<TARGET_FILE:crd-diag-alloc-fatal-specimen>")

crd_enable_default_pch(crd-memory-tests)

crd_discover_tests(crd-memory-tests)
