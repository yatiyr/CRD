# crd-hesap-diff — Phase 3.1.6 v13-l/m (Numerical-Analysis + Motion cluster). Numerical differentiation. ADR-0095.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math — deterministic scalar substrate (the moat: crd::math, never std::)
#   crd-hesap-dense — lstsq / linear solve (Savitzky-Golay normal equations, spectral helpers); SANITY 8
#   crd-hesap-fft — DCT/FFT (spectral differentiation, v13-m); SANITY 8

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-diff STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-diff
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-diff
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap          # CLI registry (hesap.diff.*); acyclic — crd-hesap is the leaf substrate
        crd-hesap-dense    # least-squares for Savitzky-Golay; acyclic (dense never references diff)
        crd-hesap-fft      # spectral differentiation (v13-m)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-diff PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-diff)
