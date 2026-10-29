# CrdSimd.cmake: the SIMD build contract (ADR-0063 determinism, ADR-0134 runtime CPU dispatch).
#
# Exposes:
#   CRD_SIMD_LEVEL          (cache string)  auto|scalar|sse2|avx2|neon: the highest kernel level the build compiles
#   CRD_DETERMINISTIC_FP    (cache bool)    enforce the ADR-0063 FP contract
#
# Produces:
#   crd-simd-flags          (interface lib) the baseline ISA of general code (x86-64 v1 with SSE2 on x64, NEON on
#                                           ARM64), CRD_SIMD_TARGET for backend.hpp, CRD_KERNEL_LEVEL_CAP and the
#                                           determinism options.
#
# General engine code builds at the architecture's baseline on every lane (ADR-0134 §4): one binary runs on every
# x86-64 CPU, and the per-level kernel units below are the only code above it. CRD_SIMD_LEVEL caps those units:
# scalar gives the portable reference build of the oracle lanes (general code on the scalar backend too), sse2 and avx2
# stop the levels there, auto compiles every level of the architecture. CRD_CPU_LEVEL selects a lower level at run time
# (crd/core/cpu_features.hpp), so one binary qualifies every level as a forced arm.
#
# Anything that links crd-math (transitively) inherits these flags and the determinism contract. Test or bench targets
# that opt out can override `CRD_DETERMINISTIC_FP=OFF` per configure.

include_guard(GLOBAL)

# CRD_SIMD_TARGET values, matching backend.hpp: 0 = scalar, 1 = sse2, 3 = neon (2, the old AVX2 general build, is
# refused there).
set(_CRD_SIMD_TARGET_SCALAR 0)
set(_CRD_SIMD_TARGET_SSE2   1)
set(_CRD_SIMD_TARGET_NEON   3)

set(CRD_SIMD_LEVEL "auto" CACHE STRING "Highest kernel level the build compiles: auto|scalar|sse2|avx2|neon")
set_property(CACHE CRD_SIMD_LEVEL PROPERTY STRINGS auto scalar sse2 avx2 neon)

option(CRD_DETERMINISTIC_FP
    "Enforce the ADR-0063 deterministic floating-point contract (no fast-math, no fp-contract reordering, no x87)"
    ON)

# ---- Resolve the level against the host arch --------------------------------

set(_crd_arch_x64    FALSE)
set(_crd_arch_arm64  FALSE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(amd64|x86_64|AMD64|x64)$")
    set(_crd_arch_x64 TRUE)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(_crd_arch_arm64 TRUE)
endif()

if(CRD_SIMD_LEVEL STREQUAL "native")
    message(FATAL_ERROR
        "[crd-simd] CRD_SIMD_LEVEL=native is gone: general code builds at the baseline ISA and the kernel units are "
        "dispatched at run time (ADR-0134). Use 'auto' for every level of this architecture.")
elseif(NOT CRD_SIMD_LEVEL MATCHES "^(auto|scalar|sse2|avx2|neon)$")
    message(FATAL_ERROR "[crd-simd] Unknown CRD_SIMD_LEVEL: '${CRD_SIMD_LEVEL}' (auto|scalar|sse2|avx2|neon)")
elseif(CRD_SIMD_LEVEL MATCHES "^(sse2|avx2)$" AND NOT _crd_arch_x64)
    message(FATAL_ERROR
        "[crd-simd] CRD_SIMD_LEVEL=${CRD_SIMD_LEVEL} requires x64; host arch is '${CMAKE_SYSTEM_PROCESSOR}'. "
        "Use 'auto', 'scalar', or 'neon'.")
elseif(CRD_SIMD_LEVEL STREQUAL "neon" AND NOT _crd_arch_arm64)
    message(FATAL_ERROR
        "[crd-simd] CRD_SIMD_LEVEL=neon requires ARM64; host arch is '${CMAKE_SYSTEM_PROCESSOR}'. "
        "Use 'auto', 'scalar', 'sse2' or 'avx2'.")
endif()

# The backend general code uses: the architecture's baseline, or scalar for the reference build.
if(CRD_SIMD_LEVEL STREQUAL "scalar" OR NOT (_crd_arch_x64 OR _crd_arch_arm64))
    set(_crd_simd_resolved "scalar")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_SCALAR})
elseif(_crd_arch_x64)
    set(_crd_simd_resolved "sse2")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_SSE2})
else()
    set(_crd_simd_resolved "neon")
    set(_crd_simd_target_value ${_CRD_SIMD_TARGET_NEON})
