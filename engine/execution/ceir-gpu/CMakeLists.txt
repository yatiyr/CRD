# crd-ceir-gpu — the CEIR-13d GPU LOWERING bridge (ADR-0125; ADR-0109 §4.2). Lowers a ceir.compute / ceir.transfer REGION
# to a pure, value-typed, INSPECTABLE command list (LoweredCommand); execution against a live IComputeContext is CEIR-13z.
# ⛔ This is a BRIDGE MODULE: it depends on BOTH crd-ceir AND crd-gpu-context (the canonical command_model.hpp value types).
# crd-ceir itself NEVER link-edges to gpu-context — the crd-ceir-invariants gate enforces the core stays host-only. ⛔ The
# crd-render-graph + crd-kir edges from ADR-0109 §4.2 are added WHEN CONSUMED (14 / 13z) — a zero-include edge is the
# acyclic gate's dead-edge smell (ADR-0125 §2.1, amendment-by-narrowing). ⭐ CEIR-22b CONSUMES crd-kir: the CEIR→CKIR
# native-provider SYNTHESIS (ckir_synth.hpp maps a declare-only ceir.linalg/ceir.tensor op → a CKIR KGraph). crd-kir is
# header-only + GPU-free, so ceir-gpu STAYS backend-free (the device gates live in tests/ceir-gpu-{vulkan,dx12}).
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-ceir-gpu STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-ceir-gpu
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-ceir-gpu
    PUBLIC
        crd-ceir         # the IR + the 4d hazard machinery (ops_hazard) the barriers derive from
        crd-gpu-context  # the canonical command model (command_model.hpp: DispatchKind, DispatchDesc, validate_dispatch)
        crd-kir          # CEIR-22b: the CKIR KGraph the native provider SYNTHESIZES (ckir_synth.hpp) — header-only, GPU-free
        crd-core
        crd-containers
        crd-memory
    PRIVATE
        crd-warnings
)

target_compile_features(crd-ceir-gpu PUBLIC cxx_std_20)
crd_enable_default_pch(crd-ceir-gpu)
