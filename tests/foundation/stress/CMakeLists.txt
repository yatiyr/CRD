# Stress / concurrency-hardening tests (detour D-002).
#
# Own main (main_stress.cpp): installs crash dumps + initialises crd-jobs once
# for the whole binary — so individual tests must NOT call jobs::init().
#
# The harness header lives alongside the sources; add this dir to the include
# path so `#include "stress_harness.hpp"` resolves.
crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-stress-tests ${TEST_SOURCES})

target_include_directories(crd-stress-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(crd-stress-tests PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-jobs
    crd-scene   # v6: scene-storage concurrency stress
    Catch2::Catch2
)

crd_enable_default_pch(crd-stress-tests)

catch_discover_tests(crd-stress-tests)
