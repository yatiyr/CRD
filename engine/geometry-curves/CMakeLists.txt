# crd-geometry-curves — Phase 3.1.7 v10.
#
# Curve / spline substrate: the 12th (and final) `crd-geometry-*` sibling
# under the substrate umbrella. v10 splits into 5 algorithmic slices:
#
#   v10a  Curve primitive types (Polyline + 7 curve kinds + 5 2D peers) +
#         `evaluate(curve, t)` parametric evaluator + `evaluate_derivative` +
#         `validate(curve)` cooker-friendly per-kind validators. THIS SLICE.
#   v10b  Sampling + flattening (sample_uniform / sample_adaptive /
#         sample_by_curvature / to_polyline).
#   v10c  Arc-length system (build_arclength_table / length_of /
#         t_at_distance / distance_at_t).
#   v10d  Curve queries (aabb_of / closest_point / distance / intersect_ray).
#   v10e  Frames + viz + sandbox showcase (tangent / normal / binormal /
#         RMF via Wang 2008 double-reflection method).
#
# Two-layer typing per ADR-0078 §5: control-point storage stays raw
# `Vec3<T>` / `Vec2<T>` inside each curve struct; the API surface uses
# `Vec3<Length<T>>` strip-compute-retag wrappers at boundaries.
#
# Determinism: no `std::sort`, no transcendentals outside
# `crd::math::deterministic` (CircularArc / EllipseArc angle math uses
# `det::sin`/`det::cos`). Bezier de Casteljau + Hermite + B-spline
# Cox-de-Boor are pure polynomial — no transcendentals on the hot path.
#
# **D186 (v10a)** — `evaluate(curve, t)` IS the algorithm definition.
# Every downstream query (sampling, arc-length, closest-point, AABB)
# eventually calls `evaluate` or `evaluate_derivative`. Mirrors the
# CPU-IS-the-algorithm-definition discipline that load-bears the rest
# of `crd-geometry` (D134 / D157 / D170).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-curves STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-curves
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-curves
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
)

target_compile_features(crd-geometry-curves PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-curves)
