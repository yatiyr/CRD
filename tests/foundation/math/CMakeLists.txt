add_executable(crd-math-tests
    test_math.cpp
    test_select.cpp
    test_simd.cpp
    test_soa.cpp
    test_deterministic.cpp
    test_reduce.cpp
    test_simd_transcendental.cpp
    test_transcendental.cpp
    test_kernel_dispatch.cpp
    test_simd_levels.cpp
)

target_link_libraries(crd-math-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

# The SIMD-type probe, one unit per CPU level the build compiles (ADR-0134 §5, §7); test_simd_levels.cpp lists them.
crd_add_cpu_kernels(crd-math-tests NAME simd_probe LEVELS portable sse2 sse42 avx2 avx512 neon INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/simd_level_probe.cpp")

crd_enable_default_pch(crd-math-tests)

crd_discover_tests(crd-math-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-math-kernels-at-<level>).
crd_add_kernel_level_tests(crd-math-tests "[kernels]")

if(WIN32)
    add_test(
        NAME    crd-no-std-math-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_std_math.ps1"
    )
    add_test(
        NAME    crd-no-std-transcendental-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_std_transcendental.ps1"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-std-sort-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_std_sort.ps1"
    )
    add_test(
        NAME    crd-no-std-containers-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_std_containers.ps1"
    )
    add_test(
        NAME    crd-no-non-ascii-test-names
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_non_ascii_test_names.ps1"
    )
    add_test(
        NAME    crd-no-untagged-physical-numeric
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_untagged_physical_numeric.ps1"
    )
    add_test(
        NAME    crd-no-malloc-allocator
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_malloc_allocator.ps1"
    )
    add_test(
        NAME    crd-no-shader-language-leak
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_no_shader_language_leak.ps1"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-ceir-invariants
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_ceir_invariants.ps1"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-hesap-v13-no-exceptions
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_hesap_v13_no_exceptions.ps1"
                "${CMAKE_SOURCE_DIR}"
    )
elseif(UNIX)
    add_test(
        NAME    crd-no-std-math-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_std_math.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-std-transcendental-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_std_transcendental.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-std-sort-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_std_sort.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-std-containers-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_std_containers.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-non-ascii-test-names
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_non_ascii_test_names.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-untagged-physical-numeric
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_untagged_physical_numeric.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-malloc-allocator
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_malloc_allocator.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-no-shader-language-leak
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_no_shader_language_leak.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-ceir-invariants
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_ceir_invariants.sh"
                "${CMAKE_SOURCE_DIR}"
    )
    add_test(
        NAME    crd-hesap-v13-no-exceptions
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_hesap_v13_no_exceptions.sh"
                "${CMAKE_SOURCE_DIR}"
    )
endif()
