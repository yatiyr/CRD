# crd-hesap-quadrature — Phase 3.1.6 v12-c (Part 2). Gauss quadrature via Golub-Welsch.
#
# Edges (acyclic; NOT a leaf — it needs the eigensolver, which is why it is separate from leaf crd-hesap-special):
#   crd-core / crd-containers / crd-memory — types, Array, IAllocator
#   crd-hesap-special — orthogonal-polynomial recurrence coeffs, Γ / B (μ₀)
#   crd-hesap-dense   — eig_sym (the REUSED symmetric eigensolver; SANITY.md rule 8)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-quadrature STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-quadrature
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-quadrature
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-hesap          # CLI registry (hesap.quad.*); acyclic — crd-hesap is the leaf substrate
        crd-hesap-special
        crd-hesap-dense
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-quadrature PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-quadrature)
