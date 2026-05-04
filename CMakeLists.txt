cmake_minimum_required(VERSION 3.25)

project(CRD
    VERSION 0.1.0
    DESCRIPTION "CRD Game/Simulation Engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CRD_BUILD_TESTS      "Build test executables"           ON)
option(CRD_BUILD_BENCHMARKS "Build benchmark executable"       ON)
option(CRD_ENABLE_ASSERTS   "Enable CRD_ASSERT macros"        ON)
option(CRD_ENABLE_PROFILING "Enable profiling instrumentation" OFF)
option(CRD_ENABLE_ASAN      "Enable Address Sanitizer"         OFF)
option(CRD_ENABLE_UBSAN     "Enable Undefined Behavior San."   OFF)
option(CRD_SHIPPING         "Maximum optimizations + debug symbol generation for crash diagnostics" OFF)
option(CRD_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(CRD_ENABLE_PCH       "Enable default precompiled headers" ON)

# ---- Log compile-time minimum level ---------------------------------------
# Anything strictly below this level is compiled out to ((void)0).
# Valid values: Trace, Debug, Info, Warn, Error, Critical
set(CRD_LOG_MIN_LEVEL "" CACHE STRING
    "Compile-time minimum log level (Trace/Debug/Info/Warn/Error/Critical). Empty = auto.")

if(CRD_LOG_MIN_LEVEL STREQUAL "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(CRD_LOG_MIN_LEVEL_RESOLVED "Trace")
    else()
        set(CRD_LOG_MIN_LEVEL_RESOLVED "Info")
    endif()
else()
    set(CRD_LOG_MIN_LEVEL_RESOLVED "${CRD_LOG_MIN_LEVEL}")
endif()

if(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Trace")
    set(CRD_LOG_MIN_LEVEL_NUM 0)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Debug")
    set(CRD_LOG_MIN_LEVEL_NUM 1)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Info")
    set(CRD_LOG_MIN_LEVEL_NUM 2)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Warn")
    set(CRD_LOG_MIN_LEVEL_NUM 3)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Error")
    set(CRD_LOG_MIN_LEVEL_NUM 4)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Critical")
    set(CRD_LOG_MIN_LEVEL_NUM 5)
else()
    message(FATAL_ERROR "Invalid CRD_LOG_MIN_LEVEL: '${CRD_LOG_MIN_LEVEL_RESOLVED}'")
endif()
message(STATUS "[crd] Log compile-time min level: ${CRD_LOG_MIN_LEVEL_RESOLVED} (${CRD_LOG_MIN_LEVEL_NUM})")

# ---- NDEBUG for optimised configs ---------------------------------------
# CMake's MSVC toolchain may leave CMAKE_CXX_FLAGS_RELEASE empty (unlike the
# C toolchain), so NDEBUG is not always propagated automatically. Ensure the
# standard Release/MinSizeRel configs always get the define, matching the
# intent of every #if defined(NDEBUG) guard in the codebase.
add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)

# ---- Sanitizers ---------------------------------------------------------

if(CRD_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "[crd] Address Sanitizer ENABLED")
endif()

if(CRD_ENABLE_UBSAN)
    if(MSVC)
        message(WARNING "[crd] UBSan not supported on MSVC, ignoring")
    else()
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
        message(STATUS "[crd] UB Sanitizer ENABLED")
    endif()
endif()

# ----- Shipping optimizations + debug symbols --------------------------------

if(CRD_SHIPPING)
    if(MSVC)
        # /fp:fast  - allow float reassociation, no NaN/Inf checks
        # /arch:AVX2 - target AVX2 (all x86 desktops since ~2013)
        # /Gw        - place each global in its own COMDAT for dead-stripping
        # /Zi        - emit a separate PDB (required for crash dump symbolication)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX,C>:/fp:fast>
            $<$<COMPILE_LANGUAGE:CXX,C>:/arch:AVX2>
            $<$<COMPILE_LANGUAGE:CXX,C>:/Gw>
            $<$<COMPILE_LANGUAGE:CXX,C>:/Zi>
        )
        # /DEBUG:FULL  - full PDB (not /DEBUG:FASTLINK) — required for WinDbg post-mortem
        # /OPT:REF     - strip unreferenced functions (dead code elimination)
        # /OPT:ICF     - merge identical COMDATs (binary size reduction)
        add_link_options(/DEBUG:FULL /OPT:REF /OPT:ICF)
    else()
        # x86-64-v3 = AVX2 + FMA3 + BMI + BMI2 + MOVBE (2013+ desktops)
        # -ffast-math   - fast floating point (implies -fno-math-errno etc.)
        # -g             - always emit DWARF debug info for crash symbolication
        # --build-id     - embed a unique build ID so crash reports can identify the binary
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX,C>:-march=x86-64-v3>
            $<$<COMPILE_LANGUAGE:CXX,C>:-ffast-math>
            $<$<COMPILE_LANGUAGE:CXX,C>:-g>
        )
        add_link_options(-Wl,--build-id)
    endif()
    message(STATUS "[crd] Shipping: max optimizations + debug symbols ON")
endif()

# ----- Compiler Warnings (Cerid targets only — propagates via crd-core) -----
# Third-party CPM packages never see these flags; they inherit them only
# if they transitively link crd-core, which they don't.

add_library(crd-warnings INTERFACE)

if(MSVC)
    # Gate flags on CXX/C only — MASM (ASM_MASM) does not accept these options.
    target_compile_options(crd-warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX,C>:/W4>
        $<$<COMPILE_LANGUAGE:CXX,C>:/WX>
        $<$<COMPILE_LANGUAGE:CXX,C>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:CXX,C>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX,C>:/EHsc>
    )
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(crd-warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:preprocessor>
        )
    endif()
else()
    # Gate on CXX/C so AT&T .S assembly files (ASM language) are not affected.
    target_compile_options(crd-warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wshadow>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wdouble-promotion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wnull-dereference>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wformat=2>
    )
endif()

# ----- Dependencies (via CPM) ----------------------------------------------

include(cmake/CPM.cmake)

if (CRD_BUILD_TESTS)
    CPMAddPackage("gh:catchorg/Catch2@3.7.1")
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()

# GLFW: window + input backend for crd-platform.
# We pin the 3.4 tag and disable everything we don't ship.
# GLFW 3.4 on Linux: disable the X11 backend so its GLX context code (which needs
# GL/gl.h) is never compiled. Wayland packages are installed in CI and suffice for
# headless test runs. X11 users can opt back in by overriding GLFW_BUILD_X11.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_X11     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ON  CACHE BOOL "" FORCE)
endif()
CPMAddPackage("gh:glfw/glfw#3.4")

CPMAddPackage("gh:marzer/tomlplusplus@3.4.0")

# zstd 1.5.5 has cmake_minimum_required(VERSION 2.8.12.2) which modern CMake rejects.
# Setting CMAKE_POLICY_VERSION_MINIMUM tells CMake to treat it as 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
CPMAddPackage(
    NAME zstd
    GITHUB_REPOSITORY facebook/zstd
    GIT_TAG v1.5.5
    SOURCE_SUBDIR build/cmake
    OPTIONS
        "ZSTD_BUILD_PROGRAMS OFF"
        "ZSTD_BUILD_TESTS OFF"
        "ZSTD_BUILD_SHARED OFF"
        "ZSTD_BUILD_STATIC ON"
)
unset(CMAKE_POLICY_VERSION_MINIMUM)

# stb: single-header image/font/audio utilities (DOWNLOAD_ONLY — no cmake build).
# Phase 2.7: stb_image used by the texture cook handler. GIT_TAG master because
# stb has no semver tags (see docs/debt.md).
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG master
    DOWNLOAD_ONLY YES
)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

