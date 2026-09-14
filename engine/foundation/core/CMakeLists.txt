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

# crd-warnings (/W4 /WX or -Wall -Werror) reaches every engine target through this edge and stays inside the
# build tree: a package consumer keeps its own warning policy.
target_link_libraries(crd-core PUBLIC $<BUILD_INTERFACE:crd-warnings>)
if(WIN32)
    target_link_libraries(crd-core PRIVATE dbghelp)
endif()

crd_enable_default_pch(crd-core)

# Cerid::core of the relocatable package: the public headers, the generated profile header and the archive.
crd_package_library(crd-core NAME core
    INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    GENERATED "${_crd_config_include}/crd/core/build_config.hpp" DESTINATION crd/core)
