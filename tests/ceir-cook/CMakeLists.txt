add_executable(crd-ceir-cook-tests
    test_program_cook.cpp    # CEIR-7a: cook a module -> a self-describing CRDR blob; round-trip, cross-Context reload, verify-reject, header validation
    test_runtime_program.cpp # CEIR-7b: RuntimeProgram + generation-safe RuntimeSlot handle; hash-validated load, splice-reject, generation staleness
    test_hot_reload.cpp      # CEIR-10a stage 2: the ReloadSet supervisor - body-edit hot-swap + handle staleness, contract-change/needs-migration reject + last-good, the 8h affected-set, dup-symbol + AssetId-0 reject
    test_plan_cache.cpp      # CEIR-10b: the execution-plan cache - content-addressed store validated on hit against live truth (deps by interface_hash), §107 extended to plans, clear/evict "never truth"
    test_band10_gate.cpp     # CEIR-10z: BAND-10 GATE (§172 matrix) - lifecycle (10a) + plan cache (10b) composed LIVE across the 3 edit classes (body-edit HotSwap keeps caller HIT, signature-reject keeps last-good, dep-edit recompiles affected(B)) with hit-count deltas
)

target_link_libraries(crd-ceir-cook-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-cook
    crd-resources      # the test FORGES CRDR containers directly (missing-CDEP / wrong-type) — the ceir-host precedent
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-cook-tests)
catch_discover_tests(crd-ceir-cook-tests)
