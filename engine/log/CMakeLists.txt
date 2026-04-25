crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-log STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-log
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-log
    PUBLIC
        crd-core
)

target_compile_features(crd-log PUBLIC cxx_std_20)
