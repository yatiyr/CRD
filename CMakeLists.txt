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

# ----- Compiler Warnings --------------------------------------------------

if (MSVC)
    # Gate flags on CXX/C only — MASM (ASM_MASM) does not accept these options.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX,C>:/W4>
        $<$<COMPILE_LANGUAGE:CXX,C>:/WX>
        $<$<COMPILE_LANGUAGE:CXX,C>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:CXX,C>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX,C>:/EHsc>
    )

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        add_compile_options($<$<COMPILE_LANGUAGE:CXX,C>:/Zc:preprocessor>)
    endif()
else()
    # Gate on CXX/C so AT&T .S assembly files (ASM language) are not affected.
    add_compile_options(
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wpedantic>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wshadow>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wdouble-promotion>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wnull-dereference>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wformat=2>
    )
endif()

# ----- Dependencies (via CPM) ----------------------------------------------

include(cmake/CPM.cmake)

if (CRD_BUILD_TESTS)
    CPMAddPackage("gh:catchorg/Catch2@3.7.1")
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)

    # Catch2 is a third-party source tree; strip our -Werror/-Wconversion flags
    # from its targets so upstream -Wconversion hits in benchmark code don't
    # fail our build (same treatment as GLFW below).
    foreach(_catch2_target IN ITEMS Catch2 Catch2WithMain)
        if(TARGET ${_catch2_target})
            if(MSVC)
                target_compile_options(${_catch2_target} PRIVATE /W0 /WX-)
            else()
                target_compile_options(${_catch2_target} PRIVATE -w)
            endif()
        endif()
    endforeach()
endif()

# GLFW: window + input backend for crd-platform.
# We pin the 3.4 tag and disable everything we don't ship (docs, examples,
# tests, the loader stubs). GLFW_BUILD_WAYLAND/X11 stay at their platform
# defaults so Linux CI can still build later.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
CPMAddPackage("gh:glfw/glfw#3.4")

# GLFW is a third-party C source tree. Its warnings shouldn't fail OUR build,
# so strip /WX (and the warning level) from GLFW targets only.
if(TARGET glfw)
    if(MSVC)
        target_compile_options(glfw PRIVATE /W0 /WX-)
    else()
        target_compile_options(glfw PRIVATE -w)
    endif()
endif()

CPMAddPackage("gh:marzer/tomlplusplus@3.4.0")
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
# add_subdirectory(engine/graphics)
# add_subdirectory(engine/scripting)

# ----- Runtime executable ---------------------------------------------------

add_subdirectory(runtime)

# ----- Tests ----------------------------------------------------------------

if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
