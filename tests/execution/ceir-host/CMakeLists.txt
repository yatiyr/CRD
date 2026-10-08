add_executable(crd-ceir-host-tests
    test_host_provider.cpp # CEIR-6b: task.parallel_for on crd::jobs -- map correctness, num_jobs determinism, pre-flight, errors
    test_plan_perf.cpp     # CEIR-11c: the crd-perf bridge adapter -- per-op dispatch counts + shape stats + self-time
    test_host_provenance.cpp # DIAG.8a: a parallel/launch/fold body error names its authored op (reference + plan agree)
    test_host_inspect.cpp  # DIAG.8b: pause with pending pooled jobs, resume/cancel, detached bodies never pause
    test_host_replay.cpp   # DIAG.9a: host run records -- the submitting thread's trace, the schedule, replay
    test_host_replay_diag.cpp # DIAG.9a: host records through replay.record / replay.run (the host executor)
    test_host_session_record.cpp # DIAG.9a: host records made under an inspection session equal unobserved ones
)

target_link_libraries(crd-ceir-host-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-host
    crd-ceir-cook       # DIAG.9a: cook the authored program and read its run record
    crd-jobs            # the test binary owns the jobs pool lifecycle (init/shutdown via a Catch listener)
    crd-perf            # CEIR-11c: read back the published counters (perf-ON builds)
    Catch2::Catch2WithMain
)

# DIAG.9a: the host replay commands run the committed authored program assets/ceir/host_replay_demo.ceir.
target_compile_definitions(crd-ceir-host-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-ceir-host-tests)
crd_discover_tests(crd-ceir-host-tests)
