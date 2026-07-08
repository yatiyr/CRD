# crd-kir tests — Phase 3.1.6 v17. Also the LINK-ISOLATION SMOKE (ADR-0098): links ONLY crd-kir (+ base substrate) —
# a clean link proves the IR + CPU reference drag no GPU API (no vulkan.h symbol) into a consumer.
add_executable(crd-kir-tests
    test_ckir_graph.cpp
    test_ckir_grad.cpp
    test_ckir_opt.cpp
    test_ckir_glsl.cpp
    test_ckir_backend.cpp
    test_ckir_tile.cpp
    test_ckir_msl.cpp
)

target_link_libraries(crd-kir-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-shader   # v17-b: compile the emitted GLSL to SPIR-V (validates the emitter without a GPU)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-tests)

catch_discover_tests(crd-kir-tests)
