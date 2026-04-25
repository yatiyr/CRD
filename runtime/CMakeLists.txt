crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_executable(crd-runtime ${SOURCES})

target_link_libraries(crd-runtime PRIVATE
    crd-core
    crd-log
)