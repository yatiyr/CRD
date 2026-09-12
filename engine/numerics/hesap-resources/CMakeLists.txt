# crd-hesap-resources -- Phase 3.1.6 v4-corpus.
#
# Bridge module: makes sparse matrices first-class cooked engine resources
# ('HMTX' CRDR + cooker + ILoader). Depends on BOTH crd-resources and
# crd-hesap-sparse; neither depends on it (one-way edge). ADR-0084.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-resources STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-resources
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-resources
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math          # Complex<T> value variants (c32/c64)
        crd-hesap         # CLI substrate (arg_value/command_registry) for hesap.matrix.*
        crd-hesap-sparse  # SparseMatrix / read_matrix_market
        crd-resources     # CRDR / ILoader / ResourceManager
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-resources PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-resources)
