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

# Native ISA variants share a solution but never inherit the AVX2 flag from another profile.
set(_crd_default_isa "1")
if(CRD_NATIVE_PROFILES)
    set(_crd_isa_configs)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(NOT CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "auto")
            list(APPEND _crd_isa_configs "${config}")
        endif()
    endforeach()
    list(JOIN _crd_isa_configs "," _crd_isa_configs)
    set(_crd_default_isa "$<NOT:$<CONFIG:${_crd_isa_configs}>>")
endif()

# Pick the integer macro + ISA flags per resolved level.
if(_crd_simd_resolved STREQUAL "avx2")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_AVX2})
    if(MSVC)
        target_compile_options(crd-simd-flags INTERFACE
            $<$<AND:${_crd_default_isa},$<COMPILE_LANGUAGE:CXX,C>>:/arch:AVX2>)
    else()
        # -mfma is REQUIRED: crd-math's single-rounded simd::fma() (Vec8f/Vec4d,
        # used by the crd-hesap microkernels per ADR-0082) lowers to the
        # _mm256_fmadd_{ps,pd} intrinsic, which gcc/clang gate behind -mfma
        # (unlike MSVC /arch:AVX2, which bundles FMA). Without it the always_inline
        # intrinsic fails with "target specific option mismatch" on every gcc/clang
        # AVX2 build — the v0-close full sweep was the first to catch this.
        #
        # This does NOT weaken the ADR-0063 determinism contract: mul_add stays
        # two-rounded. mul_add is `(a*b)+c` built from separate _mm256_mul_pd +
        # _mm256_add_pd intrinsics, and -ffp-contract=off (set below, propagated
        # to every TU via crd-math's PUBLIC link of this target) blocks the
        # compiler from contracting them into an FMA. Only the EXPLICIT simd::fma()
        # call emits the hardware FMA. The SIMD/scalar bit-exact parity tests on
        # the Linux configs are the standing guard that mul_add was not contracted.
        target_compile_options(crd-simd-flags INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:-mavx2>
            $<$<COMPILE_LANGUAGE:CXX,C>:-mfma>
            $<$<COMPILE_LANGUAGE:CXX,C>:-msse4.2>
            # F16C (VCVTPS2PH/VCVTPH2PS — crd/math/float_convert.hpp batch f16).
            # Every AVX2 CPU has F16C (it predates Haswell); MSVC /arch:AVX2
            # already implies it, GCC/Clang need the explicit flag.
            $<$<COMPILE_LANGUAGE:CXX,C>:-mf16c>)
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
                $<$<AND:${_crd_default_isa},$<COMPILE_LANGUAGE:CXX,C>>:/arch:AVX2>)
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

set(_crd_simd_definition "${_crd_simd_target_value}")
if(CRD_NATIVE_PROFILES)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "scalar")
            set(_crd_simd_definition "$<IF:$<CONFIG:${config}>,0,${_crd_simd_definition}>")
        elseif(CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "sse2")
            set(_crd_simd_definition "$<IF:$<CONFIG:${config}>,1,${_crd_simd_definition}>")
        endif()
    endforeach()
endif()
target_compile_definitions(crd-simd-flags INTERFACE CRD_SIMD_TARGET=${_crd_simd_definition})

# ---- Per-level kernel units (SIMD-DISPATCH, ADR-0134 §4-5) ------------------
#
# CRD_KERNEL_LEVELS: the CPU levels this build compiles kernel units for, lowest first (the names are the CpuLevel
# spellings of crd/core/cpu_features.hpp without the dot). CRD_KERNEL_LEVEL_CAP, on crd-simd-flags, is the CpuLevel
# value of the highest one: the dispatcher never selects above it (crd::math::kernel_level_cap()). Under the native
# Visual Studio profiles every configuration compiles the auto set and a scalar or sse2 configuration caps lower.
set(_CRD_KERNEL_LEVEL_portable 0)
set(_CRD_KERNEL_LEVEL_sse2 1)
set(_CRD_KERNEL_LEVEL_sse42 2)
set(_CRD_KERNEL_LEVEL_avx2 3)
set(_CRD_KERNEL_LEVEL_avx512 4)
set(_CRD_KERNEL_LEVEL_neon 5)
if(_crd_simd_resolved STREQUAL "scalar")
    set(CRD_KERNEL_LEVELS portable)
elseif(_crd_simd_resolved STREQUAL "sse2")
    set(CRD_KERNEL_LEVELS portable sse2)
elseif(CRD_SIMD_LEVEL STREQUAL "avx2")
    set(CRD_KERNEL_LEVELS portable sse2 sse42 avx2)
elseif(_crd_arch_x64)
    set(CRD_KERNEL_LEVELS portable sse2 sse42 avx2 avx512)
elseif(_crd_arch_arm64)
    set(CRD_KERNEL_LEVELS portable neon)
else()
    set(CRD_KERNEL_LEVELS portable)
