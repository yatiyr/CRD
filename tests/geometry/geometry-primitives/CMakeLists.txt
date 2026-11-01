# The predicates' differential test oracle (test_predicates_vs_shewchuk.cpp): Shewchuk's public-domain predicates.c,
# kept in reference/ verbatim but for two repairs (reference/README.md): its look-ahead reads are guarded and its K&R
# definitions are prototyped. It is compiled as C17 with GNU extensions (glibc declares random() only with them) under
# the tree's floating-point contract (crd-simd-flags: no contraction, no fast-math, SSE2 arithmetic on x64), because
# its expansions need each product and sum rounded once, and with the tree's warnings, as errors. The Windows SDK has
# no <sys/time.h>, which the source includes and never uses, and no random(), which only its unused random-input
# generators call: reference/win32 supplies the header, and random() becomes rand().
enable_language(C)
add_library(crd-geometry-shewchuk-oracle OBJECT reference/shewchuk-predicates.c)
set_target_properties(crd-geometry-shewchuk-oracle PROPERTIES
    C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS ON INTERPROCEDURAL_OPTIMIZATION OFF)
target_link_libraries(crd-geometry-shewchuk-oracle PRIVATE crd-simd-flags crd-warnings)
if(MSVC)
    target_include_directories(crd-geometry-shewchuk-oracle PRIVATE reference/win32)
    target_compile_definitions(crd-geometry-shewchuk-oracle PRIVATE random=rand)
endif()

add_executable(crd-geometry-primitives-tests
    test_primitives.cpp
    test_closest_point.cpp
    test_intersect.cpp
    test_barycentric.cpp
    test_formulary.cpp
    test_v0f_corpus.cpp
    test_v0f_simd.cpp
    test_primitive_kernels.cpp
    test_constants.cpp
    test_is_finite.cpp
    test_signed_distance.cpp
    test_convex_hull_view.cpp
    test_validation.cpp
    test_predicates.cpp
    test_predicates_vs_shewchuk.cpp
    test_queries_typed.cpp  # v0d-2
    test_transform.cpp      # v11
    test_circumcenter.cpp
)

target_include_directories(crd-geometry-primitives-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include" # test_corpus.hpp (v1i-c, ADR-0076 §15 validation discipline)
)

target_link_libraries(crd-geometry-primitives-tests PRIVATE
    crd-core
    crd-math                  # Vec3f/Mat3f/Mat4f + approx_equal_abs etc.
    crd-units                 # v0d-2 typed queries
    crd-geometry-primitives
    crd-geometry-shewchuk-oracle
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-primitives-tests)

crd_discover_tests(crd-geometry-primitives-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-geometry-primitives-kernels-at-<level>).
crd_add_kernel_level_tests(crd-geometry-primitives-tests "[kernels]")
