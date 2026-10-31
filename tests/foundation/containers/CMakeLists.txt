crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-containers-tests ${TEST_SOURCES})

target_link_libraries(crd-containers-tests PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-containers-tests)

crd_discover_tests(crd-containers-tests)

# ADR-0134 §8: the checksum [kernels] cases once per compiled level (crd-containers-kernels-at-<level>), and at avx512
# under SDE's icx model too, whose VPCLMULQDQ selects the 512-bit CRC fold.
crd_add_kernel_level_tests(crd-containers-tests "[kernels]" SDE_CPUS icx)

# The containers' contract fatals, one child process per mode (a fatal cannot run in-process): a sub-budget
# allocator's refusal ends in the container's out-of-memory fatal, never a null buffer adopted with a capacity, and
# HashMap::at's miss is fatal. Source and protocol: tests/support/diag/specimens/containers_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-containers-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/containers_fatal_specimen.cpp")
target_link_libraries(crd-containers-fatal-specimen PRIVATE crd-core crd-memory crd-containers)
crd_diag_fatal_tests(crd-containers-fatal-specimen PREFIX crd-containers-fatal OWNER crd-containers-tests
    MODES array-grow-refused string-grow-refused string-init-refused hashmap-grow-refused hashmap-at-missing
          ring-buffer-refused spsc-queue-refused atomic-array-refused concurrent-queue-refused)