endif()
list(GET CRD_KERNEL_LEVELS -1 _crd_kernel_top)
set(_crd_kernel_cap ${_CRD_KERNEL_LEVEL_${_crd_kernel_top}})
if(CRD_NATIVE_PROFILES)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "scalar")
            set(_crd_kernel_cap "$<IF:$<CONFIG:${config}>,${_CRD_KERNEL_LEVEL_portable},${_crd_kernel_cap}>")
        elseif(CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "sse2")
            set(_crd_kernel_cap "$<IF:$<CONFIG:${config}>,${_CRD_KERNEL_LEVEL_sse2},${_crd_kernel_cap}>")
        endif()
    endforeach()
endif()
target_compile_definitions(crd-simd-flags INTERFACE CRD_KERNEL_LEVEL_CAP=${_crd_kernel_cap})

# Each level's ISA flags (x86-64 psABI levels; MSVC's /arch, which clang-cl spells for AVX2 and AVX-512 only).
# Portable and sse2 units are pinned to the x86-64 baseline on GCC and Clang, whatever CMAKE_CXX_FLAGS adds.
set(_CRD_KERNEL_FLAGS_portable "")
set(_CRD_KERNEL_FLAGS_sse2 "")
set(_CRD_KERNEL_FLAGS_neon "")
if(MSVC)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(_CRD_KERNEL_FLAGS_sse42 /clang:-march=x86-64-v2)
    else()
        set(_CRD_KERNEL_FLAGS_sse42 /arch:SSE4.2)
    endif()
    set(_CRD_KERNEL_FLAGS_avx2 /arch:AVX2)
    set(_CRD_KERNEL_FLAGS_avx512 /arch:AVX512)
elseif(_crd_arch_x64)
    set(_CRD_KERNEL_FLAGS_portable -march=x86-64)
    set(_CRD_KERNEL_FLAGS_sse2 -march=x86-64)
    set(_CRD_KERNEL_FLAGS_sse42 -march=x86-64-v2)
    set(_CRD_KERNEL_FLAGS_avx2 -march=x86-64-v3)
    set(_CRD_KERNEL_FLAGS_avx512 -march=x86-64-v4)
endif()

# crd_add_cpu_kernels(<archive> NAME <family> LEVELS portable [<level> ...] [INCLUDES <target> ...]
#                     SOURCES <source> ...)
#
# Compiles a kernel family's sources once for each of its LEVELS that CRD_KERNEL_LEVELS holds, as one object library
# per level (<archive>-<family>-<level>) with that level's ISA flags, CRD_KERNEL_LEVEL and the ADR-0063 contract, and
# never crd-simd-flags' global ISA. The objects join the archive, which gets CRD_KERNELS_<FAMILY>: the mask of the
# built levels (bit n is CpuLevel n). LEVELS names the levels the sources have a body for; a CPU between two of them
# runs the lower one. The units see the archive's interface include directories and those of the INCLUDES targets.
# The sources include only headers that put their inline code in CRD_MATH_LEVEL_NS (crd/math/kernel_level.hpp),
# plain declarations and intrinsics: an inline function compiled at a higher level and shared with general code is the
# defect class this rule removes. Kernel sources live outside the archive's src/.
function(crd_add_cpu_kernels archive)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME" "LEVELS;INCLUDES;SOURCES")
    if(NOT arg_NAME OR NOT arg_SOURCES OR NOT "portable" IN_LIST arg_LEVELS)
        message(FATAL_ERROR "crd_add_cpu_kernels(${archive}): NAME, SOURCES and LEVELS with portable are required")
    endif()
    set(mask 0)
    foreach(level IN LISTS arg_LEVELS)
        if(NOT DEFINED _CRD_KERNEL_LEVEL_${level})
            message(FATAL_ERROR "crd_add_cpu_kernels(${archive}): unknown level '${level}'")
        endif()
        if(NOT level IN_LIST CRD_KERNEL_LEVELS)
            continue()
        endif()
        set(objects ${archive}-${arg_NAME}-${level})
        add_library(${objects} OBJECT ${arg_SOURCES})
        target_include_directories(${objects} PRIVATE $<TARGET_PROPERTY:${archive},INTERFACE_INCLUDE_DIRECTORIES>)
        foreach(target IN LISTS arg_INCLUDES)
            target_include_directories(${objects} PRIVATE $<TARGET_PROPERTY:${target},INTERFACE_INCLUDE_DIRECTORIES>)
        endforeach()
        target_link_libraries(${objects} PRIVATE crd-core)
        target_compile_features(${objects} PRIVATE cxx_std_20)
        target_compile_definitions(${objects} PRIVATE CRD_KERNEL_LEVEL=${_CRD_KERNEL_LEVEL_${level}}
                                   ${CRD_FP_CONTRACT_DEFINITION})
        target_compile_options(${objects} PRIVATE ${CRD_FP_CONTRACT_OPTIONS} ${_CRD_KERNEL_FLAGS_${level}})
        if(NOT CMAKE_CONFIGURATION_TYPES)
            # The debug types go to the archive's compile PDB, the one the relocatable package installs.
            get_target_property(archive_binary_dir ${archive} BINARY_DIR)
            set_target_properties(${objects} PROPERTIES
                COMPILE_PDB_NAME ${archive}
                COMPILE_PDB_OUTPUT_DIRECTORY "${archive_binary_dir}/CMakeFiles/${archive}.dir")
        endif()
        target_sources(${archive} PRIVATE $<TARGET_OBJECTS:${objects}>)
        math(EXPR mask "${mask} | (1 << ${_CRD_KERNEL_LEVEL_${level}})")
    endforeach()
    string(TOUPPER "${arg_NAME}" family)
    target_compile_definitions(${archive} PRIVATE CRD_KERNELS_${family}=${mask})
