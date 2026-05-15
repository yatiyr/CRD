# crd-time -- Detour D-006 (queued 2026-05-15 per Strategic Execution Plan).
#
# Timing substrate. Leaf module: deps `crd-core` + `crd-units` only (the first
# major adopter of the units substrate, validating the Layer-1 LinearUnit +
# Quantity<dim::Time, f64> Duration pattern under a real consumer).
#
# Ships: Instant + Duration + Stopwatch + MonotonicClock/WallClock/CycleCounter
# + FrameClock (variable-step + fixed-step accumulator + alpha interpolation)
# + DeterministicClock + Deadline + sleep/yield + GPU timestamp delegation API.
#
# Absorbs the previous `engine/platform/include/crd/platform/timer.hpp` per the
# ADR-0076 §13 move-and-delete pattern; `crd-platform` then drops the timer
# surface and focuses on Window/Input/OS.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-time STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-time
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-time
    PUBLIC
        crd-core
        crd-units
)

target_compile_features(crd-time PUBLIC cxx_std_20)
crd_enable_default_pch(crd-time)