endif()

# ---- Build the interface target --------------------------------------------

add_library(crd-simd-flags INTERFACE)

# GCC and Clang general code is pinned to x86-64 v1, whatever the toolchain's default or CMAKE_CXX_FLAGS adds (a
# distribution compiler may default to v2 or v3). cl and clang-cl target the x64 baseline without an /arch flag.
if(_crd_arch_x64 AND NOT MSVC)
    target_compile_options(crd-simd-flags INTERFACE $<$<COMPILE_LANGUAGE:CXX,C>:-march=x86-64>)
endif()

# The native Visual Studio profiles share one configure: a scalar configuration takes the scalar backend.
set(_crd_simd_definition "${_crd_simd_target_value}")
if(CRD_NATIVE_PROFILES)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(CRD_PROFILE_${config}_CRD_SIMD_LEVEL STREQUAL "scalar")
            set(_crd_simd_definition "$<IF:$<CONFIG:${config}>,0,${_crd_simd_definition}>")
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
elseif(CRD_SIMD_LEVEL STREQUAL "sse2")
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

# The features a level's variant may add (ADR-0134 §1): the level each extends and its flags (cl compiles every
# intrinsic at any /arch and has no such flag).
set(_CRD_KERNEL_FEATURE_LEVEL_avxvnni avx2)
set(_CRD_KERNEL_FEATURE_LEVEL_avx512vnni avx512)
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(_CRD_KERNEL_FLAGS_FEATURE_avxvnni "")
    set(_CRD_KERNEL_FLAGS_FEATURE_avx512vnni "")
else()
    set(_CRD_KERNEL_FLAGS_FEATURE_avxvnni -mavxvnni)
    set(_CRD_KERNEL_FLAGS_FEATURE_avx512vnni -mavx512vnni)
endif()

# One kernel unit: the sources as the object library <objects> at <level> (and <feature> unless it is empty), joined
# to the archive.
function(_crd_add_kernel_unit archive objects level feature)
    cmake_parse_arguments(PARSE_ARGV 4 arg "" "" "INCLUDES;SOURCES")
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
    # Machine code at compile time, never link-time IL: a unit is entered only through its table, so link-time code
    # generation has no call to inline across it, yet it ran the unit's code generation again in every image that
    # links the archive (an LTCG link of one hesap-fft specimen, five fft units, passed 40 GB on win-release). The
    # objects then hold the instructions the ISA isolation guard reads (ADR-0134 §6).
    set_target_properties(${objects} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF CRD_KERNEL_LEVEL_NAME ${level}
                          CRD_KERNEL_FEATURE_NAME "${feature}" CRD_KERNEL_ARCHIVE ${archive})
    if(feature)
        string(TOUPPER "${feature}" feature_name)
        target_compile_definitions(${objects} PRIVATE CRD_KERNEL_FEATURE_${feature_name}=1)
        target_compile_options(${objects} PRIVATE ${_CRD_KERNEL_FLAGS_FEATURE_${feature}})
    endif()
    if(NOT CMAKE_CONFIGURATION_TYPES)
        # The debug types go to the archive's compile PDB, the one the relocatable package installs.
        get_target_property(archive_binary_dir ${archive} BINARY_DIR)
        set_target_properties(${objects} PROPERTIES
            COMPILE_PDB_NAME ${archive}
            COMPILE_PDB_OUTPUT_DIRECTORY "${archive_binary_dir}/CMakeFiles/${archive}.dir")
    endif()
    target_sources(${archive} PRIVATE $<TARGET_OBJECTS:${objects}>)
endfunction()

