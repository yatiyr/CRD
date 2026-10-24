# crd-diag-harness -- the DIAG.0 specimen harness: a bounded child-process runner
# that distinguishes an expected detection from an instrument failure. Contract:
# docs/design/runtime-diagnostics.md (shared acceptance rules). Owned by every
# module whose tests register diagnostic specimens (foundation/perf at DIAG.0).
include(CrdDiag)

add_library(crd-diag-harness STATIC src/specimen_runner.cpp)
target_include_directories(crd-diag-harness PUBLIC include)
target_link_libraries(crd-diag-harness PUBLIC crd-core crd-containers)
target_compile_features(crd-diag-harness PUBLIC cxx_std_20)
# DIAG.10b: what this lane promises, from its configuration rather than from any binary's own flags, so a specimen or
# a test that loses its sanitizer fails against the promise instead of reporting InstrumentAbsent and passing. The
# routes mirror the specimens': use-after-return needs gcc or clang ASan, LeakSanitizer additionally Linux.
crd_profile_condition(_crd_diag_lane_asan CRD_ENABLE_ASAN)
if(MSVC)
    set(_crd_diag_lane_gnu 0)
else()
    set(_crd_diag_lane_gnu 1)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_crd_diag_lane_linux 1)
else()
    set(_crd_diag_lane_linux 0)
endif()
target_compile_definitions(crd-diag-harness PRIVATE
    "CRD_DIAG_LANE_ASAN=$<IF:${_crd_diag_lane_asan},1,0>"
    "CRD_DIAG_LANE_TSAN=$<IF:$<BOOL:${CRD_ENABLE_TSAN}>,1,0>"
    "CRD_DIAG_LANE_GNU=${_crd_diag_lane_gnu}"
    "CRD_DIAG_LANE_LINUX=${_crd_diag_lane_linux}")

# The detector specimens the harness runs as bounded children (EXCLUDE_FROM_ALL;
# the tests that use them depend on them explicitly and receive their paths).
crd_diag_specimen(crd-diag-clean-specimen SOURCES specimens/clean_specimen.cpp)
crd_diag_specimen(crd-diag-crash-specimen SOURCES specimens/crash_specimen.cpp)
crd_diag_specimen(crd-diag-heap-overflow-specimen SOURCES specimens/heap_overflow_specimen.cpp)
# DIAG.6c(f): the unscoped-CPU-hotspot specimen the WPR/perf sampling runbook (scripts/sample-cpu-wpr.py) records.
# Standalone; NOT a crd-perf-tests dependency -- the runbook launches it directly.
crd_diag_specimen(crd-diag-cpu-hotspot-specimen SOURCES specimens/cpu_hotspot_specimen.cpp)

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

# The DIAG.3a allocator fatal-path control: a reallocate no pool can satisfy must reach the allocator's
# out-of-memory fatal (never a wrapped size). The specimen's assert platform handler turns the expected fatal
# into exit 42; crd-memory-tests runs it once per mode as a bounded child.
crd_diag_specimen(crd-diag-alloc-fatal-specimen SOURCES specimens/alloc_fatal_specimen.cpp)
target_link_libraries(crd-diag-alloc-fatal-specimen PRIVATE crd-core crd-memory)

# The DIAG.3b allocator-poison negative control: a stale or out-of-range read of a logical allocation (use after an
# arena reset or rewind, a freed pool slot, a one-byte over/underrun) must be reported by AddressSanitizer as
# use-after-poison, and a read past an Array's size() inside its capacity as container-overflow. crd-memory-tests
# runs it once per mode; without ASan it reports InstrumentAbsent.
crd_diag_specimen(crd-diag-allocator-poison-specimen SOURCES specimens/allocator_poison_specimen.cpp)
target_link_libraries(crd-diag-allocator-poison-specimen PRIVATE crd-core crd-memory crd-containers)

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

# The DIAG.10c queue-full positive control: a push into a full injection queue or a full pinned lane must fire an
# always-on, counted fatal (not a silently dropped or overwritten job). Mode argument: injection | pinned.
crd_diag_specimen(crd-diag-queue-full-specimen SOURCES specimens/queue_full_specimen.cpp)
target_link_libraries(crd-diag-queue-full-specimen PRIVATE crd-jobs)

# The DIAG.5a crash-capture acceptance specimen: installs the real crd crash handler and faults in one of several
# modes (av / concurrent / fastfail / denied / logger_lock / stack-overflow x3 / fiber / injected-failure) so the
# harness can verify termination, the retained fault reason and the on-disk dump outcome from outside the process.
# Links crd-core (the crash API), crd-jobs (the fiber mode drives a real fiber) and Threads (concurrent/overflow modes).
crd_diag_specimen(crd-diag-crash-capture-specimen SOURCES specimens/crash_capture_specimen.cpp)
target_link_libraries(crd-diag-crash-capture-specimen PRIVATE crd-core crd-jobs Threads::Threads)

