# CrdSimd.cmake — Phase 3.1 v0a SIMD + deterministic-FP configuration.
#
# Exposes:
#   CRD_SIMD_LEVEL          (cache string)  auto|scalar|sse2|avx2|neon|native
#   CRD_DETERMINISTIC_FP    (cache bool)    enforce ADR-0063 FP contract
#
# Produces:
#   crd-simd-flags          (interface lib) — emits the right compile flags +
#                                             defines CRD_SIMD_TARGET so the
#                                             code in backend.hpp is decided
#                                             at the build-system level rather
#                                             than relying on compiler-defined
#                                             ISA macros (MSVC doesn't define
#                                             __AVX2__ unless /arch:AVX2 is
#                                             explicitly passed; this avoids
#                                             that footgun entirely).
#
# Anything that links crd-math (transitively) inherits the SIMD flags + the
# determinism contract. Test or bench targets that opt out can override
# `CRD_DETERMINISTIC_FP=OFF` per-configure.

include_guard(GLOBAL)

# CRD_SIMD_TARGET integer values must match backend.hpp:
#   0 = scalar, 1 = sse2, 2 = avx2, 3 = neon
set(_CRD_SIMD_TARGET_SCALAR 0)
set(_CRD_SIMD_TARGET_SSE2   1)
set(_CRD_SIMD_TARGET_AVX2   2)
set(_CRD_SIMD_TARGET_NEON   3)

set(CRD_SIMD_LEVEL "auto" CACHE STRING
    "SIMD instruction level: auto|scalar|sse2|avx2|neon|native")
set_property(CACHE CRD_SIMD_LEVEL PROPERTY STRINGS
    auto scalar sse2 avx2 neon native)

option(CRD_DETERMINISTIC_FP
    "Enforce the ADR-0063 deterministic floating-point contract (no fast-math, no fp-contract reordering, no x87)"
    ON)

# ---- Resolve "auto" against the host arch ----------------------------------

set(_crd_arch_x64    FALSE)
set(_crd_arch_arm64  FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(amd64|x86_64|AMD64|x64)$")
    set(_crd_arch_x64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(_crd_arch_arm64 TRUE)
endif()

if(CRD_SIMD_LEVEL STREQUAL "auto")
    if(_crd_arch_x64)
        # AVX2 has been baseline on Intel Haswell (2013) + AMD Excavator (2015);
        # safe default in 2026. Drop to sse2 if you still ship to pre-Haswell.
        set(_crd_simd_resolved "avx2")
    elseif(_crd_arch_arm64)
        set(_crd_simd_resolved "neon")
    else()
        set(_crd_simd_resolved "scalar")
    endif()
else()
    set(_crd_simd_resolved "${CRD_SIMD_LEVEL}")
endif()

# Validate against the host arch — refuse e.g. avx2 on ARM64.
if(_crd_simd_resolved STREQUAL "avx2" OR _crd_simd_resolved STREQUAL "sse2")
    if(NOT _crd_arch_x64)
        message(FATAL_ERROR
            "[crd-simd] CRD_SIMD_LEVEL=${_crd_simd_resolved} requires x64; "
            "host arch is '${CMAKE_SYSTEM_PROCESSOR}'. Use 'auto', 'scalar', or 'neon'.")
    endif()
elseif(_crd_simd_resolved STREQUAL "neon")
    if(NOT _crd_arch_arm64)
        message(FATAL_ERROR
            "[crd-simd] CRD_SIMD_LEVEL=neon requires ARM64; "
            "host arch is '${CMAKE_SYSTEM_PROCESSOR}'. Use 'auto', 'scalar', or 'avx2'/'sse2'.")
    endif()
endif()

# ---- Build the interface target --------------------------------------------

add_library(crd-simd-flags INTERFACE)

# Pick the integer macro + ISA flags per resolved level.
if(_crd_simd_resolved STREQUAL "avx2")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_AVX2})
    if(MSVC)
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:/arch:AVX2>)
    else()
        # NOTE: NOT -mfma. Cerid's mul_add explicitly does (a*b)+c with two
        # roundings (ADR-0063); enabling FMA hardware would let -ffp-contract
        # contract back into a single rounding and break replay-hash parity.
        # The /fp:precise + -ffp-contract=off below also block contraction,
        # but skipping -mfma adds belt and suspenders.
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:-mavx2>
            $<$<COMPILE_LANGUAGE:CXX,C>:-msse4.2>)
    endif()