# crd_add_cpu_kernels(<archive> NAME <family> LEVELS portable [<level> ...] [VARIANTS <level>+<feature> ...]
#                     [INCLUDES <target> ...] SOURCES <source> ...)
#
# Compiles a kernel family's sources once for each of its LEVELS that CRD_KERNEL_LEVELS holds, as one object library
# per level (<archive>-<family>-<level>) with that level's ISA flags, CRD_KERNEL_LEVEL and the ADR-0063 contract, and
# never crd-simd-flags' global ISA. The objects join the archive, which gets CRD_KERNELS_<FAMILY>: the mask of the
# built levels (bit n is CpuLevel n). LEVELS names the levels the sources have a body for; a CPU between two of them
# runs the lower one. Each of VARIANTS builds one of those levels again with one more feature
# (<archive>-<family>-<level>-<feature>, CRD_KERNEL_FEATURE_<FEATURE>=1 and its own level namespace), and the archive
# gets CRD_KERNELS_<FAMILY>_<LEVEL>_<FEATURE>, 1 when its level was built. The units see the archive's interface
# include directories and those of the INCLUDES targets.
# The sources include only headers that put their inline code in CRD_MATH_LEVEL_NS (crd/math/kernel_level.hpp),
# plain declarations and intrinsics: an inline function compiled at a higher level and shared with general code is the
# defect class this rule removes. Kernel sources live outside the archive's src/.
function(crd_add_cpu_kernels archive)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "NAME" "LEVELS;VARIANTS;INCLUDES;SOURCES")
    if(NOT arg_NAME OR NOT arg_SOURCES OR NOT "portable" IN_LIST arg_LEVELS)
        message(FATAL_ERROR "crd_add_cpu_kernels(${archive}): NAME, SOURCES and LEVELS with portable are required")
    endif()
    string(TOUPPER "${arg_NAME}" family)
    set(mask 0)
    foreach(level IN LISTS arg_LEVELS)
        if(NOT DEFINED _CRD_KERNEL_LEVEL_${level})
            message(FATAL_ERROR "crd_add_cpu_kernels(${archive}): unknown level '${level}'")
        endif()
        if(NOT level IN_LIST CRD_KERNEL_LEVELS)
            continue()
        endif()
        _crd_add_kernel_unit(${archive} ${archive}-${arg_NAME}-${level} ${level} ""
                             INCLUDES ${arg_INCLUDES} SOURCES ${arg_SOURCES})
        math(EXPR mask "${mask} | (1 << ${_CRD_KERNEL_LEVEL_${level}})")
    endforeach()
    target_compile_definitions(${archive} PRIVATE CRD_KERNELS_${family}=${mask})
    foreach(variant IN LISTS arg_VARIANTS)
        string(REPLACE "+" ";" parts "${variant}")
        list(LENGTH parts count)
        set(level "")
        set(feature "")
        if(count EQUAL 2)
            list(GET parts 0 level)
            list(GET parts 1 feature)
        endif()
        if(NOT level IN_LIST arg_LEVELS OR NOT "${_CRD_KERNEL_FEATURE_LEVEL_${feature}}" STREQUAL "${level}")
            message(FATAL_ERROR "crd_add_cpu_kernels(${archive}): variant '${variant}' is not a level of LEVELS "
                                "plus a feature that extends it")
        endif()
        set(built 0)
        if(level IN_LIST CRD_KERNEL_LEVELS)
            _crd_add_kernel_unit(${archive} ${archive}-${arg_NAME}-${level}-${feature} ${level} ${feature}
                                 INCLUDES ${arg_INCLUDES} SOURCES ${arg_SOURCES})
            set(built 1)
        endif()
        string(TOUPPER "${level}_${feature}" variant_name)
        target_compile_definitions(${archive} PRIVATE CRD_KERNELS_${family}_${variant_name}=${built})
    endforeach()
endfunction()

# CRD_SDE: Intel SDE (scripts/install-sde.py), under which the avx512 arms run whatever the host's CPU. The hosted
# lanes that run them export it before the configure; elsewhere it stays empty and no emulated arm exists.
set(CRD_SDE "$ENV{CRD_SDE}" CACHE FILEPATH "Intel SDE executable for the emulated avx512 kernel arms (empty: none)")
# The SDE CPU model every avx512 arm runs on: skx, the level's smallest CPU (AVX-512 F, CD, BW, DQ and VL only).
set(_CRD_SDE_CPUS skx)
set(_CRD_LEVEL_APPLIED_TEST "${CMAKE_CURRENT_LIST_DIR}/../tests/support/kernel-levels/test_cpu_level_applied.cpp")

