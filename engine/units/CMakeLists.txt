# crd-units — Phase 3.1.7.5 v0a.
#
# Compile-time-dimensional units substrate. Leaf module (deps: crd-core only).
# Provides Quantity<D, T> zero-overhead wrapper + Dim<8 exponents> + dimensional
# arithmetic + the 6-layer conversion system (linear + affine + nonlinear +
# compound + federated + format-parse).
#
# Per the Strategic Execution Plan 2026-05-15 + ADR-0078 (candidate; mints at
# v0a close): every physical/scientific quantity in Cerid carries a unit
# via this module. SI everywhere internally; precision tier (f32/f64)
# orthogonal to dimension; layout-equal to bare scalar so SIMD/GPU upload
# reaches `.value` raw.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-units STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-units
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        # tx-route: the crd::math::* transcendental headers are header-only (inline, deps = crd-core only) and never
        # pull scalar.hpp (the Vec3<Quantity> part that makes crd-math depend on crd-units). So units INCLUDES them
        # for its deterministic nonlinear (dB) conversions WITHOUT linking crd-math — which would be a dep cycle.
        "${CMAKE_SOURCE_DIR}/engine/math/include"
)

target_link_libraries(crd-units
    PUBLIC
        crd-core         # types only (f32 / f64 / i32 / usize)
        crd-containers   # v0d-5: String / StringView for unit_preferences format/parse
        crd-memory       # v0d-5: IAllocator + default_allocator() for String ctor
)

target_compile_features(crd-units PUBLIC cxx_std_20)
crd_enable_default_pch(crd-units)
