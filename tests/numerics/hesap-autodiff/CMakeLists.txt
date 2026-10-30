# crd-hesap-autodiff tests — Phase 3.1.6 v15. This target also SERVES AS THE LINK-ISOLATION SMOKE (ADR-0097 §1):
# it links ONLY crd-hesap-autodiff (+ the base substrate) — a clean link proves a scalar-Dual/Jet consumer drags
# neither the tensor JVPs nor the solver bridges nor the reverse tape.
add_executable(crd-hesap-autodiff-tests
    test_dual_forward.cpp
    test_jet.cpp
    test_jet_simd.cpp
    test_jet_levels.cpp
    test_jvp_rules.cpp
    test_hyperdual.cpp
    test_drivers.cpp
    test_sparsity.cpp
    test_matrix_jvp.cpp
    test_taylor.cpp
    test_complex_dual.cpp
    test_ad_cli.cpp
    test_reverse.cpp
    test_reverse_rules.cpp
    test_nn_reverse.cpp
    test_bicoloring.cpp
    test_sparse_reverse.cpp
    test_matrix_reverse.cpp
    test_suite_reverse.cpp
    test_hvp.cpp
    test_vhvp.cpp
    test_ode_adjoint.cpp
    test_implicit_diff.cpp
    test_graph_ad.cpp
    test_determinism_moat.cpp
    test_batch_gradient_arms.cpp
    test_topopt.cpp
    test_neural_ode.cpp
    test_kan.cpp
)

target_link_libraries(crd-hesap-autodiff-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-autodiff
    Catch2::Catch2WithMain
)

# The jet probe, one unit per CPU level the build compiles (ADR-0134 §5, §7); test_jet_levels.cpp lists them.
crd_add_cpu_kernels(crd-hesap-autodiff-tests NAME jet_probe LEVELS portable sse2 avx2 avx512
                    INCLUDES crd-math crd-hesap-autodiff
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/jet_level_probe.cpp")

crd_enable_default_pch(crd-hesap-autodiff-tests)

crd_discover_tests(crd-hesap-autodiff-tests)

# ADR-0134 §8: the jet probe, the canned gradients' table and the CLI gradient once per compiled level
# (crd-hesap-autodiff-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-autodiff-tests "[kernels]")

# The vectorized HVP tape's and batch_gradient's fatals, one child process per mode (a fatal cannot run in-process): a
# refused node or scratch array, a scratch pool size that overflows, a node past the capacity, more than 32 batch
# parameters and a short batch span are fatal in every build, never a null, short or overrun array written. Source
# and protocol:
# tests/support/diag/specimens/hesap_autodiff_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-autodiff-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_autodiff_fatal_specimen.cpp")
target_link_libraries(crd-hesap-autodiff-fatal-specimen PRIVATE crd-core crd-memory crd-hesap-autodiff)
crd_diag_fatal_tests(crd-hesap-autodiff-fatal-specimen PREFIX crd-hesap-autodiff-fatal OWNER crd-hesap-autodiff-tests
    MODES vtape-nodes-refused vtape-pool-refused vtape-pool-overflow vtape-capacity-exceeded
          batch-too-many-params batch-short-gbuf batch-short-tapes)