elseif(_crd_simd_resolved STREQUAL "sse2")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_SSE2})
    if(NOT MSVC)
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:-msse2>)
    endif()
    # MSVC x64 has SSE2 as the ABI baseline; no flag needed.
elseif(_crd_simd_resolved STREQUAL "neon")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_NEON})
    # NEON is baseline on AArch64; nothing to add.
elseif(_crd_simd_resolved STREQUAL "native")
    # Best-of-host. CRD_SIMD_TARGET picks the integer based on arch.
    if(_crd_arch_x64)
        set(_crd_simd_target_value ${_CRD_SIMD_TARGET_AVX2})
    elseif(_crd_arch_arm64)
        set(_crd_simd_target_value ${_CRD_SIMD_TARGET_NEON})
    else()
        set(_crd_simd_target_value ${_CRD_SIMD_TARGET_SCALAR})
    endif()
    if(MSVC)
        # MSVC has no `-march=native`. Treat 'native' as 'avx2' on x64.
        if(_crd_arch_x64)
            target_compile_options(crd-simd-flags INTERFACE
                $<$<COMPILE_LANGUAGE:CXX,C>:/arch:AVX2>)
        endif()
    else()
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:-march=native>)
    endif()
elseif(_crd_simd_resolved STREQUAL "scalar")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_SCALAR})
else()
    message(FATAL_ERROR "[crd-simd] Unknown CRD_SIMD_LEVEL: '${_crd_simd_resolved}'")
endif()

target_compile_definitions(crd-simd-flags INTERFACE
    CRD_SIMD_TARGET=${_crd_simd_target_value})

# ---- Determinism: ADR-0063 FP contract -------------------------------------
#
# The contract bans:
#   - reordering of float operations (fast-math, /fp:fast)
#   - contracting (a*b)+c into a hardware FMA (single rounding)
#   - x87 80-bit intermediate precision on legacy x86
#
# Compile flags below match what eylem (and crd-hesap later) will rely on.
# When CRD_DETERMINISTIC_FP is OFF, micro-benchmarks can opt back into
# fast-math for performance comparisons — but the production build path
# always has it ON.

if(CRD_DETERMINISTIC_FP)
    if(MSVC)
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:/fp:precise>)
    else()
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:-fno-fast-math>
            $<$<COMPILE_LANGUAGE:CXX,C>:-ffp-contract=off>)
        if(_crd_arch_x64)
            # SSE2-or-better math (no x87 80-bit intermediates).
            target_compile_options(crd-simd-flags INTERFACE
                $<$<COMPILE_LANGUAGE:CXX,C>:-mfpmath=sse>)
        endif()
    endif()
    target_compile_definitions(crd-simd-flags INTERFACE
        CRD_DETERMINISTIC_FP=1)
else()
    target_compile_definitions(crd-simd-flags INTERFACE
        CRD_DETERMINISTIC_FP=0)
endif()

# ---- Configure summary -----------------------------------------------------

string(TOUPPER "${_crd_simd_resolved}" _crd_simd_summary)
message(STATUS "[crd-simd] CRD_SIMD_LEVEL    = ${CRD_SIMD_LEVEL} -> ${_crd_simd_summary}")
message(STATUS "[crd-simd] CRD_SIMD_TARGET   = ${_crd_simd_target_value} (0=scalar, 1=sse2, 2=avx2, 3=neon)")
message(STATUS "[crd-simd] DETERMINISTIC_FP  = ${CRD_DETERMINISTIC_FP}")
message(STATUS "[crd-simd] host SYSTEM_PROC  = ${CMAKE_SYSTEM_PROCESSOR}")

# Persist resolution for downstream code that wants to query it.
set(CRD_SIMD_LEVEL_RESOLVED "${_crd_simd_resolved}" CACHE INTERNAL
    "Resolved SIMD level for this build configuration")