endfunction()

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
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl. DO NOT use /fp:precise here: clang-cl maps it to
        # -ffp-model=precise, which forces within-statement FMA contraction ON
        # *and* then errors when overridden (-Werror,-Woverriding-option).
        # clang-cl's default FP model is already IEEE-precise (no fast-math); the
        # ONLY thing it gets wrong for ADR-0063 is that contraction defaults to
        # on. So pass just -ffp-contract=off (via the /clang: forwarding escape
        # hatch — the bare flag is rejected with -Wunknown-argument). Without
        # this, clang-cl contracted `a -= b*c` into a single-rounded FMA and
        # produced different bits than cl/gcc — e.g. `4 - (2/3)*6` is exactly 0
        # with separate mul+sub but ~2.2e-16 under FMA, which made the LU
        # exact-singular `== 0` detector miss on win-clang-cl-shipping.
        # (Explicit simd::fma() still emits a hardware FMA — that is intended.)
        set(CRD_FP_CONTRACT_OPTIONS $<$<COMPILE_LANGUAGE:CXX,C>:/clang:-ffp-contract=off>)
    elseif(MSVC)
        set(CRD_FP_CONTRACT_OPTIONS $<$<COMPILE_LANGUAGE:CXX,C>:/fp:precise>)
    else()
        set(CRD_FP_CONTRACT_OPTIONS
            $<$<COMPILE_LANGUAGE:CXX,C>:-fno-fast-math>
            $<$<COMPILE_LANGUAGE:CXX,C>:-ffp-contract=off>)
        if(_crd_arch_x64)
            # SSE2-or-better math (no x87 80-bit intermediates).
            list(APPEND CRD_FP_CONTRACT_OPTIONS $<$<COMPILE_LANGUAGE:CXX,C>:-mfpmath=sse>)
        endif()
    endif()
    set(CRD_FP_CONTRACT_DEFINITION CRD_DETERMINISTIC_FP=1)
else()
    set(CRD_FP_CONTRACT_OPTIONS "")
    set(CRD_FP_CONTRACT_DEFINITION CRD_DETERMINISTIC_FP=0)
endif()
# The contract's options and definition, which the per-level kernel units take as well (crd_add_cpu_kernels).
target_compile_options(crd-simd-flags INTERFACE ${CRD_FP_CONTRACT_OPTIONS})
target_compile_definitions(crd-simd-flags INTERFACE ${CRD_FP_CONTRACT_DEFINITION})


# ---- Configure summary -----------------------------------------------------

string(TOUPPER "${_crd_simd_resolved}" _crd_simd_summary)
message(STATUS "[crd-simd] CRD_SIMD_LEVEL    = ${CRD_SIMD_LEVEL} -> ${_crd_simd_summary}")
message(STATUS "[crd-simd] CRD_SIMD_TARGET   = ${_crd_simd_target_value} (0=scalar, 1=sse2, 2=avx2, 3=neon)")
message(STATUS "[crd-simd] DETERMINISTIC_FP  = ${CRD_DETERMINISTIC_FP}")
message(STATUS "[crd-simd] KERNEL_LEVELS     = ${CRD_KERNEL_LEVELS}")
message(STATUS "[crd-simd] host SYSTEM_PROC  = ${CMAKE_SYSTEM_PROCESSOR}")

# Persist resolution for downstream code that wants to query it.
set(CRD_SIMD_LEVEL_RESOLVED "${_crd_simd_resolved}" CACHE INTERNAL
    "Resolved SIMD level for this build configuration")

# The MSVC ISA flag this resolution actually applied (empty when none). Exported because clang-tidy DROPS
# `/`-spelled flags that arrive through the compile command, so the win-tidy gate has to re-state the ISA
# flag via `--extra-arg=` (see the CRD_ENABLE_CLANG_TIDY block in the root CMakeLists). Deriving it from the
# same branch that adds the real flag is what keeps the two from drifting apart.
set(_crd_simd_msvc_arch "")
if(MSVC AND (_crd_simd_resolved STREQUAL "avx2" OR (_crd_simd_resolved STREQUAL "native" AND _crd_arch_x64)))
    set(_crd_simd_msvc_arch "/arch:AVX2")
endif()
set(CRD_SIMD_MSVC_ARCH_FLAG "${_crd_simd_msvc_arch}" CACHE INTERNAL
    "The MSVC /arch: flag this SIMD resolution applies (empty if none) — mirrored into the clang-tidy gate")
