add_executable(crd-math-tests
    test_math.cpp
    test_select.cpp
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

crd_discover_tests(crd-math-tests)

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

# CMake supplies actual object paths for Ninja and native configurations; do not reconstruct generator internals.
set(_simd_obj "$<FILTER:$<TARGET_OBJECTS:crd-math-tests>,INCLUDE,/test_simd(\\.cpp)?\\.(obj|o)$>")
set(_simd_expect "${CRD_SIMD_LEVEL_RESOLVED}")
set(_simd_ipo "$<BOOL:$<TARGET_PROPERTY:crd-math-tests,INTERPROCEDURAL_OPTIMIZATION>>")
set(_simd_configs "${CMAKE_CONFIGURATION_TYPES};${CMAKE_BUILD_TYPE}")
foreach(config IN LISTS _simd_configs)
    string(TOUPPER "${config}" _upper)
    get_property(_configured TARGET crd-math-tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_${_upper} SET)
    # Native profiles assign every per-config property after target creation; evaluate its final value at generation.
    if(CRD_NATIVE_PROFILES OR _configured)
        set(_simd_ipo "$<IF:$<CONFIG:${config}>,$<BOOL:$<TARGET_PROPERTY:crd-math-tests,INTERPROCEDURAL_OPTIMIZATION_${_upper}>>,${_simd_ipo}>")
    endif()
endforeach()
if(CRD_NATIVE_PROFILES)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(CRD_PROFILE_${config}_CRD_SIMD_LEVEL MATCHES "^(scalar|sse2)$")
            set(_simd_expect "$<IF:$<CONFIG:${config}>,${CRD_PROFILE_${config}_CRD_SIMD_LEVEL},${_simd_expect}>")
        endif()
    endforeach()
endif()

if(WIN32)
    add_test(
        NAME    crd-simd-emission-check
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/check_simd_emission.ps1"
                -Obj    "${_simd_obj}"
                -Expect "${_simd_expect}"
                -Ipo    "${_simd_ipo}"
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
        NAME    crd-simd-emission-check
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/check_simd_emission.sh"
                --obj    "${_simd_obj}"
                --expect "${_simd_expect}"
                --ipo    "${_simd_ipo}"
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

if(TEST crd-simd-emission-check)
    # This guard consumes this target's object code. Unrelated focused checks must not use an unbuilt/stale object.
    set_tests_properties(crd-simd-emission-check PROPERTIES
        LABELS "cerid.test.target=crd-math-tests" SKIP_RETURN_CODE 77)
endif()
