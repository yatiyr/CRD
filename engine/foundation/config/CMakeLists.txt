crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-config STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

target_link_libraries(crd-config
    PUBLIC
        crd-core
        crd-containers
        crd-log
        crd-math
        crd-platform
        ${_crd_toml_target}
)

target_compile_definitions(crd-config PRIVATE TOML_EXCEPTIONS=0)
target_compile_features(crd-config PUBLIC cxx_std_20)
crd_enable_default_pch(crd-config)
