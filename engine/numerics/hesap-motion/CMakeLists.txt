# crd-hesap-motion — Phase 3.1.6 v13-n/o/p/q (Numerical-Analysis + Motion cluster). Trajectory generation. ADR-0095.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math — Quat slerp/log/exp (SQUAD), Vec (the moat: crd::math, never std::)
#   crd-hesap-special — Fresnel S/C (clothoid / Euler spiral); SANITY 8

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-motion STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-motion
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-motion
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap          # CLI registry (hesap.motion.*); acyclic — crd-hesap is the leaf substrate
        crd-hesap-special  # Fresnel S/C for the clothoid; acyclic
        crd-hesap-dense    # LU KKT solve for the minimum-snap QP (v13-p); SANITY 8, acyclic
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-motion PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-motion)