CPMAddPackage("gh:ocornut/imgui#v1.92.0-docking")

# ----- Helper: auto-collect sources with CONFIGURE_DEPENDS -----------------

function(crd_collect_sources out_var directory)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${directory}/*.cpp"
    "${directory}/*.hpp"
    "${directory}/*.h"
    )
    set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

function(crd_enable_default_pch target_name)
    if(NOT CRD_ENABLE_PCH)
        return()
    endif()

    target_precompile_headers(${target_name} PRIVATE
        <crd/core/pch.hpp>
    )
endfunction()

find_program(CRD_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")

function(crd_compile_glsl out_var output_dir)
    if(NOT CRD_GLSLANG_VALIDATOR)
        message(FATAL_ERROR "glslangValidator not found; required for Vulkan shader compilation")
    endif()

    file(MAKE_DIRECTORY "${output_dir}")
    set(outputs)
    foreach(shader IN LISTS ARGN)
        get_filename_component(shader_name "${shader}" NAME)
        set(spv "${output_dir}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CRD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -o "${spv}" "${shader}"
            DEPENDS "${shader}"
            VERBATIM
        )
        list(APPEND outputs "${spv}")
    endforeach()
    set(${out_var} ${outputs} PARENT_SCOPE)
endfunction()

# ---- CMAKE Tidy ---------------------------------------------------------
if(CRD_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
        message(STATUS "[crd] clang-tidy ENABLED")
    endif()
endif()


# ----- Engine Modules ------------------------------------------------------

add_subdirectory(engine/core)
add_subdirectory(engine/memory)
add_subdirectory(engine/math)
add_subdirectory(engine/containers)
add_subdirectory(engine/log)
add_subdirectory(engine/platform)
add_subdirectory(engine/app)
add_subdirectory(engine/config)
add_subdirectory(engine/imgui)
add_subdirectory(engine/renderer)
add_subdirectory(engine/shader)
add_subdirectory(engine/rhi)
add_subdirectory(engine/rhi-vulkan)
add_subdirectory(engine/jobs)
add_subdirectory(engine/resources)
# add_subdirectory(engine/graphics)
# add_subdirectory(engine/scripting)

# ----- Tools ----------------------------------------------------------------

add_subdirectory(tools/asset_cooker)

# Optional 'cook' target: cmake --build --preset win-debug --target cook
# Requires CRD_COOK_ROOT and CRD_COOK_OUT to be set (e.g. via -D flags).
if(CRD_COOK_ROOT AND CRD_COOK_OUT)
    add_custom_target(cook
        COMMAND asset_cooker cook --root "${CRD_COOK_ROOT}" --out "${CRD_COOK_OUT}"
        DEPENDS asset_cooker
        COMMENT "Cooking assets from ${CRD_COOK_ROOT} into ${CRD_COOK_OUT}"
    )
endif()

# ----- Runtime executable ---------------------------------------------------

add_subdirectory(runtime)

# ----- Tests ----------------------------------------------------------------

if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
