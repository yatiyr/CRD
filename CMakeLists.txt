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
option(CRD_ENABLE_ASSERTS   "Enable CRD_ASSERT macros"        ON)
option(CRD_ENABLE_PROFILING "Enable profiling instrumentation" OFF)
option(CRD_ENABLE_ASAN      "Enable Address Sanitizer"         OFF)
option(CRD_ENABLE_UBSAN     "Enable Undefined Behavior San."   OFF)
option(CRD_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)

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
    add_compile_options(
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wshadow -Wdouble-promotion
        -Wnon-virtual-dtor -Woverloaded-virtual
        -Wnull-dereference -Wformat=2
    )
endif()

# ----- Dependencies (via CPM) ----------------------------------------------

include(cmake/CPM.cmake)

if (CRD_BUILD_TESTS)
    CPMAddPackage("gh:catchorg/Catch2@3.7.1")
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()

# ----- Helper: auto-collect sources with CONFIGURE_DEPENDS -----------------

function(crd_collect_sources out_var directory)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${directory}/*.cpp"
    "${directory}/*.hpp"
    "${directory}/*.h"
    )
    set(${out_var} ${sources} PARENT_SCOPE)
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
# add_subdirectory(engine/memory)
# add_subdirectory(engine/math)
# add_subdirectory(engine/containers)
# add_subdirectory(engine/log)
# add_subdirectory(engine/platform)
# add_subdirectory(engine/graphics)
# add_subdirectory(engine/scripting)

# ----- Runtime executable ---------------------------------------------------

add_subdirectory(runtime)

# ----- Tests ----------------------------------------------------------------

if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()