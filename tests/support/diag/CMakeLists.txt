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

# The DIAG.1b data-race specimen spawns two threads, so it links the platform thread library
# (a no-op on MSVC; -pthread on clang/gcc). Caught by ThreadSanitizer on the linux-clang-tsan lane.
crd_diag_specimen(crd-diag-data-race-specimen SOURCES specimens/data_race_specimen.cpp)
find_package(Threads REQUIRED)
target_link_libraries(crd-diag-data-race-specimen PRIVATE Threads::Threads)

# The DIAG.3f stack-use-after-return specimen. Qualified on the gcc/clang ASan lanes (linux-gcc-asan);
# the specimen self-declares the route unavailable under MSVC ASan (see its header), so on the Windows
# ASan lane it reports InstrumentAbsent rather than a spurious catch. InstrumentAbsent without a sanitizer.
crd_diag_specimen(crd-diag-use-after-return-specimen SOURCES specimens/use_after_return_specimen.cpp)

# The DIAG.3f heap-use-after-free specimen. Caught by core AddressSanitizer on every ASan lane
# (win-asan and linux-gcc-asan); InstrumentAbsent on a non-sanitizer build.
crd_diag_specimen(crd-diag-use-after-free-specimen SOURCES specimens/use_after_free_specimen.cpp)

# The DIAG.3f memory-leak specimen. Qualified on the LeakSanitizer lane (linux-gcc-asan); self-tags the
# route absent (InstrumentAbsent) where no LSan interface exists (MSVC ASan / plain builds).
crd_diag_specimen(crd-diag-leak-specimen SOURCES specimens/leak_specimen.cpp)
