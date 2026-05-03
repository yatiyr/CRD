crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-platform STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-platform
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-platform
    PUBLIC
        crd-core
        crd-log
        crd-containers
        crd-memory
    PRIVATE
        glfw
        crd-jobs
)

target_compile_features(crd-platform PUBLIC cxx_std_20)
target_compile_definitions(crd-platform PRIVATE GLFW_INCLUDE_NONE)
crd_enable_default_pch(crd-platform)
