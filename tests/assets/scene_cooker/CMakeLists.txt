add_executable(crd-scene-cooker-tests
    test_scene_cooker.cpp
    test_obek_cooker.cpp
)

target_link_libraries(crd-scene-cooker-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-scene
    crd-cooker
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-cooker-tests)

crd_discover_tests(crd-scene-cooker-tests)

# The cookers' out-of-memory fatals, one child process per mode (a fatal cannot run in-process): an allocator that
# refuses a cooker's reader registry leaves it out of memory, fatal in every build, never a null registry constructed
# into. Source and protocol: tests/support/diag/specimens/scene_cooker_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-scene-cooker-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/scene_cooker_fatal_specimen.cpp")
target_link_libraries(crd-scene-cooker-fatal-specimen PRIVATE crd-core crd-memory crd-scene crd-cooker)
crd_diag_fatal_tests(crd-scene-cooker-fatal-specimen PREFIX crd-scene-cooker-fatal OWNER crd-scene-cooker-tests
    MODES scene-cooker-refused obek-cooker-refused)
