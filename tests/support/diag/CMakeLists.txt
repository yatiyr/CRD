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

# The observer-lifetime negative control: a mid-flight set_observer() must trip the job system's
# quiescence fatal. Unlike the other specimens this one drives the real scheduler + observer, so it
# links crd-jobs (which brings crd-core/crd-containers transitively). The specimen installs an assert
# platform handler that turns the fatal into a deterministic exit code, so the harness reads a clean
# exit instead of hanging on the Windows assert dialog.
crd_diag_specimen(crd-diag-observer-swap-specimen SOURCES specimens/observer_swap_specimen.cpp)
target_link_libraries(crd-diag-observer-swap-specimen PRIVATE crd-jobs)

# The no-blocking-context negative control: an unenrolled thread waiting on a single-thread pool must
# trip the deadlock fatal. Links crd-jobs and the platform thread library (it spawns a std::thread).
crd_diag_specimen(crd-diag-unenrolled-wait-specimen SOURCES specimens/unenrolled_wait_specimen.cpp)
target_link_libraries(crd-diag-unenrolled-wait-specimen PRIVATE crd-jobs Threads::Threads)

# The seeded-deadlock positive control: two sibling jobs each parked on the other's completion counter form
# a wait cycle. Proves the hang watchdog's WaitCycle classification end-to-end -- impossible in-process (a
# live deadlock leaves counters acquired and shutdown asserts), so the watchdog fires on its own thread and
# _Exit(42)s on kind == WaitCycle. Drives the real scheduler + watchdog, so it links crd-jobs.
crd_diag_specimen(crd-diag-deadlock-specimen SOURCES specimens/deadlock_specimen.cpp)
target_link_libraries(crd-diag-deadlock-specimen PRIVATE crd-jobs Threads::Threads)

# The counter-pool exhaustion positive control: the second run() past max_counters must fire an always-on,
# counted fatal (not a silent nullptr the caller dereferences). Drives the real scheduler, so it links crd-jobs.
crd_diag_specimen(crd-diag-counter-exhaustion-specimen SOURCES specimens/counter_exhaustion_specimen.cpp)
target_link_libraries(crd-diag-counter-exhaustion-specimen PRIVATE crd-jobs)

# The fiber-pool exhaustion positive control: a concurrent dispatch past small_fiber_count must fire an
# always-on, counted fatal (not a silently dropped job). Spawns worker threads, so it also links Threads.
crd_diag_specimen(crd-diag-fiber-exhaustion-specimen SOURCES specimens/fiber_exhaustion_specimen.cpp)
target_link_libraries(crd-diag-fiber-exhaustion-specimen PRIVATE crd-jobs Threads::Threads)

# The DIAG.5a crash-capture acceptance specimen: installs the real crd crash handler and faults in one of several
# modes (av / concurrent / fastfail / denied / logger_lock / stack-overflow x3 / fiber / injected-failure) so the
# harness can verify termination, the retained fault reason and the on-disk dump outcome from outside the process.
# Links crd-core (the crash API), crd-jobs (the fiber mode drives a real fiber) and Threads (concurrent/overflow modes).
crd_diag_specimen(crd-diag-crash-capture-specimen SOURCES specimens/crash_capture_specimen.cpp)
target_link_libraries(crd-diag-crash-capture-specimen PRIVATE crd-core crd-jobs Threads::Threads)
