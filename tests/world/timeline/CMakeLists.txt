add_executable(crd-timeline-tests
    test_timeline.cpp # GEO-9: TIML round-trip + refusals, deterministic eval, automation oracles, THE RENDER GATE
)

target_link_libraries(crd-timeline-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-time
    crd-hesap-interp
    crd-resources
    crd-timeline
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-timeline-tests)

catch_discover_tests(crd-timeline-tests)
