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

target_include_directories(crd-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${_crd_config_include}"
)

target_compile_features(crd-core PUBLIC cxx_std_20)

target_link_libraries(crd-core PUBLIC crd-warnings)
if(WIN32)
    target_link_libraries(crd-core PRIVATE dbghelp)
endif()

crd_enable_default_pch(crd-core)