# crd_add_kernel_level_tests(<test-executable> <filter> [SDE_CPUS <cpu>...] [ENVIRONMENT <var=value>...])
#
# ADR-0134 §8: <prefix>-kernels-at-<level> runs the executable's <filter> once per level CRD_KERNEL_LEVELS holds, with
# CRD_CPU_LEVEL forcing the selection in the one binary (a level above the host is refused, and that arm runs at the
# detected level) and the ENVIRONMENT the suite needs; <prefix> is the executable's name without "-tests". With
# CRD_SDE, <prefix>-kernels-at-avx512-sde-<cpu> runs the filter under SDE's model of each _CRD_SDE_CPUS CPU and each
# SDE_CPUS CPU (a model with the features a suite's variant selects on, ADR-0134 §8) at avx512, and those arms also run
# the hidden [cpu-level-applied] case, which fails when the level was refused. The executable
# gets CRD_TEST_KERNEL_LEVELS_TOP, the CpuLevel value of the highest level the build compiles.
function(crd_add_kernel_level_tests executable filter)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "SDE_CPUS;ENVIRONMENT")
    list(GET CRD_KERNEL_LEVELS -1 top)
    target_compile_definitions(${executable} PRIVATE CRD_TEST_KERNEL_LEVELS_TOP=${_CRD_KERNEL_LEVEL_${top}})
    target_sources(${executable} PRIVATE "${_CRD_LEVEL_APPLIED_TEST}")
    string(REGEX REPLACE "-tests$" "" prefix "${executable}")
    foreach(level IN LISTS CRD_KERNEL_LEVELS)
        string(REPLACE "sse42" "sse4.2" spelling "${level}")
        add_test(NAME ${prefix}-kernels-at-${level} COMMAND ${executable} "${filter}")
        set(environment "CRD_CPU_LEVEL=${spelling}" ${arg_ENVIRONMENT})
        set_tests_properties(${prefix}-kernels-at-${level} PROPERTIES
            ENVIRONMENT "${environment}" LABELS "cerid.test.target=${executable}")
    endforeach()
    if(CRD_SDE AND "avx512" IN_LIST CRD_KERNEL_LEVELS)
        foreach(cpu IN LISTS _CRD_SDE_CPUS arg_SDE_CPUS)
            set(name ${prefix}-kernels-at-avx512-sde-${cpu})
            add_test(NAME ${name}
                     COMMAND "${CRD_SDE}" -${cpu} -- $<TARGET_FILE:${executable}> "${filter},[cpu-level-applied]")
            set(environment "CRD_CPU_LEVEL=avx512" ${arg_ENVIRONMENT})
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "${environment}" TIMEOUT 1800
                                 LABELS "cerid.test.target=${executable}")
        endforeach()
    endif()
endfunction()

# crd_peer_isa_flags(<out>): the measuring host's best ISA for a peer of the vs-reference boards built from source
# (crd_add_bench_peer, runtime/CMakeLists.txt), never for Cerid code (ADR-0134 §8). A peer runs as its own build would
# on that host, one number per workload that CRD_CPU_LEVEL never lowers. GCC and Clang take the compiler's host
# detection. cl has none, so CRD_PEER_CPU_LEVEL caches the level crd::detect_cpus() reports on the configuring host, a
# configure-time run of cpu_features.cpp (the one detection site); a cross build must set it.
function(crd_peer_isa_flags out)
    set(flags "")
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND _crd_arch_x64)
        set(flags /clang:-march=native)
    elseif(NOT MSVC AND _crd_arch_x64)
        set(flags -march=native)
    elseif(NOT MSVC AND _crd_arch_arm64)
        set(flags -mcpu=native)
    elseif(_crd_arch_x64)
        if(NOT CRD_PEER_CPU_LEVEL)
            if(CMAKE_CROSSCOMPILING)
                message(FATAL_ERROR "[crd-simd] a cross build of the board peers needs -DCRD_PEER_CPU_LEVEL=<level>")
            endif()
            try_run(run_result compile_result
                SOURCES "${CMAKE_SOURCE_DIR}/runtime/examples/peers/peer_cpu_level.cpp"
                        "${CMAKE_SOURCE_DIR}/engine/foundation/core/src/cpu_features.cpp"
                CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}/engine/foundation/core/include"
                CXX_STANDARD 20
                COMPILE_OUTPUT_VARIABLE compile_output
                RUN_OUTPUT_VARIABLE level)
            string(STRIP "${level}" level)
            if(NOT compile_result OR NOT run_result EQUAL 0 OR NOT level MATCHES "^(portable|sse2|sse4\\.2|avx2|avx512)$")
                message(FATAL_ERROR "[crd-simd] the board peers' CPU level probe failed ('${level}'):\n${compile_output}")
            endif()
            set(CRD_PEER_CPU_LEVEL "${level}" CACHE STRING "The configuring host's CPU level the board peers build at")
        endif()
        if(CRD_PEER_CPU_LEVEL STREQUAL "avx512")
            set(flags /arch:AVX512)
        elseif(CRD_PEER_CPU_LEVEL STREQUAL "avx2")
            set(flags /arch:AVX2)
        elseif(CRD_PEER_CPU_LEVEL STREQUAL "sse4.2")
            set(flags /arch:SSE4.2)
        elseif(NOT CRD_PEER_CPU_LEVEL MATCHES "^(portable|sse2)$")
            message(FATAL_ERROR "[crd-simd] unknown CRD_PEER_CPU_LEVEL '${CRD_PEER_CPU_LEVEL}'")
        endif()
    endif()
    set(${out} "${flags}" PARENT_SCOPE)
