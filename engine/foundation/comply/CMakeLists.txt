# crd-comply -- the legal and platform rules products consume (COMPLY.0; owner of compliance.obligations).
#
# Decodes the reviewed rule generations scripts/gen-comply-rules.py writes from cmake/legal-register.json
# (assets/comply/<code>.rules.toml, cerid-rules/1), swaps them live per jurisdiction and answers which rules bind a
# scope on a day, proven or not. ceridc comply serves the query. Contract: docs/design/registers.md#rule-generations.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-comply STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-comply
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-comply
    PUBLIC
        crd-core
        crd-containers
    PRIVATE
        crd-toml
)

target_compile_features(crd-comply PUBLIC cxx_std_20)
crd_enable_default_pch(crd-comply)

# Cerid::comply of the relocatable package.
crd_package_library(crd-comply NAME comply INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
