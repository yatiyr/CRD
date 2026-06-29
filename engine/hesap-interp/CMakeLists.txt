# crd-hesap-interp — Phase 3.1.6 v13 (Numerical-Analysis + Motion cluster). Interpolation. ADR-0095.
#
# Edges (acyclic; later sub-slices add special (where needed), geometry-delaunay (Sibson NNI reuse)):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math — deterministic scalar substrate (the moat: crd::math, never std::)
#   crd-hesap — Complex<T> (v13-d trig/Fourier); crd-hesap-fft — DCT-II/rFFT (v13-d Chebyshev/trig);
#   crd-hesap-dense — lstsq (v13-d Padé linear solve; v13-b/e spline/RBF systems use its tridiagonal/LU)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-interp STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-interp
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-interp
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap          # Complex<T> (v13-d trig)
        crd-hesap-fft      # DCT-II / rFFT (v13-d Chebyshev / trigonometric); SANITY 8
        crd-hesap-dense    # lstsq (v13-d Padé); acyclic — dense never references interp
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-interp PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-interp)