endfunction()

# crd_add_isa_isolation_check(): once, after every target exists (ADR-0134 §6). Writes
# <build>/isa-isolation/<config>.manifest, one line per kernel object (its level, feature and archive), general object
# (its target and the archive or program it builds) and image, and registers crd-check-isa-isolation: no general code
# above the floor, no kernel object above its level, no symbol shared between the levels and general code
# (scripts/check-isa-isolation.py). A built archive or program must have every object it lists. Where objects hold
# link-time IL, the guard judges the linked images, which MSVC links therefore map (/MAP, root CMakeLists.txt). A
# target with CRD_ISA_ROLE "peer" (a board's third-party peer, built at the host's ISA) is not Cerid code and is left out.
function(crd_add_isa_isolation_check)
    if(NOT CRD_BUILD_TESTS OR NOT (_crd_arch_x64 OR _crd_arch_arm64))
        return()
    endif()
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    _crd_collect_build_targets("${CMAKE_SOURCE_DIR}" targets)
    set(content "")
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        get_target_property(role ${target} CRD_ISA_ROLE)
        if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$"
           OR role STREQUAL "peer")
            continue()
        endif()
        get_target_property(level ${target} CRD_KERNEL_LEVEL_NAME)
        if(level)
            get_target_property(feature ${target} CRD_KERNEL_FEATURE_NAME)
            get_target_property(archive ${target} CRD_KERNEL_ARCHIVE)
            set(prefix "kernel\t${level}\t${feature}\t$<TARGET_FILE:${archive}>\t")
        elseif(type STREQUAL "OBJECT_LIBRARY")
            set(prefix "general\t${target}\t\t")
        else()
            set(prefix "general\t${target}\t$<TARGET_FILE:${target}>\t")
        endif()
        set(objects "$<TARGET_OBJECTS:${target}>")
        string(APPEND content "$<$<BOOL:${objects}>:${prefix}$<JOIN:${objects},\n${prefix}>\n>")
        if(type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
            string(APPEND content "image\t${target}\t$<TARGET_FILE:${target}>\n")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/isa-isolation/$<CONFIG>.manifest" CONTENT "${content}")
    if(_crd_arch_x64)
        set(arch x64)
    else()
        set(arch arm64)
    endif()
    set(tools "")
    if(WIN32)
        set(os windows)
    else()
        set(os linux)
        # GCC's slim LTO objects list their symbols through the compiler's plugin (gcc-nm beside gcc-ar).
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_AR MATCHES "gcc-ar(-[0-9.]+)?$")
            string(REGEX REPLACE "gcc-ar(-[0-9.]+)?$" "gcc-nm\\1" gcc_nm "${CMAKE_CXX_COMPILER_AR}")
            list(APPEND tools --gcc-nm "${gcc_nm}")
        endif()
    endif()
    add_test(NAME crd-check-isa-isolation
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/check-isa-isolation.py"
                --manifest "${CMAKE_BINARY_DIR}/isa-isolation/$<CONFIG>.manifest" --arch ${arch} --os ${os} ${tools})
    set_tests_properties(crd-check-isa-isolation PROPERTIES TIMEOUT 1800 ENVIRONMENT "PYTHONUTF8=1")
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
message(STATUS "[crd-simd] CRD_SIMD_LEVEL    = ${CRD_SIMD_LEVEL} (general code ${_crd_simd_summary})")
message(STATUS "[crd-simd] CRD_SIMD_TARGET   = ${_crd_simd_target_value} (0=scalar, 1=sse2, 3=neon)")
message(STATUS "[crd-simd] DETERMINISTIC_FP  = ${CRD_DETERMINISTIC_FP}")
message(STATUS "[crd-simd] KERNEL_LEVELS     = ${CRD_KERNEL_LEVELS}")
message(STATUS "[crd-simd] host SYSTEM_PROC  = ${CMAKE_SYSTEM_PROCESSOR}")

# The backend general code compiles to (scalar, sse2 or neon), for checks that read the build's machine code.
set(CRD_SIMD_LEVEL_RESOLVED "${_crd_simd_resolved}" CACHE INTERNAL "The SIMD backend of general code")
