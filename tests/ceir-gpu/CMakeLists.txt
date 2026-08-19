add_executable(crd-ceir-gpu-tests
    test_lower.cpp      # CEIR-13d: lower_region -> the inspectable command list (dispatches + transfers + 4d-derived barriers)
    test_execute.cpp    # CEIR-13z-1: the execution seam (ADR-0126) -- validate_lowered/execute_lowered device-free error paths
    test_work_build.cpp # CEIR-20b: build_work_ceir -> a find_work_misuse-clean ceir.work program (queue_alloc/produce/consume/compact)
    test_ckir_synth.cpp # CEIR-22b: synth_gemm -> a graph-tier CKIR Contract (the bit-exact envelope) + the typed-rejects (device-free)
    test_tensor_pipeline.cpp # CEIR-22c-1: plan_tensor_pipeline -> the device-resident buffer graph (roles/bytes/def-use wiring/aliases) + typed-rejects + genericity (device-free)
    test_expand_ml.cpp  # CEIR-24b-3: expand_ml_ops -> rewrites ml.mlp/ml.attention into gemm/transpose/softmax/relu (the 22/23 vocab); post-expansion find_ml None + find_structure None + plans clean (device-free)
    test_band24_gate.cpp # CEIR-24z: the band-24 composing gate - ml.mlp (variadic tail) + ml.attention in ONE module compose (find_ml + structure None), survive TEXT + BINARY serializer round-trips byte-clean (first variadic binary crossing), and the sec-69 partitioner assigns the mixed module (coopvec claims mlp, attention falls back)
)

target_link_libraries(crd-ceir-gpu-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-gpu
    crd-gpu-context   # the LoweredCommand names crd::gpu::DispatchKind (the live bridge edge)
    crd-kir           # CEIR-22b: the synthesized kir::KGraph (transitive via crd-ceir-gpu PUBLIC; listed for the direct include)
    Catch2::Catch2WithMain
)

# CEIR-22c-3c: test_tensor_pipeline loads the authored assets/ceir/tensor_pipeline.ceir from the repo root (parse-load gate).
target_compile_definitions(crd-ceir-gpu-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-ceir-gpu-tests)
catch_discover_tests(crd-ceir-gpu-tests)
