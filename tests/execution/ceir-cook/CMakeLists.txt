add_executable(crd-ceir-cook-tests
    test_program_cook.cpp    # CEIR-7a: cook a module -> a self-describing CRDR blob; round-trip, cross-Context reload, verify-reject, header validation
    test_runtime_program.cpp # CEIR-7b: RuntimeProgram + generation-safe RuntimeSlot handle; hash-validated load, splice-reject, generation staleness
    test_hot_reload.cpp      # CEIR-10a stage 2: the ReloadSet supervisor - body-edit hot-swap + handle staleness, contract-change/needs-migration reject + last-good, the 8h affected-set, dup-symbol + AssetId-0 reject
    test_plan_cache.cpp      # CEIR-10b: the execution-plan cache - content-addressed store validated on hit against live truth (deps by interface_hash), §107 extended to plans, clear/evict "never truth"
    test_band10_gate.cpp     # CEIR-10z: BAND-10 GATE (§172 matrix) - lifecycle (10a) + plan cache (10b) composed LIVE across the 3 edit classes (body-edit HotSwap keeps caller HIT, signature-reject keeps last-good, dep-edit recompiles affected(B)) with hit-count deltas
    test_reload_provenance.cpp # DIAG.8a: cook/reload provenance - file+positions in the blob, failed cook sites, installed positions kept/refreshed
    test_reload_inspect.cpp # DIAG.8b: runtime inspection across a hot reload - generation-bound sessions, line breakpoint rebinding, stale-generation refusals
    test_inspect_host.cpp   # DIAG.8b: the inspect host the headless and sandbox consumers share - the committed authored demo stopped, stepped, snapshotted, cancelled and reloaded
    test_program_diag.cpp   # DIAG.8c: the program.provenance diagnostic command - text, binary and cooked forms at their authored positions; refusals before work
    test_inspect_diag.cpp   # DIAG.8c: the program.inspect diagnostic command - Execute authority, scripted stops and values as items, argument refusals, stop bound, caller cancel
    test_replay_diag.cpp    # DIAG.8c: the replay.prepare diagnostic command - each replay input's need from effective effects (through calls), unknown for opaque ops, missing with its reason
    test_host_record.cpp    # DIAG.9a: recording at the inspect host - an inspected run records the unobserved trace, names its asset and generation across a reload, and refuses running, cancelled and unrecorded runs
    test_replay_inputs.cpp  # DIAG.9a: recorded host inputs - every input.random draw kept in the record and fed back on replay, never from a live source; refused reads named at their op; incomplete or uncaptured streams stored missing
    test_replay_record.cpp  # DIAG.9a: run records - replay.record writes the artifact, build, arguments, input states and a bounded trace; replay.run reproduces after an asset edit, names the first divergence and refuses incompatible records
    test_large_graph.cpp     # CEIR-35d Q6: large-graph SCALABILITY correctness - cook + load + execute a synthetic ~8k-op arith graph to the exact sum (no O(N^2)/buffer/recursion blowup); time-scaling is the Q7 board
)

target_link_libraries(crd-ceir-cook-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-cook
    crd-resources      # the test FORGES CRDR containers directly (missing-CDEP / wrong-type) — the ceir-host precedent
    crd-perf           # DIAG.8c: the diagnostic command service program.provenance, program.inspect and replay.prepare register into
    Catch2::Catch2WithMain
)

# DIAG.8b/9a: the inspect-host and replay gates read the committed authored programs under assets/ceir.
target_compile_definitions(crd-ceir-cook-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-ceir-cook-tests)
crd_discover_tests(crd-ceir-cook-tests)