# The DIAG.9b fatal-path flight-recorder specimen: records events, then enters a real fatal path (a failed
# CRD_DIAG_INVARIANT, or a headless assertion) with the flight triggers installed; the trigger's sink writes the
# window to a bundle before the process dies. crd-perf-tests runs it once per mode and reads the bundle.
crd_diag_specimen(crd-diag-flight-fatal-specimen SOURCES specimens/flight_fatal_specimen.cpp)
target_link_libraries(crd-diag-flight-fatal-specimen PRIVATE crd-perf)

# The DIAG.11a diagnostic-host specimen: a small application that starts the engine's DiagnosticHost from an authored
# profile, records its work and fails on purpose (a fatal invariant, a headless assertion, a write after free, a wait
# cycle or a job race); the profile's capture directory gets the flight bundle. crd-perf-tests runs each mode faulty
# and repaired (DIAG.12) and reads the bundle.
crd_diag_specimen(crd-diag-host-specimen SOURCES specimens/diag_host_specimen.cpp)
target_link_libraries(crd-diag-host-specimen PRIVATE crd-perf)

# The DIAG.9c corruption specimen: an off-by-one write into a sentinel that every reverse-debugging and watchpoint
# route of scripts/reverse-debug-recipes.py must name at its marked line. Debug information is part of that contract,
# so it is requested on every configuration. Unlike the other specimens no test executable depends on it, so a target
# in the default build carries it onto every lane, where it must compile and link.
crd_diag_specimen(crd-diag-corruption-specimen SOURCES specimens/corruption_specimen.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(crd-diag-corruption-specimen PRIVATE -g)
endif()
add_custom_target(crd-diag-reverse-debug-specimens ALL)
add_dependencies(crd-diag-reverse-debug-specimens crd-diag-corruption-specimen)

# The live gdb routes on Linux: a hardware watchpoint on every lane, and gdb's process record with an offline replay
# of its saved log plus the incomplete-recording control (an instruction log too small to hold the write must be
# reported, never passed). A host without gdb fails these tests as an instrument failure; it never skips them.
# Process record is not a route on the sanitizer lanes: the live recording works (134k instructions under ASan), but
# `record save` writes the process image as a core file and never finishes on the terabytes of shadow and allocator
# space the ASan and TSan runtimes reserve (gdb 15.1, measured past 300 s). That limit is declared here, at configure
# time, and the watchpoint route still runs there.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_crd_rd_script "${CMAKE_SOURCE_DIR}/scripts/reverse-debug-recipes.py")
    # Each debugger leg gets 60 s (a healthy one takes about 2 s), so the script reports a timeout as its own verdict
    # well inside the CTest budget below: the natural run and up to two gdb sessions.
    set(_crd_rd_args --specimen "$<TARGET_FILE:crd-diag-corruption-specimen>" --timeout 60
        --source "${CMAKE_CURRENT_SOURCE_DIR}/specimens/corruption_specimen.cpp")
    set(_crd_rd_tests crd-diag-reverse-debug-gdb-watch crd-diag-reverse-debug-gdb-watch-clean)
    add_test(NAME crd-diag-reverse-debug-gdb-watch
        COMMAND Python3::Interpreter "${_crd_rd_script}" --route gdb-watch --mode corrupt --expect FOUND
                ${_crd_rd_args} --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/reverse-debug/watch-corrupt")
    add_test(NAME crd-diag-reverse-debug-gdb-watch-clean
        COMMAND Python3::Interpreter "${_crd_rd_script}" --route gdb-watch --mode clean --expect CLEAN
                ${_crd_rd_args} --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/reverse-debug/watch-clean")
    if(CRD_ENABLE_ASAN OR CRD_ENABLE_TSAN)
        message(STATUS "[crd] DIAG.9c: gdb process record is not a route on a sanitizer build (its saved log would "
                       "hold the sanitizer's reserved address space); the watchpoint route runs")
    else()
        list(APPEND _crd_rd_tests crd-diag-reverse-debug-gdb-record crd-diag-reverse-debug-gdb-record-clean
             crd-diag-reverse-debug-gdb-record-incomplete)
        add_test(NAME crd-diag-reverse-debug-gdb-record
            COMMAND Python3::Interpreter "${_crd_rd_script}" --route gdb-record --mode corrupt --expect FOUND
                    ${_crd_rd_args} --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/reverse-debug/record-corrupt")
        add_test(NAME crd-diag-reverse-debug-gdb-record-clean
            COMMAND Python3::Interpreter "${_crd_rd_script}" --route gdb-record --mode clean --expect CLEAN
                    ${_crd_rd_args} --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/reverse-debug/record-clean")
        add_test(NAME crd-diag-reverse-debug-gdb-record-incomplete
            COMMAND Python3::Interpreter "${_crd_rd_script}" --route gdb-record --mode corrupt --insn-max 2000
                    --expect INCOMPLETE_RECORDING --expect-reason "fell out of the bounded log"
                    ${_crd_rd_args} --trace-dir "${CMAKE_CURRENT_BINARY_DIR}/reverse-debug/record-incomplete")
    endif()
    set_tests_properties(${_crd_rd_tests} PROPERTIES TIMEOUT 180 ENVIRONMENT "PYTHONUTF8=1")
endif()
