crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-vm STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-vm
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-vm
    PUBLIC
        crd-core
        crd-log
)

target_compile_features(crd-vm PUBLIC cxx_std_20)
crd_enable_default_pch(crd-vm)
