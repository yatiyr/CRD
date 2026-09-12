crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(CRD_DEBUG 1)
else()
    set(CRD_RELEASE 1)
endif()


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/crd/core/build_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/crd/core/build_config.hpp"
    @ONLY
)

add_library(crd-core STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
)

target_compile_features(crd-core PUBLIC cxx_std_20)

target_link_libraries(crd-core PUBLIC crd-warnings)
if(WIN32)
    target_link_libraries(crd-core PRIVATE dbghelp)
endif()

crd_enable_default_pch(crd-core)
