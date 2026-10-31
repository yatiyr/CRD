crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

crd_configure_build_header(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/crd/core/build_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
)
set(_crd_config_include "${CMAKE_CURRENT_BINARY_DIR}/include")
if(CRD_NATIVE_PROFILES)
    string(APPEND _crd_config_include "/$<CONFIG>")
endif()

add_library(crd-core STATIC ${SOURCES} ${HEADERS})

# Build-tree paths only in the build tree; the installed package (cmake/CrdPackage.cmake) adds its own include/.
target_include_directories(crd-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${_crd_config_include}>"
)

target_compile_features(crd-core PUBLIC cxx_std_20)

# DIAG.9b: the I/O fault seam (crd/core/fault_seam.hpp) is compiled out of a shipping build, so nothing can install
# a fault hook there. Private: callers ask fault_seams_compiled(), never the macro.
if(NOT DEFINED _crd_shipping)
    message(FATAL_ERROR "[crd] crd-core needs the top-level shipping condition (_crd_shipping)")
endif()
target_compile_definitions(crd-core PRIVATE "CRD_FAULT_SEAMS=$<IF:${_crd_shipping},0,1>")

# The kernel dispatch's build cap (crd/core/kernel_dispatch.hpp, ADR-0134 §4): the value crd-simd-flags carries.
# Private: crd-core takes no ISA or floating-point options from crd-simd-flags, and callers ask kernel_level_cap().
if(NOT DEFINED _crd_kernel_cap)
    message(FATAL_ERROR "[crd] crd-core needs CrdSimd.cmake's kernel level cap (_crd_kernel_cap)")
endif()
target_compile_definitions(crd-core PRIVATE "CRD_KERNEL_LEVEL_CAP=${_crd_kernel_cap}")

# crd-warnings (/W4 /WX or -Wall -Werror) reaches every engine target through this edge and stays inside the
# build tree: a package consumer keeps its own warning policy.
target_link_libraries(crd-core PUBLIC $<BUILD_INTERFACE:crd-warnings>)
if(WIN32)
    # ntdll: the lifetime ledger's handle-table oracle (crd/core/os_resource_probe.hpp) asks NtQueryInformationProcess.
    target_link_libraries(crd-core PRIVATE dbghelp ntdll)
endif()

# An applied CRD_CPU_LEVEL lowers the CRT's own ISA dispatch from a startup object in cpu_features.cpp (ADR-0134 §8);
# the include keeps it in every image that links crd-core, also one that never asks for the selection.
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(amd64|x86_64|AMD64|x64)$")
    target_link_options(crd-core INTERFACE "LINKER:/INCLUDE:kCrdCpuSelectAtStart")
endif()

crd_enable_default_pch(crd-core)

# Cerid::core of the relocatable package: the public headers, the generated profile header and the archive.
crd_package_library(crd-core NAME core
    INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    GENERATED "${_crd_config_include}/crd/core/build_config.hpp" DESTINATION crd/core)
