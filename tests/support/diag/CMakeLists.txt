# crd-diag-harness -- the DIAG.0 specimen harness: a bounded child-process runner
# that distinguishes an expected detection from an instrument failure. Contract:
# docs/design/runtime-diagnostics.md (shared acceptance rules). Owned by every
# module whose tests register diagnostic specimens (foundation/perf at DIAG.0).
include(CrdDiag)

add_library(crd-diag-harness STATIC src/specimen_runner.cpp)
target_include_directories(crd-diag-harness PUBLIC include)
target_link_libraries(crd-diag-harness PUBLIC crd-core crd-containers)
target_compile_features(crd-diag-harness PUBLIC cxx_std_20)

# The detector specimens the harness runs as bounded children (EXCLUDE_FROM_ALL;
# the tests that use them depend on them explicitly and receive their paths).
crd_diag_specimen(crd-diag-clean-specimen SOURCES specimens/clean_specimen.cpp)
crd_diag_specimen(crd-diag-crash-specimen SOURCES specimens/crash_specimen.cpp)
crd_diag_specimen(crd-diag-heap-overflow-specimen SOURCES specimens/heap_overflow_specimen.cpp)
