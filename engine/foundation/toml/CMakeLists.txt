# crd-toml -- native TOML reader/writer on the engine containers (retires tomlplusplus).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-toml STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-toml
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-toml
    PUBLIC
        crd-core
        crd-containers
)

target_compile_features(crd-toml PUBLIC cxx_std_20)
crd_enable_default_pch(crd-toml)
