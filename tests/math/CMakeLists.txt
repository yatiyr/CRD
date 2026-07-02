add_executable(crd-math-tests
    test_math.cpp
    test_simd.cpp
    test_soa.cpp
    test_deterministic.cpp
    test_reduce.cpp
    test_simd_transcendental.cpp
    test_transcendental.cpp
)

target_link_libraries(crd-math-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-math-tests)

catch_discover_tests(crd-math-tests)

# ---- SIMD-emission regression check ---------------------------------------
# Disassembles the test_simd.cpp obj and verifies the actual machine code
# matches CRD_SIMD_LEVEL_RESOLVED. Catches the class of bugs where the
# CMake plumbing silently stops passing /arch:AVX2 / -mavx2, so backend.hpp
# falls back to SSE2 unnoticed. ADR-0063 + ADR-0065 substrate guard.
#
# Targets the obj (not the linked exe) so the disasm is pure project code
# without CRT pollution (MSVC's CRT contains AVX2-vectorized memcpy etc.
# regardless of /arch flag). Auto-skips on LTCG configs where the obj is
# IL-only — those configs are guarded by the non-LTCG sibling configs
# hitting the same code path.

set(_simd_obj
    "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/crd-math-tests.dir/test_simd.cpp${CMAKE_CXX_OUTPUT_EXTENSION}")

if(WIN32)
    add_test(
        NAME    crd-simd-emission-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_simd_emission.ps1"
                -Obj    "${_simd_obj}"
                -Expect "${CRD_SIMD_LEVEL_RESOLVED}"
    )
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
        NAME    crd-hesap-v13-no-exceptions
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_hesap_v13_no_exceptions.ps1"
                "${CMAKE_SOURCE_DIR}"
    )
elseif(UNIX)
    add_test(
        NAME    crd-simd-emission-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_simd_emission.sh"
                --obj    "${_simd_obj}"
                --expect "${CRD_SIMD_LEVEL_RESOLVED}"
    )
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
        NAME    crd-hesap-v13-no-exceptions
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_hesap_v13_no_exceptions.sh"
                "${CMAKE_SOURCE_DIR}"
    )
endif()
